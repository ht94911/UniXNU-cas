/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Native Adreno 840 Heap, SharedEvent, DepthStencilState, and ICB wrappers. */
#import "A840MTLHeapAndEvents.h"
#import "A840MTLDiagnostics.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"

static uint8_t convertCompareFunc(MTLCompareFunction fn)
{
    switch (fn) {
    case MTLCompareFunctionNever: return 0u;
    case MTLCompareFunctionLess: return 1u;
    case MTLCompareFunctionEqual: return 2u;
    case MTLCompareFunctionLessEqual: return 3u;
    case MTLCompareFunctionGreater: return 4u;
    case MTLCompareFunctionNotEqual: return 5u;
    case MTLCompareFunctionGreaterEqual: return 6u;
    case MTLCompareFunctionAlways: return 7u;
    default: return 7u;
    }
}

static uint8_t convertStencilOp(MTLStencilOperation op)
{
    switch (op) {
    case MTLStencilOperationKeep: return 0u;
    case MTLStencilOperationZero: return 1u;
    case MTLStencilOperationReplace: return 2u;
    case MTLStencilOperationIncrementClamp: return 3u;
    case MTLStencilOperationDecrementClamp: return 4u;
    case MTLStencilOperationInvert: return 5u;
    case MTLStencilOperationIncrementWrap: return 6u;
    case MTLStencilOperationDecrementWrap: return 7u;
    default: return 0u;
    }
}

static A840StencilFace convertStencilFace(MTLStencilDescriptor *desc)
{
    A840StencilFace face{};
    if (!desc) {
        face.compare = 7u;
        face.read_mask = 255u;
        face.write_mask = 255u;
        return face;
    }
    face.fail = convertStencilOp(desc.stencilFailureOperation);
    face.depth_fail = convertStencilOp(desc.depthFailureOperation);
    face.pass = convertStencilOp(desc.depthStencilPassOperation);
    face.compare = convertCompareFunc(desc.stencilCompareFunction);
    face.read_mask = (uint8_t)(desc.readMask & 255u);
    face.write_mask = (uint8_t)(desc.writeMask & 255u);
    return face;
}

@implementation A840MTLHeap {
    MTLHeapDescriptor *_descriptor;
}
@synthesize label = _label;

- (instancetype)initWithOwner:(A840MTLDevice *)owner descriptor:(MTLHeapDescriptor *)descriptor
{
    if (!(self = [super init]) || !owner || !owner.systemFrontend || !descriptor || !descriptor.size) {
        return nil;
    }
    _owner = owner;
    _descriptor = [descriptor copy];

    A840HeapDesc hdesc{};
    hdesc.type = (descriptor.type == MTLHeapTypePlacement) ? A840_HEAP_TYPE_PLACEMENT : A840_HEAP_TYPE_AUTOMATIC;
    hdesc.size_bytes = (uint64_t)descriptor.size;
    hdesc.alignment = A840_RT_LARGE_ALIGN_BYTES;
    hdesc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_SHADER;
    hdesc.cpu_visible = (descriptor.storageMode == MTLStorageModeShared);

    A840SysHeapHandle handle = A840_SYS_INVALID_HANDLE;
    if (a840_sys_heap_create(owner.systemFrontend, &hdesc, &handle) != A840_SYS_OK) {
        return nil;
    }
    _heapHandle = handle;
    return self;
}

- (void)dealloc
{
    if (_owner && _owner.systemFrontend && _heapHandle != A840_SYS_INVALID_HANDLE) {
        (void)a840_sys_heap_destroy(_owner.systemFrontend, _heapHandle);
        _heapHandle = A840_SYS_INVALID_HANDLE;
    }
}

- (A840Heap *)nativeHeap
{
    return (_owner && _owner.systemFrontend) ? a840_sys_heap_resolve(_owner.systemFrontend, _heapHandle) : nullptr;
}

