/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * End-to-end native Phase 2 pipeline provider:
 *   .metallib -> libnative_metal_translate.a -> libadreno_compile_ir3.a ->
 *   a840_binding_plan -> GPU ISA & bindless descriptor BO upload -> A8XX PM4. */
#import "A840MTLPipelineProvider.h"
#import "A840MTLEncoders.h"
#import "A840MTLResource.h"
#import "A840MTLDiagnostics.h"

#include "Adreno840Caps.h"
#include "AdrenoCompileABI.h"
#include "NativeMetalTranslate.h"
#include "a840_binding_plan.h"
#include "a840_gfx_bind_sds.h"
#include "a840_ir3_io_info.h"
#include "gen/a840_bitfields.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"
#pragma clang diagnostic ignored "-Wobjc-protocol-property-synthesis"

namespace {

constexpr uint32_t kDescTypeSampler = 0u;
constexpr uint32_t kDescTypeSampledImage = 2u;
constexpr uint32_t kDescTypeStorageImage = 3u;
constexpr uint32_t kDescTypeUniformBuffer = 6u;
constexpr uint32_t kDescTypeStorageBuffer = 7u;
constexpr uint32_t kDescTypeUniformBufferDynamic = 8u;
constexpr uint64_t kKaanapaliChipId = UINT64_C(1141180977); /* 0x44050a31 */
constexpr uint64_t kIdentityAddressBias = UINT64_C(0x80000000);
constexpr size_t kIdentityBindlessSlotBytes = 64u;
constexpr size_t kIdentityMaxPushBytes = 256u;

static uint32_t packNominalLocalSize(MTLSize local)
{
    const uint32_t lx = (local.width >= 1u && local.width <= 1024u) ? static_cast<uint32_t>(local.width) : 1u;
    const uint32_t ly = (local.height >= 1u && local.height <= 1024u) ? static_cast<uint32_t>(local.height) : 1u;
    const uint32_t lz = (local.depth >= 1u && local.depth <= 1024u) ? static_cast<uint32_t>(local.depth) : 1u;
    return (lx - 1u) | ((ly - 1u) << 10u) | ((lz - 1u) << 20u);
}

static void writeFieldBytes(uint8_t *pushBuf, size_t pushCap,
                            uint8_t *argBuf, size_t argCap,
                            const NmbSpirvPushField &pf,
                            const void *src, size_t srcBytes)
{
    uint8_t *dst = pf.in_arg_table ? argBuf : pushBuf;
    const size_t cap = pf.in_arg_table ? argCap : pushCap;
    if (!dst || !src || pf.size_bytes == 0u) return;
    const size_t n = srcBytes < pf.size_bytes ? srcBytes : static_cast<size_t>(pf.size_bytes);
    if (static_cast<size_t>(pf.offset_bytes) + n > cap) return;
    std::memcpy(dst + pf.offset_bytes, src, n);
}

static A840RtBuffer *allocZeroPageBuffer(A840RtDevice *rt)
{
    if (!rt) return nullptr;
    A840RtBufferDesc desc{};
    desc.size_bytes = 4096u;
    desc.alignment = 4096u;
    desc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE;
    A840RtBuffer *bo = nullptr;
    if (a840_rt_buffer_create(rt, &desc, &bo) != A840_RT_OK || !bo || !bo->cpu_ptr) {
        if (bo) a840_rt_buffer_release(bo);
        return nullptr;
    }
    std::memset(bo->cpu_ptr, 0, 4096u);
    return bo;
}

struct A840IdentityBindState {
    A840RtBuffer *descBo = nullptr;
    uint64_t setOffsets[ADRENO_COMPILE_MAX_BIND_SETS] = {};
    bool setActive[ADRENO_COMPILE_MAX_BIND_SETS] = {};
    uint32_t activeSetCount = 0u;
    uint8_t pushBuf[2][kIdentityMaxPushBytes] = {};
    uint32_t pushVec4Count[2] = {};
};

static A840CompileTarget makeKaanapaliTarget()
{
    A840CompileTarget t{};
    t.abi_version = ADRENO_COMPILE_ABI_VERSION;
    t.chip_id = kKaanapaliChipId;
    t.num_slices = 3u;
    t.num_ccu = 6u;
    t.highest_bank_bit = 16u;
    t.ubwc_swizzle = 6u;
    t.macrotile_mode = 1u;
    t.flags = 0u;
    return t;
}

static size_t alignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static A840RtBuffer *uploadIsaBuffer(A840RtDevice *rt, const uint32_t *words, uint32_t dwords)
{
    if (!rt || !words || dwords == 0u) return nullptr;
    const size_t rawBytes = static_cast<size_t>(dwords) * sizeof(uint32_t);
    const size_t allocBytes = alignUp(rawBytes < 256u ? 256u : rawBytes, 256u);
    A840RtBufferDesc desc{};
    desc.size_bytes = allocBytes;
    desc.alignment = 4096u;
    desc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_SHADER | A840_BUF_USAGE_CPU_VISIBLE;
    A840RtBuffer *bo = nullptr;
    if (a840_rt_buffer_create(rt, &desc, &bo) != A840_RT_OK || !bo) return nullptr;
    if (bo->cpu_ptr) {
        std::memset(bo->cpu_ptr, 0, allocBytes);
        std::memcpy(bo->cpu_ptr, words, rawBytes);
    }
    return bo;
}

static A840RtBuffer *allocPrivateScratch(A840RtDevice *rt, uint32_t privateMemBytes)
{
    if (!rt || privateMemBytes == 0u) return nullptr;
    if (privateMemBytes > (A840_PRIVATE_ITEM_MAX << A840_PRIVATE_ITEM_SHR)) return nullptr;
    uint32_t item = UINT32_C(1) << A840_PRIVATE_ITEM_SHR;
    while (item < privateMemBytes) item *= 2u;
    const size_t need = static_cast<size_t>(item) * A840_LIMIT_FIBERS_PER_SP * A840_LIMIT_NUM_SP_CORES;
    const size_t totalBytes = alignUp(need, 4096u);
    A840RtBufferDesc desc{};
    desc.size_bytes = totalBytes;
    desc.alignment = 4096u;
    desc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE;
    A840RtBuffer *bo = nullptr;
    if (a840_rt_buffer_create(rt, &desc, &bo) != A840_RT_OK) return nullptr;
    return bo;
}

static bool isUboType(uint32_t dtype)
{
    return dtype == kDescTypeUniformBuffer || dtype == kDescTypeUniformBufferDynamic;
}

static const A840BindingLocation *findBindingLocation(const A840BindingPlan *plan,
                                                      uint32_t set, uint32_t binding)
{
    for (uint32_t i = 0u; i < plan->binding_count; ++i) {
        if (plan->bindings[i].set == set && plan->bindings[i].binding == binding)
            return &plan->bindings[i];
    }
    return nullptr;
}

} /* namespace */


@protocol A840OriginalConstantValue <NSObject>
- (MTLDataType)dataType;
- (const void *)data;
- (NSUInteger)index;
- (NSString *)name;
@end

@interface MTLFunctionConstantValues (A840OriginalSPI)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end

struct A840OwnedConstant {
    uint32_t dataType;
    uint32_t index;
    bool hasName;
    std::string name;
    std::vector<uint8_t> bytes;
};

static bool a840MethodMatchesABI(id object, SEL selector, const char *encoding)
{
    if (!object || !selector || !encoding) return false;
    Method m = class_getInstanceMethod(object_getClass(object), selector);
    return m && method_getTypeEncoding(m) && std::strcmp(method_getTypeEncoding(m), encoding) == 0;
}

static size_t a840ConstantByteLength(uint32_t mtlDataType)
{
    switch (mtlDataType) {
    case 3:  case 29: case 33: return 4u;   /* float, int, uint */
    case 4:  case 30: case 34: return 8u;   /* float2, int2, uint2 */
    case 5:  case 31: case 35: return 12u;  /* float3, int3, uint3 */
    case 6:  case 32: case 36: return 16u;  /* float4, int4, uint4 */
    case 16: case 37: case 41: return 2u;   /* half, short, ushort */
    case 17: case 38: case 42: return 4u;   /* half2, short2, ushort2 */
    case 18: case 39: case 43: return 6u;   /* half3, short3, ushort3 */
    case 19: case 40: case 44: return 8u;   /* half4, short4, ushort4 */
    case 45: case 49: case 53: return 1u;   /* char, uchar, bool */
    case 46: case 50: case 54: return 2u;   /* char2, uchar2, bool2 */
    case 47: case 51: case 55: return 3u;   /* char3, uchar3, bool3 */
    case 48: case 52: case 56: return 4u;   /* char4, uchar4, bool4 */
    case 81: case 85: return 8u;            /* long, ulong */
    case 82: case 86: return 16u;           /* long2, ulong2 */
    case 83: case 87: return 24u;           /* long3, ulong3 */
    case 84: case 88: return 32u;           /* long4, ulong4 */
    default: return 0u;
    }
}

static bool a840ExtractConstantValues(MTLFunctionConstantValues *values,
                                      std::vector<A840OwnedConstant> &out,
                                      NSError **error)
{
    out.clear();
    if (!values) return true;
    if (!a840MethodMatchesABI(values, @selector(newIndexedConstantArray), "@16@0:8") ||
        !a840MethodMatchesABI(values, @selector(newNamedConstantArray), "@16@0:8")) {
        if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"Unsupported MTLFunctionConstantValues SPI");
        return false;
    }
    MTLFunctionConstantValues *snapshot = [values copy];
    NSArray *indexed = [snapshot newIndexedConstantArray];
    NSArray *named = [snapshot newNamedConstantArray];
    for (unsigned group = 0u; group < 2u; ++group) {
        NSArray *arr = group ? named : indexed;
        for (id item in arr) {
            if (!a840MethodMatchesABI(item, @selector(dataType), "Q16@0:8") ||
                !a840MethodMatchesABI(item, @selector(data), "^v16@0:8") ||
                !a840MethodMatchesABI(item, group ? @selector(name) : @selector(index),
                                      group ? "@16@0:8" : "Q16@0:8")) {
                if (error) *error = a840_metal_error(A840_RT_UNSUPPORTED, @"Unsupported constant value element ABI");
                return false;
            }
            id<A840OriginalConstantValue> v = item;
            const uint32_t dt = static_cast<uint32_t>(v.dataType);
            const size_t nbytes = a840ConstantByteLength(dt);
            const void *ptr = v.data;
            if (!nbytes || !ptr) {
                if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Invalid function constant type or null data");
                return false;
            }
            A840OwnedConstant owned{};
            owned.dataType = dt;
            owned.hasName = (group != 0u);
            if (owned.hasName) {
                const char *cname = v.name.UTF8String;
                if (!cname || !cname[0]) continue;
                owned.name = cname;
            } else {
                owned.index = static_cast<uint32_t>(v.index);
            }
            owned.bytes.assign(static_cast<const uint8_t *>(ptr),
                               static_cast<const uint8_t *>(ptr) + nbytes);
            out.push_back(std::move(owned));
        }
    }
    return true;
}

static void a840MarshalConstants(const std::vector<A840OwnedConstant> &owned,
                                 std::vector<NmbFunctionConstant> &ffiOut)
{
    ffiOut.clear();
    ffiOut.reserve(owned.size());
    for (const auto &c : owned) {
        NmbFunctionConstant fc{};
        fc.mtl_data_type = c.dataType;
        fc.index = c.hasName ? 0u : c.index;
        fc.name = c.hasName ? c.name.c_str() : nullptr;
        fc.bytes = c.bytes.data();
        fc.length = c.bytes.size();
        ffiOut.push_back(fc);
    }
}

@interface A840MTLFunction ()
- (const std::vector<A840OwnedConstant> &)ownedConstants;
@end

@implementation A840MTLFunction {
    NSString *_name;
    MTLFunctionType _type;
    std::vector<A840OwnedConstant> _constants;
    uint8_t _bitCodeHash[32];
}
@synthesize label = _label;

