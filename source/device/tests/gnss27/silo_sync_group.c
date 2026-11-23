/* SPDX-License-Identifier: BSD-2-Clause */
#include <dispatch/dispatch.h>
#include <Block.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static void owned_sync(dispatch_queue_t queue, dispatch_block_t work) {
    dispatch_assert_queue_not(queue);
    dispatch_group_t group = dispatch_group_create();
    dispatch_group_async(group, queue, work);
    assert(dispatch_group_wait(group, DISPATCH_TIME_FOREVER) == 0);
    dispatch_release(group);
}

struct call { dispatch_queue_t queue; dispatch_semaphore_t submitted;
    _Atomic int returned, complete; int result; };
static void *caller(void *context) {
    struct call *call = context;
    dispatch_semaphore_signal(call->submitted);
    owned_sync(call->queue, ^{ call->result = 73;
        atomic_store_explicit(&call->complete, 1, memory_order_release); });
    assert(atomic_load_explicit(&call->complete, memory_order_acquire));
    assert(call->result == 73);
    atomic_store_explicit(&call->returned, 1, memory_order_release);
    return NULL;
}

static void nested(dispatch_queue_t outer, dispatch_queue_t inner) {
    __block int result = 0;
    owned_sync(outer, ^{ owned_sync(inner, ^{ result = 31; }); assert(result == 31); });
    assert(result == 31);
}

int main(void) {
    dispatch_queue_t queue = dispatch_queue_create("gnss-lifetime-serial", NULL);
    dispatch_queue_t other = dispatch_queue_create("gnss-lifetime-other", NULL);
    dispatch_semaphore_t entered = dispatch_semaphore_create(0);
    dispatch_semaphore_t gate = dispatch_semaphore_create(0);
    dispatch_async(queue, ^{ dispatch_semaphore_signal(entered);
        dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); });
    assert(dispatch_semaphore_wait(entered, dispatch_time(DISPATCH_TIME_NOW, 2000000000)) == 0);
    struct call call = { .queue = queue, .submitted = dispatch_semaphore_create(0) };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, caller, &call) == 0);
    assert(dispatch_semaphore_wait(call.submitted, dispatch_time(DISPATCH_TIME_NOW, 2000000000)) == 0);
    assert(!atomic_load_explicit(&call.returned, memory_order_acquire));
    assert(!atomic_load_explicit(&call.complete, memory_order_acquire));
    dispatch_semaphore_signal(gate);
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load_explicit(&call.returned, memory_order_acquire));
    // Every synchronous caller sees its preceding async job and its real result.
    __block unsigned value = 0;
    for (unsigned i = 0; i < 512; ++i) {
        dispatch_async(queue, ^{ ++value; });
        owned_sync(queue, ^{ assert(value == 2*i+1); ++value; });
        assert(value == 2*i+2);
    }
    nested(queue, other);
    dispatch_release(call.submitted);
    dispatch_release(gate);
    dispatch_release(entered);
    dispatch_release(other);
    dispatch_release(queue);
    puts("PASS real libdispatch: delayed completion, 512 FIFO result handoffs, nested distinct queues");
    return 0;
}
