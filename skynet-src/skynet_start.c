#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/**
 * skynet监控数据结构
 */
struct monitor {
	// 线程数量
	int count;
	struct skynet_monitor ** m;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	int sleep;
	int quit;
};

/**
 * 工作线程的参数
 */
struct worker_parm {
	// 线程监控
	struct monitor *m;
	// 线程id
	int id;
	int weight;
};

static volatile int SIG = 0;

static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

#define CHECK_ABORT if (skynet_context_total()==0) break;

static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond);
	}
}

/**
 * socket线程入口
 */
static void *
thread_socket(void *p) {
	struct monitor * m = p;
	// 初始化线程的TSD
	skynet_initthread(THREAD_SOCKET);
	// 不断死循环
	for (;;) {
		// 处理socket消息
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		wakeup(m,0);
	}
	return NULL;
}

static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}

static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;
	skynet_initthread(THREAD_MONITOR);
	for (;;) {
		CHECK_ABORT
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		for (i=0;i<5;i++) {
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

static void
signal_hup() {
	// make log file reopen

	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		skynet_context_push(logger, &smsg);
	}
}

static void *
thread_timer(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_TIMER);
	for (;;) {
		skynet_updatetime();
		skynet_socket_updatetime();
		CHECK_ABORT
		wakeup(m,m->count-1);
		usleep(2500);
		if (SIG) {
			signal_hup();
			SIG = 0;
		}
	}
	// wakeup socket thread
	skynet_socket_exit();
	// wakeup all worker thread
	pthread_mutex_lock(&m->mutex);
	m->quit = 1;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->mutex);
	return NULL;
}

/**
 * worker线程启动入口
 */
static void *
thread_worker(void *p) {
	// worker参数
	struct worker_parm *wp = p;
	int id = wp->id;
	int weight = wp->weight;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	// 和之前一样初始化TSD
	skynet_initthread(THREAD_WORKER);
	struct message_queue * q = NULL;
	// 监控不退我不退
	while (!m->quit) {
		// 尝试从全局队列里面弹出一个消息队列进行消费
		// 函数的字面意思就是ctx消息分发
		q = skynet_context_message_dispatch(sm, q, weight);
		// 如果发现取不到消息的话，就直接跑休眠的逻辑
		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

/**
 * skynet启动工作线程
 */
static void
start(int thread) {
	// 这里使用pthread启动的线程
	// 配置默认是8个工作线程，这里本质上会额外加3个，总共11个，但最终常驻为skynet服务的基本上就是8个
	// 多出来的3个分别是:
	// 1. 监控线程
	// 2. 定时器线程
	// 3. socket线程
	pthread_t pid[thread+3];

	// 总监控，也就是管理工作线程的
	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;
	m->sleep = 0;

	// 每个线程自己也会有一个线程监控
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}
	// 互斥量初始化
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	// 监控线程启动
	create_thread(&pid[0], thread_monitor, m);
	// 定时器线程启动
	create_thread(&pid[1], thread_timer, m);
	// socket线程启动
	create_thread(&pid[2], thread_socket, m);

	// 每个工作线程都是竞争拿到消息队列去消费的，这个weight大概率是用来控制权重的
	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };
	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			wp[i].weight = 0;
		}
		// 前面的weight先跳过，关注这里就是worker实际开始执行的地方
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	// 等待所有线程跑完
	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 
	}

	free_monitor(m);
}

/**
 * 一般是启动snlua服务
 * 前面会启动logger的c服务，现在是启动第一个lua服务
 * 但在启动lua服务前，我们需要启动一个snlua的c服务
 */
static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	// 这里会去执行命令行, 一般就是`snlua bootstrap`
	int sz = strlen(cmdline);
	char name[sz+1];
	char args[sz+1];
	int arg_pos;
	sscanf(cmdline, "%s", name);  
	arg_pos = strlen(name);
	if (arg_pos < sz) {
		while(cmdline[arg_pos] == ' ') {
			arg_pos++;
		}
		strncpy(args, cmdline + arg_pos, sz);
	} else {
		args[0] = '\0';
	}
	// 这里和c服务一样，都是会去开启一个新的ctx，这个服务就叫snlua
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

/**
 	* skynet实际启动入口
 	*/
void 
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
	// 注册挂起信号处理函数
	struct sigaction sa;
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);

	// 守护进程部分的逻辑
	if (config->daemon) {
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}

	// 集群初始化  TODO: 暂时不看集群
	skynet_harbor_init(config->harbor);
	// 句柄初始化
	skynet_handle_init(config->harbor);
	// 队列初始化
	skynet_mq_init();
	// 模块初始化
	skynet_module_init(config->module_path);
	// 定时器初始化
	skynet_timer_init();
	// socket初始化
	skynet_socket_init();
	// 统计信息是否启用(直接修改GNODE)
	skynet_profile_enable(config->profile);

	// 注册一个log服务，每个服务都会绑定一个ctx对象
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	// 给log服务的ctx句柄命名为`logger`
	skynet_handle_namehandle(skynet_context_handle(ctx), "logger");

	// 启动snlua服务(默认是这个)
	bootstrap(ctx, config->bootstrap);

	// 启动各种线程(默认8个工作线程, 3个额外线程)，这个函数跑完skynet已经启动完成了
	start(config->thread);

	// 走到这里意味着skynet要关闭了
	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();
	skynet_socket_free();
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
