## 从Skynet根据配置启动

> 终于有时间好好看看并学习Skynet的源码了，这次一定要看个够！！！

需要注意的是，代码篇幅很大，这里只是抽一些比较能看的且不影响整体阅读的代码展示，实际可以看看我的仓库。

> https://github.com/Rcklos/skynet

### skynet怎么启动

```bash
# 官方的启动示例
./skynet examples/config
```

从官方的启动示例来看，启动一个skynet主程序需要带着一个参数，不难知道这个参数是一个配置文件的路径。具体配置是什么先不看，如果需要了解就先去把Skynet应用看完，可能Skynet使用的笔记也会放出来。

现在看看`skynet_main.c`启动时先做了什么

```c
/**
	* 整个skynet程序的入口
 	*/
int main(int argc, char *argv[]) {
	const char * config_file = NULL ;
	if (argc > 1) {
	  // 从启动参数中获取配置文件
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

	// 节点初始化(GNODE)
	skynet_globalinit();
	// 环境初始化
	skynet_env_init();

	// 防止收到SIGPIPE信号而导致进程被杀死
	sigign();

	struct skynet_config config;

#ifdef LUA_CACHELIB
	// init the lock of code cache
	luaL_initcodecache();
#endif

	// 创建lua_state
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#luaL_newstate
	struct lua_State *L = luaL_newstate();
	// 链接lua的一些库
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#luaL_openlibs
	luaL_openlibs(L);	// link lua lib

	// 加载【配置预处理】的lua文本代码(注意这里还没有加载进配置)
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#luaL_loadbufferx
	int err =  luaL_loadbufferx(L, load_config, strlen(load_config), "=[skynet config]", "t");
	assert(err == LUA_OK);
	// 压入配置文件路径
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_pushstring
	lua_pushstring(L, config_file);

	// 开始预处理
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_pcall
	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}
	// 这里才是真正写入配置的时候
	_init_env(L);
	// 临时的lua_state使命完成了，安心去吧(close)
	lua_close(L);

	// 读取配置
	config.thread =  optint("thread",8);
	config.module_path = optstring("cpath","./cservice/?.so");
	config.harbor = optint("harbor", 1);
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	config.daemon = optstring("daemon", NULL);
	config.logger = optstring("logger", NULL);
	config.logservice = optstring("logservice", "logger");
	config.profile = optboolean("profile", 1);

	// 根据配置启动skynet
	// 到了这一步终于是开始启动了
	skynet_start(&config);

	// skynet退出的逻辑, 本质上就是回收线程的TSD
	skynet_globalexit();

	return 0;
}
```

总结下来好像就干了几件事情：

1. 初始化GNODE结构
2. 初始化全局env
3. 干掉SIGPIPE
4. 读取配置文件并写入env
5. 从env读取config并启动skynet

### 如何初始化GNODE

以下代码略看看就行，主要就是赋一些默认值的事情

```c
/**
 * 初始化skynet的GNODE
 */
void skynet_globalinit(void) {
	// 置0，这里要用原子操作，意味着这个字段是需要线程安全的
	ATOM_INIT(&G_NODE.total , 0); 
	// 监听器句柄置0，这个字段暂时用不着，无视它先(主要我现在还不知道它有什么用)
	G_NODE.monitor_exit = 0;
	// 是否已经初始化
	G_NODE.init = 1;
	// 创建私有数据Key
	// 学习文档: https://www.cnblogs.com/zhangxuan/p/6515264.html
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}
	// set mainthread's key
	// 设置主线程的私有数据
	skynet_initthread(THREAD_MAIN);
}
```

GNODE主要是还没看到，所以这里先不玩，我们先记下GNODE结构就好

```c
/**
 	* skynet的GNODE数据结构
 	*/
struct skynet_node {
	// 上下文的数量
	ATOM_INT total;
	// 是否已经初始化
	int init;
	// 一般是用于谁监听释放了什么的，但初步搜了下基本上没有地方在用
	uint32_t monitor_exit;
	// 线程私有数据句柄
	pthread_key_t handle_key;
	// 是否启用统计信息, 默认启用
	bool profile;	// default is on
};
```

### 初始化全局env

```c
/**
* skynet_env结构
*/
struct skynet_env {
	struct spinlock lock;
	lua_State *L;
};

// 全局变量
static struct skynet_env *E = NULL;

/**
 	* 初始化全局env
 	*/
void
skynet_env_init() {
	// 这里给E分配一块env的内存
	E = skynet_malloc(sizeof(*E));
	// 初始化自旋锁, 代表这个env是全局共用的，每个线程都能读写
	SPIN_INIT(E)
	// 分配一块lua_state，代表这块env维护是用Lua做的
	// 有一说一，看到这里的时候感觉作者真的好爱lua
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#luaL_newstate
	E->L = luaL_newstate();
}
```

需要注意的是，skynet的env全部都是存到一个`lua_state`的，而且读取配置的时候还挺有趣。

`lua_state`是lua的一个虚拟机，里面存放着执行lua的包括上下文、堆栈等数据，在C这里的表现本质上就是一块内存的指针。


### 干掉SIGPIPE信号

这个单独说下，因为以前也有遇到过这个坑，skynet还专门处理的，看看代码就好

```c
/**
 * 在鹅城，老子只办一件事！
 * 那就是无视掉SIGPIPE消息
 */
int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	// SIGPIPE信号存在于读写管道，读进程无了，写进程还往里面写东西的话，就会出现管道破裂而收到SIGPIPE信号
	// 离谱的是，SIGPIPE信号默认会让写进程去死(Crash)
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}
```

### 读取配置文件到env

从上面的`main`函数的逻辑中可以看出来，配置文件是临时启用一个lua_state去读取的，然后通过调用`_init_env(L)`进行初始化env的数据。

```c
/**
 * 根据配置文件读取到的数据来初始化全局env
 */
static void _init_env(lua_State *L) {
	// 压入个空值
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_pushnil
	lua_pushnil(L);  /* first key */
	// 遍历预加载的配置result
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_next
	// 其实我也很好奇，为什么让要一个lua_state跑完大费周章再写入env的lua_state呢?
	while (lua_next(L, -2) != 0) {
		// 获取键
		// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_type
		int keyt = lua_type(L, -2);
		// 如果建不是字符串类型, 那么就代表预加载是有问题的
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		// 没问题就直接获取键字符串
			// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_tostring
		const char * key = lua_tostring(L,-2);
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			// 如果值是布尔类型，那么就要转成波尔值的字符串，不然认不得
			// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_toboolean
			int b = lua_toboolean(L,-1);
			// 把配置写入env
			skynet_setenv(key,b ? "true" : "false" );
		} else {
			// 字符串就直接点了，读出来写入就行了
			// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_tostring
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			// 把配置写入env
			skynet_setenv(key,value);
		}
		// 遍历`table`的时候还是要注意把`值`出栈，留下`键`，等着下次遍历
		lua_pop(L,1);
	}
	// 出栈，已经没有什么必要了
	lua_pop(L,1);
}
```

看到这里的时候，再回去结合`main`函数，已经把配置读完了，接下来的事情就是启动skynet后，它又会干些什么。

配置学习主要的收获点：

在这里就已经开始接触学习lua_state的基础用法了，skynet不会用的很复杂，反而就是很入门的级别去方便新人理解是怎么用的.(指名一些源码极其难理解的项目)

