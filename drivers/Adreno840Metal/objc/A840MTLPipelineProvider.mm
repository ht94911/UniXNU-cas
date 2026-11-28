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

#import <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cstdio>
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

/* Minimal valid SPIR-V 1.0 empty Fragment shader (`void main() {}`) for
 * vertex-only depth/stencil-only Metal render pipelines (fragmentFunction == nil). */
static const uint32_t kEmptyFragmentSpirv[] = {
    0x07230203u, 0x00010000u, 0x00000000u, 6u, 0u,
    (2u << 16) | 17u, 1u,
    (3u << 16) | 14u, 0u, 1u,
    (5u << 16) | 15u, 4u, 4u, 0x6e69616du, 0u,
    (3u << 16) | 16u, 4u, 7u,
    (2u << 16) | 19u, 1u,
    (3u << 16) | 33u, 2u, 1u,
    (5u << 16) | 54u, 1u, 4u, 0u, 2u,
    (2u << 16) | 248u, 5u,
    (1u << 16) | 253u,
    (1u << 16) | 56u,
};

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
    A840RtBuffer *descBo = nullptr; /* CB-owned transient slab (no release by caller). */
    uint64_t descOffset = 0u;
    uint64_t setOffsets[ADRENO_COMPILE_MAX_BIND_SETS] = {};
    bool setActive[ADRENO_COMPILE_MAX_BIND_SETS] = {};
    uint32_t activeSetCount = 0u;
    uint8_t pushBuf[2][kIdentityMaxPushBytes] = {};
    uint32_t pushVec4Count[2] = {};
    uint64_t vertexInlineVa[32] = {};
    uint32_t vertexInlineLen[32] = {};
};

static void encodeDefaultConstexprSampler(uint32_t outWords[A840_SAMP_CONST_DWORDS])
{
    const A840SampDescInput in{
        A840_FILTER_LINEAR, A840_FILTER_LINEAR, A840_MIP_NONE,
        A840_ADDRESS_CLAMP_TO_EDGE, A840_ADDRESS_CLAMP_TO_EDGE, A840_ADDRESS_CLAMP_TO_EDGE,
        1u, 0.0f, 4095.0f / 256.0f, 0.0f, A840_COMPARE_DISABLED, 0u, 0u,
    };
    if (a840_encode_samp_const(&in, outWords) != A840_EMIT_OK) {
        std::memset(outWords, 0, A840_SAMP_CONST_DWORDS * sizeof(uint32_t));
    } else {
        outWords[2] |= 1u; /* FASTBORDERCOLOREN | A6XX_BORDER_COLOR_0_0_0_0 */
    }
}

static uint32_t clampConstexprAnisoPow2(uint32_t raw)
{
    if (raw >= 16u) return 16u;
    if (raw >= 8u) return 8u;
    if (raw >= 4u) return 4u;
    if (raw >= 2u) return 2u;
    return 1u;
}

static void encodeReflectedConstexprSampler(const NmbSpirvConstexprSampler &cs,
                                            uint32_t outWords[A840_SAMP_CONST_DWORDS])
{
    const float kMaxLod = 4095.0f / 256.0f;
    float minLod = (cs.min_lod >= 0.0f) ? cs.min_lod : 0.0f;
    if (minLod > kMaxLod) minLod = kMaxLod;
    float maxLod = (cs.max_lod >= minLod) ? cs.max_lod : minLod;
    if (maxLod > kMaxLod) maxLod = kMaxLod;
    float lodBias = cs.lod_bias;
    if (!(lodBias >= -16.0f)) lodBias = -16.0f;
    if (lodBias > kMaxLod) lodBias = kMaxLod;

    /* VkBorderColor raw 0..5 -> Mesa bcolor_builtin index + A8XX FASTBORDERCOLOR. */
    uint32_t bcolorIdx = 0u;
    uint32_t fastBorder = 0u;
    switch (cs.border_color) {
    case 0u: bcolorIdx = 0u; fastBorder = 0u; break; /* FLOAT_TRANSPARENT_BLACK */
    case 1u: bcolorIdx = 1u; fastBorder = 0u; break; /* INT_TRANSPARENT_BLACK */
    case 2u: bcolorIdx = 2u; fastBorder = 1u; break; /* FLOAT_OPAQUE_BLACK */
    case 3u: bcolorIdx = 3u; fastBorder = 1u; break; /* INT_OPAQUE_BLACK */
    case 4u: bcolorIdx = 4u; fastBorder = 3u; break; /* FLOAT_OPAQUE_WHITE */
    case 5u: bcolorIdx = 5u; fastBorder = 3u; break; /* INT_OPAQUE_WHITE */
    default: bcolorIdx = 0u; fastBorder = 0u; break;
    }

    const A840SampDescInput in{
        cs.min_filter ? A840_FILTER_LINEAR : A840_FILTER_NEAREST,
        cs.mag_filter ? A840_FILTER_LINEAR : A840_FILTER_NEAREST,
        cs.mip_filter <= A840_MIP_LINEAR ? cs.mip_filter : A840_MIP_NONE,
        cs.address_u <= A840_ADDRESS_MIRROR_CLAMP ? cs.address_u : A840_ADDRESS_CLAMP_TO_EDGE,
        cs.address_v <= A840_ADDRESS_MIRROR_CLAMP ? cs.address_v : A840_ADDRESS_CLAMP_TO_EDGE,
        cs.address_w <= A840_ADDRESS_MIRROR_CLAMP ? cs.address_w : A840_ADDRESS_CLAMP_TO_EDGE,
        clampConstexprAnisoPow2(cs.max_aniso),
        minLod,
        maxLod,
        lodBias,
        cs.compare_op <= A840_COMPARE_DISABLED ? cs.compare_op : A840_COMPARE_DISABLED,
        bcolorIdx,
        /* metal2vulkan-v2 (convert.rs:127,151) never uses Vulkan unnormalized samplers;
         * pixel-coordinate samplers are normalized in SPIR-V. Keep hardware unnorm=0. */
        0u,
    };
    if (a840_encode_samp_const(&in, outWords) != A840_EMIT_OK) {
        encodeDefaultConstexprSampler(outWords);
    } else {
        outWords[2] |= 1u | ((fastBorder & 3u) << 1u);
    }
}

static bool stagePushWindow(const A840CompiledStage &stage,
                            const A840Ir3StageIoInfo &io,
                            const uint8_t pushBuf[kIdentityMaxPushBytes],
                            uint32_t fallbackVec4Count,
                            uint32_t *outDstVec4,
                            uint32_t *outCountVec4,
                            uint32_t outWords[kIdentityMaxPushBytes / 4u])
{
    if (stage.constlen_vec4 == 0u) return false;
    uint32_t baseBytes = 0u;
    uint32_t dstVec4 = 0u;
    uint32_t countVec4 = 0u;
    if (io.push_const_size_vec4 > 0u && io.push_const_dst_vec4 < stage.constlen_vec4) {
        baseBytes = io.push_const_base_bytes;
        dstVec4 = io.push_const_dst_vec4;
        const uint32_t avail = stage.constlen_vec4 - dstVec4;
        countVec4 = io.push_const_size_vec4 < avail ? io.push_const_size_vec4 : avail;
    } else if (fallbackVec4Count > 0u) {
        countVec4 = fallbackVec4Count < stage.constlen_vec4 ? fallbackVec4Count : stage.constlen_vec4;
    }
    if (countVec4 == 0u) return false;
    const uint32_t maxVec4 = static_cast<uint32_t>(kIdentityMaxPushBytes / 16u);
    if (countVec4 > maxVec4) countVec4 = maxVec4;
    std::memset(outWords, 0, static_cast<size_t>(countVec4) * 16u);
    if (baseBytes < kIdentityMaxPushBytes) {
        const size_t availBytes = kIdentityMaxPushBytes - baseBytes;
        const size_t copyBytes = static_cast<size_t>(countVec4) * 16u;
        std::memcpy(outWords, pushBuf + baseBytes, availBytes < copyBytes ? availBytes : copyBytes);
    }
    *outDstVec4 = dstVec4;
    *outCountVec4 = countVec4;
    return true;
}

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

static NSData *a840ExtractMtlbBitcode(NSData *libData, const char *wantedName)
{
    if (!libData || !wantedName || !wantedName[0]) return nil;
    const uint8_t *raw = static_cast<const uint8_t *>(libData.bytes);
    const size_t rawLen = libData.length;
    size_t airOff = 0u, airLen = 0u;
    if (!nmb_extract_air_slice(raw, rawLen, &airOff, &airLen)) return nil;
    if (airLen < 88u || airOff > rawLen || airLen > rawLen - airOff) return nil;
    const uint8_t *file = raw + airOff;
    if (std::memcmp(file, "MTLB", 4u) != 0) return nil;

    uint64_t listAt = 0u, bitcodeAt = 0u, bitcodeSize = 0u;
    std::memcpy(&listAt, file + 24u, 8u);
    std::memcpy(&bitcodeAt, file + 72u, 8u);
    std::memcpy(&bitcodeSize, file + 80u, 8u);
    if (listAt > airLen - 4u || bitcodeAt > airLen || bitcodeSize > airLen - bitcodeAt) return nil;

    uint32_t count = 0u;
    std::memcpy(&count, file + listAt, 4u);
    size_t at = static_cast<size_t>(listAt) + 4u;
    for (uint32_t i = 0u; i < count; ++i) {
        if (at > airLen - 4u) return nil;
        uint32_t entrySize = 0u;
        std::memcpy(&entrySize, file + at, 4u);
        if (entrySize < 8u || entrySize > airLen - at) return nil;
        const size_t end = at + entrySize;
        std::string entryName;
        uint64_t mdSize = 0u, bcOff = 0u;
        bool hasSize = false, hasOff = false;
        for (size_t t = at + 4u; t + 4u <= end && std::memcmp(file + t, "ENDT", 4u) != 0;) {
            if (t + 6u > end) return nil;
            uint16_t n = 0u;
            std::memcpy(&n, file + t + 4u, 2u);
            const uint8_t *v = file + t + 6u;
            if (n > end - (t + 6u)) return nil;
            if (std::memcmp(file + t, "NAME", 4u) == 0) {
                entryName.assign(reinterpret_cast<const char *>(v), strnlen(reinterpret_cast<const char *>(v), n));
            } else if (std::memcmp(file + t, "MDSZ", 4u) == 0 && n >= 8u) {
                std::memcpy(&mdSize, v, 8u);
                hasSize = true;
            } else if (std::memcmp(file + t, "OFFT", 4u) == 0 && n >= 24u) {
                std::memcpy(&bcOff, v + 16u, 8u);
                hasOff = true;
            }
            t += 6u + n;
        }
        if (entryName == wantedName) {
            if (!hasSize || !hasOff || bcOff > bitcodeSize || mdSize > bitcodeSize - bcOff) return nil;
            return [NSData dataWithBytes:file + bitcodeAt + bcOff length:static_cast<NSUInteger>(mdSize)];
        }
        at = end;
    }
    return nil;
}