- (instancetype)initWithLibrary:(A840MTLLibrary *)library name:(NSString *)name
                   functionType:(MTLFunctionType)functionType
{
    return [self initWithLibrary:library name:name functionType:functionType constantValues:nil error:nil];
}

- (instancetype)initWithLibrary:(A840MTLLibrary *)library name:(NSString *)name
                   functionType:(MTLFunctionType)functionType
                 constantValues:(MTLFunctionConstantValues *)constantValues
                          error:(NSError **)error
{
    if (!(self = [super init])) return nil;
    if (constantValues && !a840ExtractConstantValues(constantValues, _constants, error)) {
        return nil;
    }
    _ownerLibrary = library;
    _name = [name copy];
    _type = functionType;
    return self;
}
- (const std::vector<A840OwnedConstant> &)ownedConstants { return _constants; }
- (id<MTLDevice>)device { return _ownerLibrary.device; }
- (NSString *)name { return _name; }
- (MTLFunctionType)functionType { return _type; }
- (MTLPatchType)patchType { return MTLPatchTypeNone; }
- (NSInteger)patchControlPointCount { return 0; }
- (const void *)bitCodeHash
{
    uint64_t h1 = 14695981039346656037ULL;
    uint64_t h2 = 1099511628211ULL;
    const char *s = _name.UTF8String ?: "";
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
        h1 ^= *p; h1 *= 1099511628211ULL;
        h2 = (h2 * 1315423911ULL) ^ *p;
    }
    const uintptr_t libPtr = reinterpret_cast<uintptr_t>((__bridge void *)_ownerLibrary);
    std::memcpy(_bitCodeHash + 0, &h1, 8u);
    std::memcpy(_bitCodeHash + 8, &h2, 8u);
    std::memcpy(_bitCodeHash + 16, &libPtr, sizeof(libPtr));
    std::memcpy(_bitCodeHash + 24, &_type, sizeof(uint64_t));
    return _bitCodeHash;
}
- (NSArray<MTLVertexAttribute *> *)vertexAttributes { return nil; }
- (NSArray<MTLAttribute *> *)stageInputAttributes { return nil; }
- (NSDictionary<NSString *, MTLFunctionConstant *> *)functionConstantsDictionary { return @{}; }
- (MTLFunctionOptions)options { return MTLFunctionOptionNone; }
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLFunction", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
@end

@implementation A840MTLLibrary {
    __weak A840MTLDevice *_device;
    NSDictionary<NSString *, NSNumber *> *_functions;
    NSArray<NSString *> *_externNames;
    NSString *_overrideTriple;
    BOOL _shaderValidationEnabled;
    NmbDevice *_parseDevice;
    NmbLibrary *_parsedLib;
}
@synthesize label = _label;

- (instancetype)initWithDevice:(A840MTLDevice *)device data:(NSData *)data error:(NSError **)error
{
    if (!(self = [super init])) return nil;
    if (!device || !data || data.length < 4u) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Invalid metallib data");
        return nil;
    }
    _device = device;
    _metallibData = [data copy];

    _parseDevice = nmb_device_create_a840(0u);
    if (!_parseDevice) {
        if (error) *error = a840_metal_error(A840_RT_STATE_ERROR, @"Failed to create translator device");
        return nil;
    }
    _parsedLib = nmb_library_parse(_parseDevice, static_cast<const uint8_t *>(_metallibData.bytes),
                                   _metallibData.length);
    if (!_parsedLib) {
        const char *msg = nmb_last_error();
        if (error) {
            NSString *desc = msg && msg[0] ? [NSString stringWithUTF8String:msg] : @"Failed to parse metallib";
            *error = a840_metal_error(A840_RT_INVALID_ARG, desc);
        }
        return nil;
    }

    NSMutableDictionary<NSString *, NSNumber *> *map = [NSMutableDictionary new];
    NSMutableArray<NSString *> *externs = [NSMutableArray new];
    const size_t count = nmb_library_function_count(_parsedLib);
    char nameBuf[512];
    for (size_t i = 0u; i < count; ++i) {
        if (!nmb_library_function_name(_parsedLib, i, nameBuf, sizeof(nameBuf))) continue;
        uint32_t kind = NMB_FUNCTION_KERNEL;
        (void)nmb_library_function_stage_kind(_parsedLib, i, &kind);
        MTLFunctionType ftype = MTLFunctionTypeKernel;
        if (kind == NMB_FUNCTION_VERTEX) ftype = MTLFunctionTypeVertex;
        else if (kind == NMB_FUNCTION_FRAGMENT) ftype = MTLFunctionTypeFragment;
        else if (kind == NMB_FUNCTION_EXPORTED || kind == NMB_FUNCTION_VISIBLE || kind == NMB_FUNCTION_EXTERN) ftype = MTLFunctionTypeVisible;
        else if (kind == NMB_FUNCTION_INTERSECTION) ftype = MTLFunctionTypeIntersection;
        else if (kind == NMB_FUNCTION_MESH) ftype = (MTLFunctionType)7;
        else if (kind == NMB_FUNCTION_OBJECT) ftype = (MTLFunctionType)8;
        NSString *fnName = [NSString stringWithUTF8String:nameBuf];
        if (fnName) {
            map[fnName] = @(ftype);
            if (kind == NMB_FUNCTION_EXTERN) [externs addObject:fnName];
        }
    }
    _functions = [map copy];
    _externNames = [externs copy];
    return self;
}

- (void)dealloc
{
    if (_parsedLib) nmb_library_destroy(_parsedLib);
    if (_parseDevice) nmb_device_destroy(_parseDevice);
}

- (void *)parsedNmbLibrary { return _parsedLib; }
- (id<MTLDevice>)device { return _device; }
- (NSArray<NSString *> *)functionNames { return _functions.allKeys; }

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    NSNumber *kind = name ? _functions[name] : nil;
    if (!kind) return nil;
    return [[A840MTLFunction alloc] initWithLibrary:self name:name
                                       functionType:static_cast<MTLFunctionType>(kind.unsignedIntegerValue)];
}
- (MTLLibraryType)type { return MTLLibraryTypeExecutable; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)constantValues
                                 error:(NSError **)error
{
    NSNumber *kind = name ? _functions[name] : nil;
    if (!kind) {
        if (error) {
            *error = a840_metal_error(A840_RT_INVALID_ARG,
                                      [NSString stringWithFormat:@"Function not found: %@", name ?: @""]);
        }
        return nil;
    }
    id<MTLFunction> fn = [[A840MTLFunction alloc] initWithLibrary:self
                                                             name:name
                                                     functionType:static_cast<MTLFunctionType>(kind.unsignedIntegerValue)
                                                   constantValues:constantValues
                                                            error:error];
    if (fn && error) *error = nil;
    return fn;
}
- (void)newFunctionWithName:(NSString *)name
             constantValues:(MTLFunctionConstantValues *)constantValues
          completionHandler:(void (^)(id<MTLFunction>, NSError *))completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    id<MTLFunction> fn = [self newFunctionWithName:name constantValues:constantValues error:&err];
    completionHandler(fn, err);
}
- (NSArray<NSString *> *)externFunctionNames { return _externNames ?: @[]; }
- (id<MTLFunction>)newExternFunctionWithName:(NSString *)name { return [self newFunctionWithName:name]; }
- (NSString *)overrideTriple { return _overrideTriple; }
- (void)setOverrideTriple:(NSString *)triple { _overrideTriple = [triple copy]; }
- (BOOL)shaderValidationEnabled { return _shaderValidationEnabled; }
- (void)setShaderValidationEnabled:(BOOL)enabled { _shaderValidationEnabled = enabled; }
- (NSString *)installName { return nil; }
- (NSUUID *)libraryIdentifier { return nil; }
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor error:(NSError **)error
{
    if (!descriptor || !descriptor.name.length) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Function descriptor name required");
        return nil;
    }
    return [self newFunctionWithName:descriptor.name constantValues:descriptor.constantValues error:error];
}
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor
                completionHandler:(void (^)(id<MTLFunction>, NSError *))completionHandler
{
    if (!completionHandler) return;
    NSError *err = nil;
    id<MTLFunction> fn = [self newFunctionWithDescriptor:descriptor error:&err];
    completionHandler(fn, err);
}
- (void)doesNotRecognizeSelector:(SEL)selector
{
    a840diag::recordMissingSelector("A840MTLLibrary", sel_getName(selector));
    [super doesNotRecognizeSelector:selector];
}
@end

@interface A840MTLCompiledPlan : NSObject {
@public
    BOOL isCompute;
    A840RtDevice *rtDevice;
    A840CompiledStage stages[2];
    A840Ir3StageIoInfo ioInfo[2];
    uint32_t stageCount;
    A840RtBuffer *codeBuffers[2];
    A840RtBuffer *privateBuffers[2];
    A840BindingPlan bindingPlan;
    std::vector<NmbSpirvPushField> pushFields[2];
    uint32_t argTableBytes[2];
    NmbSpirvPushConstantRange pushRanges[2];
    int32_t sampledImageSet;
    int32_t storageImageSet;
    int32_t samplerSet;
    A840RtBuffer *zeroPageBo;
    NSMutableArray<NSValue *> *transientBuffers;
    A840MTLFunction *computeFunction;
    A840RtComputePipelineDesc computeDesc;
    NSMutableDictionary<NSString *, A840MTLCompiledPlan *> *computeVariants;
}
@end

@implementation A840MTLCompiledPlan
- (instancetype)init
{
    if ((self = [super init])) {
        argTableBytes[0] = 0u;
        argTableBytes[1] = 0u;
        pushRanges[0] = {};
        pushRanges[1] = {};
        sampledImageSet = -1;
        storageImageSet = -1;
        samplerSet = -1;
        zeroPageBo = nullptr;
        transientBuffers = [NSMutableArray new];
        computeVariants = [NSMutableDictionary new];
    }
    return self;
}
- (void)dealloc
{
    if (zeroPageBo) a840_rt_buffer_release(zeroPageBo);
    for (NSValue *val in transientBuffers) {
        auto *bo = static_cast<A840RtBuffer *>(val.pointerValue);
        if (bo) a840_rt_buffer_release(bo);
    }
    for (uint32_t i = 0u; i < 2u; ++i) {
        if (codeBuffers[i]) a840_rt_buffer_release(codeBuffers[i]);
        if (privateBuffers[i]) a840_rt_buffer_release(privateBuffers[i]);
        a840_compile_stage_free(&stages[i]);
    }
}
@end

@implementation A840MTLNativePipelineProvider {
    NmbDevice *_translator;
}

- (instancetype)init
{
    if (!(self = [super init])) return nil;
    _translator = nmb_device_create_a840(0u);
    if (!_translator) return nil;
    return self;
}

- (void)dealloc
{
    if (_translator) nmb_device_destroy(_translator);
}

- (id<MTLLibrary>)defaultLibraryForDevice:(A840MTLDevice *)device
{
    const char *envPath = std::getenv("A840_DEFAULT_METALLIB");
    NSURL *url = nil;
    if (envPath && envPath[0]) {
        url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:envPath]];
    } else {
        url = [[NSBundle mainBundle] URLForResource:@"default" withExtension:@"metallib"];
    }
    if (!url) return nil;
    return [self libraryWithURL:url forDevice:device error:nil];
}

- (id<MTLLibrary>)libraryWithURL:(NSURL *)url forDevice:(A840MTLDevice *)device error:(NSError **)error
{
    if (!url) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Library URL is required");
        return nil;
    }
    NSData *data = [NSData dataWithContentsOfURL:url options:0 error:error];
    if (!data) return nil;
    return [[A840MTLLibrary alloc] initWithDevice:device data:data error:error];
}

