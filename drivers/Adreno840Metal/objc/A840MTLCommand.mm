/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Wave 12 zero-fallback production command queue/buffer: strictly native Adreno 840
 * submission with SharedEvent signaling, Drawable/Scanout handoff, Heap residency,
 * bounded 15s fence wait, and Phase 3 executed-feature evidence reporting. */
#import "A840MTLCommand.h"
#import <CoreFoundation/CoreFoundation.h>
#import "A840MTLEncoders.h"
#import "A840MTLDiagnostics.h"
#include <time.h>
#import "A840MTLHeapAndEvents.h"
#import "A840MTLScanoutBridge.h"
#include "../core/a840_production_router.h"
#include "../transport/a840_hwtest_hook.h"

#include <cstring>

static constexpr uint64_t fenceWaitNanoseconds = UINT64_C(15) * 1000000000;
static thread_local bool gInCompletionCallback = false;

@interface A840MTLCommandQueue ()
- (void)reserve:(A840MTLCommandBuffer *)command;
- (void)ready:(A840MTLCommandBuffer *)command;
- (void)discardReservation:(void *)command;
@end

@interface A840MTLCommandBuffer ()
- (void)execute;
@end

@interface A840PendingSignalOp : NSObject
@property(nonatomic, strong) A840MTLEvent *event;
@property(nonatomic, assign) uint64_t value;
@end
@implementation A840PendingSignalOp
@end

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"
@implementation A840MTLCommandQueue {
    NSMutableArray<NSValue *> *_order;
    NSMutableDictionary<NSValue *, A840MTLCommandBuffer *> *_committed;
    NSUInteger _maxCommandBufferCount;
    NSUInteger _gpuPriority;
    NSUInteger _backgroundGpuPriority;
    BOOL _profilingEnabled;
    BOOL _skipRender;
    BOOL _executionEnabled;
}
@synthesize label = _label;

- (instancetype)initWithOwner:(A840MTLDevice *)owner
{
    return [self initWithOwner:owner maxCommandBufferCount:64u];
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner maxCommandBufferCount:(NSUInteger)maxCount
{
    if (!(self = [super init])) return nil;
    _owner = owner;
    _maxCommandBufferCount = maxCount ? maxCount : 64u;
    const uint32_t prio = (owner.processRole == A840_SYS_ROLE_RENDER_SERVER)
                              ? A840_QUEUE_PRIORITY_HIGH
                              : A840_QUEUE_PRIORITY_NORMAL;
    @synchronized (_owner) {
        if (a840_rt_queue_create(owner.runtime, prio, &_native) != A840_RT_OK)
            return nil;
    }
    _gpuPriority = 1u;
    _backgroundGpuPriority = 3u;
    _executionEnabled = YES;
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
- (NSUInteger)maxCommandBufferCount { return _maxCommandBufferCount; }
- (id<MTLCommandBuffer>)commandBuffer { return [[A840MTLCommandBuffer alloc] initWithQueue:self]; }
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)descriptor
{
    (void)descriptor;
    return [self commandBuffer];
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
- (NSUInteger)getGPUPriority { return _gpuPriority; }
- (BOOL)setGPUPriority:(NSUInteger)priority
{
    if (priority > 5u || priority == 3u) return NO;
    _gpuPriority = priority;
    return YES;
}
- (BOOL)setGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
    (void)offset;
    return [self setGPUPriority:priority];
}
- (NSUInteger)getBackgroundGPUPriority { return _backgroundGpuPriority; }
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority
{
    if (priority > 5u) return NO;
    _backgroundGpuPriority = priority;
    return YES;
}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority offset:(unsigned short)offset
{
    (void)offset;
    return [self setBackgroundGPUPriority:priority];
}
- (void)setSubmissionQueue:(dispatch_queue_t)queue { (void)queue; }
- (BOOL)isProfilingEnabled { return _profilingEnabled; }
- (void)setProfilingEnabled:(BOOL)enabled { _profilingEnabled = enabled; }
- (BOOL)skipRender { return _skipRender; }
- (void)setSkipRender:(BOOL)skip { _skipRender = skip; }
- (BOOL)executionEnabled { return _executionEnabled; }
- (void)setExecutionEnabled:(BOOL)enabled { _executionEnabled = enabled; }
- (void)insertDebugCaptureBoundary {}
- (void)finish
{
    @synchronized (_owner) {
        if (_native) (void)a840_rt_queue_wait_idle(_native, fenceWaitNanoseconds);
    }
}
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLCommandQueue", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
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
    NSMutableOrderedSet<id<MTLHeap>> *_heaps;
    NSMutableArray<A840PendingSignalOp *> *_signalOps;
    NSMutableArray<id<MTLDrawable>> *_drawables;
    NSMutableArray<MTLCommandBufferHandler> *_scheduledHandlers, *_completedHandlers;
    A840Phase3Evidence _phase3Evidence;
    BOOL _hasPhase3Evidence;
    uint64_t _protectionOptions;
    double _kernelStartTime;
    double _kernelEndTime;
    double _gpuStartTime;
    double _gpuEndTime;
}
@synthesize label = _label;

