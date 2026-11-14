#pragma once
using wait_result_t=int;using thread_t=void *;
#define KERN_SUCCESS 0
inline int kernel_thread_start(void (*)(void *,wait_result_t),void *,thread_t *out){*out=nullptr;return 0;}
inline void thread_deallocate(thread_t){}
inline void thread_terminate(thread_t){}
inline thread_t current_thread(){return nullptr;}