- (BOOL)captureIdentityReflection:(const NmbSpirvResult *)spv
                         intoPlan:(A840MTLCompiledPlan *)plan
                       stageCount:(uint32_t)stageCount
{
    if (!spv || !plan || stageCount == 0u || stageCount > 2u) return NO;
    bool needsZeroPage = false;
    for (uint32_t s = 0u; s < stageCount; ++s) {
        plan->pushFields[s].clear();
        plan->argTableBytes[s] = nmb_spirv_arg_table_bytes(spv, s);
        nmb_spirv_push_constant_range(spv, s, &plan->pushRanges[s]);
        const size_t pfCount = nmb_spirv_push_field_count(spv, s);
        plan->pushFields[s].reserve(pfCount);
        for (size_t i = 0u; i < pfCount; ++i) {
            NmbSpirvPushField pf{};
            if (nmb_spirv_push_field(spv, s, i, &pf)) {
                if (pf.kind == NMB_PUSH_ZERO_ADDRESS || pf.kind == NMB_PUSH_BUFFER_ADDRESS) {
                    needsZeroPage = true;
                }
                plan->pushFields[s].push_back(pf);
            }
        }
    }
    const size_t bCount = nmb_spirv_binding_count(spv);
    for (size_t i = 0u; i < bCount; ++i) {
        NmbSpirvBinding b{};
        if (!nmb_spirv_binding(spv, i, &b)) continue;
        if (b.descriptor_set >= ADRENO_COMPILE_MAX_BIND_SETS) continue;
        if (b.binding == 0u && b.descriptor_type == kDescTypeSampledImage && b.texture_index == UINT32_MAX) {
            plan->sampledImageSet = static_cast<int32_t>(b.descriptor_set);
        } else if (b.binding == 0u && b.descriptor_type == kDescTypeStorageImage && b.texture_index == UINT32_MAX) {
            plan->storageImageSet = static_cast<int32_t>(b.descriptor_set);
        } else if (b.binding == 0u && b.descriptor_type == kDescTypeSampler && b.sampler_index == UINT32_MAX) {
            plan->samplerSet = static_cast<int32_t>(b.descriptor_set);
        }
    }
    if (needsZeroPage && !plan->zeroPageBo) {
        plan->zeroPageBo = allocZeroPageBuffer(plan->rtDevice);
        if (!plan->zeroPageBo) return NO;
    }
    return YES;
}

- (BOOL)planUsesIdentityPush:(A840MTLCompiledPlan *)plan
{
    return plan && (!plan->pushFields[0].empty() || !plan->pushFields[1].empty());
}