- (id<MTLDevice>)device { return _owner; }
- (MTLStorageMode)storageMode { return _descriptor.storageMode; }
- (MTLCPUCacheMode)cpuCacheMode { return _descriptor.cpuCacheMode; }
- (MTLHazardTrackingMode)hazardTrackingMode { return _descriptor.hazardTrackingMode; }
- (MTLResourceOptions)resourceOptions { return _descriptor.resourceOptions; }
- (MTLHeapType)type { return _descriptor.type; }
- (NSUInteger)size { A840Heap *h = self.nativeHeap; return h ? (NSUInteger)h->size_bytes : 0u; }
- (NSUInteger)usedSize { A840Heap *h = self.nativeHeap; return h ? (NSUInteger)a840_heap_used_bytes(h) : 0u; }
- (NSUInteger)currentAllocatedSize { return self.size; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
    (void)alignment;
    A840Heap *h = self.nativeHeap;
    return h ? (NSUInteger)a840_heap_free_bytes(h) : 0u;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    A840Heap *h = self.nativeHeap;
    if (!h) return MTLPurgeableStateNonVolatile;
    A840PurgeableState prev = A840_PURGEABLE_NONVOLATILE;
    if (state == MTLPurgeableStateKeepCurrent) {
        prev = a840_heap_get_purgeable(h);
    } else {
        const A840PurgeableState target =
            (state == MTLPurgeableStateVolatile) ? A840_PURGEABLE_VOLATILE :
            (state == MTLPurgeableStateEmpty) ? A840_PURGEABLE_EMPTY : A840_PURGEABLE_NONVOLATILE;
        (void)a840_heap_set_purgeable(h, target, &prev);
    }
    return prev == A840_PURGEABLE_VOLATILE ? MTLPurgeableStateVolatile :
           prev == A840_PURGEABLE_EMPTY ? MTLPurgeableStateEmpty : MTLPurgeableStateNonVolatile;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    return [self newBufferWithLength:length options:options offset:0u];
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
    A840Heap *h = self.nativeHeap;
    if (!h || !length) return nil;
    A840HeapBufferDesc bdesc{};
    bdesc.size_bytes = length;
    bdesc.alignment = A840_HEAP_MIN_ALIGN_BYTES;
    bdesc.placement_offset = offset;
    bdesc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_SHADER;
    A840HeapSubAlloc *sub = nullptr;
    @synchronized (_owner) {
        if (a840_heap_alloc_buffer(h, &bdesc, &sub) != A840_RT_OK || !sub) return nil;
    }
    return [[A840MTLBuffer alloc] initWithOwner:_owner heap:self subAlloc:sub options:options];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)desc
{
    return [self newTextureWithDescriptor:desc offset:0u];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)desc offset:(NSUInteger)offset
{
    A840Heap *h = self.nativeHeap;
    if (!h || !desc || !desc.width || !desc.height) return nil;
    A840HeapTextureDesc tdesc{};
    tdesc.mtl_pixel_format = (uint32_t)desc.pixelFormat;
    tdesc.tex_type = A840_TEX_2D;
    tdesc.width = (uint32_t)desc.width;
    tdesc.height = (uint32_t)desc.height;
    tdesc.depth = (uint32_t)(desc.depth ? desc.depth : 1u);
    tdesc.array_layers = (uint32_t)(desc.arrayLength ? desc.arrayLength : 1u);
    tdesc.mip_levels = (uint32_t)(desc.mipmapLevelCount ? desc.mipmapLevelCount : 1u);
    tdesc.samples = (uint32_t)(desc.sampleCount ? desc.sampleCount : 1u);
    tdesc.tile_mode = (desc.storageMode == MTLStorageModePrivate) ? A840_TILE_UBWC : A840_TILE_LINEAR;
    tdesc.highest_bank_bit = A840_KAANAPALI_HIGHEST_BANK_BIT;
    tdesc.placement_offset = offset;
    tdesc.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_RENDER_TARGET;
    A840HeapSubAlloc *sub = nullptr;
    @synchronized (_owner) {
        if (a840_heap_alloc_texture(h, &tdesc, &sub) != A840_RT_OK || !sub) return nil;
    }
    return [[A840MTLTexture alloc] initWithOwner:_owner heap:self subAlloc:sub descriptor:desc];
}
- (uint64_t)protectionOptions { return 0u; }
- (id<MTLBuffer>)bufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
    return [self newBufferWithLength:length options:options offset:offset];
}
- (id<MTLTexture>)textureWithDescriptor:(MTLTextureDescriptor *)desc offset:(NSUInteger)offset
{
    return [self newTextureWithDescriptor:desc offset:offset];
}
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLHeap", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
@end

