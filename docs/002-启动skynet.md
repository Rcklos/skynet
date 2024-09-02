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

	// 启动logger服务
	bootstrap(ctx, config->bootstrap);

	// 启动工作线程(默认8个)，这里skynet已经启动完成了
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
