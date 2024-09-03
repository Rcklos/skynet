#include "skynet.h"

#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32

/**
 * c模块组件数据结构
 */
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

static struct modules * M = NULL;

/**
 * 尝试加载模块
 * 对应配置里面的`cpath`
 */
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

	int sz = path_size + name_size;
	//search path
	void * dl = NULL;
	char tmp[sz];
	do
	{
		memset(tmp,0,sz);
		while (*path == ';') path++;
		if (*path == '\0') break;
		l = strchr(path, ';');
		if (l == NULL) l = path + strlen(path);
		int len = l - path;
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
		memcpy(tmp+i,name,name_size);
		if (path[i] == '?') {
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
		path = l;
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}

/**
 * 查询模块缓存，有就直接返回该模块指针
 * 没有就返回空指针
 */
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

static void *
get_api(struct skynet_module *mod, const char *api_name) {
	size_t name_size = strlen(mod->name);
	size_t api_size = strlen(api_name);
	char tmp[name_size + api_size + 1];
	memcpy(tmp, mod->name, name_size);
	memcpy(tmp+name_size, api_name, api_size+1);
	char *ptr = strrchr(tmp, '.');
	if (ptr == NULL) {
		ptr = tmp;
	} else {
		ptr = ptr + 1;
	}
	return dlsym(mod->module, ptr);
}

static int
open_sym(struct skynet_module *mod) {
	mod->create = get_api(mod, "_create");
	mod->init = get_api(mod, "_init");
	mod->release = get_api(mod, "_release");
	mod->signal = get_api(mod, "_signal");

	return mod->init == NULL;
}

/**
 	* 查询模块
 	*/
struct skynet_module * 
skynet_module_query(const char * name) {
	// 如果服务已经注册过了，就直接返回缓存即可
	struct skynet_module * result = _query(name);
	if (result)
		return result;

	// 第一次注册，就先把本模块给锁一下
	SPIN_LOCK(M)

	// 因为刚刚查询的时候可能就有另一个线程刚好在注册，所以这个时候还要再检查一次
	result = _query(name); // double check

	// 目前计数里面只能支持最多注册32种模块
	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		// 计数值就是新模块的索引
		int index = M->count;
		// 打开对应的链接库(.so)等，这里会读取配置中的cpath路径，所以需要注意这里的配置
		void * dl = _try_open(M,name);
		if (dl) {
			// 加载成功就放入缓存
			M->m[index].name = name;
			M->m[index].module = dl;

			if (open_sym(&M->m[index]) == 0) {
				M->m[index].name = skynet_strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}

	// 解锁
	SPIN_UNLOCK(M)

	return result;
}

/**
* 创建c模块的实例
* 这里需要对应的c模块要注册上对应的`xxx_create`函数
* 例如logger模块需要有`logger_create`函数
*/
void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

/**
 	* 初始化c模块的实例
 	* 需要c模块实现`xxx_init`
 	*/
int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

void 
skynet_module_init(const char *path) {
	// 申请一块空间
	struct modules *m = skynet_malloc(sizeof(*m));
	// 模块数量初始化为0
	m->count = 0;
	// 模块路径
	m->path = skynet_strdup(path);

	// 自旋锁初始化
	SPIN_INIT(m)

	// 这个时候只是做完初始化工作，没有加载模块
	M = m;
}