@implementation A840MTLFunction {
    NSString *_sourceName;
    NSString *_specializedName;
    MTLFunctionType _type;
    BOOL _specialized;
    std::vector<A840OwnedConstant> _constants;
    uint8_t _bitCodeHash[32];
    BOOL _bitCodeHashReady;
}
@synthesize label = _label;

- (instancetype)initWithLibrary:(A840MTLLibrary *)library name:(NSString *)name
                   functionType:(MTLFunctionType)functionType
{
    return [self initWithLibrary:library sourceName:name specializedName:nil
                    functionType:functionType constantValues:nil specialized:NO error:nil];
}

- (instancetype)initWithLibrary:(A840MTLLibrary *)library name:(NSString *)name
                   functionType:(MTLFunctionType)functionType
                 constantValues:(MTLFunctionConstantValues *)constantValues
                          error:(NSError **)error
{
    return [self initWithLibrary:library sourceName:name specializedName:nil
                    functionType:functionType constantValues:constantValues
                     specialized:(constantValues != nil) error:error];
}

- (instancetype)initWithLibrary:(A840MTLLibrary *)library
                     sourceName:(NSString *)sourceName
                specializedName:(NSString *)specializedName
                   functionType:(MTLFunctionType)functionType
                 constantValues:(MTLFunctionConstantValues *)constantValues
                    specialized:(BOOL)specialized
                          error:(NSError **)error
{
    if (!(self = [super init])) return nil;
    if (constantValues && !a840ExtractConstantValues(constantValues, _constants, error)) {
        return nil;
    }
    _ownerLibrary = library;
    _sourceName = [sourceName copy];
    _specializedName = specializedName.length ? [specializedName copy] : nil;
    _type = functionType;
    _specialized = specialized || !_constants.empty();
    return self;
}
- (const std::vector<A840OwnedConstant> &)ownedConstants { return _constants; }
- (id<MTLDevice>)device { return _ownerLibrary.device; }
- (NSString *)name { return _specializedName ?: _sourceName; }
- (NSString *)sourceName { return _sourceName; }
- (NSString *)specializedName { return _specializedName; }
- (BOOL)isSpecialized { return _specialized; }
- (MTLFunctionType)functionType { return _type; }
- (MTLPatchType)patchType { return MTLPatchTypeNone; }
- (NSInteger)patchControlPointCount { return 0; }
- (NSData *)extractBitcodeData
{
    return a840ExtractMtlbBitcode(_ownerLibrary.metallibData, _sourceName.UTF8String);
}
- (const void *)bitCodeHash
{
    if (!_bitCodeHashReady) {
        NSData *bc = [self extractBitcodeData];
        if (bc && bc.length > 0u) {
            CC_SHA256(bc.bytes, static_cast<CC_LONG>(bc.length), _bitCodeHash);
        } else {
            const char *s = _sourceName.UTF8String ?: "";
            CC_SHA256(s, static_cast<CC_LONG>(std::strlen(s)), _bitCodeHash);
        }
        _bitCodeHashReady = YES;
    }
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
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorFunctionNotFound
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                    [NSString stringWithFormat:@"Function not found: %@", name ?: @""]}];
        }
        return nil;
    }
    id<MTLFunction> fn = [[A840MTLFunction alloc] initWithLibrary:self
                                                       sourceName:name
                                                  specializedName:nil
                                                     functionType:static_cast<MTLFunctionType>(kind.unsignedIntegerValue)
                                                   constantValues:constantValues
                                                      specialized:YES
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
    NSNumber *kind = _functions[descriptor.name];
    if (!kind) {
        if (error) {
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorFunctionNotFound
                                     userInfo:@{NSLocalizedDescriptionKey:
                                                    [NSString stringWithFormat:@"Function not found: %@", descriptor.name]}];
        }
        return nil;
    }
    NSString *specName = descriptor.specializedName;
    id<MTLFunction> fn = [[A840MTLFunction alloc] initWithLibrary:self
                                                       sourceName:descriptor.name
                                                  specializedName:specName
                                                     functionType:static_cast<MTLFunctionType>(kind.unsignedIntegerValue)
                                                   constantValues:descriptor.constantValues
                                                      specialized:YES
                                                            error:error];
    if (fn && error) *error = nil;
    return fn;
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
    BOOL usesFetchedColor;
    A840RtBuffer *zeroPageBo;
    NSMutableArray<NSValue *> *transientBuffers;
    A840MTLFunction *computeFunction;
    MTLLinkedFunctions *computeLinkedFunctions;
    A840RtComputePipelineDesc computeDesc;
    NSMutableDictionary<NSString *, A840MTLCompiledPlan *> *computeVariants;
    /* Base compute SPIR-V + specialization metadata for fast in-memory variant compilation
     * on (localSize, imageblockWidth/Height, threadgroupMemoryLengths). */
    std::vector<uint32_t> baseSpirvWords;
    std::string baseEntryPoint;
    std::vector<NmbSpirvBinding> baseBindings;
    std::vector<NmbSpirvSpecConstant> specConstants;
    uint32_t imageblockCellBytes;
    BOOL usesImageblock;
    uint32_t usedTgMask;
    uint32_t activeImageblock[2];
    uint32_t activeTgLengths[32];
    /* Hardware VFD vertex fetch metadata (packed 1:1 with VFD_DEST_CNTL_0..N-1). */
    uint32_t vfdInputCount;
    uint32_t vfdInputAttrLocations[A840_MAX_VERTEX_INPUTS];
    std::vector<NmbSpirvVertexInput> reflectedVertexInputs;
    std::vector<NmbSpirvConstexprSampler> constexprSamplers;
    uint32_t vertexAttributeCount;
    NmbVertexAttribute vertexAttributes[NMB_MAX_VERTEX_ATTRIBUTES];
    uint32_t vertexLayoutCount;
    NmbVertexLayout vertexLayouts[NMB_MAX_VERTEX_LAYOUTS];
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
        usesFetchedColor = NO;
        zeroPageBo = nullptr;
        transientBuffers = [NSMutableArray new];
        computeVariants = [NSMutableDictionary new];
        imageblockCellBytes = 0u;
        usesImageblock = NO;
        usedTgMask = 0u;
        activeImageblock[0] = 1u;
        activeImageblock[1] = 1u;
        std::memset(activeTgLengths, 0, sizeof(activeTgLengths));
        vfdInputCount = 0u;
        vertexAttributeCount = 0u;
        vertexLayoutCount = 0u;
    }
    return self;
}
- (NSUInteger)imageblockCellBytes
{
    return static_cast<NSUInteger>(imageblockCellBytes);
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
    plan->usesFetchedColor = NO;
    for (uint32_t s = 0u; s < 2u; ++s) {
        plan->pushFields[s].clear();
        plan->argTableBytes[s] = 0u;
        plan->pushRanges[s] = {};
    }
    for (uint32_t s = 0u; s < stageCount; ++s) {
        plan->argTableBytes[s] = nmb_spirv_arg_table_bytes(spv, s);
        nmb_spirv_push_constant_range(spv, s, &plan->pushRanges[s]);
        const size_t pfCount = nmb_spirv_push_field_count(spv, s);
        plan->pushFields[s].reserve(pfCount);
        for (size_t i = 0u; i < pfCount; ++i) {
            NmbSpirvPushField pf{};
            if (nmb_spirv_push_field(spv, s, i, &pf)) {
                if (pf.kind == NMB_PUSH_ZERO_ADDRESS ||
                    pf.kind == NMB_PUSH_BUFFER_ADDRESS ||
                    pf.kind == NMB_PUSH_ARGUMENT_TABLE_ADDRESS ||
                    pf.kind == NMB_PUSH_BUFFER_BOUNDS) {
                    needsZeroPage = true;
                }
                if (pf.kind == NMB_PUSH_FETCHED_COLOR) {
                    plan->usesFetchedColor = YES;
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
    plan->reflectedVertexInputs.clear();
    const size_t viCount = nmb_spirv_vertex_input_count(spv);
    plan->reflectedVertexInputs.reserve(viCount);
    for (size_t i = 0u; i < viCount; ++i) {
        NmbSpirvVertexInput vi{};
        if (nmb_spirv_vertex_input(spv, i, &vi)) plan->reflectedVertexInputs.push_back(vi);
    }
    plan->constexprSamplers.clear();
    const size_t csSampCount = nmb_spirv_constexpr_sampler_count(spv);
    plan->constexprSamplers.reserve(csSampCount);
    for (size_t i = 0u; i < csSampCount; ++i) {
        NmbSpirvConstexprSampler cs{};
        if (nmb_spirv_constexpr_sampler(spv, i, &cs)) plan->constexprSamplers.push_back(cs);
    }
    if (needsZeroPage && (!plan->rtDevice || plan->rtDevice->zero_page_va == 0u) && !plan->zeroPageBo) {
        plan->zeroPageBo = allocZeroPageBuffer(plan->rtDevice);
        if (!plan->zeroPageBo) return NO;
    }
    return YES;
}

- (BOOL)planUsesIdentityPush:(A840MTLCompiledPlan *)plan
{
    return plan && (!plan->pushFields[0].empty() || !plan->pushFields[1].empty());
}

- (BOOL)planUsesFetchedColor:(id)planObj
{
    if (![planObj isKindOfClass:[A840MTLCompiledPlan class]]) return NO;
    return static_cast<A840MTLCompiledPlan *>(planObj)->usesFetchedColor;
}

- (BOOL)buildIdentityBindStateForPlan:(A840MTLCompiledPlan *)plan
                        renderEncoder:(A840RtRenderEncoder *)renderEnc
                       computeEncoder:(A840RtComputeEncoder *)computeEnc
                        stageBindings:(A840MTLBindings *const *)stageBindings
                           stageCount:(uint32_t)stageCount
                         colorTargets:(id<MTLTexture> const __unsafe_unretained *)colorTargets
                   colorTargetNatives:(A840RtTexture * const *)colorTargetNatives
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
    std::vector<A840RtTexture *> uniqueTexNatives;
    NSMutableArray<A840MTLSamplerState *> *uniqueSamplers = [NSMutableArray new];
    NSMutableDictionary<NSString *, NSNumber *> *texSlotMap = [NSMutableDictionary new];
    NSMutableDictionary<NSString *, NSNumber *> *sampSlotMap = [NSMutableDictionary new];
    uint32_t fetchedColorSlots[A840_MAX_COLOR_TARGETS] = {};
    const uint32_t constexprSamplerSlots = plan->constexprSamplers.empty()
                                               ? 1u
                                               : static_cast<uint32_t>(plan->constexprSamplers.size());

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
                uniqueTexNatives.push_back(tex.native);
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
                    slotIdx = constexprSamplerSlots + static_cast<uint32_t>(i);
                    break;
                }
            }
            if (slotIdx == 0u) {
                [uniqueSamplers addObject:samp];
                slotIdx = constexprSamplerSlots + static_cast<uint32_t>(uniqueSamplers.count - 1u);
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
            A840RtTexture *targetRt = (colorTargetNatives && colorTargetNatives[c]) ? colorTargetNatives[c] : tex.native;
            if (!targetRt) continue;
            uint32_t slotIdx = 0u;
            for (NSUInteger i = 0u; i < uniqueTextures.count; ++i) {
                if (uniqueTextures[i] == tex && uniqueTexNatives[i] == targetRt) {
                    slotIdx = static_cast<uint32_t>(i + 1u);
                    break;
                }
            }
            if (slotIdx == 0u) {
                [uniqueTextures addObject:tex];
                uniqueTexNatives.push_back(targetRt);
                slotIdx = static_cast<uint32_t>(uniqueTextures.count);
            }
            fetchedColorSlots[c] = slotIdx;
        }
    }

    /* Plan contiguous per-mip storage descriptor runs for NMB_PUSH_TEXTURE_LEVEL_BASE
     * (e.g. QuartzCore variable_blur_downsample_compute writing mip levels > 0). */
    struct StorageLevelRun {
        A840RtTexture *rtTex;
        uint32_t baseSlot;
        uint32_t levelCount;
    };
    std::vector<StorageLevelRun> levelRuns;
    NSMutableDictionary<NSString *, NSNumber *> *levelRunBaseMap = [NSMutableDictionary new];
    uint32_t storageSlotCount = static_cast<uint32_t>(1u + uniqueTextures.count);
    if (plan->storageImageSet >= 0) {
        for (uint32_t s = 0u; s < stageCount; ++s) {
            for (const NmbSpirvPushField &pf : plan->pushFields[s]) {
                if (pf.kind != NMB_PUSH_TEXTURE_LEVEL_BASE) continue;
                NSString *tag = [NSString stringWithFormat:@"%u:%u", s, pf.metal_slot];
                if (levelRunBaseMap[tag]) continue;
                NSNumber *slotNum = texSlotMap[tag];
                if (!slotNum || slotNum.unsignedIntValue == 0u) continue;
                A840RtTexture *rtTex = uniqueTexNatives[slotNum.unsignedIntValue - 1u];
                if (!rtTex || rtTex->mip_levels <= 1u) {
                    levelRunBaseMap[tag] = slotNum;
                    continue;
                }
                uint32_t existingBase = 0u;
                for (const auto &run : levelRuns) {
                    if (run.rtTex == rtTex) { existingBase = run.baseSlot; break; }
                }
                if (existingBase != 0u) {
                    levelRunBaseMap[tag] = @(existingBase);
                } else {
                    const uint32_t base = storageSlotCount;
                    storageSlotCount += rtTex->mip_levels;
                    levelRuns.push_back(StorageLevelRun{rtTex, base, rtTex->mip_levels});
                    levelRunBaseMap[tag] = @(base);
                }
            }
        }
    }

    /* 2. Lay out single 64B-aligned transient BO: bindless sets + inline setBytes + arg tables.
     * Always bind slot-0 zeroed null descriptors whenever a sampled/storage image set exists
     * so SP_GFX_BINDLESS_BASE_N is never left at 0x0 (prevents TPL1 SMMU fault on null slot). */
    size_t cursor = 0u;
    if (plan->sampledImageSet >= 0) {
        const size_t setIdx = static_cast<size_t>(plan->sampledImageSet);
        outRes->setOffsets[setIdx] = cursor;
        outRes->setActive[setIdx] = true;
        if (static_cast<uint32_t>(setIdx + 1u) > outRes->activeSetCount) {
            outRes->activeSetCount = static_cast<uint32_t>(setIdx + 1u);
        }
        cursor += (1u + uniqueTextures.count) * kIdentityBindlessSlotBytes;
    }
    if (plan->storageImageSet >= 0) {
        const size_t setIdx = static_cast<size_t>(plan->storageImageSet);
        outRes->setOffsets[setIdx] = cursor;
        outRes->setActive[setIdx] = true;
        if (static_cast<uint32_t>(setIdx + 1u) > outRes->activeSetCount) {
            outRes->activeSetCount = static_cast<uint32_t>(setIdx + 1u);
        }
        cursor += static_cast<size_t>(storageSlotCount) * kIdentityBindlessSlotBytes;
    }
    /* Always bind samplerSet when present and populate slot 0 for constexpr samplers. */
    if (plan->samplerSet >= 0) {
        const size_t setIdx = static_cast<size_t>(plan->samplerSet);
        outRes->setOffsets[setIdx] = cursor;
        outRes->setActive[setIdx] = true;
        if (static_cast<uint32_t>(setIdx + 1u) > outRes->activeSetCount) {
            outRes->activeSetCount = static_cast<uint32_t>(setIdx + 1u);
        }
        cursor += (static_cast<size_t>(constexprSamplerSlots) + uniqueSamplers.count) * kIdentityBindlessSlotBytes;
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

    const size_t totalBytes = alignUp(cursor ? cursor : 64u, 64u);
    A840RtBuffer *bo = nullptr;
    uint64_t boOff = 0u;
    void *rawCpu = nullptr;
    uint64_t baseVa = 0u;
    const A840RtStatus allocSt = renderEnc
        ? a840_rt_render_alloc_transient(renderEnc, totalBytes, 64u, &bo, &boOff, &rawCpu, &baseVa)
        : a840_rt_compute_alloc_transient(computeEnc, totalBytes, 64u, &bo, &boOff, &rawCpu, &baseVa);
    if (allocSt != A840_RT_OK || !bo || !rawCpu) {
        return NO;
    }
    auto *baseCpu = static_cast<uint8_t *>(rawCpu);
    std::memset(baseCpu, 0, totalBytes);
    outRes->descBo = bo;
    outRes->descOffset = boOff;

    /* 3. Write 64B A8XX hardware descriptors into bindless sets and retain each
     * referenced texture on the active command buffer so its BO stays mapped until completion. */
    for (NSUInteger i = 0u; i < uniqueTextures.count; ++i) {
        const size_t slotOff = (i + 1u) * kIdentityBindlessSlotBytes;
        A840RtTexture *rtTex = uniqueTexNatives[i];
        if (renderEnc) (void)a840_rt_render_retain_texture(renderEnc, rtTex);
        else if (computeEnc) (void)a840_rt_compute_retain_texture(computeEnc, rtTex);
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
                uint32_t storWords[A840_TEX_CONST_DWORDS] = {};
                if (a840_rt_texture_encode_storage_level(rtTex, 0u, storWords) == A840_RT_OK) {
                    std::memcpy(baseCpu + outRes->setOffsets[setIdx] + slotOff,
                                storWords, sizeof(storWords));
                } else {
                    std::memcpy(baseCpu + outRes->setOffsets[setIdx] + slotOff,
                                rtTex->tex_const, sizeof(rtTex->tex_const));
                }
            }
        }
    }
    if (plan->storageImageSet >= 0 && !levelRuns.empty()) {
        const size_t setIdx = static_cast<size_t>(plan->storageImageSet);
        if (outRes->setActive[setIdx]) {
            for (const auto &run : levelRuns) {
                for (uint32_t lvl = 0u; lvl < run.levelCount; ++lvl) {
                    uint32_t lvlWords[A840_TEX_CONST_DWORDS] = {};
                    if (a840_rt_texture_encode_storage_level(run.rtTex, lvl, lvlWords) == A840_RT_OK) {
                        const size_t lvlOff = static_cast<size_t>(run.baseSlot + lvl) * kIdentityBindlessSlotBytes;
                        std::memcpy(baseCpu + outRes->setOffsets[setIdx] + lvlOff,
                                    lvlWords, sizeof(lvlWords));
                    }
                }
            }
        }
    }
    if (plan->samplerSet >= 0) {
        const size_t setIdx = static_cast<size_t>(plan->samplerSet);
        if (outRes->setActive[setIdx]) {
            uint8_t *sampBase = baseCpu + outRes->setOffsets[setIdx];
            if (plan->constexprSamplers.empty()) {
                uint32_t defaultSamp[A840_SAMP_CONST_DWORDS] = {};
                encodeDefaultConstexprSampler(defaultSamp);
                std::memcpy(sampBase, defaultSamp, A840_SAMP_CONST_DWORDS * sizeof(uint32_t));
            } else {
                for (size_t ci = 0u; ci < plan->constexprSamplers.size(); ++ci) {
                    uint32_t csWords[A840_SAMP_CONST_DWORDS] = {};
                    encodeReflectedConstexprSampler(plan->constexprSamplers[ci], csWords);
                    std::memcpy(sampBase + ci * kIdentityBindlessSlotBytes,
                                csWords, A840_SAMP_CONST_DWORDS * sizeof(uint32_t));
                }
            }
            for (NSUInteger i = 0u; i < uniqueSamplers.count; ++i) {
                const size_t slotOff = (static_cast<size_t>(constexprSamplerSlots) + i) * kIdentityBindlessSlotBytes;
                const uint32_t *words = uniqueSamplers[i].descriptorWords;
                std::memcpy(sampBase + slotOff,
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
            const uint32_t slotKey = key.unsignedIntValue;
            NSString *tag = [NSString stringWithFormat:@"%u:%u", s, slotKey];
            inlineVaMap[tag] = @(baseVa + inlineCur);
            inlineLenMap[tag] = @(d.length);
            if (s == 0u && slotKey < 32u) {
                outRes->vertexInlineVa[slotKey] = baseVa + inlineCur;
                outRes->vertexInlineLen[slotKey] = static_cast<uint32_t>(d.length);
            }
            inlineCur += alignUp(d.length, 64u);
        }
    }

    /* 5. Pack metal2vulkan-v2 identity-domain push constants & optional argument table.
     * Prefer the device's permanent 64 KiB zero_page_va (matching metal2vulkan Ring::nil = 64 KiB,
     * device-lifetime mapped RO) so unbound buffers never fault past 4 KiB or across pipeline release. */
    const uint64_t zeroVa = (plan->rtDevice && plan->rtDevice->zero_page_va != 0u)
                                ? plan->rtDevice->zero_page_va
                                : (plan->zeroPageBo ? plan->zeroPageBo->gpu_va : baseVa);
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
                        if (nativeBuf && off < nativeBuf->size_bytes) {
                            va = nativeBuf->gpu_va + off;
                            if (renderEnc) (void)a840_rt_render_retain_buffer(renderEnc, nativeBuf);
                            else if (computeEnc) (void)a840_rt_compute_retain_buffer(computeEnc, nativeBuf);
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
                        if (nativeBuf && off < nativeBuf->size_bytes) {
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
                if (slotNum && slotNum.unsignedIntValue >= constexprSamplerSlots) {
                    const uint32_t slotIdx = slotNum.unsignedIntValue;
                    A840MTLSamplerState *samp = uniqueSamplers[slotIdx - constexprSamplerSlots];
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
                NSNumber *runBase = levelRunBaseMap[tag];
                if (runBase) {
                    baseSlot = runBase.unsignedIntValue;
                } else {
                    NSNumber *slotNum = texSlotMap[tag];
                    if (slotNum) baseSlot = slotNum.unsignedIntValue;
                }
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &baseSlot, sizeof(baseSlot));
                break;
            }
            case NMB_PUSH_INTEGER_CHANNELS: {
                uint32_t word = 0u;
                NSString *tag = [NSString stringWithFormat:@"%u:%u", s, pf.metal_slot];
                NSNumber *slotNum = texSlotMap[tag];
                if (slotNum && slotNum.unsignedIntValue > 0u) {
                    A840MTLTexture *tex = uniqueTextures[slotNum.unsignedIntValue - 1u];
                    word = nmb_encode_integer_channels_word(static_cast<uint32_t>(tex.pixelFormat));
                }
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &word, sizeof(word));
                break;
            }
            case NMB_PUSH_BICUBIC_TEXELS: {
                uint32_t word = nmb_encode_bicubic_texels_word(0u);
                NSString *tag = [NSString stringWithFormat:@"%u:%u", s, pf.metal_slot];
                NSNumber *slotNum = texSlotMap[tag];
                if (slotNum && slotNum.unsignedIntValue > 0u) {
                    A840MTLTexture *tex = uniqueTextures[slotNum.unsignedIntValue - 1u];
                    word = nmb_encode_bicubic_texels_word(static_cast<uint32_t>(tex.pixelFormat));
                }
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &word, sizeof(word));
                break;
            }
            case NMB_PUSH_ARGUMENT_TABLE_ADDRESS: {
                const uint64_t argVa = plan->argTableBytes[s] > 0u ? (baseVa + argTableOff[s]) : zeroVa;
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
            case NMB_PUSH_CONSTEXPR_SAMPLER: {
                const uint32_t cidx = pf.extra < constexprSamplerSlots ? pf.extra : 0u;
                writeFieldBytes(outRes->pushBuf[s], kIdentityMaxPushBytes, argCpu, argCap, pf, &cidx, sizeof(cidx));
                break;
            }
            case NMB_PUSH_TAIL_WORD:
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
    for (uint32_t s = 0u; s < outRes->activeSetCount; ++s) {
        if (outRes->setActive[s]) outRes->setOffsets[s] += boOff;
    }
    return YES;
}

struct A840MarshaledLinked {
    std::vector<NmbLinkedFunction> entries;
    std::vector<std::vector<NmbFunctionConstant>> constStorage;
};

static bool a840MarshalLinkedFunctions(MTLLinkedFunctions *linked, A840MarshaledLinked &out)
{
    out.entries.clear();
    out.constStorage.clear();
    if (!linked) return true;
    NSArray<id<MTLFunction>> *groups[2] = {linked.functions, linked.privateFunctions};
    size_t total = 0u;
    for (unsigned g = 0u; g < 2u; ++g) total += groups[g].count;
    out.entries.reserve(total);
    out.constStorage.reserve(total);
    for (unsigned g = 0u; g < 2u; ++g) {
        for (id<MTLFunction> rawFn in groups[g]) {
            if (![(id)rawFn isKindOfClass:[A840MTLFunction class]]) return false;
            auto *fn = static_cast<A840MTLFunction *>(rawFn);
            auto *lib = static_cast<NmbLibrary *>(fn.ownerLibrary.parsedNmbLibrary);
            const char *srcName = (fn.sourceName ?: fn.name).UTF8String;
            if (!lib || !srcName || !srcName[0]) return false;
            out.constStorage.emplace_back();
            a840MarshalConstants([fn ownedConstants], out.constStorage.back());
            NmbLinkedFunction lf{};
            lf.library = lib;
            lf.name = srcName;
            lf.specialized_name = fn.specializedName.length ? fn.specializedName.UTF8String : nullptr;
            lf.constants = out.constStorage.back().empty() ? nullptr : out.constStorage.back().data();
            lf.constant_count = out.constStorage.back().size();
            lf.is_private = (g == 1u) ? 1u : 0u;
            lf.specialized = fn.isSpecialized ? 1u : 0u;
            out.entries.push_back(lf);
        }
    }
    return true;
}

static uint32_t a840AlignConstlenForPush(uint32_t rawConstlenVec4, const NmbSpirvPushConstantRange &push)
{
    uint32_t aligned = (rawConstlenVec4 + 3u) & ~3u;
    if (push.size_bytes > 0u) {
        const uint32_t mask = A840_BINDING_PLAN_SLOT_BYTES - 1u;
        const uint32_t src = push.offset_bytes & ~mask;
        const uint32_t end = (push.offset_bytes + push.size_bytes + mask) & ~mask;
        const uint32_t minVec4 = (end - src) / 16u;
        if (aligned < minVec4) aligned = minVec4;
    }
    if (aligned > A840_BINDING_PLAN_MAX_CONST_VEC4) aligned = A840_BINDING_PLAN_MAX_CONST_VEC4;
    return aligned;
}

- (A840MTLCompiledPlan *)compileComputeFunction:(A840MTLFunction *)fn
                                linkedFunctions:(MTLLinkedFunctions *)linkedFunctions
                                      localSize:(MTLSize)localSize
                                      forDevice:(A840MTLDevice *)device
                                     descriptor:(A840RtComputePipelineDesc *)descriptor
{
    if (!fn || !device || !descriptor) return nil;
    if (localSize.width == 0u || localSize.height == 0u || localSize.depth == 0u ||
        localSize.width > UINT32_MAX || localSize.height > UINT32_MAX || localSize.depth > UINT32_MAX)
        return nil;
    const char *entry = (fn.sourceName ?: fn.name).UTF8String;
    auto *lib = static_cast<NmbLibrary *>(fn.ownerLibrary.parsedNmbLibrary);
    if (!lib || !entry) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE cs-nolib %.40s", entry ?: "?");
        a840diag::reportText(msg);
        return nil;
    }

    std::vector<NmbFunctionConstant> ffiConsts;
    a840MarshalConstants([fn ownedConstants], ffiConsts);
    A840MarshaledLinked ffiLinked{};
    if (!a840MarshalLinkedFunctions(linkedFunctions, ffiLinked)) {
        a840diag::reportText("A840PIPE cs-linked-foreign");
        return nil;
    }

    NmbComputePipelineDesc cdesc{};
    cdesc.local_size[0] = static_cast<uint32_t>(localSize.width);
    cdesc.local_size[1] = static_cast<uint32_t>(localSize.height);
    cdesc.local_size[2] = static_cast<uint32_t>(localSize.depth);
    cdesc.zeroed_threadgroup_memory = 1u;
    NmbSpirvResult *spv = nmb_compile_compute_spirv_v3(
        _translator, lib, entry,
        ffiConsts.empty() ? nullptr : ffiConsts.data(), ffiConsts.size(),
        ffiLinked.entries.empty() ? nullptr : ffiLinked.entries.data(), ffiLinked.entries.size(),
        fn.isSpecialized ? 1u : 0u,
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
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE cs-nospv %.40s", entry);
        a840diag::reportText(msg);
        nmb_spirv_result_free(spv);
        return nil;
    }

    A840MTLCompiledPlan *plan = [A840MTLCompiledPlan new];
    plan->isCompute = YES;
    plan->rtDevice = device.runtime;
    plan->stageCount = 1u;
    plan->computeFunction = fn;
    plan->computeLinkedFunctions = [linkedFunctions copy];

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
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE cs-ir3 %.28s st=%d %.36s", entry, (int)cst, errBuf[0] ? errBuf : "io");
        a840diag::reportText(msg);
        nmb_spirv_result_free(spv);
        return nil;
    }
    const size_t bcount = nmb_spirv_binding_count(spv);
    std::vector<NmbSpirvBinding> bindings(bcount);
    for (size_t i = 0u; i < bcount; ++i)
        (void)nmb_spirv_binding(spv, i, &bindings[i]);
    NmbSpirvPushConstantRange pushRange{};
    (void)nmb_spirv_push_constant_range(spv, 0u, &pushRange);
    plan->stages[0].constlen_vec4 = a840AlignConstlenForPush(plan->stages[0].constlen_vec4, pushRange);
    if (![self captureIdentityReflection:spv intoPlan:plan stageCount:1u]) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE cs-refl %.40s", entry);
        a840diag::reportText(msg);
        nmb_spirv_result_free(spv);
        return nil;
    }
    plan->baseSpirvWords.assign(spvWords, spvWords + (spvBytes / sizeof(uint32_t)));
    plan->baseEntryPoint = spvEntry;
    plan->baseBindings = bindings;
    plan->imageblockCellBytes = nmb_spirv_compute_imageblock_cell_bytes(spv);
    plan->specConstants.clear();
    const size_t specCount = nmb_spirv_spec_constant_count(spv, 0u);
    plan->specConstants.reserve(specCount);
    for (size_t i = 0u; i < specCount; ++i) {
        NmbSpirvSpecConstant sc{};
        if (nmb_spirv_spec_constant(spv, 0u, i, &sc)) {
            plan->specConstants.push_back(sc);
            if (sc.kind == NMB_SPEC_IMAGEBLOCK_WIDTH || sc.kind == NMB_SPEC_IMAGEBLOCK_HEIGHT) {
                plan->usesImageblock = YES;
            } else if (sc.kind == NMB_SPEC_TG_LENGTH && sc.index < 32u) {
                plan->usedTgMask |= (1u << sc.index);
            }
        }
    }
    nmb_spirv_result_free(spv);

    const auto bpst = a840_binding_plan_build(bindings.empty() ? nullptr : bindings.data(),
                                              bindings.size(), &plan->stages[0], 1u,
                                              &pushRange, &plan->bindingPlan);
    if (bpst != A840_BINDING_PLAN_OK) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE cs-bp %.36s st=%d", entry, (int)bpst);
        a840diag::reportText(msg);
        return nil;
    }

    plan->codeBuffers[0] = uploadIsaBuffer(device.runtime, plan->stages[0].isa_words,
                                           plan->stages[0].isa_dwords);
    if (!plan->codeBuffers[0]) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE cs-isa %.40s", entry);
        a840diag::reportText(msg);
        return nil;
    }
    if (plan->stages[0].private_mem_bytes > 0u) {
        plan->privateBuffers[0] = allocPrivateScratch(device.runtime, plan->stages[0].private_mem_bytes);
        if (!plan->privateBuffers[0]) {
            char msg[128] = {};
            snprintf(msg, sizeof(msg), "A840PIPE cs-pvt %.40s", entry);
            a840diag::reportText(msg);
            return nil;
        }
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
    return [self compileComputeFunction:fn linkedFunctions:nil localSize:MTLSizeMake(1, 1, 1)
                              forDevice:device descriptor:descriptor];
}

