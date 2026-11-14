#pragma once
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {int held;} IOLock;
static inline IOLock *IOLockAlloc(void){static IOLock l;return &l;}
static inline void IOLockLock(IOLock *p){assert(p&&!p->held);p->held=1;}
static inline void IOLockUnlock(IOLock *p){assert(p&&p->held);p->held=0;}
static inline int IOLockTryLock(IOLock *p){if(!p||p->held)return 0;p->held=1;return 1;}
