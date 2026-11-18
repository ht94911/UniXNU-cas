#pragma once
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef NP_HOST_LOG_THREADS
#include <pthread.h>
#include <stdlib.h>
typedef struct {pthread_mutex_t mutex;} IOLock;
extern void np_host_log_lock_wait(IOLock *);
static inline IOLock *IOLockAlloc(void){IOLock *p=calloc(1,sizeof(*p));assert(p&&!pthread_mutex_init(&p->mutex,0));return p;}
static inline void IOLockLock(IOLock *p){assert(p);np_host_log_lock_wait(p);assert(!pthread_mutex_lock(&p->mutex));}
static inline void IOLockUnlock(IOLock *p){assert(p&&!pthread_mutex_unlock(&p->mutex));}
static inline int IOLockTryLock(IOLock *p){return p&&!pthread_mutex_trylock(&p->mutex);}
#else
typedef struct {int held;} IOLock;
static inline IOLock *IOLockAlloc(void){static IOLock l;return &l;}
#ifdef NP_HOST_LOG_WAIT
extern void np_host_log_wait(IOLock *);
static inline void IOLockLock(IOLock *p){assert(p);if(p->held)np_host_log_wait(p);assert(!p->held);p->held=1;}
#else
static inline void IOLockLock(IOLock *p){assert(p&&!p->held);p->held=1;}
#endif
static inline void IOLockUnlock(IOLock *p){assert(p&&p->held);p->held=0;}
static inline int IOLockTryLock(IOLock *p){if(!p||p->held)return 0;p->held=1;return 1;}
#endif
