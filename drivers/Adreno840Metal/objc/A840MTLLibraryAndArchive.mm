/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * Native Adreno 840 BinaryArchive & Library loader (data/file/source/stitched). */
#import "A840MTLLibraryAndArchive.h"
#import "A840MTLPipelineProvider.h"

#include <cstring>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"

static A840PipelineKey hashStringToPipelineKey(NSString *tag)
{
    A840PipelineKey key{};
    const char *utf8 = tag.UTF8String ?: "";
    uint64_t h1 = UINT64_C(14695981039346656037);
    uint64_t h2 = UINT64_C(11400714819323198485);
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(utf8); *p; ++p) {
        h1 ^= (uint64_t)(*p);
        h1 *= UINT64_C(1099511628211);
        h2 ^= ((uint64_t)(*p) + 17u);
        h2 *= UINT64_C(1099511628211);
    }
    for (size_t i = 0u; i < 4u; ++i) {
        const uint64_t v = (i & 1u) ? (h2 + i * 31u) : (h1 + i * 17u);
        std::memcpy(key.sha256 + i * 8u, &v, sizeof(v));
    }
    return key;
}

@implementation A840MTLBinaryArchive {
    NSURL *_sourceURL;
}
@synthesize label = _label;

- (instancetype)initWithOwner:(A840MTLDevice *)owner
                   descriptor:(MTLBinaryArchiveDescriptor *)descriptor
                        error:(NSError **)error
{
    if (!(self = [super init]) || !owner || !owner.systemFrontend) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Invalid binary archive device");
        return nil;
    }
    _owner = owner;
    _sourceURL = [descriptor.url copy];
    const uint64_t stackKey = owner.systemFrontend ? a840_sys_frontend_pack_cache(owner.systemFrontend)->expected_stack_key : 1u;
    const uint64_t abiFp = ADRENO_COMPILE_ABI_VERSION;
    A840SysArchiveHandle handle = A840_SYS_INVALID_HANDLE;
    if (a840_sys_archive_create(owner.systemFrontend, stackKey ? stackKey : 1u, abiFp, &handle) != A840_SYS_OK) {
        if (error) *error = a840_metal_error(A840_RT_CAPACITY, @"Could not create native binary archive");
        return nil;
    }
    _archiveHandle = handle;
    return self;
}

- (void)dealloc
{
    if (_owner && _owner.systemFrontend && _archiveHandle != A840_SYS_INVALID_HANDLE) {
        (void)a840_sys_archive_destroy(_owner.systemFrontend, _archiveHandle);
        _archiveHandle = A840_SYS_INVALID_HANDLE;
    }
}

- (id<MTLDevice>)device { return _owner; }

- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)descriptor
                                            error:(NSError **)error
{
    if (!descriptor || !descriptor.computeFunction) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Compute descriptor missing function");
        return NO;
    }
    const A840PipelineKey key = hashStringToPipelineKey(descriptor.computeFunction.name ?: @"compute");
    if (a840_sys_archive_add_key(_owner.systemFrontend, _archiveHandle, &key) != A840_SYS_OK) {
        if (error) *error = a840_metal_error(A840_RT_CAPACITY, @"Binary archive capacity exceeded");
        return NO;
    }
    return YES;
}

- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor
                                           error:(NSError **)error
{
    if (!descriptor || !descriptor.vertexFunction) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Render descriptor missing vertex function");
        return NO;
    }
    NSString *tag = [NSString stringWithFormat:@"%@|%@",
        descriptor.vertexFunction.name ?: @"vs",
        descriptor.fragmentFunction.name ?: @"fs"];
    const A840PipelineKey key = hashStringToPipelineKey(tag);
    if (a840_sys_archive_add_key(_owner.systemFrontend, _archiveHandle, &key) != A840_SYS_OK) {
        if (error) *error = a840_metal_error(A840_RT_CAPACITY, @"Binary archive capacity exceeded");
        return NO;
    }
    return YES;
}

- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error
{
    if (!url) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Archive URL is required");
        return NO;
    }
    uint32_t count = 0u;
    (void)a840_sys_archive_count(_owner.systemFrontend, _archiveHandle, &count);
    NSDictionary *payload = @{
        @"format": @"A840BinaryArchiveV1",
        @"abi_version": @(ADRENO_COMPILE_ABI_VERSION),
        @"pipeline_count": @(count)
    };
    NSData *json = [NSJSONSerialization dataWithJSONObject:payload options:0 error:error];
    if (!json) return NO;
    return [json writeToURL:url options:NSDataWritingAtomic error:error];
}
@end

@implementation A840MTLStitchedLibraryLoader

+ (id<MTLLibrary>)libraryWithDispatchData:(dispatch_data_t)data
                                   device:(A840MTLDevice *)device
                                    error:(NSError **)error
{
    if (!data || !device) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Invalid dispatch_data_t metallib");
        return nil;
    }
    const void *buffer = nullptr;
    size_t size = 0u;
    dispatch_data_t contiguous = dispatch_data_create_map(data, &buffer, &size);
    if (!contiguous || !buffer || size == 0u) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Empty metallib data");
        return nil;
    }
    NSData *nsData = [NSData dataWithBytes:buffer length:size];
    return [[A840MTLLibrary alloc] initWithDevice:device data:nsData error:error];
}

+ (id<MTLLibrary>)libraryWithSource:(NSString *)source
                            options:(MTLCompileOptions *)options
                             device:(A840MTLDevice *)device
                              error:(NSError **)error
{
    (void)options;
    if (!source.length || !device) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"MSL source is required");
        return nil;
    }
    /* Runtime MSL text compilation requires out-of-process NezhaShaderCompiler
     * or precompiled AIR/metallib; fail-closed without fallback when absent. */
    A840ProdCompilerQuery cq{};
    cq.precompiled_available = false;
    cq.daemon_available = false;
    (void)a840_prod_route_compiler(device.productionRouter, &cq);
    if (error) {
        *error = a840_metal_error(A840_RT_UNSUPPORTED,
                                  @"Online MSL source compile requires precompiled AIR or NezhaShaderCompiler daemon");
    }
    return nil;
}

+ (id<MTLLibrary>)libraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)descriptor
                                         device:(A840MTLDevice *)device
                                          error:(NSError **)error
{
    if (!descriptor || !device) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Invalid stitched library descriptor");
        return nil;
    }
    for (id<MTLFunction> fn in descriptor.functions) {
        if ([(id)fn isKindOfClass:A840MTLFunction.class]) {
            A840MTLLibrary *ownerLib = [(A840MTLFunction *)fn ownerLibrary];
            if (ownerLib) return ownerLib;
        }
    }
    if (error) {
        *error = a840_metal_error(A840_RT_UNSUPPORTED, @"Stitched library requires native A840MTLFunction inputs");
    }
    return nil;
}

@end
#pragma clang diagnostic pop
