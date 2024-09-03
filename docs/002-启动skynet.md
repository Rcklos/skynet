### 启动skynet

这篇和上篇不一样，因为上篇不是很复杂，一次性全贴上代码都可以。

这篇就不行了，毕竟这里想做一下筛选，暂时不重要的就不想聊。例如集群的概念，集群大概率会放到后面再聊。

### skynet启动做了什么?

1. 注册挂起信号的处理函数，用于重新打开log文件
2. (跳过) 【启用守护进程】 初始化守护进程
3. (跳过) 初始化港口(Harbor)
4. 初始化消息队列
5. 初始化定时器
6. 初始化socket
7. 启用/关闭统计信息
8. 注册并启用logger服务
9. 启动skynet工作线程开始工作
10. skynet关闭收尾

### 怎么去初始化消息队列

```c

/**
 * 全局队列的数据结构
 * 队列里面是消息队列的队列
 * 是的，嵌套队列
 */
struct global_queue {
	// 头部节点
	struct message_queue *head;
	// 尾部节点
	struct message_queue *tail;
	// 自旋锁
	struct spinlock lock;
};

/**
 	* 初始化全局队列
 	*/
void skynet_mq_init() {
	// 全局队列
	struct global_queue *q = skynet_malloc(sizeof(*q));
	// 清空队列
	memset(q,0,sizeof(*q));
	// 全局队列自旋锁初始化
	SPIN_INIT(q);
	Q=q;
}
```

这些初始化的工作本质上都很差不多，有锁就初始化锁，有字段就先设置一个初始值。

后面就列举下各种数据结构的定义即可。

### 初始化的各种数据结构

```c
/**
 * 句柄存储数据结构
 */
struct handle_storage {
	// 读写锁
	struct rwlock lock;

	// 集群
	uint32_t harbor;
	// 句柄索引
	uint32_t handle_index;
	// 存储相关
	int slot_size;
	struct skynet_context ** slot;
	
	// 命名相关
	int name_cap;
	int name_count;
	struct handle_name *name;
};


/**
 * c模块组件数据结构
 */
#define MAX_MODULE_TYPE 32
struct modules {
	// 计数，模块数量
	int count;
	// 自旋锁
	struct spinlock lock;
	// 模块路径
	const char * path;
	// 模块缓存
	struct skynet_module m[MAX_MODULE_TYPE];
};

/**
* 定时器数据结构
*/
struct timer {
	// 时间轮(https://yuerer.com/Skynet%E6%97%B6%E9%97%B4%E8%BD%AE%E5%89%96%E6%9E%90/)
	struct link_list near[TIME_NEAR];
	struct link_list t[4][TIME_LEVEL];
	struct spinlock lock;
	uint32_t time;
	uint32_t starttime;
	uint64_t current;
	uint64_t current_point;
};


/**
 * socket服务器数据结构
 */
struct socket_server {
	// 时间
	volatile uint64_t time;
	// 保留描述符
	int reserve_fd;	// for EMFILE
	// 接收控制描述符
	int recvctrl_fd;
	// 发送控制描述符
	int sendctrl_fd;
	// 检查控制
	int checkctrl;
	// poll事件描述符
	poll_fd event_fd;
	// 分配id
	ATOM_INT alloc_id;
	// 事件数量
	int event_n;
	// 事件索引
	int event_index;
	// socket对象接口(缓冲区)
	struct socket_object_interface soi;
	// 事件存储
	struct event ev[MAX_EVENT];
	// socket存储
	struct socket slot[MAX_SOCKET];
	// 缓冲区(缓存一些共用的数据)
	char buffer[MAX_INFO];
	// udp的buff缓冲区
	uint8_t udpbuffer[MAX_UDP_PACKAGE];
	// 可以看看哪些是可读的描述符
	fd_set rfds;
};
```

以上每一块部分，例如定时器的时间轮设计、模块的缓存机制之类、socket服务器的设计都可以一看，但现在的目标是优先把流程看完，所以了解个大概就可以了。后续有时间可以单独看一看，单独写一写笔记。

### skynet中C服务模块的加载过程

skynet支持加载C语言编写的服务模块，同时加载lua模块的时候，本质上也是用一个`snlua`的C服务模块去加载的。

skynet中第一个启动的C服务模块应该就是`logger`模块了。

