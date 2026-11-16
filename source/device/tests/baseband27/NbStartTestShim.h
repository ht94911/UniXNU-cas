/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef NB_START_SHIM
#define NB_START_SHIM
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void *DTEntry;typedef int wait_result_t;typedef void *thread_t;
#define THREAD_NULL NULL
#define KERN_SUCCESS 0
#define Z_WAITOK 1
#define Z_ZERO 2
#define kSuccess 0
int SecureDTLookupEntry(void *,const char *,DTEntry *);
int SecureDTGetProperty(DTEntry,const char *,const void **,unsigned *);
void absolutetime_to_nanoseconds(uint64_t,uint64_t *);uint64_t mach_absolute_time(void);
void *kalloc_data(size_t,int);void IOSleep(unsigned);
int kernel_thread_start(void (*)(void *,wait_result_t),void *,thread_t *);
void thread_deallocate(thread_t);void thread_terminate(thread_t);thread_t current_thread(void);
#endif