- (BOOL)buildIdentityBindStateForPlan:(A840MTLCompiledPlan *)plan
                        stageBindings:(A840MTLBindings *const *)stageBindings
                           stageCount:(uint32_t)stageCount
                         colorTargets:(id<MTLTexture> const __unsafe_unretained *)colorTargets
                     colorTargetCount:(uint32_t)colorTargetCount
                             gridSize:(MTLSize)grid
                      threadgroupSize:(MTLSize)local
                           nonuniform:(BOOL)nonuniform
                               outRes:(A840IdentityBindState *)outRes
{
    if (!plan || !outRes || stageCount == 0u || stageCount > 2u) return NO;
    *outRes = A840IdentityBindState{};

    /* 1. Assign 1-based global bindless slots (slot 0 is reserved null/zeroed). */
    NSMutableArray<A840MTLTexture *> *uniqueTextures = [NSMutableArray new];
    NSMutableArray<A840MTLSamplerState *> *uniqueSamplers = [NSMutableArray new];
    NSMutableDictionary<NSString *, NSNumber *> *texSlotMap = [NSMutableDictionary new];
    NSMutableDictionary<NSString *, NSNumber *> *sampSlotMap = [NSMutableDictionary new];
    uint32_t fetchedColorSlots[A840_MAX_COLOR_TARGETS] = {};

    for (uint32_t s = 0u; s < stageCount; ++s) {
        A840MTLBindings *b = stageBindings[s];
        if (!b) continue;
        NSArray<NSNumber *> *texKeys = [b.textures.allKeys sortedArrayUsingSelector:@selector(compare:)];
        for (NSNumber *key in texKeys) {
            id<MTLTexture> rawTex = b.textures[key];
            if (![rawTex isKindOfClass:[A840MTLTexture class]]) continue;
            auto *tex = static_cast<A840MTLTexture *>(rawTex);
            if (!tex.native) continue;
            uint32_t slotIdx = 0u;
            for (NSUInteger i = 0u; i < uniqueTextures.count; ++i) {
                if (uniqueTextures[i] == tex) {
                    slotIdx = static_cast<uint32_t>(i + 1u);
                    break;
                }
            }
            if (slotIdx == 0u) {
                [uniqueTextures addObject:tex];
                slotIdx = static_cast<uint32_t>(uniqueTextures.count);
            }
            NSString *tag = [NSString stringWithFormat:@"%u:%u", s, key.unsignedIntValue];
            texSlotMap[tag] = @(slotIdx);
        }
        NSArray<NSNumber *> *sampKeys = [b.samplers.allKeys sortedArrayUsingSelector:@selector(compare:)];
        for (NSNumber *key in sampKeys) {
            id<MTLSamplerState> rawSamp = b.samplers[key];
            if (![rawSamp isKindOfClass:[A840MTLSamplerState class]]) continue;
            auto *samp = static_cast<A840MTLSamplerState *>(rawSamp);
            if (!samp.descriptorWords) continue;
            uint32_t slotIdx = 0u;
            for (NSUInteger i = 0u; i < uniqueSamplers.count; ++i) {
                if (uniqueSamplers[i] == samp) {
                    slotIdx = static_cast<uint32_t>(i + 1u);
                    break;
                }
            }
            if (slotIdx == 0u) {
                [uniqueSamplers addObject:samp];
                slotIdx = static_cast<uint32_t>(uniqueSamplers.count);
            }
            NSString *tag = [NSString stringWithFormat:@"%u:%u", s, key.unsignedIntValue];
            sampSlotMap[tag] = @(slotIdx);
        }
    }

    if (colorTargets && colorTargetCount > 0u) {
        const uint32_t lim = colorTargetCount > A840_MAX_COLOR_TARGETS ? A840_MAX_COLOR_TARGETS : colorTargetCount;
        for (uint32_t c = 0u; c < lim; ++c) {
            id<MTLTexture> rawTex = colorTargets[c];
            if (![rawTex isKindOfClass:[A840MTLTexture class]]) continue;
            auto *tex = static_cast<A840MTLTexture *>(rawTex);
            if (!tex.native) continue;
            uint32_t slotIdx = 0u;
            for (NSUInteger i = 0u; i < uniqueTextures.count; ++i) {
                if (uniqueTextures[i] == tex) {
                    slotIdx = static_cast<uint32_t>(i + 1u);
                    break;
                }
            }
            if (slotIdx == 0u) {
                [uniqueTextures addObject:tex];
                slotIdx = static_cast<uint32_t>(uniqueTextures.count);
            }
            fetchedColorSlots[c] = slotIdx;
        }
    }

    /* 2. Lay out single 64B-aligned transient BO: bindless sets + inline setBytes + arg tables. */
    size_t cursor = 0u;
    if (plan->sampledImageSet >= 0 && uniqueTextures.count > 0u) {
        const size_t setIdx = static_cast<size_t>(plan->sampledImageSet);
        outRes->setOffsets[setIdx] = cursor;
        outRes->setActive[setIdx] = true;
        if (static_cast<uint32_t>(setIdx + 1u) > outRes->activeSetCount) {
            outRes->activeSetCount = static_cast<uint32_t>(setIdx + 1u);
        }
        cursor += (1u + uniqueTextures.count) * kIdentityBindlessSlotBytes;
    }
    if (plan->storageImageSet >= 0 && uniqueTextures.count > 0u) {
        const size_t setIdx = static_cast<size_t>(plan->storageImageSet);
        outRes->setOffsets[setIdx] = cursor;
        outRes->setActive[setIdx] = true;
        if (static_cast<uint32_t>(setIdx + 1u) > outRes->activeSetCount) {
            outRes->activeSetCount = static_cast<uint32_t>(setIdx + 1u);
        }
        cursor += (1u + uniqueTextures.count) * kIdentityBindlessSlotBytes;
    }
    if (plan->samplerSet >= 0 && uniqueSamplers.count > 0u) {
        const size_t setIdx = static_cast<size_t>(plan->samplerSet);
        outRes->setOffsets[setIdx] = cursor;
        outRes->setActive[setIdx] = true;
        if (static_cast<uint32_t>(setIdx + 1u) > outRes->activeSetCount) {
            outRes->activeSetCount = static_cast<uint32_t>(setIdx + 1u);
        }
        cursor += (1u + uniqueSamplers.count) * kIdentityBindlessSlotBytes;
    }

    const size_t inlineStart = cursor;
    size_t inlineSlabBytes = 0u;
    for (uint32_t s = 0u; s < stageCount; ++s) {
        A840MTLBindings *b = stageBindings[s];
        if (!b) continue;
        for (NSNumber *key in b.inlineBytes) {
            NSData *d = b.inlineBytes[key];
            if (d.length > 0u) inlineSlabBytes += alignUp(d.length, 64u);
        }
    }
    cursor += inlineSlabBytes;

    size_t argTableOff[2] = {0u, 0u};
    for (uint32_t s = 0u; s < stageCount; ++s) {
        if (plan->argTableBytes[s] > 0u) {
            argTableOff[s] = cursor;
            cursor += alignUp(plan->argTableBytes[s], 64u);
        }
    }

    const size_t totalBytes = alignUp(cursor ? cursor : 64u, 256u);
    A840RtBufferDesc desc{};
    desc.size_bytes = totalBytes;
    desc.alignment = 4096u;
    desc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE;
    A840RtBuffer *bo = nullptr;
    if (a840_rt_buffer_create(plan->rtDevice, &desc, &bo) != A840_RT_OK || !bo || !bo->cpu_ptr) {
        if (bo) a840_rt_buffer_release(bo);
        return NO;
    }
    auto *baseCpu = static_cast<uint8_t *>(bo->cpu_ptr);
    std::memset(baseCpu, 0, totalBytes);
    [plan->transientBuffers addObject:[NSValue valueWithPointer:bo]];
    outRes->descBo = bo;

    /* 3. Write 64B A8XX hardware descriptors into bindless sets. */
    for (NSUInteger i = 0u; i < uniqueTextures.count; ++i) {
        const size_t slotOff = (i + 1u) * kIdentityBindlessSlotBytes;
        A840RtTexture *rtTex = uniqueTextures[i].native;
        if (plan->sampledImageSet >= 0) {
            const size_t setIdx = static_cast<size_t>(plan->sampledImageSet);
            if (outRes->setActive[setIdx]) {
                std::memcpy(baseCpu + outRes->setOffsets[setIdx] + slotOff,
                            rtTex->tex_const, sizeof(rtTex->tex_const));
            }
        }
        if (plan->storageImageSet >= 0) {
            const size_t setIdx = static_cast<size_t>(plan->storageImageSet);
            if (outRes->setActive[setIdx]) {
                std::memcpy(baseCpu + outRes->setOffsets[setIdx] + slotOff,
                            rtTex->tex_const, sizeof(rtTex->tex_const));
            }
        }
    }
    for (NSUInteger i = 0u; i < uniqueSamplers.count; ++i) {
        const size_t slotOff = (i + 1u) * kIdentityBindlessSlotBytes;
        const uint32_t *words = uniqueSamplers[i].descriptorWords;
        if (plan->samplerSet >= 0) {
            const size_t setIdx = static_cast<size_t>(plan->samplerSet);
            if (outRes->setActive[setIdx]) {
                std::memcpy(baseCpu + outRes->setOffsets[setIdx] + slotOff,
                            words, A840_SAMP_CONST_DWORDS * sizeof(uint32_t));
            }
        }
    }

    /* 4. Copy inline setBytes data into transient BO and record GPU VAs. */
    NSMutableDictionary<NSString *, NSNumber *> *inlineVaMap = [NSMutableDictionary new];
    NSMutableDictionary<NSString *, NSNumber *> *inlineLenMap = [NSMutableDictionary new];
    size_t inlineCur = inlineStart;
    for (uint32_t s = 0u; s < stageCount; ++s) {
        A840MTLBindings *b = stageBindings[s];
        if (!b) continue;
        for (NSNumber *key in b.inlineBytes) {
            NSData *d = b.inlineBytes[key];
            if (!d || d.length == 0u) continue;
            std::memcpy(baseCpu + inlineCur, d.bytes, d.length);
            NSString *tag = [NSString stringWithFormat:@"%u:%u", s, key.unsignedIntValue];
            inlineVaMap[tag] = @(bo->gpu_va + inlineCur);
            inlineLenMap[tag] = @(d.length);
            inlineCur += alignUp(d.length, 64u);
        }
    }

    /* 5. Pack metal2vulkan-v2 identity-domain push constants & optional argument table. */
    const uint64_t zeroVa = plan->zeroPageBo ? plan->zeroPageBo->gpu_va : bo->gpu_va;
    const uint32_t lx = local.width ? static_cast<uint32_t>(local.width) : 1u;
    const uint32_t ly = local.height ? static_cast<uint32_t>(local.height) : 1u;
    const uint32_t lz = local.depth ? static_cast<uint32_t>(local.depth) : 1u;
    const uint32_t tx = nonuniform ? static_cast<uint32_t>(grid.width) : static_cast<uint32_t>(grid.width) * lx;
    const uint32_t ty = nonuniform ? static_cast<uint32_t>(grid.height) : static_cast<uint32_t>(grid.height) * ly;
    const uint32_t tz = nonuniform ? static_cast<uint32_t>(grid.depth) : static_cast<uint32_t>(grid.depth) * lz;
    const uint32_t ragged = (nonuniform && ((tx % lx) != 0u || (ty % ly) != 0u || (tz % lz) != 0u)) ? 1u : 0u;

    for (uint32_t s = 0u; s < stageCount; ++s) {
        A840MTLBindings *b = stageBindings[s];
        uint8_t *argCpu = plan->argTableBytes[s] > 0u ? (baseCpu + argTableOff[s]) : nullptr;
        const size_t argCap = plan->argTableBytes[s];
        uint32_t maxEndBytes = plan->pushRanges[s].offset_bytes + plan->pushRanges[s].size_bytes;

        for (const NmbSpirvPushField &pf : plan->pushFields[s]) {
            if (!pf.in_arg_table && pf.offset_bytes + pf.size_bytes > maxEndBytes) {
                maxEndBytes = pf.offset_bytes + pf.size_bytes;
            }
            switch (pf.kind) {
            case NMB_PUSH_BUFFER_ADDRESS: {
                uint64_t va = zeroVa;
                if (b) {
                    id<MTLBuffer> rawBuf = b.buffers[@(pf.metal_slot)];
                    if ([rawBuf isKindOfClass:[A840MTLBuffer class]]) {
                        auto *nativeBuf = static_cast<A840MTLBuffer *>(rawBuf).native;
                        const uint64_t off = b.bufferOffsets[@(pf.metal_slot)].unsignedLongLongValue;
                        if (nativeBuf && off <= nativeBuf->size_bytes) {
                            va = nativeBuf->gpu_va + off;
                        }
                    } else {
                        NSString *tag = [NSString stringWithFormat:@"%u:%u", s, pf.metal_slot];
                        NSNumber *vaNum = inlineVaMap[tag];
                        if (vaNum) va = vaNum.unsignedLongLongValue;
                    }
                }
                const uint64_t biased = va - kIdentityAddressBias;
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &biased, sizeof(biased));
                break;
            }
            case NMB_PUSH_BUFFER_BOUNDS: {
                uint32_t bounds[2] = {0u, 0u};
                if (b) {
                    id<MTLBuffer> rawBuf = b.buffers[@(pf.metal_slot)];
                    if ([rawBuf isKindOfClass:[A840MTLBuffer class]]) {
                        auto *nativeBuf = static_cast<A840MTLBuffer *>(rawBuf).native;
                        const uint64_t off = b.bufferOffsets[@(pf.metal_slot)].unsignedLongLongValue;
                        if (nativeBuf) {
                            bounds[0] = static_cast<uint32_t>(off);
                            bounds[1] = static_cast<uint32_t>(nativeBuf->size_bytes);
                        }
                    } else {
                        NSString *tag = [NSString stringWithFormat:@"%u:%u", s, pf.metal_slot];
                        NSNumber *lenNum = inlineLenMap[tag];
                        if (lenNum) {
                            bounds[0] = 0u;
                            bounds[1] = lenNum.unsignedIntValue;
                        }
                    }
                }
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, bounds, sizeof(bounds));
                break;
            }
            case NMB_PUSH_ZERO_ADDRESS: {
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &zeroVa, sizeof(zeroVa));
                break;
            }
            case NMB_PUSH_TEXTURE_ID: {
                uint64_t handle = 0u;
                NSString *tag = [NSString stringWithFormat:@"%u:%u", s, pf.metal_slot];
                NSNumber *slotNum = texSlotMap[tag];
                if (slotNum && slotNum.unsignedIntValue > 0u) {
                    const uint32_t slotIdx = slotNum.unsignedIntValue;
                    A840MTLTexture *tex = uniqueTextures[slotIdx - 1u];
                    const uint32_t word = nmb_encode_texture_word(static_cast<uint32_t>(tex.pixelFormat));
                    handle = static_cast<uint64_t>(slotIdx) | (static_cast<uint64_t>(word) << 32u);
                }
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &handle, sizeof(handle));
                break;
            }
            case NMB_PUSH_SAMPLER_ID: {
                uint64_t handle = 0u;
                NSString *tag = [NSString stringWithFormat:@"%u:%u", s, pf.metal_slot];
                NSNumber *slotNum = sampSlotMap[tag];
                if (slotNum && slotNum.unsignedIntValue > 0u) {
                    const uint32_t slotIdx = slotNum.unsignedIntValue;
                    A840MTLSamplerState *samp = uniqueSamplers[slotIdx - 1u];
                    MTLSamplerDescriptor *d = samp.descriptor;
                    const uint32_t borderVal = d ? static_cast<uint32_t>(d.borderColor) : 0u;
                    const uint32_t word = d ? nmb_encode_sampler_word(
                        d.normalizedCoordinates ? true : false,
                        static_cast<uint32_t>(d.minFilter),
                        static_cast<uint32_t>(d.magFilter),
                        static_cast<uint32_t>(d.mipFilter),
                        static_cast<uint32_t>(d.sAddressMode),
                        static_cast<uint32_t>(d.tAddressMode),
                        static_cast<uint32_t>(d.rAddressMode),
                        borderVal,
                        static_cast<uint32_t>(d.compareFunction)) : 0u;
                    handle = static_cast<uint64_t>(slotIdx) | (static_cast<uint64_t>(word) << 32u);
                }
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &handle, sizeof(handle));
                break;
            }
            case NMB_PUSH_DISPATCH_REGION: {
                const uint32_t region[8] = {
                    0u, 0u, 0u, packNominalLocalSize(local),
                    tx, ty, tz, ragged,
                };
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, region, sizeof(region));
                break;
            }
            case NMB_PUSH_TEXTURE_LEVELS: {
                uint32_t lvls = 0u;
                NSString *tag = [NSString stringWithFormat:@"%u:%u", s, pf.metal_slot];
                NSNumber *slotNum = texSlotMap[tag];
                if (slotNum && slotNum.unsignedIntValue > 0u) {
                    lvls = static_cast<uint32_t>(uniqueTextures[slotNum.unsignedIntValue - 1u].mipmapLevelCount);
                }
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &lvls, sizeof(lvls));
                break;
            }
            case NMB_PUSH_TEXTURE_LEVEL_BASE: {
                uint32_t baseSlot = 0u;
                NSString *tag = [NSString stringWithFormat:@"%u:%u", s, pf.metal_slot];
                NSNumber *slotNum = texSlotMap[tag];
                if (slotNum) baseSlot = slotNum.unsignedIntValue;
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &baseSlot, sizeof(baseSlot));
                break;
            }
            case NMB_PUSH_ARGUMENT_TABLE_ADDRESS: {
                const uint64_t argVa = plan->argTableBytes[s] > 0u ? (bo->gpu_va + argTableOff[s]) : zeroVa;
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &argVa, sizeof(argVa));
                break;
            }
            case NMB_PUSH_RENDER_TARGET_ARRAY_LENGTH:
            case NMB_PUSH_VIEWPORT_COUNT: {
                const uint32_t one = 1u;
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &one, sizeof(one));
                break;
            }
            case NMB_PUSH_VERTICES_PER_PRIMITIVE: {
                const uint32_t three = 3u;
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &three, sizeof(three));
                break;
            }
            case NMB_PUSH_FETCHED_COLOR: {
                uint64_t handle = 0u;
                if (pf.metal_slot < A840_MAX_COLOR_TARGETS && fetchedColorSlots[pf.metal_slot] > 0u) {
                    const uint32_t slotIdx = fetchedColorSlots[pf.metal_slot];
                    A840MTLTexture *tex = uniqueTextures[slotIdx - 1u];
                    const uint32_t word = nmb_encode_texture_word(static_cast<uint32_t>(tex.pixelFormat));
                    handle = static_cast<uint64_t>(slotIdx) | (static_cast<uint64_t>(word) << 32u);
                }
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &handle, sizeof(handle));
                break;
            }
            case NMB_PUSH_TAIL_WORD:
            case NMB_PUSH_INTEGER_CHANNELS:
            case NMB_PUSH_STAGE_IN_REGION:
            case NMB_PUSH_ZERO_U32:
            case NMB_PUSH_ZERO_U64:
            default:
                /* Buffer already zero-initialized. */
                break;
            }
        }

        if (maxEndBytes > 0u && plan->stages[s].constlen_vec4 > 0u) {
            if (maxEndBytes > kIdentityMaxPushBytes) maxEndBytes = static_cast<uint32_t>(kIdentityMaxPushBytes);
            uint32_t vec4s = (maxEndBytes + 15u) / 16u;
            if (vec4s > plan->stages[s].constlen_vec4) vec4s = plan->stages[s].constlen_vec4;
            outRes->pushVec4Count[s] = vec4s;
        }
    }
    return YES;
}

