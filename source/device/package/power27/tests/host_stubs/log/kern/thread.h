#pragma once
typedef int wait_result_t;typedef void *thread_t;
#define KERN_SUCCESS 0
static inline int kernel_thread_start(void (*f)(void *,wait_result_t),void *p,thread_t *out){(void)f;(void)p;*out=0;return 0;}
static inline void thread_deallocate(thread_t t){(void)t;}
static inline void thread_terminate(thread_t t){(void)t;}
static inline thread_t current_thread(void){return 0;}
