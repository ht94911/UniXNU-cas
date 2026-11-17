#pragma once
#include <sys/types.h>
struct vnode {int character,tag;dev_t device;};
typedef struct vnode *vnode_t;
enum {VT_DEVFS=17};
int vnode_ischr(vnode_t);
int vnode_tag(vnode_t);
dev_t vnode_specrdev(vnode_t);
/* IO_NDELAY is a vnode I/O flag in XNU 12377, independent of open flags. */
#define IO_NDELAY 0x0010