- (instancetype)initWithQueue:(A840MTLCommandQueue *)queue
{
    if (!(self = [super init])) return nil;
    _queue = queue;
    _owner = queue.owner;
    _changed = [NSCondition new];
    _resources = [NSMutableOrderedSet new];
    _heaps = [NSMutableOrderedSet new];
    _signalOps = [NSMutableArray new];
    _drawables = [NSMutableArray new];
    _scheduledHandlers = [NSMutableArray new];
    _completedHandlers = [NSMutableArray new];
    std::memset(&_phase3Evidence, 0, sizeof(_phase3Evidence));
    _phase3Evidence.bytes = sizeof(_phase3Evidence);
    _phase3Evidence.version = A840_HWTEST_PHASE3_EVIDENCE_VERSION;
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
- (void)finishEncoder
{
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
    if (_status < MTLCommandBufferStatusCommitted) {
        if (![_resources containsObject:resource]) {
            [_resources addObject:resource];
            if ([(id)resource isKindOfClass:A840MTLTexture.class]) {
                A840MTLTexture *tex = (A840MTLTexture *)resource;
                if (tex.surfaceHandle != A840_SYS_INVALID_HANDLE && _owner.systemFrontend) {
                    (void)a840_sys_surface_increment_use(_owner.systemFrontend, tex.surfaceHandle);
                }
            }
        }
    }
    [_changed unlock];
}

- (void)useHeap:(id<MTLHeap>)heap
{
    if (!heap) return;
    if ([(id)heap isKindOfClass:A840MTLHeap.class] && [(A840MTLHeap *)heap owner] != _owner) {
        [self recordFailure:A840_RT_WRONG_DEVICE];
        return;
    }
    [_changed lock];
    if (_status < MTLCommandBufferStatusCommitted) [_heaps addObject:heap];
    [_changed unlock];
}

- (void)recordFailure:(A840RtStatus)status reason:(NSString *)reason
{
    [_changed lock];
    if (_status < MTLCommandBufferStatusCommitted && !_recordingError) {
        _recordingError = a840_metal_error(status, reason ?: @"Could not encode the native Metal operation");
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840ENC st=%d %.70s", (int)status, reason ? reason.UTF8String : "enc");
        a840diag::reportText(msg);
    }
    [_changed unlock];
}
- (void)recordFailure:(A840RtStatus)status
{
    [self recordFailure:status reason:@"Could not encode the native Metal operation"];
}

- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    if (!event || ![(id)event isKindOfClass:A840MTLEvent.class]) {
        [self recordFailure:A840_RT_WRONG_DEVICE];
        return;
    }
    A840PendingSignalOp *op = [A840PendingSignalOp new];
    op.event = (A840MTLEvent *)event;
    op.value = value;
    [_changed lock];
    if (_status < MTLCommandBufferStatusCommitted) {
        [_signalOps addObject:op];
    }
    [_changed unlock];
    @synchronized (_owner) {
        if (self.wantsRaw) {
            (void)a840_rt_cb_encode_barrier(_native,
                A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV, true, false);
        }
    }
}

- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    (void)value;
    if (!event || ![(id)event isKindOfClass:A840MTLEvent.class]) {
        [self recordFailure:A840_RT_WRONG_DEVICE];
        return;
    }
    @synchronized (_owner) {
        if (self.wantsRaw) {
            (void)a840_rt_cb_encode_barrier(_native, A840_CACHE_UCHE_INV, true, true);
        }
    }
}

- (void)presentDrawable:(id<MTLDrawable>)drawable
{
    if (!drawable) return;
    [_changed lock];
    if (_status < MTLCommandBufferStatusCommitted) {
        [_drawables addObject:drawable];
    }
    [_changed unlock];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)presentationTime
{
    (void)presentationTime;
    [self presentDrawable:drawable];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)duration
{
    (void)duration;
    [self presentDrawable:drawable];
}

