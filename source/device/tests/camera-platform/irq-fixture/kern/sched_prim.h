#pragma once
#include <stdint.h>
#include <kern/thread.h>
typedef uintptr_t event_t;
extern "C" wait_result_t assert_wait_timeout(event_t, int, uint32_t, uint32_t);
extern "C" int clear_wait(thread_t, wait_result_t);
extern "C" wait_result_t thread_block(thread_continue_t);
extern "C" int thread_wakeup_prim(event_t, bool, wait_result_t);
#define thread_wakeup(event) thread_wakeup_prim((event), false, THREAD_AWAKENED)
