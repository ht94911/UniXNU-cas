#include <stddef.h>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif
struct proc;
struct uio;
struct cdevsw {
 int (*d_open)(dev_t,int,int,struct proc *),(*d_close)(dev_t,int,int,struct proc *);
 int (*d_read)(dev_t,struct uio *,int),(*d_write)(dev_t,struct uio *,int);
 int (*d_ioctl)(dev_t,u_long,caddr_t,int,struct proc *);
 void *d_stop,*d_reset,*d_select,*d_mmap,*d_strategy;
 int d_type;
};
#define eno_rdwrt ((int (*)(dev_t,struct uio *,int))0)
#define eno_ioctl NULL
#define eno_stop NULL
#define eno_reset NULL
#define eno_select NULL
#define eno_mmap NULL
#define eno_strat NULL
int cdevsw_add(int,const struct cdevsw *);