- (A840MTLCompiledPlan *)compileComputeFunction:(A840MTLFunction *)fn
                                      localSize:(MTLSize)localSize
                                      forDevice:(A840MTLDevice *)device
                                     descriptor:(A840RtComputePipelineDesc *)descriptor
{
    if (!fn || !device || !descriptor) return nil;
    if (localSize.width == 0u || localSize.height == 0u || localSize.depth == 0u ||
        localSize.width > UINT32_MAX || localSize.height > UINT32_MAX || localSize.depth > UINT32_MAX)
        return nil;
    const char *entry = fn.name.UTF8String;
    auto *lib = static_cast<NmbLibrary *>(fn.ownerLibrary.parsedNmbLibrary);
    if (!lib || !entry) return nil;

    std::vector<NmbFunctionConstant> ffiConsts;
    a840MarshalConstants([fn ownedConstants], ffiConsts);

    NmbComputePipelineDesc cdesc{};
    cdesc.local_size[0] = static_cast<uint32_t>(localSize.width);
    cdesc.local_size[1] = static_cast<uint32_t>(localSize.height);
    cdesc.local_size[2] = static_cast<uint32_t>(localSize.depth);
    cdesc.zeroed_threadgroup_memory = 1u;
    NmbSpirvResult *spv = nmb_compile_compute_spirv_v2(
        _translator, lib, entry,
        ffiConsts.empty() ? nullptr : ffiConsts.data(), ffiConsts.size(),
        &cdesc);
    if (!spv) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE cs %.32s %.64s", entry, nmb_last_error() ?: "err");
        a840diag::reportText(msg);
        return nil;
    }

    size_t spvBytes = 0u;
    const uint32_t *spvWords = nmb_spirv_words(spv, 0u, &spvBytes);
    const char *spvEntry = nmb_spirv_entry_point(spv, 0u);
    if (!spvWords || spvBytes == 0u || !spvEntry) {
        nmb_spirv_result_free(spv);
        return nil;
    }

    A840MTLCompiledPlan *plan = [A840MTLCompiledPlan new];
    plan->isCompute = YES;
    plan->rtDevice = device.runtime;
    plan->stageCount = 1u;
    plan->computeFunction = fn;

    const A840CompileTarget target = makeKaanapaliTarget();
    A840StageInput stageIn{};
    stageIn.abi_version = ADRENO_COMPILE_ABI_VERSION;
    stageIn.stage = A840_STAGE_COMPUTE;
    stageIn.spirv_words = spvWords;
    stageIn.spirv_bytes = spvBytes;
    stageIn.entry_point = spvEntry;
    stageIn.local_size[0] = cdesc.local_size[0];
    stageIn.local_size[1] = cdesc.local_size[1];
    stageIn.local_size[2] = cdesc.local_size[2];
    stageIn.sample_count = 1u;
    stageIn.flags = target.flags;

    char errBuf[256] = {};
    const auto cst = a840_compile_stage(&target, &stageIn, &plan->stages[0], errBuf, sizeof(errBuf));
    if (cst != A840_COMPILE_OK || !a840_ir3_get_stage_io_info(&plan->stages[0], &plan->ioInfo[0])) {
        nmb_spirv_result_free(spv);
        return nil;
    }
    plan->stages[0].constlen_vec4 = (plan->stages[0].constlen_vec4 + 3u) & ~3u;

    const size_t bcount = nmb_spirv_binding_count(spv);
    std::vector<NmbSpirvBinding> bindings(bcount);
    for (size_t i = 0u; i < bcount; ++i)
        (void)nmb_spirv_binding(spv, i, &bindings[i]);
    NmbSpirvPushConstantRange pushRange{};
    (void)nmb_spirv_push_constant_range(spv, 0u, &pushRange);
    if (![self captureIdentityReflection:spv intoPlan:plan stageCount:1u]) {
        nmb_spirv_result_free(spv);
        return nil;
    }
    nmb_spirv_result_free(spv);

    const auto bpst = a840_binding_plan_build(bindings.empty() ? nullptr : bindings.data(),
                                              bindings.size(), &plan->stages[0], 1u,
                                              &pushRange, &plan->bindingPlan);
    if (bpst != A840_BINDING_PLAN_OK) return nil;

    plan->codeBuffers[0] = uploadIsaBuffer(device.runtime, plan->stages[0].isa_words,
                                           plan->stages[0].isa_dwords);
    if (!plan->codeBuffers[0]) return nil;
    if (plan->stages[0].private_mem_bytes > 0u) {
        plan->privateBuffers[0] = allocPrivateScratch(device.runtime, plan->stages[0].private_mem_bytes);
        if (!plan->privateBuffers[0]) return nil;
    }

    std::memset(descriptor, 0, sizeof(*descriptor));
    descriptor->stage = &plan->stages[0];
    descriptor->code_buffer = plan->codeBuffers[0];
    descriptor->code_offset = 0u;
    descriptor->private_buffer = plan->privateBuffers[0];
    descriptor->private_offset = 0u;
    descriptor->bindless_tex = plan->ioInfo[0].bindless_tex != 0u;
    descriptor->bindless_samp = plan->ioInfo[0].bindless_samp != 0u;
    descriptor->bindless_uav = plan->ioInfo[0].bindless_uav != 0u;
    descriptor->bindless_ubo = plan->ioInfo[0].bindless_ubo != 0u;
    descriptor->local_id_regid = plan->ioInfo[0].cs_local_id_regid;
    descriptor->wg_id_regid = plan->ioInfo[0].cs_wg_id_regid;
    descriptor->linear_dispatch = true;
    plan->computeDesc = *descriptor;
    return plan;
}

- (id)prepareCompute:(id<MTLFunction>)function forDevice:(A840MTLDevice *)device
          descriptor:(A840RtComputePipelineDesc *)descriptor
{
    if (![function isKindOfClass:[A840MTLFunction class]] || !device || !descriptor) return nil;
    auto *fn = static_cast<A840MTLFunction *>(function);
    return [self compileComputeFunction:fn localSize:MTLSizeMake(1, 1, 1)
                              forDevice:device descriptor:descriptor];
}

- (id)prepareComputeVariantForPlan:(id)planObj localSize:(MTLSize)localSize
                         forDevice:(A840MTLDevice *)device
                        descriptor:(A840RtComputePipelineDesc *)descriptor
{
    if (![planObj isKindOfClass:[A840MTLCompiledPlan class]] || !device || !descriptor) return nil;
    auto *base = static_cast<A840MTLCompiledPlan *>(planObj);
    if (!base->isCompute || !base->computeFunction) return nil;
    if (base->stages[0].local_size[0] == localSize.width &&
        base->stages[0].local_size[1] == localSize.height &&
        base->stages[0].local_size[2] == localSize.depth) {
        *descriptor = base->computeDesc;
        return base;
    }
    NSString *key = [NSString stringWithFormat:@"%lu:%lu:%lu",
                     static_cast<unsigned long>(localSize.width),
                     static_cast<unsigned long>(localSize.height),
                     static_cast<unsigned long>(localSize.depth)];
    A840MTLCompiledPlan *cached = base->computeVariants[key];
    if (cached) {
        *descriptor = cached->computeDesc;
        return cached;
    }
    A840MTLCompiledPlan *variant = [self compileComputeFunction:base->computeFunction localSize:localSize
                                                      forDevice:device descriptor:descriptor];
    if (!variant) return nil;
    base->computeVariants[key] = variant;
    return variant;
}