@implementation A840MTLEvent
@synthesize label = _label;

- (instancetype)initWithOwner:(A840MTLDevice *)owner
{
    return [self initWithOwner:owner shared:NO initialValue:0u machPort:0u];
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                       shared:(BOOL)shared
                 initialValue:(uint64_t)initialValue
                     machPort:(uint32_t)machPort
{
    if (!(self = [super init]) || !owner || !owner.systemFrontend) return nil;
    _owner = owner;
    A840SysEventHandle handle = A840_SYS_INVALID_HANDLE;
    if (a840_sys_event_create(owner.systemFrontend, shared ? true : false,
                              initialValue, machPort, &handle) != A840_SYS_OK) {
        return nil;
    }
    _eventHandle = handle;
    return self;
}

- (void)dealloc
{
    if (_owner && _owner.systemFrontend && _eventHandle != A840_SYS_INVALID_HANDLE) {
        (void)a840_sys_event_destroy(_owner.systemFrontend, _eventHandle);
        _eventHandle = A840_SYS_INVALID_HANDLE;
    }
}

- (id<MTLDevice>)device { return _owner; }
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLEvent", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
@end

struct A840SharedEventNotifyCtx {
    __strong id<MTLSharedEvent> event;
    __strong dispatch_queue_t queue;
    __strong MTLSharedEventNotificationBlock block;
};

static void sharedEventNotifyTrampoline(void *rawCtx, A840SysEventHandle handle, uint64_t value)
{
    (void)handle;
    auto *ctx = static_cast<A840SharedEventNotifyCtx *>(rawCtx);
    if (!ctx) return;
    id<MTLSharedEvent> event = ctx->event;
    dispatch_queue_t queue = ctx->queue;
    MTLSharedEventNotificationBlock block = ctx->block;
    delete ctx;
    if (block && queue) {
        dispatch_async(queue, ^{
            block(event, value);
        });
    } else if (block) {
        block(event, value);
    }
}

@implementation A840MTLSharedEvent

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                 initialValue:(uint64_t)initialValue
                     machPort:(uint32_t)machPort
{
    return [super initWithOwner:owner shared:YES initialValue:initialValue machPort:machPort];
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner handle:(MTLSharedEventHandle *)handle
{
    if (!handle) return nil;
    return [super initWithOwner:owner shared:YES initialValue:0u machPort:(uint32_t)(uintptr_t)(__bridge void *)handle];
}

- (uint64_t)signaledValue
{
    uint64_t value = 0u;
    if (self.owner && self.owner.systemFrontend) {
        (void)a840_sys_event_query(self.owner.systemFrontend, self.eventHandle, &value, nullptr, nullptr);
    }
    return value;
}

- (void)setSignaledValue:(uint64_t)signaledValue
{
    if (self.owner && self.owner.systemFrontend) {
        (void)a840_sys_event_signal(self.owner.systemFrontend, self.eventHandle, signaledValue);
    }
}

- (void)notifyListener:(MTLSharedEventListener *)listener
               atValue:(uint64_t)value
                 block:(MTLSharedEventNotificationBlock)block
{
    if (!block || !self.owner || !self.owner.systemFrontend) return;
    auto *ctx = new A840SharedEventNotifyCtx{
        self,
        listener.dispatchQueue ?: self.owner.callbackQueue,
        [block copy]
    };
    if (a840_sys_event_register_waiter(self.owner.systemFrontend, self.eventHandle, value,
                                       sharedEventNotifyTrampoline, ctx) != A840_SYS_OK) {
        delete ctx;
    }
}

- (MTLSharedEventHandle *)newSharedEventHandle
{
    /* Cross-process Mach capability handle requires KMD v3 shared-event broker. */
    return nil;
}
- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds
{
    if (self.signaledValue >= value) return YES;
    const uint64_t stepUs = 500u;
    const uint64_t limitUs = milliseconds * 1000u;
    for (uint64_t waited = 0u; waited < limitUs; waited += stepUs) {
        if (self.signaledValue >= value) return YES;
        usleep((useconds_t)stepUs);
    }
    return self.signaledValue >= value;
}
@end

@implementation A840MTLDepthStencilState {
    A840DepthStencilVariantDesc _desc;
    NSString *_label;
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner descriptor:(MTLDepthStencilDescriptor *)descriptor
{
    if (!(self = [super init]) || !owner || !owner.systemFrontend || !descriptor) return nil;
    _owner = owner;
    _label = [descriptor.label copy];
    _desc = {};
    _desc.depth_compare = convertCompareFunc(descriptor.depthCompareFunction);
    _desc.depth_write = descriptor.isDepthWriteEnabled ? true : false;
    _desc.depth_test = (descriptor.depthCompareFunction != MTLCompareFunctionAlways) || _desc.depth_write;
    _desc.read_only_depth = _desc.depth_test && !_desc.depth_write;
    _desc.z_mode = A840_ZMODE_EARLY_Z;
    _desc.front = convertStencilFace(descriptor.frontFaceStencil);
    _desc.back = convertStencilFace(descriptor.backFaceStencil);
    _desc.stencil_test = (descriptor.frontFaceStencil != nil) || (descriptor.backFaceStencil != nil);
    _desc.separate_stencil_faces = (descriptor.frontFaceStencil != nil) && (descriptor.backFaceStencil != nil);

    A840SysDepthStencilHandle handle = A840_SYS_INVALID_HANDLE;
    if (a840_sys_depth_stencil_create(owner.systemFrontend, &_desc, &handle) != A840_SYS_OK) {
        return nil;
    }
    _depthStencilHandle = handle;
    return self;
}

- (void)dealloc
{
    if (_owner && _owner.systemFrontend && _depthStencilHandle != A840_SYS_INVALID_HANDLE) {
        (void)a840_sys_depth_stencil_destroy(_owner.systemFrontend, _depthStencilHandle);
        _depthStencilHandle = A840_SYS_INVALID_HANDLE;
    }
}

- (id<MTLDevice>)device { return _owner; }
- (NSString *)label { return _label; }
- (const A840DepthStencilVariantDesc *)nativeDesc { return &_desc; }
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLDepthStencilState", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
@end

@interface A840MTLIndirectRenderCommand : NSObject <MTLIndirectRenderCommand>
- (instancetype)initWithIcb:(A840MTLIndirectCommandBuffer *)icb index:(NSUInteger)index;
@end

@implementation A840MTLIndirectRenderCommand {
    A840MTLIndirectCommandBuffer *_icb;
    uint32_t _index;
}
- (instancetype)initWithIcb:(A840MTLIndirectCommandBuffer *)icb index:(NSUInteger)index
{
    if ((self = [super init])) {
        _icb = icb;
        _index = (uint32_t)index;
    }
    return self;
}
- (void)reset
{
    A840IndirectCmdBuffer *native = _icb.nativeIcb;
    if (native) (void)a840_icb_reset_range(native, _index, 1u);
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipelineState
{
    A840IndirectCmdBuffer *native = _icb.nativeIcb;
    if (native && pipelineState) {
        (void)a840_icb_set_pipeline(native, _index, (uint64_t)(uintptr_t)(__bridge void *)pipelineState, nullptr);
    }
}
- (void)drawPrimitives:(MTLPrimitiveType)primitiveType vertexStart:(NSUInteger)vertexStart
           vertexCount:(NSUInteger)vertexCount instanceCount:(NSUInteger)instanceCount
          baseInstance:(NSUInteger)baseInstance
{
    A840IndirectCmdBuffer *native = _icb.nativeIcb;
    if (!native) return;
    A840DrawDesc draw{};
    draw.primitive = (uint32_t)primitiveType;
    draw.index_type = A840_INDEX_NONE;
    draw.count = (uint32_t)vertexCount;
    draw.instance_count = (uint32_t)instanceCount;
    draw.first_vertex = (uint32_t)vertexStart;
    draw.first_instance = (uint32_t)baseInstance;
    (void)a840_icb_encode_draw(native, _index, &draw);
}
@end

@interface A840MTLIndirectComputeCommand : NSObject <MTLIndirectComputeCommand>
- (instancetype)initWithIcb:(A840MTLIndirectCommandBuffer *)icb index:(NSUInteger)index;
@end

@implementation A840MTLIndirectComputeCommand {
    A840MTLIndirectCommandBuffer *_icb;
    uint32_t _index;
}
- (instancetype)initWithIcb:(A840MTLIndirectCommandBuffer *)icb index:(NSUInteger)index
{
    if ((self = [super init])) {
        _icb = icb;
        _index = (uint32_t)index;
    }
    return self;
}
- (void)reset
{
    A840IndirectCmdBuffer *native = _icb.nativeIcb;
    if (native) (void)a840_icb_reset_range(native, _index, 1u);
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipelineState
{
    A840IndirectCmdBuffer *native = _icb.nativeIcb;
    if (native && pipelineState) {
        (void)a840_icb_set_pipeline(native, _index, (uint64_t)(uintptr_t)(__bridge void *)pipelineState, nullptr);
    }
}
- (void)concurrentDispatchThreadgroups:(MTLSize)threadgroupsPerGrid
                 threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup
{
    A840IndirectCmdBuffer *native = _icb.nativeIcb;
    if (!native) return;
    const uint32_t local[3] = {
        (uint32_t)threadsPerThreadgroup.width,
        (uint32_t)threadsPerThreadgroup.height,
        (uint32_t)threadsPerThreadgroup.depth
    };
    const uint32_t groups[3] = {
        (uint32_t)threadgroupsPerGrid.width,
        (uint32_t)threadgroupsPerGrid.height,
        (uint32_t)threadgroupsPerGrid.depth
    };
    (void)a840_icb_encode_dispatch(native, _index, local, groups);
}
@end

@implementation A840MTLIndirectCommandBuffer {
    NSUInteger _size;
}

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                   descriptor:(MTLIndirectCommandBufferDescriptor *)descriptor
              maxCommandCount:(NSUInteger)maxCount
                      options:(MTLResourceOptions)options
{
    if (!(self = [super initWithOwner:owner options:options]) || !owner.systemFrontend || !descriptor || !maxCount) {
        return nil;
    }
    A840IcbDesc idesc{};
    const BOOL isCompute = (descriptor.commandTypes &
        (MTLIndirectCommandTypeConcurrentDispatch | MTLIndirectCommandTypeConcurrentDispatchThreads)) != 0;
    idesc.type = isCompute ? A840_ICB_TYPE_COMPUTE : A840_ICB_TYPE_RENDER;
    idesc.max_commands = (uint32_t)maxCount;
    idesc.inherit_pipeline = descriptor.inheritPipelineState ? true : false;
    idesc.inherit_buffers = descriptor.inheritBuffers ? true : false;

    A840SysIcbHandle handle = A840_SYS_INVALID_HANDLE;
    if (a840_sys_icb_create(owner.systemFrontend, &idesc, &handle) != A840_SYS_OK) {
        return nil;
    }
    _icbHandle = handle;
    _size = maxCount;
    return self;
}

- (void)dealloc
{
    if (self.owner && self.owner.systemFrontend && _icbHandle != A840_SYS_INVALID_HANDLE) {
        (void)a840_sys_icb_destroy(self.owner.systemFrontend, _icbHandle);
        _icbHandle = A840_SYS_INVALID_HANDLE;
    }
}

- (A840IndirectCmdBuffer *)nativeIcb
{
    return (self.owner && self.owner.systemFrontend)
               ? a840_sys_icb_resolve(self.owner.systemFrontend, _icbHandle)
               : nullptr;
}

- (NSUInteger)size { return _size; }
- (MTLResourceID)gpuResourceID { return MTLResourceID{_icbHandle}; }
- (void)resetWithRange:(NSRange)range
{
    A840IndirectCmdBuffer *native = self.nativeIcb;
    if (native) (void)a840_icb_reset_range(native, (uint32_t)range.location, (uint32_t)range.length);
}
- (id<MTLIndirectRenderCommand>)indirectRenderCommandAtIndex:(NSUInteger)commandIndex
{
    if (commandIndex >= _size) return nil;
    return [[A840MTLIndirectRenderCommand alloc] initWithIcb:self index:commandIndex];
}
- (id<MTLIndirectComputeCommand>)indirectComputeCommandAtIndex:(NSUInteger)commandIndex
{
    if (commandIndex >= _size) return nil;
    return [[A840MTLIndirectComputeCommand alloc] initWithIcb:self index:commandIndex];
}
@end

#pragma clang diagnostic pop