```c

// 注册一个log服务，每个服务都会绑定一个ctx对象
struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
if (ctx == NULL) {
	fprintf(stderr, "Can't launch %s service\n", config->logservice);
	exit(1);
}

// 给log服务的ctx句柄命名为`logger`
skynet_handle_namehandle(skynet_context_handle(ctx), "logger");
```

`logger`模块是有命名的，除开命名之外，注册加载一个C语言服务模块就只需要调用一个`skynet_context_new`函数，然后该函数会返回一个`skynet_context`类型的指针，后续会用`ctx`表示。

进入`skynet_context_new`函数看一下

```c
/**
 * 创建一个新的ctx
 * 实际上就是注册一个服务，然后创建并绑定一个ctx
 */
struct skynet_context * skynet_context_new(const char * name, const char *param) {
	// 加载c服务模块(如果加载过了这一步就会直接返回缓存的结果)
	struct skynet_module * mod = skynet_module_query(name);

	// 加载失败就嗝屁
	if (mod == NULL)
		return NULL;

	// 创建实例，这里会调用`xxx_create`函数
	void *inst = skynet_module_instance_create(mod);
	// 失败了就嗝屁
	if (inst == NULL)
		return NULL;
	// 创建一个新的ctx实例
	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx));
	// c模块已经加载完成了，这时候可以绑定一下ctx
	CHECKCALLING_INIT(ctx)

	// 绑定工作
	ctx->mod = mod;
	ctx->instance = inst;
	ATOM_INIT(&ctx->ref , 2);
	ctx->cb = NULL;
	ctx->cb_ud = NULL;
	ctx->session_id = 0;
	ATOM_INIT(&ctx->logfile, (uintptr_t)NULL);

	ctx->init = false;
	ctx->endless = false;

	ctx->cpu_cost = 0;
	ctx->cpu_start = 0;
	ctx->message_count = 0;
	ctx->profile = G_NODE.profile;
	// Should set to 0 first to avoid skynet_handle_retireall get an uninitialized handle
	// 初始化ctx句柄之前先置0
	ctx->handle = 0;	
	// 注册句柄
	ctx->handle = skynet_handle_register(ctx);
	// 每个ctx都会附带一个消息队列，skynet中，服务之间的交互都是用消息队列实现的
	// ctx和mq的绑定还有其他机制，这里暂时先直接理解为ctx都附带着一个消息队列
	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);
	// init function maybe use ctx->handle, so it must init at last
	context_inc(); // skynet_node.total原子自增

	CHECKCALLING_BEGIN(ctx)
	// 这时候就是调用c模块对应的`init`的时候了
	int r = skynet_module_instance_init(mod, inst, ctx, param);
	CHECKCALLING_END(ctx)
	if (r == 0) {
		// 调用就直接释放ctx
		struct skynet_context * ret = skynet_context_release(ctx);
		if (ret) {
			// 这里ctx是引用计数的，一开始的时候ref == 2，所以这个时候还不会直接delete
			// 所以只要引用计数正常，就意味着初始化已经成功了
			ctx->init = true;
		}
		// 这里会把消息队列压入全局队列里，因为ctx没必要一直持有
		skynet_globalmq_push(queue);
		if (ret) {
			// 这里只是用了error的打印样式
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
		}
		return ret;
	} else {
		skynet_error(ctx, "FAILED launch %s", name);
		uint32_t handle = ctx->handle;
		skynet_context_release(ctx);
		skynet_handle_retire(handle);
		struct drop_t d = { handle };
		skynet_mq_release(queue, drop_message, &d);
		return NULL;
	}
}

```

上面的注释已经该说的都说了，启动完`logger`模块后，skynet还会调用一个函数，用于启动`snlua`服务。

> 本质上是配置的，一般没改的话`snlua`还不会做什么事情

```c
 // 启动snlua服务(默认是这个)
 bootstrap(ctx, config->bootstrap);
```

### 启动Skynet现场

本质上skynet只有四种线程：

1. 监控线程
2. 定时器线程
3. socket线程
4. 服务线程(worker)

前三种线程在skynet中都是唯一的，服务线程一般会通过配置去指定数量，默认是8个。

```c
 // 启动各种线程(默认8个工作线程, 3个额外线程)，这个函数跑完skynet已经启动完成了
 start(config->thread);
```

### 完整启动代码

```c
/**
 	* skynet实际启动入口
 	*/
void skynet_start(struct skynet_config * config) {
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
```