- (id)prepareComputeWithDescriptor:(MTLComputePipelineDescriptor *)pipelineDescriptor
                         forDevice:(A840MTLDevice *)device
                        descriptor:(A840RtComputePipelineDesc *)descriptor
{
    if (!pipelineDescriptor || ![pipelineDescriptor.computeFunction isKindOfClass:[A840MTLFunction class]] ||
        !device || !descriptor) {
        return nil;
    }
    auto *fn = static_cast<A840MTLFunction *>(pipelineDescriptor.computeFunction);
    return [self compileComputeFunction:fn linkedFunctions:pipelineDescriptor.linkedFunctions
                              localSize:MTLSizeMake(1, 1, 1)
                              forDevice:device descriptor:descriptor];
}

- (id)prepareComputeVariantForPlan:(id)planObj localSize:(MTLSize)localSize
                         forDevice:(A840MTLDevice *)device
                        descriptor:(A840RtComputePipelineDesc *)descriptor
{
    return [self prepareComputeVariantForPlan:planObj
                                    localSize:localSize
                              imageblockWidth:1u
                             imageblockHeight:1u
                           threadgroupLengths:nullptr
                       threadgroupLengthCount:0u
                                    forDevice:device
                                   descriptor:descriptor];
}

- (id)prepareComputeVariantForPlan:(id)planObj
                         localSize:(MTLSize)localSize
                   imageblockWidth:(uint32_t)imageblockWidth
                  imageblockHeight:(uint32_t)imageblockHeight
                threadgroupLengths:(const uint32_t *)threadgroupLengths
            threadgroupLengthCount:(uint32_t)threadgroupLengthCount
                         forDevice:(A840MTLDevice *)device
                        descriptor:(A840RtComputePipelineDesc *)descriptor
{
    if (![planObj isKindOfClass:[A840MTLCompiledPlan class]] || !device || !descriptor) return nil;
    auto *base = static_cast<A840MTLCompiledPlan *>(planObj);
    if (!base->isCompute || !base->computeFunction) return nil;
    if (localSize.width == 0u || localSize.height == 0u || localSize.depth == 0u ||
        localSize.width > UINT32_MAX || localSize.height > UINT32_MAX || localSize.depth > UINT32_MAX) {
        return nil;
    }
    const uint32_t effIbW = base->usesImageblock ? (imageblockWidth ? imageblockWidth : 1u) : 1u;
    const uint32_t effIbH = base->usesImageblock ? (imageblockHeight ? imageblockHeight : 1u) : 1u;
    uint32_t effTg[32] = {};
    if (base->usedTgMask != 0u && threadgroupLengths && threadgroupLengthCount > 0u) {
        const uint32_t lim = threadgroupLengthCount < 32u ? threadgroupLengthCount : 32u;
        for (uint32_t i = 0u; i < lim; ++i) {
            if ((base->usedTgMask & (1u << i)) != 0u) effTg[i] = threadgroupLengths[i];
        }
    }
    bool matchesBase =
        base->stages[0].local_size[0] == localSize.width &&
        base->stages[0].local_size[1] == localSize.height &&
        base->stages[0].local_size[2] == localSize.depth &&
        (!base->usesImageblock || (base->activeImageblock[0] == effIbW && base->activeImageblock[1] == effIbH));
    if (matchesBase && base->usedTgMask != 0u) {
        for (uint32_t i = 0u; i < 32u; ++i) {
            if ((base->usedTgMask & (1u << i)) != 0u && base->activeTgLengths[i] != effTg[i]) {
                matchesBase = false;
                break;
            }
        }
    }
    if (matchesBase) {
        *descriptor = base->computeDesc;
        return base;
    }

    NSMutableString *key = [NSMutableString stringWithFormat:@"%lu:%lu:%lu|%u:%u",
                            static_cast<unsigned long>(localSize.width),
                            static_cast<unsigned long>(localSize.height),
                            static_cast<unsigned long>(localSize.depth),
                            effIbW, effIbH];
    if (base->usedTgMask != 0u) {
        for (uint32_t i = 0u; i < 32u; ++i) {
            if ((base->usedTgMask & (1u << i)) != 0u) {
                [key appendFormat:@"|t%u=%u", i, effTg[i]];
            }
        }
    }
    A840MTLCompiledPlan *cached = base->computeVariants[key];
    if (cached) {
        *descriptor = cached->computeDesc;
        return cached;
    }

    /* Fast path: specialize cached base SPIR-V in-memory without re-parsing AIR bitcode! */
    if (!base->baseSpirvWords.empty() && !base->baseEntryPoint.empty()) {
        std::vector<uint32_t> patched = base->baseSpirvWords;
        const uint32_t lsize[3] = {
            static_cast<uint32_t>(localSize.width),
            static_cast<uint32_t>(localSize.height),
            static_cast<uint32_t>(localSize.depth),
        };
        const uint32_t ibSize[2] = {effIbW, effIbH};
        if (nmb_patch_compute_spec_words(patched.data(), patched.size(),
                                         base->specConstants.empty() ? nullptr : base->specConstants.data(),
                                         base->specConstants.size(),
                                         lsize, ibSize, effTg, 32u)) {
            A840MTLCompiledPlan *variant = [A840MTLCompiledPlan new];
            variant->isCompute = YES;
            variant->rtDevice = device.runtime;
            variant->stageCount = 1u;
            variant->computeFunction = base->computeFunction;
            variant->computeLinkedFunctions = base->computeLinkedFunctions;
            variant->pushFields[0] = base->pushFields[0];
            variant->argTableBytes[0] = base->argTableBytes[0];
            variant->pushRanges[0] = base->pushRanges[0];
            variant->sampledImageSet = base->sampledImageSet;
            variant->storageImageSet = base->storageImageSet;
            variant->samplerSet = base->samplerSet;
            variant->constexprSamplers = base->constexprSamplers;
            variant->imageblockCellBytes = base->imageblockCellBytes;
            variant->usesImageblock = base->usesImageblock;
            variant->usedTgMask = base->usedTgMask;
            variant->activeImageblock[0] = effIbW;
            variant->activeImageblock[1] = effIbH;
            std::memcpy(variant->activeTgLengths, effTg, sizeof(effTg));

            const A840CompileTarget target = makeKaanapaliTarget();
            A840StageInput stageIn{};
            stageIn.abi_version = ADRENO_COMPILE_ABI_VERSION;
            stageIn.stage = A840_STAGE_COMPUTE;
            stageIn.spirv_words = patched.data();
            stageIn.spirv_bytes = patched.size() * sizeof(uint32_t);
            stageIn.entry_point = base->baseEntryPoint.c_str();
            stageIn.local_size[0] = lsize[0];
            stageIn.local_size[1] = lsize[1];
            stageIn.local_size[2] = lsize[2];
            stageIn.sample_count = 1u;
            stageIn.flags = target.flags;

            char errBuf[256] = {};
            const auto cst = a840_compile_stage(&target, &stageIn, &variant->stages[0], errBuf, sizeof(errBuf));
            if (cst == A840_COMPILE_OK && a840_ir3_get_stage_io_info(&variant->stages[0], &variant->ioInfo[0])) {
                variant->stages[0].constlen_vec4 =
                    a840AlignConstlenForPush(variant->stages[0].constlen_vec4, variant->pushRanges[0]);
                const auto bpst = a840_binding_plan_build(
                    base->baseBindings.empty() ? nullptr : base->baseBindings.data(),
                    base->baseBindings.size(), &variant->stages[0], 1u,
                    &variant->pushRanges[0], &variant->bindingPlan);
                if (bpst == A840_BINDING_PLAN_OK) {
                    variant->codeBuffers[0] = uploadIsaBuffer(device.runtime, variant->stages[0].isa_words,
                                                              variant->stages[0].isa_dwords);
                    if (variant->codeBuffers[0] &&
                        (variant->stages[0].private_mem_bytes == 0u ||
                         (variant->privateBuffers[0] = allocPrivateScratch(device.runtime,
                             variant->stages[0].private_mem_bytes)) != nullptr)) {
                        std::memset(descriptor, 0, sizeof(*descriptor));
                        descriptor->stage = &variant->stages[0];
                        descriptor->code_buffer = variant->codeBuffers[0];
                        descriptor->code_offset = 0u;
                        descriptor->private_buffer = variant->privateBuffers[0];
                        descriptor->private_offset = 0u;
                        descriptor->bindless_tex = variant->ioInfo[0].bindless_tex != 0u;
                        descriptor->bindless_samp = variant->ioInfo[0].bindless_samp != 0u;
                        descriptor->bindless_uav = variant->ioInfo[0].bindless_uav != 0u;
                        descriptor->bindless_ubo = variant->ioInfo[0].bindless_ubo != 0u;
                        descriptor->local_id_regid = variant->ioInfo[0].cs_local_id_regid;
                        descriptor->wg_id_regid = variant->ioInfo[0].cs_wg_id_regid;
                        descriptor->linear_dispatch = true;
                        variant->computeDesc = *descriptor;
                        base->computeVariants[key] = variant;
                        return variant;
                    }
                }
            }
        }
    }

    A840MTLCompiledPlan *variant = [self compileComputeFunction:base->computeFunction
                                                linkedFunctions:base->computeLinkedFunctions
                                                      localSize:localSize
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
    if (pipeline.fragmentFunction != nil && ![pipeline.fragmentFunction isKindOfClass:[A840MTLFunction class]]) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE rp-badfs %.40s", pipeline.vertexFunction.name.UTF8String ?: "?");
        a840diag::reportText(msg);
        return nil;
    }

    const BOOL hasFs = (pipeline.fragmentFunction != nil);
    auto *vsFn = static_cast<A840MTLFunction *>(pipeline.vertexFunction);
    auto *fsFn = hasFs ? static_cast<A840MTLFunction *>(pipeline.fragmentFunction) : nil;
    auto *vsLib = static_cast<NmbLibrary *>(vsFn.ownerLibrary.parsedNmbLibrary);
    auto *fsLib = hasFs ? static_cast<NmbLibrary *>(fsFn.ownerLibrary.parsedNmbLibrary) : nullptr;
    const char *vsName = (vsFn.sourceName ?: vsFn.name).UTF8String;
    const char *fsName = hasFs ? (fsFn.sourceName ?: fsFn.name).UTF8String : nullptr;
    const char *fsLabel = fsName ? fsName : "<empty>";
    if (!vsLib || !vsName || (hasFs && (!fsLib || !fsName))) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE rp-nolib %.24s/%.24s", vsName ?: "?", fsLabel);
        a840diag::reportText(msg);
        return nil;
    }

    NmbRenderPipelineDescV3 rdescV3{};
    NmbRenderPipelineDesc &rdesc = rdescV3.base;
    rdesc.sample_count = pipeline.rasterSampleCount ? static_cast<uint32_t>(pipeline.rasterSampleCount) : 1u;
    rdesc.depth_mtl_format = static_cast<uint32_t>(pipeline.depthAttachmentPixelFormat);
    rdesc.stencil_mtl_format = static_cast<uint32_t>(pipeline.stencilAttachmentPixelFormat);
    rdescV3.has_color_states = 1u;
    rdescV3.rasterization_enabled = pipeline.isRasterizationEnabled ? 1u : 0u;
    rdescV3.alpha_to_coverage_enabled = pipeline.isAlphaToCoverageEnabled ? 1u : 0u;
    rdescV3.alpha_to_one_enabled = pipeline.isAlphaToOneEnabled ? 1u : 0u;
    rdescV3.input_primitive_topology = static_cast<uint32_t>(pipeline.inputPrimitiveTopology);
    uint32_t colorMask = 0u;
    for (uint32_t i = 0u; i < NMB_MAX_COLORS; ++i) {
        MTLRenderPipelineColorAttachmentDescriptor *att = pipeline.colorAttachments[i];
        const uint32_t fmt = static_cast<uint32_t>(att.pixelFormat);
        rdesc.color_mtl_formats[i] = fmt;
        if (fmt != 0u) {
            rdesc.color_attachment_count = i + 1u;
            colorMask |= (1u << i);
            NmbColorAttachmentDesc &cs = rdescV3.color_states[i];
            cs.pixel_format = fmt;
            cs.write_mask = static_cast<uint32_t>(att.writeMask) & 0xfu;
            cs.blending_enabled = att.isBlendingEnabled ? 1u : 0u;
            cs.src_rgb_factor = static_cast<uint32_t>(att.sourceRGBBlendFactor);
            cs.dst_rgb_factor = static_cast<uint32_t>(att.destinationRGBBlendFactor);
            cs.rgb_blend_op = static_cast<uint32_t>(att.rgbBlendOperation);
            cs.src_alpha_factor = static_cast<uint32_t>(att.sourceAlphaBlendFactor);
            cs.dst_alpha_factor = static_cast<uint32_t>(att.destinationAlphaBlendFactor);
            cs.alpha_blend_op = static_cast<uint32_t>(att.alphaBlendOperation);
        }
    }
    if (rdesc.color_attachment_count == 0u &&
        rdesc.depth_mtl_format == 0u &&
        rdesc.stencil_mtl_format == 0u) {
        rdesc.color_attachment_count = 1u;
        rdesc.color_mtl_formats[0] = static_cast<uint32_t>(MTLPixelFormatBGRA8Unorm);
        rdescV3.color_states[0].pixel_format = static_cast<uint32_t>(MTLPixelFormatBGRA8Unorm);
        rdescV3.color_states[0].write_mask = 0xfu;
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
    if (hasFs) a840MarshalConstants([fsFn ownedConstants], fsConsts);

    A840MarshaledLinked vsLinked{}, fsLinked{};
    if (!a840MarshalLinkedFunctions(pipeline.vertexLinkedFunctions, vsLinked) ||
        (hasFs && !a840MarshalLinkedFunctions(pipeline.fragmentLinkedFunctions, fsLinked))) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE rp-linked %.22s/%.22s", vsName, fsLabel);
        a840diag::reportText(msg);
        return nil;
    }
    rdescV3.vs_linked = vsLinked.entries.empty() ? nullptr : vsLinked.entries.data();
    rdescV3.vs_linked_count = vsLinked.entries.size();
    rdescV3.fs_linked = fsLinked.entries.empty() ? nullptr : fsLinked.entries.data();
    rdescV3.fs_linked_count = fsLinked.entries.size();
    rdescV3.vs_specialized = vsFn.isSpecialized ? 1u : 0u;
    rdescV3.fs_specialized = (hasFs && fsFn.isSpecialized) ? 1u : 0u;

    NmbSpirvResult *spv = nmb_compile_render_spirv_v3(
        _translator,
        vsLib, vsName, vsConsts.empty() ? nullptr : vsConsts.data(), vsConsts.size(),
        fsLib, fsName, fsConsts.empty() ? nullptr : fsConsts.data(), fsConsts.size(),
        &rdescV3);
    const uint32_t spvStages = spv ? static_cast<uint32_t>(nmb_spirv_stage_count(spv)) : 0u;
    if (!spv || spvStages < (hasFs ? 2u : 1u)) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE rp %.18s/%.18s f=%u,%u d=%u %.38s",
                 vsName, fsLabel, rdesc.color_mtl_formats[0], rdesc.color_mtl_formats[1],
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
        const uint32_t *spvWords = nullptr;
        const char *spvEntry = nullptr;
        if (s == 1u && (!hasFs || spvStages < 2u)) {
            spvWords = kEmptyFragmentSpirv;
            spvBytes = sizeof(kEmptyFragmentSpirv);
            spvEntry = "main";
        } else {
            spvWords = nmb_spirv_words(spv, s, &spvBytes);
            spvEntry = nmb_spirv_entry_point(spv, s);
        }
        if (!spvWords || spvBytes == 0u || !spvEntry) {
            char msg[128] = {};
            snprintf(msg, sizeof(msg), "A840PIPE rp-nospv s=%u %.22s/%.22s", s, vsName, fsLabel);
            a840diag::reportText(msg);
            nmb_spirv_result_free(spv);
            return nil;
        }
        A840StageInput stageIn{};
        stageIn.abi_version = ADRENO_COMPILE_ABI_VERSION;
        stageIn.stage = stageKinds[s];
        stageIn.spirv_words = spvWords;
        stageIn.spirv_bytes = spvBytes;
        stageIn.entry_point = spvEntry;
        stageIn.color_rt_mask = (s == 1u && hasFs) ? colorMask : 0u;
        stageIn.sample_count = rdesc.sample_count;
        stageIn.flags = target.flags;

        char errBuf[256] = {};
        const auto cst = a840_compile_stage(&target, &stageIn, &plan->stages[s], errBuf, sizeof(errBuf));
        if (cst != A840_COMPILE_OK || !a840_ir3_get_stage_io_info(&plan->stages[s], &plan->ioInfo[s])) {
            char msg[128] = {};
            snprintf(msg, sizeof(msg), "A840PIPE ir3 s=%u %.18s/%.18s st=%d %.36s",
                     s, vsName, fsLabel, (int)cst, errBuf[0] ? errBuf : "io");
            a840diag::reportText(msg);
            nmb_spirv_result_free(spv);
            return nil;
        }
    }

    const size_t bcount = nmb_spirv_binding_count(spv);
    std::vector<NmbSpirvBinding> bindings(bcount);
    for (size_t i = 0u; i < bcount; ++i)
        (void)nmb_spirv_binding(spv, i, &bindings[i]);
    NmbSpirvPushConstantRange pushRanges[2]{};
    (void)nmb_spirv_push_constant_range(spv, 0u, &pushRanges[0]);
    if (spvStages >= 2u) (void)nmb_spirv_push_constant_range(spv, 1u, &pushRanges[1]);
    for (uint32_t s = 0u; s < 2u; ++s) {
        plan->stages[s].constlen_vec4 = a840AlignConstlenForPush(plan->stages[s].constlen_vec4, pushRanges[s]);
    }
    const uint32_t reflStages = spvStages >= 2u ? 2u : 1u;
    if (![self captureIdentityReflection:spv intoPlan:plan stageCount:reflStages]) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE rp-refl %.24s/%.24s", vsName, fsLabel);
        a840diag::reportText(msg);
        nmb_spirv_result_free(spv);
        return nil;
    }
    nmb_spirv_result_free(spv);

    const auto bpst = a840_binding_plan_build(bindings.empty() ? nullptr : bindings.data(),
                                              bindings.size(), plan->stages, 2u,
                                              pushRanges, &plan->bindingPlan);
    if (bpst != A840_BINDING_PLAN_OK) {
        char msg[128] = {};
        snprintf(msg, sizeof(msg), "A840PIPE bp %.22s/%.22s st=%d", vsName, fsLabel, (int)bpst);
        a840diag::reportText(msg);
        return nil;
    }

    for (uint32_t s = 0u; s < 2u; ++s) {
        plan->codeBuffers[s] = uploadIsaBuffer(device.runtime, plan->stages[s].isa_words,
                                               plan->stages[s].isa_dwords);
        if (!plan->codeBuffers[s]) {
            char msg[128] = {};
            snprintf(msg, sizeof(msg), "A840PIPE rp-isa s=%u %.22s/%.22s", s, vsName, fsLabel);
            a840diag::reportText(msg);
            return nil;
        }
        if (plan->stages[s].private_mem_bytes > 0u) {
            plan->privateBuffers[s] = allocPrivateScratch(device.runtime, plan->stages[s].private_mem_bytes);
            if (!plan->privateBuffers[s]) {
                char msg[128] = {};
                snprintf(msg, sizeof(msg), "A840PIPE rp-pvt s=%u %.22s/%.22s", s, vsName, fsLabel);
                a840diag::reportText(msg);
                return nil;
            }
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

    plan->vertexAttributeCount = rdesc.vertex_attribute_count;
    if (rdesc.vertex_attribute_count > 0u) {
        std::memcpy(plan->vertexAttributes, rdesc.vertex_attributes,
                    rdesc.vertex_attribute_count * sizeof(NmbVertexAttribute));
    }
    plan->vertexLayoutCount = rdesc.vertex_layout_count;
    if (rdesc.vertex_layout_count > 0u) {
        std::memcpy(plan->vertexLayouts, rdesc.vertex_layouts,
                    rdesc.vertex_layout_count * sizeof(NmbVertexLayout));
    }
    /* Mesa NIR VERT_ATTRIB_GENERIC0 == 15; slot - 15 equals Metal [[attribute(N)]]. */
    constexpr uint32_t kNirVertAttribGeneric0 = 15u;
    plan->vfdInputCount = 0u;
    for (uint32_t i = 0u; i < vsIo.input_count && ed.vertex_input_count < A840_MAX_VERTEX_INPUTS; ++i) {
        if (vsIo.inputs[i].sysval || vsIo.inputs[i].compmask == 0u ||
            vsIo.inputs[i].regid >= A840_IR3_REGID_UNUSED) continue;
        ed.vertex_inputs[ed.vertex_input_count].regid = vsIo.inputs[i].regid;
        ed.vertex_inputs[ed.vertex_input_count].component_mask = vsIo.inputs[i].compmask;
        const uint32_t slot = vsIo.inputs[i].slot;
        plan->vfdInputAttrLocations[plan->vfdInputCount++] =
            (slot >= kNirVertAttribGeneric0) ? (slot - kNirVertAttribGeneric0) : slot;
        ++ed.vertex_input_count;
    }
    if (plan->vfdInputCount > 0u && (!plan->rtDevice || plan->rtDevice->zero_page_va == 0u) && !plan->zeroPageBo) {
        plan->zeroPageBo = allocZeroPageBuffer(plan->rtDevice);
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
    /* Adreno 840 (gen80200) has supports_double_threadsize = false (dual-wave dispatch);
     * keep SP_PS_CNTL_0.THREADSIZE = THREAD64 alongside ir3 IR3_SINGLE_ONLY. */
    ed.fs_thread128 = false;
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
                               renderEncoder:(A840RtRenderEncoder *)renderEnc
                              computeEncoder:(A840RtComputeEncoder *)computeEnc
                               stageBindings:(A840MTLBindings *const *)stageBindings
                                  stageKinds:(const uint32_t *)stageKinds
                                  stageCount:(uint32_t)stageCount
                                   outOffset:(uint64_t *)outOffset
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
    const size_t totalBytes = alignUp((tableBytes + inlineSlabBytes) ? (tableBytes + inlineSlabBytes) : 64u, 64u);
    A840RtBuffer *bo = nullptr;
    uint64_t boOff = 0u;
    void *rawCpu = nullptr;
    uint64_t baseVa = 0u;
    const A840RtStatus allocSt = renderEnc
        ? a840_rt_render_alloc_transient(renderEnc, totalBytes, 64u, &bo, &boOff, &rawCpu, &baseVa)
        : a840_rt_compute_alloc_transient(computeEnc, totalBytes, 64u, &bo, &boOff, &rawCpu, &baseVa);
    if (allocSt != A840_RT_OK || !bo || !rawCpu) {
        return nullptr;
    }
    if (outOffset) *outOffset = boOff;
    auto *baseCpu = static_cast<uint8_t *>(rawCpu);
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
            inlineVaMap[tag] = @(baseVa + cursor);
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

- (A840RtStatus)emitVfdForPlan:(A840MTLCompiledPlan *)plan
                       encoder:(A840RtRenderEncoder *)encoder
                vertexBindings:(A840MTLBindings *)vertex
                inlineVertexVa:(const uint64_t *)inlineVa
               inlineVertexLen:(const uint32_t *)inlineLen
                    fallbackBo:(A840RtBuffer *)fallbackBo
                        intoCs:(A840CmdStream *)sdsCs
{
    if (!plan || plan->vfdInputCount == 0u) return A840_RT_OK;
    A840GfxVfdBuffer vbs[A840_MAX_VERTEX_INPUTS] = {};
    uint32_t vbMetalIdx[A840_MAX_VERTEX_INPUTS] = {};
    uint32_t vbCount = 0u;
    A840GfxVfdAttr attrs[A840_MAX_VERTEX_INPUTS] = {};
    const uint64_t zeroVa = (plan->rtDevice && plan->rtDevice->zero_page_va != 0u)
                                ? plan->rtDevice->zero_page_va
                                : (plan->zeroPageBo ? plan->zeroPageBo->gpu_va
                                                    : (fallbackBo ? fallbackBo->gpu_va : 0u));

    for (uint32_t i = 0u; i < plan->vfdInputCount; ++i) {
        const uint32_t loc = plan->vfdInputAttrLocations[i];
        const NmbSpirvVertexInput *refVi = nullptr;
        for (const NmbSpirvVertexInput &vi : plan->reflectedVertexInputs) {
            if (vi.location == loc) {
                refVi = &vi;
                break;
            }
        }
        const NmbVertexAttribute *attr = nullptr;
        const NmbVertexLayout *layout = nullptr;
        if (!refVi) {
            for (uint32_t a = 0u; a < plan->vertexAttributeCount; ++a) {
                if (plan->vertexAttributes[a].attribute_index == loc) {
                    attr = &plan->vertexAttributes[a];
                    break;
                }
            }
            if (attr) {
                for (uint32_t l = 0u; l < plan->vertexLayoutCount; ++l) {
                    if (plan->vertexLayouts[l].buffer_index == attr->buffer_index) {
                        layout = &plan->vertexLayouts[l];
                        break;
                    }
                }
            }
        }
        const uint32_t metalBufIdx = refVi ? refVi->metal_buffer_index : (attr ? attr->buffer_index : 0u);
        const uint32_t attrOffset = refVi ? refVi->offset : (attr ? attr->offset : 0u);
        const bool isConstStep = refVi ? (refVi->stride == 0u || (refVi->input_rate == 1u && refVi->divisor == 0u))
                                       : (layout && layout->step_function == 0u);
        const bool isInstanced = refVi ? (refVi->input_rate == 1u)
                                       : (layout && layout->step_function == 2u);
        const uint32_t stepRate = refVi ? (isInstanced ? refVi->divisor : 0u)
                                        : (isInstanced ? (layout->step_rate ? layout->step_rate : 1u) : 0u);
        uint32_t resolvedStride = 0u;
        if (!isConstStep) {
            if (refVi) {
                resolvedStride = (refVi->stride != UINT32_MAX) ? refVi->stride : 0u;
            } else if (layout && layout->stride != UINT32_MAX) {
                resolvedStride = layout->stride;
            }
        }

        uint32_t vbSlot = UINT32_MAX;
        for (uint32_t b = 0u; b < vbCount; ++b) {
            if (vbMetalIdx[b] == metalBufIdx && vbs[b].stride_bytes == (resolvedStride > 4095u ? 4095u : resolvedStride)) {
                vbSlot = b;
                break;
            }
        }
        if (vbSlot == UINT32_MAX && vbCount < A840_MAX_VERTEX_INPUTS) {
            vbSlot = vbCount++;
            vbMetalIdx[vbSlot] = metalBufIdx;
            uint64_t va = zeroVa;
            uint32_t sizeBytes = 4096u;
            if (vertex) {
                id<MTLBuffer> rawBuf = vertex.buffers[@(metalBufIdx)];
                if ([(id)rawBuf isKindOfClass:[A840MTLBuffer class]]) {
                    auto *nativeBuf = ((A840MTLBuffer *)rawBuf).native;
                    const uint64_t off = vertex.bufferOffsets[@(metalBufIdx)].unsignedLongLongValue;
                    if (nativeBuf && off < nativeBuf->size_bytes) {
                        va = nativeBuf->gpu_va + off;
                        const uint64_t rem = nativeBuf->size_bytes - off;
                        sizeBytes = rem > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(rem);
                        const auto retSt = a840_rt_render_retain_buffer(encoder, nativeBuf);
                        if (retSt != A840_RT_OK) return retSt;
                    }
                } else if (inlineVa && inlineLen && metalBufIdx < 32u && inlineVa[metalBufIdx] != 0u) {
                    va = inlineVa[metalBufIdx];
                    sizeBytes = inlineLen[metalBufIdx];
                }
            }
            uint32_t stride = resolvedStride;
            if (stride > 4095u) stride = 4095u;
            vbs[vbSlot].gpu_va = va;
            vbs[vbSlot].size_bytes = sizeBytes;
            vbs[vbSlot].stride_bytes = stride;
        }

        uint32_t fmtId = A840_FMT6_32_32_32_32_FLOAT;
        uint32_t swap = A840_WZYX;
        uint8_t isFloat = 1u;
        if (refVi) {
            (void)a840_vfd_map_vk_vertex_format(refVi->vk_format, &fmtId, &swap, &isFloat);
        } else if (attr) {
            (void)a840_vfd_map_mtl_vertex_format(attr->mtl_vertex_format, &fmtId, &swap, &isFloat);
        }
        attrs[i].vb_slot = (vbSlot < A840_MAX_VERTEX_INPUTS) ? vbSlot : 0u;
        attrs[i].offset_bytes = (attrOffset <= 4095u) ? attrOffset : 0u;
        attrs[i].format_id = fmtId;
        attrs[i].swap = swap;
        attrs[i].instanced = isInstanced ? 1u : 0u;
        attrs[i].step_rate = stepRate;
        attrs[i].is_float = isFloat;
    }

    const auto est = a840_gfx_sds_emit_vfd(sdsCs, vbCount, vbs, plan->vfdInputCount, attrs);
    return a840_rt_status_from_emit(est);
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
        if (![self buildIdentityBindStateForPlan:plan renderEncoder:nullptr computeEncoder:encoder
                                   stageBindings:stageBindings stageCount:1u
                                    colorTargets:nullptr colorTargetNatives:nullptr colorTargetCount:0u
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
        const A840Ir3StageIoInfo &io = plan->ioInfo[0];
        uint32_t pushDstVec4 = 0u, pushCountVec4 = 0u;
        uint32_t pushWords[kIdentityMaxPushBytes / 4u] = {};
        if (stagePushWindow(stage, io, idRes.pushBuf[0], idRes.pushVec4Count[0],
                            &pushDstVec4, &pushCountVec4, pushWords)) {
            const auto st = a840_rt_compute_set_consts_direct(
                encoder, pushDstVec4, pushCountVec4, stage.constlen_vec4, pushWords);
            if (st != A840_RT_OK) return st;
        }
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
    uint64_t descOff = 0u;
    A840RtBuffer *descBo = [self buildDescriptorTableForPlan:plan renderEncoder:nullptr computeEncoder:encoder
                                               stageBindings:stageBindings stageKinds:&stageKind
                                                  stageCount:1u outOffset:&descOff];
    if (!descBo) return A840_RT_OOM;

    const A840BindingPlan &bp = plan->bindingPlan;
    A840RtBuffer *setBos[ADRENO_COMPILE_MAX_BIND_SETS] = {};
    uint64_t setOffsets[ADRENO_COMPILE_MAX_BIND_SETS] = {};
    uint32_t activeSets = 0u;
    for (uint32_t s = 0u; s < ADRENO_COMPILE_MAX_BIND_SETS; ++s) {
        if (bp.sets[s].table_bytes > 0u) {
            setBos[s] = descBo;
            setOffsets[s] = descOff + bp.sets[s].base_offset_bytes;
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
    return [self bindRender:encoder plan:planObj vertexBindings:vertex fragmentBindings:fragment
               colorTargets:colorTargets colorTargetNatives:nullptr colorTargetCount:colorTargetCount];
}

- (A840RtStatus)bindRender:(A840RtRenderEncoder *)encoder plan:(id)planObj
            vertexBindings:(A840MTLBindings *)vertex fragmentBindings:(A840MTLBindings *)fragment
              colorTargets:(id<MTLTexture> const __unsafe_unretained *)colorTargets
        colorTargetNatives:(A840RtTexture * const *)colorTargetNatives
          colorTargetCount:(uint32_t)colorTargetCount
{
    if (!encoder || ![planObj isKindOfClass:[A840MTLCompiledPlan class]]) return A840_RT_INVALID_ARG;
    auto *plan = static_cast<A840MTLCompiledPlan *>(planObj);
    if (plan->isCompute || plan->stageCount == 0u || plan->stageCount > 2u) return A840_RT_INVALID_ARG;

    const uint32_t stageKinds[2] = {A840_STAGE_VERTEX, A840_STAGE_FRAGMENT};
    A840MTLBindings *stageBindings[2] = {vertex, fragment};

    uint32_t sdsWords[1024];
    A840CmdStream sdsCs{};
    if (a840_cs_init(&sdsCs, sdsWords, 1024u, nullptr, 0u) != A840_EMIT_OK)
        return A840_RT_STATE_ERROR;

    if ([self planUsesIdentityPush:plan]) {
        A840IdentityBindState idRes{};
        if (![self buildIdentityBindStateForPlan:plan renderEncoder:encoder computeEncoder:nullptr
                                   stageBindings:stageBindings stageCount:plan->stageCount
                                    colorTargets:colorTargets colorTargetNatives:colorTargetNatives
                                colorTargetCount:colorTargetCount
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
        if (plan->vfdInputCount > 0u) {
            const auto vfdSt = [self emitVfdForPlan:plan encoder:encoder vertexBindings:vertex
                                     inlineVertexVa:idRes.vertexInlineVa inlineVertexLen:idRes.vertexInlineLen
                                         fallbackBo:idRes.descBo intoCs:&sdsCs];
            if (vfdSt != A840_RT_OK) return vfdSt;
        }
        for (uint32_t s = 0u; s < 2u; ++s) {
            const A840CompiledStage &stage = plan->stages[s];
            const A840Ir3StageIoInfo &io = plan->ioInfo[s];
            uint32_t pushDstVec4 = 0u, pushCountVec4 = 0u;
            uint32_t pushWords[kIdentityMaxPushBytes / 4u] = {};
            if (stagePushWindow(stage, io, idRes.pushBuf[s], idRes.pushVec4Count[s],
                                &pushDstVec4, &pushCountVec4, pushWords)) {
                A840GfxConstUpload up{};
                up.stage = stageKinds[s];
                up.dst_vec4 = pushDstVec4;
                up.count_vec4 = pushCountVec4;
                up.constlen_vec4 = stage.constlen_vec4;
                up.words = pushWords;
                const auto est = a840_gfx_sds_emit_consts(&sdsCs, &up);
                if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
            }
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
        const size_t sdsBytes = alignUp(sdsCs.used_dwords * sizeof(uint32_t), 64u);
        A840RtBuffer *sdsBo = nullptr;
        uint64_t sdsOff = 0u;
        void *sdsCpu = nullptr;
        uint64_t sdsVa = 0u;
        const auto sdsAllocSt = a840_rt_render_alloc_transient(encoder, sdsBytes, 64u, &sdsBo, &sdsOff, &sdsCpu, &sdsVa);
        if (sdsAllocSt != A840_RT_OK || !sdsBo || !sdsCpu) return A840_RT_OOM;
        std::memcpy(sdsCpu, sdsWords, sdsCs.used_dwords * sizeof(uint32_t));

        A840RtSdsGroupDesc grp{};
        grp.group_id = 0u;
        grp.enable_mask = A840_SDS_SYSMEM | A840_SDS_GMEM;
        grp.state_dwords = static_cast<uint32_t>(sdsCs.used_dwords);
        grp.state_buffer = sdsBo;
        grp.state_offset = sdsOff;
        return a840_rt_render_set_sds(encoder, &grp, 1u);
    }

    uint64_t descOff = 0u;
    A840RtBuffer *descBo = [self buildDescriptorTableForPlan:plan renderEncoder:encoder computeEncoder:nullptr
                                               stageBindings:stageBindings stageKinds:stageKinds
                                                  stageCount:2u outOffset:&descOff];
    if (!descBo) return A840_RT_OOM;

    const A840BindingPlan &bp = plan->bindingPlan;
    uint64_t setVas[ADRENO_COMPILE_MAX_BIND_SETS] = {};
    uint32_t activeSets = 0u;
    for (uint32_t s = 0u; s < ADRENO_COMPILE_MAX_BIND_SETS; ++s) {
        if (bp.sets[s].table_bytes > 0u) {
            setVas[s] = descBo->gpu_va + descOff + bp.sets[s].base_offset_bytes;
            activeSets = s + 1u;
        }
    }
    if (activeSets > 0u) {
        const auto est = a840_gfx_sds_emit_bindless(&sdsCs, activeSets, setVas);
        if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    }
    if (plan->vfdInputCount > 0u) {
        const auto vfdSt = [self emitVfdForPlan:plan encoder:encoder vertexBindings:vertex
                                 inlineVertexVa:nullptr inlineVertexLen:nullptr
                                     fallbackBo:descBo intoCs:&sdsCs];
        if (vfdSt != A840_RT_OK) return vfdSt;
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
    const size_t sdsBytes = alignUp(sdsCs.used_dwords * sizeof(uint32_t), 64u);
    A840RtBuffer *sdsBo = nullptr;
    uint64_t sdsOff = 0u;
    void *sdsCpu = nullptr;
    uint64_t sdsVa = 0u;
    const auto sdsAllocSt = a840_rt_render_alloc_transient(encoder, sdsBytes, 64u, &sdsBo, &sdsOff, &sdsCpu, &sdsVa);
    if (sdsAllocSt != A840_RT_OK || !sdsBo || !sdsCpu) return A840_RT_OOM;
    std::memcpy(sdsCpu, sdsWords, sdsCs.used_dwords * sizeof(uint32_t));

    A840RtSdsGroupDesc grp{};
    grp.group_id = 0u;
    grp.enable_mask = A840_SDS_SYSMEM | A840_SDS_GMEM;
    grp.state_dwords = static_cast<uint32_t>(sdsCs.used_dwords);
    grp.state_buffer = sdsBo;
    grp.state_offset = sdsOff;
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
