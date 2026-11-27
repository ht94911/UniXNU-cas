/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Phase 6 zero-fallback production command queue/buffer: strictly native Adreno 840
 * submission with 15s bounded fence wait; zero fallback replay or delegate paths. */
#import "A840MTLCommand.h"
#import "A840MTLEncoders.h"
#include "../core/a840_production_router.h"
#include "../transport/a840_hwtest_hook.h"

static constexpr uint64_t fenceWaitNanoseconds = UINT64_C(15) * 1000000000;

@interface A840MTLCommandQueue ()
- (void)reserve:(A840MTLCommandBuffer *)command;
- (void)ready:(A840MTLCommandBuffer *)command;
- (void)discardReservation:(void *)command;
@end

@interface A840MTLCommandBuffer ()
- (void)execute;
@end

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"
@implementation A840MTLCommandQueue {
    NSMutableArray<NSValue *> *_order;
    NSMutableDictionary<NSValue *, A840MTLCommandBuffer *> *_committed;
}
@synthesize label = _label;

- (instancetype)initWithOwner:(A840MTLDevice *)owner
{
    if (!(self = [super init])) return nil;
    _owner = owner;
    @synchronized (_owner) {
        if (a840_rt_queue_create(owner.runtime, A840_QUEUE_PRIORITY_NORMAL, &_native) != A840_RT_OK)
            return nil;
    }
    _order = [NSMutableArray new];
    _committed = [NSMutableDictionary new];
    return self;
}

- (void)dealloc
{
    @synchronized (_owner) {
        if (_native) {
            (void)a840_rt_queue_wait_idle(_native, fenceWaitNanoseconds);
            a840_rt_queue_destroy(_native);
        }
    }
}

- (id<MTLDevice>)device { return _owner; }
- (id<MTLCommandBuffer>)commandBuffer { return [[A840MTLCommandBuffer alloc] initWithQueue:self]; }
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }

- (id<MTLCommandQueue>)fallbackQueue
{
    (void)a840_prod_route_request_fallback(_owner.productionRouter);
    return nil;
}

- (void)launchReady
{
    while (_order.count) {
        NSValue *key = _order.firstObject;
        A840MTLCommandBuffer *command = _committed[key];
        if (!command) break;
        [_order removeObjectAtIndex:0];
        [_committed removeObjectForKey:key];
        dispatch_async(_owner.workQueue, ^{ @autoreleasepool { [command execute]; } });
    }
}
- (void)reserve:(A840MTLCommandBuffer *)command
{
    @synchronized (self) { [_order addObject:[NSValue valueWithPointer:(__bridge void *)command]]; }
}
- (void)ready:(A840MTLCommandBuffer *)command
{
    @synchronized (self) {
        NSValue *key = [NSValue valueWithPointer:(__bridge void *)command];
        if (![_order containsObject:key]) [_order addObject:key];
        _committed[key] = command;
        [self launchReady];
    }
}
- (void)discardReservation:(void *)command
{
    @synchronized (self) {
        [_order removeObject:[NSValue valueWithPointer:command]];
        [self launchReady];
    }
}
@end

@implementation A840MTLCommandBuffer {
    A840MTLCommandQueue *_queue;
    NSCondition *_changed;
    MTLCommandBufferStatus _status;
    NSError *_error;
    NSError *_recordingError;
    BOOL _activeEncoder, _scheduled, _callbacksDone;
    NSMutableOrderedSet<id<MTLResource>> *_resources;
    NSMutableArray<MTLCommandBufferHandler> *_scheduledHandlers, *_completedHandlers;
}
@synthesize label = _label;

- (instancetype)initWithQueue:(A840MTLCommandQueue *)queue
{
    if (!(self = [super init])) return nil;
    _queue = queue;
    _owner = queue.owner;
    _changed = [NSCondition new];
    _resources = [NSMutableOrderedSet new];
    _scheduledHandlers = [NSMutableArray new];
    _completedHandlers = [NSMutableArray new];
    @synchronized (_owner) {
        if (a840_rt_cb_create(queue.native, nullptr, &_native) != A840_RT_OK)
            return nil;
    }
    a840_hwtest_forget_command_buffer((__bridge void *)self);
    return self;
}