- (id)prepareRender:(MTLRenderPipelineDescriptor *)pipeline forDevice:(A840MTLDevice *)device
         descriptor:(A840RtGraphicsProgramDesc *)descriptor
{
    if (!pipeline || !device || !descriptor) return nil;
    if (![pipeline.vertexFunction isKindOfClass:[A840MTLFunction class]]) {
        a840diag::reportText("A840PIPE rp-novs");
        return nil;
    }
    if (![pipeline.fragmentFunction isKindOfClass:[A840MTLFunction class]]) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE rp-nofs %.40s", pipeline.vertexFunction.name.UTF8String ?: "?");
        a840diag::reportText(msg);
        return nil;
    }

    auto *vsFn = static_cast<A840MTLFunction *>(pipeline.vertexFunction);
    auto *fsFn = static_cast<A840MTLFunction *>(pipeline.fragmentFunction);
    auto *vsLib = static_cast<NmbLibrary *>(vsFn.ownerLibrary.parsedNmbLibrary);
    auto *fsLib = static_cast<NmbLibrary *>(fsFn.ownerLibrary.parsedNmbLibrary);
    const char *vsName = vsFn.name.UTF8String;
    const char *fsName = fsFn.name.UTF8String;
    if (!vsLib || !fsLib || !vsName || !fsName) return nil;

    NmbRenderPipelineDesc rdesc{};
    rdesc.sample_count = pipeline.rasterSampleCount ? static_cast<uint32_t>(pipeline.rasterSampleCount) : 1u;
    rdesc.depth_mtl_format = static_cast<uint32_t>(pipeline.depthAttachmentPixelFormat);
    rdesc.stencil_mtl_format = static_cast<uint32_t>(pipeline.stencilAttachmentPixelFormat);
    uint32_t colorMask = 0u;
    for (uint32_t i = 0u; i < NMB_MAX_COLORS; ++i) {
        const uint32_t fmt = static_cast<uint32_t>(pipeline.colorAttachments[i].pixelFormat);
        rdesc.color_mtl_formats[i] = fmt;
        if (fmt != 0u) {
            rdesc.color_attachment_count = i + 1u;
            colorMask |= (1u << i);
        }
    }
    if (rdesc.color_attachment_count == 0u &&
        rdesc.depth_mtl_format == 0u &&
        rdesc.stencil_mtl_format == 0u) {
        rdesc.color_attachment_count = 1u;
        rdesc.color_mtl_formats[0] = static_cast<uint32_t>(MTLPixelFormatBGRA8Unorm);
        colorMask = 1u;
    }

    if (MTLVertexDescriptor *vd = pipeline.vertexDescriptor) {
        uint32_t attrCount = 0u;
        uint32_t layoutUsedMask = 0u;
        for (uint32_t i = 0u; i < NMB_MAX_VERTEX_ATTRIBUTES; ++i) {
            MTLVertexAttributeDescriptor *a = vd.attributes[i];
            if (!a || a.format == MTLVertexFormatInvalid) continue;
            if (a.bufferIndex >= NMB_MAX_VERTEX_LAYOUTS) continue;
            NmbVertexAttribute &dst = rdesc.vertex_attributes[attrCount++];
            dst.attribute_index = i;
            dst.buffer_index = static_cast<uint32_t>(a.bufferIndex);
            dst.offset = static_cast<uint32_t>(a.offset);
            dst.mtl_vertex_format = static_cast<uint32_t>(a.format);
            layoutUsedMask |= (1u << a.bufferIndex);
        }
        rdesc.vertex_attribute_count = attrCount;

        uint32_t layoutCount = 0u;
        for (uint32_t i = 0u; i < NMB_MAX_VERTEX_LAYOUTS; ++i) {
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
            const BOOL referenced = (layoutUsedMask & (1u << i)) != 0u;
            if (!l || (!referenced && l.stride == 0u)) continue;
            NmbVertexLayout &dst = rdesc.vertex_layouts[layoutCount++];
            dst.buffer_index = i;
            dst.stride = (l.stride == NSUIntegerMax) ? UINT32_MAX : static_cast<uint32_t>(l.stride);
            dst.step_function = static_cast<uint32_t>(l.stepFunction);
            dst.step_rate = static_cast<uint32_t>(l.stepRate ? l.stepRate : 1u);
        }
        rdesc.vertex_layout_count = layoutCount;
    }

    std::vector<NmbFunctionConstant> vsConsts, fsConsts;
    a840MarshalConstants([vsFn ownedConstants], vsConsts);
    a840MarshalConstants([fsFn ownedConstants], fsConsts);

    NmbSpirvResult *spv = nmb_compile_render_spirv_v2(
        _translator,
        vsLib, vsName, vsConsts.empty() ? nullptr : vsConsts.data(), vsConsts.size(),
        fsLib, fsName, fsConsts.empty() ? nullptr : fsConsts.data(), fsConsts.size(),
        &rdesc);
    if (!spv || nmb_spirv_stage_count(spv) < 2u) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE rp %.18s/%.18s f=%u,%u d=%u %.38s",
                 vsName, fsName, rdesc.color_mtl_formats[0], rdesc.color_mtl_formats[1],
                 rdesc.depth_mtl_format, nmb_last_error() ?: "err");
        a840diag::reportText(msg);
        nmb_spirv_result_free(spv);
        return nil;
    }

    A840MTLCompiledPlan *plan = [A840MTLCompiledPlan new];
    plan->isCompute = NO;
    plan->rtDevice = device.runtime;
    plan->stageCount = 2u;

    const A840CompileTarget target = makeKaanapaliTarget();
    const uint32_t stageKinds[2] = {A840_STAGE_VERTEX, A840_STAGE_FRAGMENT};
    for (uint32_t s = 0u; s < 2u; ++s) {
        size_t spvBytes = 0u;
        const uint32_t *spvWords = nmb_spirv_words(spv, s, &spvBytes);
        const char *spvEntry = nmb_spirv_entry_point(spv, s);
        if (!spvWords || spvBytes == 0u || !spvEntry) {
            nmb_spirv_result_free(spv);
            return nil;
        }
        A840StageInput stageIn{};
        stageIn.abi_version = ADRENO_COMPILE_ABI_VERSION;
        stageIn.stage = stageKinds[s];
        stageIn.spirv_words = spvWords;
        stageIn.spirv_bytes = spvBytes;
        stageIn.entry_point = spvEntry;
        stageIn.color_rt_mask = (s == 1u) ? colorMask : 0u;
        stageIn.sample_count = rdesc.sample_count;
        stageIn.flags = target.flags;

        char errBuf[256] = {};
        const auto cst = a840_compile_stage(&target, &stageIn, &plan->stages[s], errBuf, sizeof(errBuf));
        if (cst != A840_COMPILE_OK || !a840_ir3_get_stage_io_info(&plan->stages[s], &plan->ioInfo[s])) {
            char msg[128] = {};
            snprintf(msg, sizeof(msg), "A840PIPE ir3 s=%u %.18s/%.18s st=%d %.36s",
                     s, vsName, fsName, (int)cst, errBuf[0] ? errBuf : "io");
            a840diag::reportText(msg);
            nmb_spirv_result_free(spv);
            return nil;
        }
        plan->stages[s].constlen_vec4 = (plan->stages[s].constlen_vec4 + 3u) & ~3u;
    }

    const size_t bcount = nmb_spirv_binding_count(spv);
    std::vector<NmbSpirvBinding> bindings(bcount);
    for (size_t i = 0u; i < bcount; ++i)
        (void)nmb_spirv_binding(spv, i, &bindings[i]);
    NmbSpirvPushConstantRange pushRanges[2]{};
    (void)nmb_spirv_push_constant_range(spv, 0u, &pushRanges[0]);
    (void)nmb_spirv_push_constant_range(spv, 1u, &pushRanges[1]);
    if (![self captureIdentityReflection:spv intoPlan:plan stageCount:2u]) {
        nmb_spirv_result_free(spv);
        return nil;
    }
    nmb_spirv_result_free(spv);

    const auto bpst = a840_binding_plan_build(bindings.empty() ? nullptr : bindings.data(),
                                              bindings.size(), plan->stages, 2u,
                                              pushRanges, &plan->bindingPlan);
    if (bpst != A840_BINDING_PLAN_OK) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE bp %.22s/%.22s st=%d", vsName, fsName, (int)bpst);
        a840diag::reportText(msg);
        return nil;
    }

    for (uint32_t s = 0u; s < 2u; ++s) {
        plan->codeBuffers[s] = uploadIsaBuffer(device.runtime, plan->stages[s].isa_words,
                                               plan->stages[s].isa_dwords);
        if (!plan->codeBuffers[s]) return nil;
        if (plan->stages[s].private_mem_bytes > 0u) {
            plan->privateBuffers[s] = allocPrivateScratch(device.runtime, plan->stages[s].private_mem_bytes);
            if (!plan->privateBuffers[s]) return nil;
        }
    }

    std::memset(descriptor, 0, sizeof(*descriptor));
    const A840Ir3StageIoInfo &vsIo = plan->ioInfo[0];
    const A840Ir3StageIoInfo &fsIo = plan->ioInfo[1];
    A840GraphicsProgramDesc &ed = descriptor->emit_desc;

    ed.vs.stage = &plan->stages[0];
    ed.vs.bindless_tex = vsIo.bindless_tex != 0u;
    ed.vs.bindless_samp = vsIo.bindless_samp != 0u;
    ed.vs.bindless_uav = vsIo.bindless_uav != 0u;
    ed.vs.bindless_ubo = vsIo.bindless_ubo != 0u;

    ed.fs.stage = &plan->stages[1];
    ed.fs.bindless_tex = fsIo.bindless_tex != 0u;
    ed.fs.bindless_samp = fsIo.bindless_samp != 0u;
    ed.fs.bindless_uav = fsIo.bindless_uav != 0u;
    ed.fs.bindless_ubo = fsIo.bindless_ubo != 0u;

    ed.vertex_id = vsIo.vs_vertex_id_regid;
    ed.instance_id = vsIo.vs_instance_id_regid;
    ed.primitive_id = vsIo.vs_primitive_id_regid;
    ed.view_id = vsIo.vs_view_id_regid;
    ed.position = vsIo.vs_pos_regid;
    ed.point_size = vsIo.vs_psize_regid;

    for (uint32_t i = 0u; i < vsIo.input_count && ed.vertex_input_count < A840_MAX_VERTEX_INPUTS; ++i) {
        if (vsIo.inputs[i].sysval || vsIo.inputs[i].compmask == 0u ||
            vsIo.inputs[i].regid >= A840_IR3_REGID_UNUSED) continue;
        ed.vertex_inputs[ed.vertex_input_count].regid = vsIo.inputs[i].regid;
        ed.vertex_inputs[ed.vertex_input_count].component_mask = vsIo.inputs[i].compmask;
        ++ed.vertex_input_count;
    }

    std::vector<const A840Ir3InputSlot *> baryInputs;
    for (uint32_t i = 0u; i < fsIo.input_count; ++i) {
        if (fsIo.inputs[i].bary && fsIo.inputs[i].compmask != 0u)
            baryInputs.push_back(&fsIo.inputs[i]);
    }
    std::sort(baryInputs.begin(), baryInputs.end(),
              [](const A840Ir3InputSlot *a, const A840Ir3InputSlot *b) { return a->inloc < b->inloc; });
    for (const A840Ir3InputSlot *fin : baryInputs) {
        if (ed.varying_count >= ADRENO_COMPILE_MAX_VARYINGS) break;
        uint8_t vsReg = A840_IR3_REGID_UNUSED;
        for (uint32_t j = 0u; j < vsIo.output_count; ++j) {
            if (vsIo.outputs[j].slot == fin->slot) {
                vsReg = vsIo.outputs[j].regid;
                break;
            }
        }
        uint8_t varMode = A840_VARYING_NORMAL;
        if (vsReg >= A840_IR3_REGID_UNUSED) {
            /* Match Mesa ir3_link_shaders (pack_vs_out=true -> regid(63,0)) + tu_pipeline.cc:690-718:
             * FS-only/unwritten slots (VARYING_SLOT_PNTC=25 gl_PointCoord, unwritten LAYER/VIEWPORT,
             * dead varyings) reserve VPC locations + replace/zero mode without emitting a bogus VS output. */
            vsReg = A840_REGID_UNUSED;
            varMode = (fin->slot == 25u) ? A840_VARYING_POINT_COORD : A840_VARYING_ZERO;
        }
        ed.varyings[ed.varying_count].vs_regid = vsReg;
        ed.varyings[ed.varying_count].component_mask = fin->compmask;
        ed.varyings[ed.varying_count].flat = fin->flat != 0u;
        ed.varyings[ed.varying_count].fs_inloc = fin->inloc;
        ed.varyings[ed.varying_count].mode = varMode;
        ed.varyings[ed.varying_count].explicit_inloc = true;
        ++ed.varying_count;
    }

    ed.fs_inputs.face = fsIo.fs_face_regid;
    ed.fs_inputs.sample_id = fsIo.fs_sample_id_regid;
    ed.fs_inputs.sample_mask = fsIo.fs_sample_mask_regid;
    ed.fs_inputs.center_rhw = fsIo.fs_center_rhw_regid;
    ed.fs_inputs.persp_pixel = fsIo.fs_persp_pixel_regid;
    ed.fs_inputs.linear_pixel = fsIo.fs_linear_pixel_regid;
    ed.fs_inputs.persp_centroid = fsIo.fs_persp_centroid_regid;
    ed.fs_inputs.linear_centroid = fsIo.fs_linear_centroid_regid;
    ed.fs_inputs.persp_sample = fsIo.fs_persp_sample_regid;
    ed.fs_inputs.linear_sample = fsIo.fs_linear_sample_regid;
    ed.fs_inputs.frag_coord = (fsIo.fs_frag_coord_regid <= A840_REGID_UNUSED - 4u)
                                  ? fsIo.fs_frag_coord_regid
                                  : A840_REGID_UNUSED;
    ed.fs_inputs.frag_coord_mask = (ed.fs_inputs.frag_coord < A840_REGID_UNUSED)
                                       ? (fsIo.fs_frag_coord_mask ? (fsIo.fs_frag_coord_mask & 0xfu) : 0xfu)
                                       : 0u;
    ed.fs_inputs.shading_rate = fsIo.fs_shading_rate_regid;
    ed.depth_output = fsIo.fs_depth_out_regid;
    ed.stencil_output = fsIo.fs_stencil_out_regid;
    ed.sample_mask_output = fsIo.fs_sample_mask_out_regid;
    ed.pixel_lod = fsIo.fs_pixel_lod != 0u;
    ed.full_quad = fsIo.fs_full_quad != 0u;
    ed.fs_thread128 = fsIo.fs_thread128 != 0u;
    for (uint32_t i = 0u; i < A840_MAX_COLOR_TARGETS; ++i) {
        const uint8_t reg = (fsIo.fs_colors[i].regid < A840_REGID_UNUSED)
                                ? fsIo.fs_colors[i].regid
                                : A840_REGID_UNUSED;
        ed.colors[i].regid = reg;
        ed.colors[i].component_mask = (reg < A840_REGID_UNUSED) ? (fsIo.fs_colors[i].compmask & 0xfu) : 0u;
        ed.colors[i].half_precision = fsIo.fs_colors[i].half_precision != 0u;
    }

    descriptor->vs_code_buffer = plan->codeBuffers[0];
    descriptor->vs_code_offset = 0u;
    descriptor->vs_private_buffer = plan->privateBuffers[0];
    descriptor->vs_private_offset = 0u;
    descriptor->fs_code_buffer = plan->codeBuffers[1];
    descriptor->fs_code_offset = 0u;
    descriptor->fs_private_buffer = plan->privateBuffers[1];
    descriptor->fs_private_offset = 0u;
    return plan;
}

