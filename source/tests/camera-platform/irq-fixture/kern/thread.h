#pragma once
typedef int wait_result_t;
typedef void *thread_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
#define THREAD_UNINT 0
#define THREAD_AWAKENED 0
#define THREAD_WAITING 1
#define THREAD_CONTINUE_NULL nullptr
extern "C" thread_t current_thread(void);
