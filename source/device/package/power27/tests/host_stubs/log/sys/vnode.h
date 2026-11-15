#pragma once
#include <sys/types.h>
#include <stdint.h>
#include <string.h>
typedef void *vfs_context_t;typedef struct test_node *vnode_t;
#define NULLVP ((vnode_t)0)
#define VREG 1
struct vnode_attr {uint64_t va_data_size;unsigned va_uid,va_nlink;};
#define VATTR_INIT(v) memset(v,0,sizeof(*(v)))
#define VATTR_WANTED(v,x) ((void)(v))
#define VATTR_IS_SUPPORTED(v,x) (1)
static inline vfs_context_t vfs_context_kernel(void){return 0;}
static inline void *vfs_context_ucred(vfs_context_t c){return c;}
static inline void *vfs_context_proc(vfs_context_t c){return c;}
int vnode_open(const char *,int,int,int,vnode_t *,vfs_context_t);
int vnode_getattr(vnode_t,struct vnode_attr *,vfs_context_t);
int vnode_vtype(vnode_t);
int vnode_close(vnode_t,int,vfs_context_t);
int vn_rdwr(int,vnode_t,caddr_t,int,off_t,int,int,void *,int *,void *);