- (A840RtBuffer *)buildDescriptorTableForPlan:(A840MTLCompiledPlan *)plan
                               stageBindings:(A840MTLBindings *const *)stageBindings
                                  stageKinds:(const uint32_t *)stageKinds
                                  stageCount:(uint32_t)stageCount
{
    const A840BindingPlan &bp = plan->bindingPlan;
    size_t tableBytes = alignUp(bp.total_table_bytes, 64u);
    size_t inlineSlabBytes = 0u;
    for (uint32_t s = 0u; s < stageCount; ++s) {
        A840MTLBindings *b = stageBindings[s];
        if (!b) continue;
        for (NSNumber *key in b.inlineBytes) {
            NSData *d = b.inlineBytes[key];
            if (d.length > 0u) inlineSlabBytes += alignUp(d.length, 64u);
        }
    }
    const size_t totalBytes = alignUp((tableBytes + inlineSlabBytes) ? (tableBytes + inlineSlabBytes) : 64u, 256u);
    A840RtBufferDesc desc{};
    desc.size_bytes = totalBytes;
    desc.alignment = 4096u;
    desc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE;
    A840RtBuffer *bo = nullptr;
    if (a840_rt_buffer_create(plan->rtDevice, &desc, &bo) != A840_RT_OK || !bo || !bo->cpu_ptr) {
        if (bo) a840_rt_buffer_release(bo);
        return nullptr;
    }
    auto *baseCpu = static_cast<uint8_t *>(bo->cpu_ptr);
    std::memset(baseCpu, 0, totalBytes);

    NSMutableDictionary<NSString *, NSNumber *> *inlineVaMap = [NSMutableDictionary new];
    NSMutableDictionary<NSString *, NSNumber *> *inlineLenMap = [NSMutableDictionary new];
    size_t cursor = tableBytes;
    for (uint32_t s = 0u; s < stageCount; ++s) {
        A840MTLBindings *b = stageBindings[s];
        if (!b) continue;
        for (NSNumber *key in b.inlineBytes) {
            NSData *d = b.inlineBytes[key];
            if (!d || d.length == 0u) continue;
            std::memcpy(baseCpu + cursor, d.bytes, d.length);
            NSString *tag = [NSString stringWithFormat:@"%u:%u", stageKinds[s], key.unsignedIntValue];
            inlineVaMap[tag] = @(bo->gpu_va + cursor);
            inlineLenMap[tag] = @(d.length);
            cursor += alignUp(d.length, 64u);
        }
    }

    for (uint32_t i = 0u; i < bp.metal_lookup_count; ++i) {
        const A840MetalBindingLocation &loc = bp.metal_lookup[i];
        A840MTLBindings *b = nullptr;
        for (uint32_t s = 0u; s < stageCount; ++s) {
            if (stageKinds[s] == loc.stage) {
                b = stageBindings[s];
                break;
            }
        }
        if (!b) continue;
        if (loc.table_offset_bytes + A840_BINDING_PLAN_SLOT_BYTES > tableBytes) continue;
        auto *dstWords = reinterpret_cast<uint32_t *>(baseCpu + loc.table_offset_bytes);
        const A840BindingLocation *bl = findBindingLocation(&bp, loc.set, loc.binding);
        const uint32_t dtype = bl ? bl->descriptor_type : kDescTypeStorageBuffer;

        if (loc.kind == A840_METAL_SLOT_BUFFER) {
            id<MTLBuffer> buf = b.buffers[@(loc.metal_index)];
            if ([buf isKindOfClass:[A840MTLBuffer class]]) {
                auto *nativeBuf = static_cast<A840MTLBuffer *>(buf).native;
                const uint64_t off = b.bufferOffsets[@(loc.metal_index)].unsignedLongLongValue;
                if (nativeBuf && off < nativeBuf->size_bytes) {
                    const uint64_t va = nativeBuf->gpu_va + off;
                    const uint32_t rem = static_cast<uint32_t>(nativeBuf->size_bytes - off);
                    if (isUboType(dtype)) a840_fdl_encode_ubo_view_a8xx(va, rem, dstWords);
                    else a840_fdl_encode_buffer_view_a8xx(va, rem, 4u, dstWords);
                }
            } else {
                NSString *tag = [NSString stringWithFormat:@"%u:%u", loc.stage, loc.metal_index];
                NSNumber *vaNum = inlineVaMap[tag];
                NSNumber *lenNum = inlineLenMap[tag];
                if (vaNum && lenNum) {
                    if (isUboType(dtype))
                        a840_fdl_encode_ubo_view_a8xx(vaNum.unsignedLongLongValue, lenNum.unsignedIntValue, dstWords);
                    else
                        a840_fdl_encode_buffer_view_a8xx(vaNum.unsignedLongLongValue, lenNum.unsignedIntValue, 4u, dstWords);
                }
            }
        } else if (loc.kind == A840_METAL_SLOT_TEXTURE) {
            id<MTLTexture> tex = b.textures[@(loc.metal_index)];
            if ([tex isKindOfClass:[A840MTLTexture class]]) {
                auto *nativeTex = static_cast<A840MTLTexture *>(tex).native;
                if (nativeTex) std::memcpy(dstWords, nativeTex->tex_const, sizeof(nativeTex->tex_const));
            }
        } else if (loc.kind == A840_METAL_SLOT_SAMPLER) {
            id<MTLSamplerState> samp = b.samplers[@(loc.metal_index)];
            if ([samp isKindOfClass:[A840MTLSamplerState class]]) {
                const uint32_t *words = static_cast<A840MTLSamplerState *>(samp).descriptorWords;
                if (words) std::memcpy(dstWords, words, A840_BINDING_PLAN_SAMPLER_BYTES);
            }
        }
    }

    [plan->transientBuffers addObject:[NSValue valueWithPointer:bo]];
    return bo;
}

- (const uint8_t *)resolveBoundBufferCpu:(A840MTLBindings *)bindings
                                  plan:(const A840BindingPlan *)plan
                                 stage:(uint32_t)stage
                                   set:(uint32_t)set
                               binding:(uint32_t)binding
                               outSize:(size_t *)outSize
{
    if (!bindings || !plan || !outSize) return nullptr;
    for (uint32_t i = 0u; i < plan->metal_lookup_count; ++i) {
        const A840MetalBindingLocation &loc = plan->metal_lookup[i];
        if (loc.stage != stage || loc.kind != A840_METAL_SLOT_BUFFER ||
            loc.set != set || loc.binding != binding)
            continue;
        id<MTLBuffer> buf = bindings.buffers[@(loc.metal_index)];
        if ([buf isKindOfClass:[A840MTLBuffer class]]) {
            auto *nativeBuf = static_cast<A840MTLBuffer *>(buf).native;
            const uint64_t off = bindings.bufferOffsets[@(loc.metal_index)].unsignedLongLongValue;
            if (nativeBuf && nativeBuf->cpu_ptr && off < nativeBuf->size_bytes) {
                *outSize = static_cast<size_t>(nativeBuf->size_bytes - off);
                return static_cast<const uint8_t *>(nativeBuf->cpu_ptr) + off;
            }
        }
        NSData *inlineData = bindings.inlineBytes[@(loc.metal_index)];
        if (inlineData && inlineData.length > 0u) {
            *outSize = inlineData.length;
            return static_cast<const uint8_t *>(inlineData.bytes);
        }
    }
    return nullptr;
}

- (A840RtStatus)bindCompute:(A840RtComputeEncoder *)encoder plan:(id)planObj
                    bindings:(A840MTLBindings *)bindings
{
    MTLSize defaultLocal = MTLSizeMake(1, 1, 1);
    if ([planObj isKindOfClass:[A840MTLCompiledPlan class]]) {
        auto *p = static_cast<A840MTLCompiledPlan *>(planObj);
        defaultLocal = MTLSizeMake(p->stages[0].local_size[0] ? p->stages[0].local_size[0] : 1u,
                                   p->stages[0].local_size[1] ? p->stages[0].local_size[1] : 1u,
                                   p->stages[0].local_size[2] ? p->stages[0].local_size[2] : 1u);
    }
    return [self bindCompute:encoder plan:planObj bindings:bindings
                    gridSize:MTLSizeMake(1, 1, 1) threadgroupSize:defaultLocal nonuniform:NO];
}

- (A840RtStatus)bindCompute:(A840RtComputeEncoder *)encoder plan:(id)planObj
                    bindings:(A840MTLBindings *)bindings
                    gridSize:(MTLSize)grid
             threadgroupSize:(MTLSize)local
                  nonuniform:(BOOL)nonuniform
{
    if (!encoder || ![planObj isKindOfClass:[A840MTLCompiledPlan class]]) return A840_RT_INVALID_ARG;
    auto *plan = static_cast<A840MTLCompiledPlan *>(planObj);
    if (!plan->isCompute) return A840_RT_INVALID_ARG;

    if ([self planUsesIdentityPush:plan]) {
        A840MTLBindings *stageBindings[1] = {bindings};
        A840IdentityBindState idRes{};
        if (![self buildIdentityBindStateForPlan:plan stageBindings:stageBindings stageCount:1u
                                    colorTargets:nullptr colorTargetCount:0u
                                        gridSize:grid threadgroupSize:local nonuniform:nonuniform
                                          outRes:&idRes]) {
            return A840_RT_OOM;
        }
        if (idRes.activeSetCount > 0u) {
            A840RtBuffer *setBos[ADRENO_COMPILE_MAX_BIND_SETS] = {};
            uint64_t setOffsets[ADRENO_COMPILE_MAX_BIND_SETS] = {};
            for (uint32_t s = 0u; s < idRes.activeSetCount; ++s) {
                if (idRes.setActive[s]) {
                    setBos[s] = idRes.descBo;
                    setOffsets[s] = idRes.setOffsets[s];
                }
            }
            const auto st = a840_rt_compute_set_bindless(encoder, idRes.activeSetCount, setBos, setOffsets);
            if (st != A840_RT_OK) return st;
        }
        const A840CompiledStage &stage = plan->stages[0];
        if (idRes.pushVec4Count[0] > 0u) {
            const auto st = a840_rt_compute_set_consts_direct(
                encoder, 0u, idRes.pushVec4Count[0], stage.constlen_vec4,
                reinterpret_cast<const uint32_t *>(idRes.pushBuf[0]));
            if (st != A840_RT_OK) return st;
        }
        const A840Ir3StageIoInfo &io = plan->ioInfo[0];
        if (stage.const_data_words && stage.const_data_dwords > 0u && io.imm_offset_vec4 != UINT32_MAX) {
            const uint32_t countVec4 = (stage.const_data_dwords + 3u) / 4u;
            if (io.imm_offset_vec4 + countVec4 <= stage.constlen_vec4) {
                std::vector<uint32_t> padded(countVec4 * 4u, 0u);
                std::memcpy(padded.data(), stage.const_data_words, stage.const_data_dwords * sizeof(uint32_t));
                const auto st = a840_rt_compute_set_consts_direct(encoder, io.imm_offset_vec4, countVec4,
                                                                  stage.constlen_vec4, padded.data());
                if (st != A840_RT_OK) return st;
            }
        }
        return A840_RT_OK;
    }

    const uint32_t stageKind = A840_STAGE_COMPUTE;
    A840MTLBindings *stageBindings[1] = {bindings};
    A840RtBuffer *descBo = [self buildDescriptorTableForPlan:plan stageBindings:stageBindings
                                                  stageKinds:&stageKind stageCount:1u];
    if (!descBo) return A840_RT_OOM;

    const A840BindingPlan &bp = plan->bindingPlan;
    A840RtBuffer *setBos[ADRENO_COMPILE_MAX_BIND_SETS] = {};
    uint64_t setOffsets[ADRENO_COMPILE_MAX_BIND_SETS] = {};
    uint32_t activeSets = 0u;
    for (uint32_t s = 0u; s < ADRENO_COMPILE_MAX_BIND_SETS; ++s) {
        if (bp.sets[s].table_bytes > 0u) {
            setBos[s] = descBo;
            setOffsets[s] = bp.sets[s].base_offset_bytes;
            activeSets = s + 1u;
        }
    }
    if (activeSets > 0u) {
        const auto st = a840_rt_compute_set_bindless(encoder, activeSets, setBos, setOffsets);
        if (st != A840_RT_OK) return st;
    }

    const A840CompiledStage &stage = plan->stages[0];
    const A840Ir3StageIoInfo &io = plan->ioInfo[0];
    if (stage.const_data_words && stage.const_data_dwords > 0u && io.imm_offset_vec4 != UINT32_MAX) {
        const uint32_t countVec4 = (stage.const_data_dwords + 3u) / 4u;
        if (io.imm_offset_vec4 + countVec4 <= stage.constlen_vec4) {
            std::vector<uint32_t> padded(countVec4 * 4u, 0u);
            std::memcpy(padded.data(), stage.const_data_words, stage.const_data_dwords * sizeof(uint32_t));
            const auto st = a840_rt_compute_set_consts_direct(encoder, io.imm_offset_vec4, countVec4,
                                                              stage.constlen_vec4, padded.data());
            if (st != A840_RT_OK) return st;
        }
    }

    const A840StageBindingPlan &sbp = bp.stages[0];
    for (uint32_t i = 0u; i < sbp.inline_ubo_count; ++i) {
        const A840InlineUbo &iu = sbp.inline_ubos[i];
        if (iu.source != A840_INLINE_UBO_BINDLESS || iu.copy.size_bytes == 0u) continue;
        size_t avail = 0u;
        const uint8_t *src = [self resolveBoundBufferCpu:bindings plan:&bp stage:A840_STAGE_COMPUTE
                                                     set:iu.set binding:iu.binding outSize:&avail];
        if (!src || iu.copy.src_offset_bytes >= avail) continue;
        const uint32_t dstVec4 = iu.copy.dst_offset_bytes / 16u;
        const uint32_t countVec4 = (iu.copy.size_bytes + 15u) / 16u;
        if (dstVec4 + countVec4 > stage.constlen_vec4) continue;
        std::vector<uint32_t> padded(countVec4 * 4u, 0u);
        const size_t toCopy = (avail - iu.copy.src_offset_bytes) < iu.copy.size_bytes
                                  ? (avail - iu.copy.src_offset_bytes)
                                  : iu.copy.size_bytes;
        std::memcpy(padded.data(), src + iu.copy.src_offset_bytes, toCopy);
        const auto st = a840_rt_compute_set_consts_direct(encoder, dstVec4, countVec4,
                                                          stage.constlen_vec4, padded.data());
        if (st != A840_RT_OK) return st;
    }
    return A840_RT_OK;
}

