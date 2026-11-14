#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <kern/locks.h>
#define PZERO 0
#define PCATCH 1
int msleep(void *,lck_mtx_t *,int,const char *,const struct timespec *);
void wakeup(void *);