- (void)dealloc
{
    if (_status == MTLCommandBufferStatusEnqueued) [_queue discardReservation:(__bridge void *)self];
    a840_hwtest_forget_command_buffer((__bridge void *)self);
    @synchronized (_owner) { a840_rt_cb_release(_native); }
}

- (id<MTLDevice>)device { return _owner; }
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (BOOL)retainedReferences { return YES; }
- (MTLCommandBufferStatus)status
{
    [_changed lock];
    MTLCommandBufferStatus status = _status;
    [_changed unlock];
    return status;
}
- (NSError *)error
{
    [_changed lock];
    NSError *error = _error;
    [_changed unlock];
    return error;
}

- (BOOL)wantsRaw
{
    [_changed lock];
    const BOOL raw = !_recordingError && _status < MTLCommandBufferStatusCommitted;
    [_changed unlock];
    return raw;
}

- (BOOL)beginEncoder
{
    [_changed lock];
    const BOOL canBegin = !_activeEncoder && !_recordingError && _status < MTLCommandBufferStatusCommitted;
    if (canBegin) _activeEncoder = YES;
    [_changed unlock];
    return canBegin;
}
- (void)finishEncoderWithReplay:(A840MTLReplay)replay
{
    (void)replay;
    [_changed lock];
    if (_activeEncoder && _status < MTLCommandBufferStatusCommitted) {
        _activeEncoder = NO;
    }
    [_changed unlock];
}
- (void)abandonEncoder
{
    [_changed lock];
    if (_status < MTLCommandBufferStatusCommitted && _activeEncoder) {
        _recordingError = a840_metal_error(A840_RT_STATE_ERROR, @"Command encoder was not ended");
        _activeEncoder = NO;
    }
    [_changed unlock];
}

- (void)useResource:(id<MTLResource>)resource
{
    if (!resource) return;
    if ([(id)resource isKindOfClass:A840MTLAllocation.class] &&
        [(A840MTLAllocation *)resource owner] != _owner) {
        [self recordFailure:A840_RT_WRONG_DEVICE];
        return;
    }
    [_changed lock];
    if (_status < MTLCommandBufferStatusCommitted) [_resources addObject:resource];
    [_changed unlock];
}