- (A840RtStatus)bindRender:(A840RtRenderEncoder *)encoder plan:(id)planObj
            vertexBindings:(A840MTLBindings *)vertex fragmentBindings:(A840MTLBindings *)fragment
{
    return [self bindRender:encoder plan:planObj vertexBindings:vertex fragmentBindings:fragment
               colorTargets:nullptr colorTargetCount:0u];
}

- (A840RtStatus)bindRender:(A840RtRenderEncoder *)encoder plan:(id)planObj
            vertexBindings:(A840MTLBindings *)vertex fragmentBindings:(A840MTLBindings *)fragment
              colorTargets:(id<MTLTexture> const __unsafe_unretained *)colorTargets
          colorTargetCount:(uint32_t)colorTargetCount
{
    if (!encoder || ![planObj isKindOfClass:[A840MTLCompiledPlan class]]) return A840_RT_INVALID_ARG;
    auto *plan = static_cast<A840MTLCompiledPlan *>(planObj);
    if (plan->isCompute || plan->stageCount == 0u || plan->stageCount > 2u) return A840_RT_INVALID_ARG;

    const uint32_t stageKinds[2] = {A840_STAGE_VERTEX, A840_STAGE_FRAGMENT};
    A840MTLBindings *stageBindings[2] = {vertex, fragment};

    uint32_t sdsWords[512];
    A840CmdStream sdsCs{};
    if (a840_cs_init(&sdsCs, sdsWords, 512u, nullptr, 0u) != A840_EMIT_OK)
        return A840_RT_STATE_ERROR;

    if ([self planUsesIdentityPush:plan]) {
        A840IdentityBindState idRes{};
        if (![self buildIdentityBindStateForPlan:plan stageBindings:stageBindings stageCount:plan->stageCount
                                    colorTargets:colorTargets colorTargetCount:colorTargetCount
                                        gridSize:MTLSizeMake(1, 1, 1) threadgroupSize:MTLSizeMake(1, 1, 1)
                                      nonuniform:NO outRes:&idRes]) {
            return A840_RT_OOM;
        }
        if (idRes.activeSetCount > 0u) {
            uint64_t setVas[ADRENO_COMPILE_MAX_BIND_SETS] = {};
            for (uint32_t s = 0u; s < idRes.activeSetCount; ++s) {
                if (idRes.setActive[s]) setVas[s] = idRes.descBo->gpu_va + idRes.setOffsets[s];
            }
            const auto est = a840_gfx_sds_emit_bindless(&sdsCs, idRes.activeSetCount, setVas);
            if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
        }
        for (uint32_t s = 0u; s < 2u; ++s) {
            const A840CompiledStage &stage = plan->stages[s];
            if (idRes.pushVec4Count[s] > 0u && stage.constlen_vec4 > 0u) {
                A840GfxConstUpload up{};
                up.stage = stageKinds[s];
                up.dst_vec4 = 0u;
                up.count_vec4 = idRes.pushVec4Count[s];
                up.constlen_vec4 = stage.constlen_vec4;
                up.words = reinterpret_cast<const uint32_t *>(idRes.pushBuf[s]);
                const auto est = a840_gfx_sds_emit_consts(&sdsCs, &up);
                if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
            }
            const A840Ir3StageIoInfo &io = plan->ioInfo[s];
            if (stage.const_data_words && stage.const_data_dwords > 0u && io.imm_offset_vec4 != UINT32_MAX) {
                const uint32_t countVec4 = (stage.const_data_dwords + 3u) / 4u;
                if (io.imm_offset_vec4 + countVec4 <= stage.constlen_vec4) {
                    std::vector<uint32_t> padded(countVec4 * 4u, 0u);
                    std::memcpy(padded.data(), stage.const_data_words, stage.const_data_dwords * sizeof(uint32_t));
                    A840GfxConstUpload up{};
                    up.stage = stageKinds[s];
                    up.dst_vec4 = io.imm_offset_vec4;
                    up.count_vec4 = countVec4;
                    up.constlen_vec4 = stage.constlen_vec4;
                    up.words = padded.data();
                    const auto est = a840_gfx_sds_emit_consts(&sdsCs, &up);
                    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
                }
            }
        }
        if (sdsCs.used_dwords == 0u) return A840_RT_OK;
        A840RtBufferDesc sdsBoDesc{};
        sdsBoDesc.size_bytes = alignUp(sdsCs.used_dwords * sizeof(uint32_t), 256u);
        sdsBoDesc.alignment = 4096u;
        sdsBoDesc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE;
        A840RtBuffer *sdsBo = nullptr;
        if (a840_rt_buffer_create(plan->rtDevice, &sdsBoDesc, &sdsBo) != A840_RT_OK || !sdsBo || !sdsBo->cpu_ptr) {
            if (sdsBo) a840_rt_buffer_release(sdsBo);
            return A840_RT_OOM;
        }
        std::memcpy(sdsBo->cpu_ptr, sdsWords, sdsCs.used_dwords * sizeof(uint32_t));
        [plan->transientBuffers addObject:[NSValue valueWithPointer:sdsBo]];

        A840RtSdsGroupDesc grp{};
        grp.group_id = 0u;
        grp.enable_mask = A840_SDS_SYSMEM | A840_SDS_GMEM;
        grp.state_dwords = static_cast<uint32_t>(sdsCs.used_dwords);
        grp.state_buffer = sdsBo;
        grp.state_offset = 0u;
        return a840_rt_render_set_sds(encoder, &grp, 1u);
    }

    A840RtBuffer *descBo = [self buildDescriptorTableForPlan:plan stageBindings:stageBindings
                                                  stageKinds:stageKinds stageCount:2u];
    if (!descBo) return A840_RT_OOM;

    const A840BindingPlan &bp = plan->bindingPlan;
    uint64_t setVas[ADRENO_COMPILE_MAX_BIND_SETS] = {};
    uint32_t activeSets = 0u;
    for (uint32_t s = 0u; s < ADRENO_COMPILE_MAX_BIND_SETS; ++s) {
        if (bp.sets[s].table_bytes > 0u) {
            setVas[s] = descBo->gpu_va + bp.sets[s].base_offset_bytes;
            activeSets = s + 1u;
        }
    }
    if (activeSets > 0u) {
        const auto est = a840_gfx_sds_emit_bindless(&sdsCs, activeSets, setVas);
        if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    }

    for (uint32_t s = 0u; s < 2u; ++s) {
        const A840CompiledStage &stage = plan->stages[s];
        const A840Ir3StageIoInfo &io = plan->ioInfo[s];
        if (stage.const_data_words && stage.const_data_dwords > 0u && io.imm_offset_vec4 != UINT32_MAX) {
            const uint32_t countVec4 = (stage.const_data_dwords + 3u) / 4u;
            if (io.imm_offset_vec4 + countVec4 <= stage.constlen_vec4) {
                std::vector<uint32_t> padded(countVec4 * 4u, 0u);
                std::memcpy(padded.data(), stage.const_data_words, stage.const_data_dwords * sizeof(uint32_t));
                A840GfxConstUpload up{};
                up.stage = stageKinds[s];
                up.dst_vec4 = io.imm_offset_vec4;
                up.count_vec4 = countVec4;
                up.constlen_vec4 = stage.constlen_vec4;
                up.words = padded.data();
                const auto est = a840_gfx_sds_emit_consts(&sdsCs, &up);
                if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
            }
        }
        const A840StageBindingPlan &sbp = bp.stages[s];
        for (uint32_t i = 0u; i < sbp.inline_ubo_count; ++i) {
            const A840InlineUbo &iu = sbp.inline_ubos[i];
            if (iu.source != A840_INLINE_UBO_BINDLESS || iu.copy.size_bytes == 0u) continue;
            size_t avail = 0u;
            const uint8_t *src = [self resolveBoundBufferCpu:stageBindings[s] plan:&bp stage:stageKinds[s]
                                                         set:iu.set binding:iu.binding outSize:&avail];
            if (!src || iu.copy.src_offset_bytes >= avail) continue;
            const uint32_t dstVec4 = iu.copy.dst_offset_bytes / 16u;
            const uint32_t countVec4 = (iu.copy.size_bytes + 15u) / 16u;
            if (dstVec4 + countVec4 > stage.constlen_vec4) continue;
            std::vector<uint32_t> padded(countVec4 * 4u, 0u);
            const size_t toCopy = (avail - iu.copy.src_offset_bytes) < iu.copy.size_bytes
                                      ? (avail - iu.copy.src_offset_bytes)
                                      : iu.copy.size_bytes;
            std::memcpy(padded.data(), src + iu.copy.src_offset_bytes, toCopy);
            A840GfxConstUpload up{};
            up.stage = stageKinds[s];
            up.dst_vec4 = dstVec4;
            up.count_vec4 = countVec4;
            up.constlen_vec4 = stage.constlen_vec4;
            up.words = padded.data();
            const auto est = a840_gfx_sds_emit_consts(&sdsCs, &up);
            if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
        }
    }

    if (sdsCs.used_dwords == 0u) return A840_RT_OK;
    A840RtBufferDesc sdsBoDesc{};
    sdsBoDesc.size_bytes = alignUp(sdsCs.used_dwords * sizeof(uint32_t), 256u);
    sdsBoDesc.alignment = 4096u;
    sdsBoDesc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE;
    A840RtBuffer *sdsBo = nullptr;
    if (a840_rt_buffer_create(plan->rtDevice, &sdsBoDesc, &sdsBo) != A840_RT_OK || !sdsBo || !sdsBo->cpu_ptr) {
        if (sdsBo) a840_rt_buffer_release(sdsBo);
        return A840_RT_OOM;
    }
    std::memcpy(sdsBo->cpu_ptr, sdsWords, sdsCs.used_dwords * sizeof(uint32_t));
    [plan->transientBuffers addObject:[NSValue valueWithPointer:sdsBo]];

    A840RtSdsGroupDesc grp{};
    grp.group_id = 0u;
    grp.enable_mask = A840_SDS_SYSMEM | A840_SDS_GMEM;
    grp.state_dwords = static_cast<uint32_t>(sdsCs.used_dwords);
    grp.state_buffer = sdsBo;
    grp.state_offset = 0u;
    return a840_rt_render_set_sds(encoder, &grp, 1u);
}
@end

#pragma clang diagnostic pop

id<MTLDevice> a840_metal_create_direct_device(NSError **error)
{
    A840IokitTransport *transport = [[A840IokitTransport alloc] initWithDefaultAcceleratorError:error];
    if (!transport) return nil;
    A840MTLNativePipelineProvider *provider = [[A840MTLNativePipelineProvider alloc] init];
    if (!provider) {
        if (error) *error = a840_metal_error(A840_RT_OOM, @"Failed to create native pipeline provider");
        return nil;
    }
    return [[A840MTLDevice alloc] initWithTransport:transport
                                   pipelineProvider:provider error:error];
}

id<MTLDevice> a840_metal_create_mock_device(A840BackendTransport backend, NSError **error)
{
    A840IokitTransport *transport = [[A840IokitTransport alloc] initWithBackend:backend];
    if (!transport) {
        if (error) *error = a840_metal_error(A840_RT_INVALID_ARG, @"Invalid mock backend");
        return nil;
    }
    A840MTLNativePipelineProvider *provider = [[A840MTLNativePipelineProvider alloc] init];
    if (!provider) {
        if (error) *error = a840_metal_error(A840_RT_OOM, @"Failed to create native pipeline provider");
        return nil;
    }
    return [[A840MTLDevice alloc] initWithTransport:transport
                                   pipelineProvider:provider error:error];
}