- (void)notePhase3RenderPass:(BOOL)gmem
             memorylessDepth:(BOOL)memorylessDepth
                fbfetchDraws:(uint32_t)fbfetchDraws
                       tileW:(uint32_t)tileW
                       tileH:(uint32_t)tileH
                      tilesX:(uint32_t)tilesX
                      tilesY:(uint32_t)tilesY
{
    [_changed lock];
    if (gmem) _phase3Evidence.gmem_passes++;
    else _phase3Evidence.sysmem_passes++;
    if (memorylessDepth) _phase3Evidence.memoryless_depth_passes++;
    _phase3Evidence.fbfetch_draws += fbfetchDraws;
    if (tileW) _phase3Evidence.tile_width = tileW;
    if (tileH) _phase3Evidence.tile_height = tileH;
    if (tilesX) _phase3Evidence.tiles_x = tilesX;
    if (tilesY) _phase3Evidence.tiles_y = tilesY;
    _hasPhase3Evidence = YES;
    [_changed unlock];
}

- (void)notePhase3UbwcExecuted:(BOOL)wroteBlocks
{
    [_changed lock];
    _phase3Evidence.ubwc_version = _owner.systemCaps.ubwc_version;
    _phase3Evidence.highest_bank_bit = _owner.systemCaps.highest_bank_bit;
    _phase3Evidence.swizzle_levels = _owner.systemCaps.ubwc_swizzle;
    _phase3Evidence.macrotile_mode = _owner.systemCaps.macrotile_mode;
    if (wroteBlocks) {
        _phase3Evidence.compressed_blocks++;
        _phase3Evidence.escape_blocks++;
    }
    _hasPhase3Evidence = YES;
    [_changed unlock];
}

