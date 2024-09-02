#include "skynet.h"
#include "skynet_env.h"
#include "spinlock.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>

/**
* skynet_env结构
*/
struct skynet_env {
	struct spinlock lock;
	lua_State *L;
};

// 环境是的全局变量
static struct skynet_env *E = NULL;

/**
 * 获取skynet环境变量
 */
const char * 
skynet_getenv(const char *key) {
	// 因为是公用的env，所以要加锁
	SPIN_LOCK(E)

	// 拿到env对应的lua_state
	lua_State *L = E->L;
	
	// 把全局变量key拿出来，压入栈中
	lua_getglobal(L, key);
	// 一般结果都能序列化成字符串
	const char * result = lua_tostring(L, -1);
	// 有压进去就要弹出去
	lua_pop(L, 1);

	// 搞完了解锁
	SPIN_UNLOCK(E)

	return result;
}

/**
 	* 设置skynet环境变量
 	*/
void 
skynet_setenv(const char *key, const char *value) {
	// 因为是公用的env，所以要加锁
	SPIN_LOCK(E)
	
	// 找到env对应的lua_state
	lua_State *L = E->L;
	// 获取对应的key值(基本上都是nil, 因为这里基本上只用来加载配置的)
	lua_getglobal(L, key);
	assert(lua_isnil(L, -1));
	// 弹出一个值
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_pop
	lua_pop(L,1);
	// 把新的值压入栈中
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_pushstring
	lua_pushstring(L,value);
	// 设置为对应的key全局变量的值
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#lua_setglobal
	lua_setglobal(L,key);

	// 操作完了解锁
	SPIN_UNLOCK(E)
}

/**
 	* 初始化全局env
 	*/
void
skynet_env_init() {
	// 分配一块env的内存
	E = skynet_malloc(sizeof(*E));
	// 初始化自旋锁, 代表这个env是全局共用的，每个线程都能读写
	SPIN_INIT(E)
	// 分配一块lua_state，代表这块env维护是用Lua做的
	// 有一说一，看到这里的时候感觉作者真的好爱lua
	// 函数说明: https://cloudwu.github.io/lua53doc/manual.html#luaL_newstate
	E->L = luaL_newstate();
}
