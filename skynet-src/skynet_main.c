#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

/**
 * 从env读取十进制的整数配置
 * opt: 默认值
 */
static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt);
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);
}

/**
 * 从env读取布尔值
 * opt: 默认值
 */
static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}

/**
 * 从env读取字符串
 * opt: 默认值
 */
static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

/**
 * 根据配置文件读取到的数据来初始化全局env
 */
static void
_init_env(lua_State *L) {
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

// 这个lua脚本不涉及任何参数处理，只是单纯地根据env环境变量正则一些内容
static const char * load_config = "\
	local result = {}\n\
	local function getenv(name) return assert(os.getenv(name), [[os.getenv() failed: ]] .. name) end\n\
	local sep = package.config:sub(1,1)\n\
	local current_path = [[.]]..sep\n\
	local function include(filename)\n\
		local last_path = current_path\n\
		local path, name = filename:match([[(.*]]..sep..[[)(.*)$]])\n\
		if path then\n\
			if path:sub(1,1) == sep then	-- root\n\
				current_path = path\n\
			else\n\
				current_path = current_path .. path\n\
			end\n\
		else\n\
			name = filename\n\
		end\n\
		local f = assert(io.open(current_path .. name))\n\
		local code = assert(f:read [[*a]])\n\
		code = string.gsub(code, [[%$([%w_%d]+)]], getenv)\n\
		f:close()\n\
		assert(load(code,[[@]]..filename,[[t]],result))()\n\
		current_path = last_path\n\
	end\n\
	setmetatable(result, { __index = { include = include } })\n\
	local config_name = ...\n\
	include(config_name)\n\
	setmetatable(result, nil)\n\
	return result\n\
";

/**
 	* 整个skynet程序的入口
 	*/
int
main(int argc, char *argv[]) {
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