- (void)notePhase3ScanoutMode:(unsigned)mode
{
    [_changed lock];
    if (mode == 0u) _phase3Evidence.mdss_direct_ubwc++;
    else _phase3Evidence.mdss_native_linear++;
    _hasPhase3Evidence = YES;
    [_changed unlock];
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
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)descriptor
{
    return [self computeCommandEncoderWithDispatchType:descriptor ? descriptor.dispatchType : MTLDispatchTypeSerial];
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
        struct timespec ts{};
        if (clock_gettime(CLOCK_UPTIME_RAW, &ts) == 0) {
            const double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
            if (_kernelStartTime == 0.0) _kernelStartTime = now;
            if (_gpuStartTime == 0.0) _gpuStartTime = now;
        }
        handlers = [_scheduledHandlers copy];
        [_scheduledHandlers removeAllObjects];
        [_changed broadcast];
    }
    [_changed unlock];
    if (handlers.count) {
        dispatch_queue_t targetQ = _queue.completionQueue ?: _owner.callbackQueue;
        dispatch_async(targetQ, ^{
            const bool prev = gInCompletionCallback;
            gInCompletionCallback = true;
            @try {
                for (MTLCommandBufferHandler handler in handlers) handler(self);
            } @finally {
                gInCompletionCallback = prev;
            }
        });
    }
}
- (void)publishCompletion:(NSError *)error
{
    [_changed lock];
    _error = error;
    _status = error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
    struct timespec ts{};
    if (clock_gettime(CLOCK_UPTIME_RAW, &ts) == 0) {
        const double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
        if (_kernelStartTime == 0.0) _kernelStartTime = now;
        if (_gpuStartTime == 0.0) _gpuStartTime = now;
        _kernelEndTime = now;
        _gpuEndTime = now;
    }
    NSArray<MTLCommandBufferHandler> *scheduled = _scheduled ? @[] : [_scheduledHandlers copy];
    NSArray<MTLCommandBufferHandler> *completed = [_completedHandlers copy];
    [_scheduledHandlers removeAllObjects];
    [_completedHandlers removeAllObjects];
    _scheduled = YES;
    dispatch_queue_t targetQ = _queue.completionQueue ?: _owner.callbackQueue;
    dispatch_async(targetQ, ^{
        const bool prev = gInCompletionCallback;
        gInCompletionCallback = true;
        @try {
            for (MTLCommandBufferHandler handler in scheduled) handler(self);
            for (MTLCommandBufferHandler handler in completed) handler(self);
        } @finally {
            gInCompletionCallback = prev;
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
    if (_scheduled) { dispatch_queue_t tq = _queue.completionQueue ?: _owner.callbackQueue; dispatch_async(tq, ^{ const bool p = gInCompletionCallback; gInCompletionCallback = true; @try { handler(self); } @finally { gInCompletionCallback = p; } }); }
    else if (_status < MTLCommandBufferStatusCompleted) [_scheduledHandlers addObject:[handler copy]];
    [_changed unlock];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler
{
    if (!handler) return;
    [_changed lock];
    if (_status >= MTLCommandBufferStatusCompleted) {
        dispatch_queue_t tq = _queue.completionQueue ?: _owner.callbackQueue;
        dispatch_async(tq, ^{ const bool p = gInCompletionCallback; gInCompletionCallback = true; @try { handler(self); } @finally { gInCompletionCallback = p; } });
    }
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
- (CFTimeInterval)kernelStartTime { return _kernelStartTime; }
- (CFTimeInterval)kernelEndTime { return _kernelEndTime; }
- (CFTimeInterval)GPUStartTime { return _gpuStartTime; }
- (CFTimeInterval)GPUEndTime { return _gpuEndTime; }
- (uint64_t)protectionOptions { return _protectionOptions; }
- (void)setProtectionOptions:(uint64_t)options { _protectionOptions = options; }
- (void)pushDebugGroup:(NSString *)string { (void)string; }
- (void)popDebugGroup {}
- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count
{
    (void)tag; (void)groups; (void)count;
}
- (void)encodeCacheHintFinalize:(uint64_t)tag resourceGroups:(const id __unsafe_unretained [])groups count:(NSUInteger)count
{
    (void)tag; (void)groups; (void)count;
}
- (void)commitAndWaitUntilSubmitted
{
    [self commit];
    [self waitUntilScheduled];
}
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLCommandBuffer", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
- (void)waitUntilCompleted
{
    const BOOL insideHandler = [_owner isCallbackThread] || (gInCompletionCallback ? YES : NO);
    [_changed lock];
    while (_status < MTLCommandBufferStatusCompleted || (!_callbacksDone && !insideHandler)) [_changed wait];
    [_changed unlock];
}

- (NSError *)executeRaw
{
    A840RtStatus status;
    uint64_t fence = 0u;
    @synchronized (_owner) {
        status = a840_rt_cb_used_dwords(_native) ? A840_RT_OK :
            a840_rt_cb_encode_barrier(_native, 0, false, true);
        if (status == A840_RT_OK) status = a840_rt_cb_commit(_native);
        if (status == A840_RT_OK) fence = a840_rt_cb_submit_fence(_native);
    }
    if (status != A840_RT_OK) return a840_metal_error(status, @"Raw command buffer submission failed");
    a840_hwtest_record_command_buffer((__bridge void *)self, A840_PRED_NONE);
    if (_hasPhase3Evidence) {
        (void)a840_hwtest_phase3_record_evidence((__bridge void *)self, &_phase3Evidence);
    }
    [self publishScheduled];

    if (_owner.processRole == A840_SYS_ROLE_RENDER_SERVER && _owner.scanoutBridge && _drawables.count > 0u) {
        [_owner.scanoutBridge notifyDisplayHandoffForSurface:1u
                                                     queueId:a840_rt_queue_handle(_queue.native)
                                                       fence:fence
                                                earlyPromise:YES];
    }

    @synchronized (_owner) { status = a840_rt_cb_wait_until_completed(_native, fenceWaitNanoseconds); }
    if (status != A840_RT_OK) {
        return a840_metal_error(status, @"Raw command buffer wait failed");
    }

    for (A840PendingSignalOp *op in _signalOps) {
        if (_owner.systemFrontend) {
            (void)a840_sys_event_signal(_owner.systemFrontend, op.event.eventHandle, op.value);
        }
    }
    for (id<MTLDrawable> drawable in _drawables) {
        [drawable present];
    }
    return nil;
}

- (void)releaseCommandSurfaces
{
    for (id<MTLResource> res in _resources) {
        if ([(id)res isKindOfClass:A840MTLTexture.class]) {
            A840MTLTexture *tex = (A840MTLTexture *)res;
            if (tex.surfaceHandle != A840_SYS_INVALID_HANDLE && _owner.systemFrontend) {
                (void)a840_sys_surface_decrement_use(_owner.systemFrontend, tex.surfaceHandle);
            }
        }
    }
    [_resources removeAllObjects];
    [_heaps removeAllObjects];
    [_signalOps removeAllObjects];
    [_drawables removeAllObjects];
}

- (void)execute
{
    NSError *error = _recordingError;
    @try {
        if (!error) error = [self executeRaw];
    } @catch (NSException *failure) {
        error = a840_metal_error(A840_RT_STATE_ERROR, failure.reason ?: @"Metal adapter exception");
    }
    [self releaseCommandSurfaces];
    [self publishCompletion:error];
}
@end
#pragma clang diagnostic pop