- (void)requireFallback:(A840FallbackPredicate)reason
{
    (void)a840_prod_route_eval_predicate(_owner.productionRouter,
                                         static_cast<A840ProdPredicateId>(reason), true);
    [self recordFailure:A840_RT_UNSUPPORTED];
}
- (void)recordFailure:(A840RtStatus)status
{
    [_changed lock];
    if (_status < MTLCommandBufferStatusCommitted && !_recordingError)
        _recordingError = a840_metal_error(status, @"Could not encode the native Metal operation");
    [_changed unlock];
}
- (BOOL)acceptStatus:(A840RtStatus)status fallback:(A840FallbackPredicate)reason
{
    if (status == A840_RT_OK) return YES;
    if (status == A840_RT_FALLBACK_REQUIRED) {
        (void)a840_prod_route_eval_predicate(_owner.productionRouter,
                                             static_cast<A840ProdPredicateId>(reason), true);
        [self recordFailure:A840_RT_UNSUPPORTED];
    } else {
        [self recordFailure:status];
    }
    return NO;
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
    return [[A840MTLBlitEncoder alloc] initWithCommand:self];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
    return [self computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)dispatchType
{
    return [[A840MTLComputeEncoder alloc] initWithCommand:self dispatchType:dispatchType];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)descriptor
{
    return descriptor ? [[A840MTLRenderEncoder alloc] initWithCommand:self pass:descriptor] : nil;
}

- (void)enqueue
{
    BOOL reserved = NO;
    [_changed lock];
    if (_status == MTLCommandBufferStatusNotEnqueued) {
        _status = MTLCommandBufferStatusEnqueued;
        reserved = YES;
    }
    [_changed unlock];
    if (reserved) [_queue reserve:self];
}
- (void)commit
{
    BOOL shouldSubmit = NO;
    [_changed lock];
    if (_status < MTLCommandBufferStatusCommitted) {
        if (_activeEncoder && !_recordingError)
            _recordingError = a840_metal_error(A840_RT_STATE_ERROR, @"Active command encoder at commit");
        _status = MTLCommandBufferStatusCommitted;
        shouldSubmit = YES;
    }
    [_changed unlock];
    if (shouldSubmit) [_queue ready:self];
}

- (void)publishScheduled
{
    NSArray<MTLCommandBufferHandler> *handlers = nil;
    [_changed lock];
    if (!_scheduled) {
        _scheduled = YES;
        if (_status < MTLCommandBufferStatusScheduled) _status = MTLCommandBufferStatusScheduled;
        handlers = [_scheduledHandlers copy];
        [_scheduledHandlers removeAllObjects];
        [_changed broadcast];
    }
    [_changed unlock];
    if (handlers.count) {
        dispatch_async(_owner.callbackQueue, ^{
            for (MTLCommandBufferHandler handler in handlers) handler(self);
        });
    }
}
- (void)publishCompletion:(NSError *)error
{
    [_changed lock];
    _error = error;
    _status = error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
    NSArray<MTLCommandBufferHandler> *scheduled = _scheduled ? @[] : [_scheduledHandlers copy];
    NSArray<MTLCommandBufferHandler> *completed = [_completedHandlers copy];
    [_scheduledHandlers removeAllObjects];
    [_completedHandlers removeAllObjects];
    _scheduled = YES;
    dispatch_async(_owner.callbackQueue, ^{
        @try {
            for (MTLCommandBufferHandler handler in scheduled) handler(self);
            for (MTLCommandBufferHandler handler in completed) handler(self);
        } @finally {
            [self->_changed lock];
            self->_callbacksDone = YES;
            [self->_changed broadcast];
            [self->_changed unlock];
        }
    });
    [_changed broadcast];
    [_changed unlock];
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)handler
{
    if (!handler) return;
    [_changed lock];
    if (_scheduled) dispatch_async(_owner.callbackQueue, ^{ handler(self); });
    else if (_status < MTLCommandBufferStatusCompleted) [_scheduledHandlers addObject:[handler copy]];
    [_changed unlock];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler
{
    if (!handler) return;
    [_changed lock];
    if (_status >= MTLCommandBufferStatusCompleted)
        dispatch_async(_owner.callbackQueue, ^{ handler(self); });
    else
        [_completedHandlers addObject:[handler copy]];
    [_changed unlock];
}
- (void)waitUntilScheduled
{
    [_changed lock];
    while (_status < MTLCommandBufferStatusScheduled) [_changed wait];
    [_changed unlock];
}
- (void)waitUntilCompleted
{
    const BOOL insideHandler = [_owner isCallbackThread];
    [_changed lock];
    while (_status < MTLCommandBufferStatusCompleted || (!_callbacksDone && !insideHandler)) [_changed wait];
    [_changed unlock];
}

- (NSError *)executeRaw
{
    A840RtStatus status;
    @synchronized (_owner) {
        status = a840_rt_cb_used_dwords(_native) ? A840_RT_OK :
            a840_rt_cb_encode_barrier(_native, 0, false, true);
        if (status == A840_RT_OK) status = a840_rt_cb_commit(_native);
    }
    if (status != A840_RT_OK) return a840_metal_error(status, @"Raw command buffer submission failed");
    a840_hwtest_record_command_buffer((__bridge void *)self, A840_PRED_NONE);
    [self publishScheduled];
    @synchronized (_owner) { status = a840_rt_cb_wait_until_completed(_native, fenceWaitNanoseconds); }
    return status == A840_RT_OK ? nil : a840_metal_error(status, @"Raw command buffer wait failed");
}

- (void)execute
{
    NSError *error = _recordingError;
    @try {
        if (!error) error = [self executeRaw];
    } @catch (NSException *failure) {
        error = a840_metal_error(A840_RT_STATE_ERROR, failure.reason ?: @"Metal adapter exception");
    }
    [_resources removeAllObjects];
    [self publishCompletion:error];
}
@end
#pragma clang diagnostic pop
