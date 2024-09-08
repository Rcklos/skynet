## 基于消息的skynet服务

`skynet`基本上没有消息走不动路，或者说，没有消息可能各个进程都闲得在拍蚊子了。

### 工作进程启动后都在做什么

> 这里的工作进程指的是`thread_worker`

先看一下`thread_worker`线程启动后在干嘛,

```c
/**
 * worker线程启动入口
 */
static void *thread_worker(void *p) {
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
```

从以上的代码可以知道，这个工作线程就是会去`skynet`里面拿出一个消息队列，然后进行消费(派发)，所以这个我们可以关注的函数就是`skynet_context_message_dispatch`。

```c
/**
 * ctx消费消息
 */
struct message_queue *skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
	if (q == NULL) {
		// 尝试从全局队列里面弹出一个消息队列
		q = skynet_globalmq_pop();
		if (q==NULL)
			// 弹不出来东西就让工作线程歇着吧
			return NULL;
	}

	// 取到消息队列的ctx句柄，也就是每个消息队列都是有对应的服务去消费的
	uint32_t handle = skynet_mq_handle(q);

	// 根据句柄获取到对应的ctx对象
	struct skynet_context * ctx = skynet_handle_grab(handle);
	// 拿到的ctx已经无了的话，这里就是有问题的
	if (ctx == NULL) {
		struct drop_t d = { handle };
		skynet_mq_release(q, drop_message, &d);
		return skynet_globalmq_pop();
	}

	int i,n=1;
	struct skynet_message msg;

	for (i=0;i<n;i++) {
		// 消息队列到手，尝试弹出消息
		if (skynet_mq_pop(q,&msg)) {
			skynet_context_release(ctx);
			return skynet_globalmq_pop();
		} else if (i==0 && weight >= 0) {
			// 权重越高消费能力越强
			n = skynet_mq_length(q);
			n >>= weight;
		}
		int overload = skynet_mq_overload(q);
		if (overload) {
			skynet_error(ctx, "May overload, message queue length = %d", overload);
		}

		// 监控消息触发
		skynet_monitor_trigger(sm, msg.source , handle);

		if (ctx->cb == NULL) {
			skynet_free(msg.data);
		} else {
			// 派发消息
			dispatch_message(ctx, &msg);
		}

		// 监控消息重置
		skynet_monitor_trigger(sm, 0,0);
	}

	assert(q == ctx->queue);
	struct message_queue *nq = skynet_globalmq_pop();
	if (nq) {
		// If global mq is not empty , push q back, and return next queue (nq)
		// Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)
		skynet_globalmq_push(q);
		q = nq;
	} 
	skynet_context_release(ctx);

	return q;
}
```

以上代码看的只干了三件事：

1. 从全局队列里取出消息队列
2. 尝试从消息队列里面取消息，然后对消息进行消费
3. 将消息队列压回全局队列里，并且释放ctx

这里的话，队列操作就比较常规了，所以真正核心的函数就只有一个：`dispatch_message`，所以进来看看实现方法。

```c
/**
 * 派发消息
 */
static void dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
  // ... 减少阅读压力，这里不帖别的内容了
  // 这里实际上就是在消费了
  reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);
  // ... 减少阅读压力，这里不帖别的内容了
}
```

前面的笔记里知道，`ctx`本质上就是一个服务，其`cb`字段和`cb_ud`字段是服务接收消息的回调函数和固定参数，剩下的基本上就是消息内容了。

> 以上就是一个工作线程的工作内容，这里主要的目的是能够了解到`skynet`如何产生service的消息。

这里开始继续探究`skynet`的话，就能产出两个新的问题：

1. `skynet`消息队列的消息是怎么产生的?
2. `skynet`的`ctx`是怎么消费消息的?

先关心第一个问题，消息队列的消息从哪来的。`skynet`是一个游戏服务端的框架，服务端的消息很直接地，就是从`socket`过来的，直接猜测都能知道`socket`连接客户端，然后将从客户端读到的字节串消息压入对应的消息队列里。

> 所以下一步，需要去看看socket在干什么。

### Skynet怎么管理socket

前面的笔记中有提到一行代码，用于启动`socket`线程。

```C
// socket线程启动
create_thread(&pid[2], thread_socket, m);
```

现在就看看它怎么启动。

```c
/**
 * socket线程入口
 */
static void *thread_socket(void *p) {
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
```

比较核心的函数是`skynet_socket_poll`，可以进去在看看，但下面就只贴一行核心的代码，

```c
// 转发消息
forward_message(SKYNET_SOCKET_TYPE_XXX, padding, &result);
```

其中`SKYNET_SOCKET_TYPE_XXX`是消息类型，连接(connect)、关闭(close)、数据(data)等，然后`padding`用于拆包，有些服务是可以直接处理字节串的，所以这个`padding`...
