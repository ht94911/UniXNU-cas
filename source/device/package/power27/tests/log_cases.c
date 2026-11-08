/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Execute actual logger lifecycle against an in-memory VFS model. */
#include <assert.h>
#include <stdlib.h>
#include "../platform/NezhaPowerLog.c"
struct test_node {char data[262144];unsigned size,created,opens,closes,writes;} node;
static unsigned mode,produced;
int vnode_open(const char *p,int f,int perm,int flags,vnode_t *out,vfs_context_t ctx){(void)flags;(void)ctx;
 assert(!strcmp(p,"/private/var/log/nezha-power-123456.log"));assert((f&O_NOFOLLOW)&&perm==0600);
 if((f&O_EXCL)&&node.created)return EEXIST;
 if(!(f&O_CREAT)&&!node.created)return ENOENT;
 node.created=1;++node.opens;*out=&node;return 0;}
int vnode_getattr(vnode_t v,struct vnode_attr *a,vfs_context_t ctx){(void)ctx;assert(v==&node);a->va_data_size=mode==5?300000:node.size;a->va_uid=mode==1?501:0;a->va_nlink=mode==2?2:1;return 0;}
int vnode_vtype(vnode_t v){assert(v==&node);return VREG;}
int vnode_close(vnode_t v,int flags,vfs_context_t ctx){(void)flags;(void)ctx;assert(v==&node);++node.closes;return 0;}
int vn_rdwr(int direction,vnode_t v,caddr_t bytes,int n,off_t offset,int space,int flags,void *cred,int *residual,void *proc){(void)offset;(void)space;(void)cred;(void)proc;
 assert(direction==UIO_WRITE&&v==&node&&(flags&IO_APPEND)&&(flags&IO_SYNC));assert(n>=0&&node.size+(unsigned)n<=sizeof(node.data));
 unsigned got=mode==3?(unsigned)n-1:(unsigned)n;memcpy(node.data+node.size,bytes,got);node.size+=got;++node.writes;*residual=n-(int)got;return 0;}
void IOSleep(unsigned ms){(void)ms;if(!produced){produced=1;
 const char config[]="CPREG initial_enable=0 sampled_before_update=0000\n";
 assert(!np_power_log_push(config,sizeof(config)-1));
 for(unsigned i=0;i<(mode==6?40u:1u);++i)np_power_log_push("P1 test=1\n",10);
 if(mode==7)node.created=0; /* A named file disappeared after its header. */
 np_power_log_stop();}}
int main(int argc,char **argv){if(argc>1)mode=(unsigned)atoi(argv[1]);if(mode==4){node.created=1;memcpy(node.data,"USER",4);node.size=4;}
 assert(!np_power_log_start());log_worker(0,0);NpPowerLogStatus s;assert(np_power_log_status(&s));assert(node.opens==node.closes);
 if(!mode||mode==6){assert(s.phase==4&&s.written==(mode==6?32u:2u)&&s.dropped==(mode==6?9u:0u));
  assert(argc>2);FILE *f=fopen(argv[2],"wb");assert(f);assert(fwrite(node.data,1,node.size,f)==node.size);fclose(f);
 }else{assert(s.phase==3&&s.error);if(mode!=3&&mode!=7)assert(!node.writes);if(mode==4)assert(node.size==4&&!memcmp(node.data,"USER",4));
  if(mode==7){assert(s.error==ENOENT&&s.written==0&&node.writes==1&&!node.created);}}
 printf("PASS log case %u: bounded, exclusive no-follow file, close/error semantics\n",mode);return 0;}
