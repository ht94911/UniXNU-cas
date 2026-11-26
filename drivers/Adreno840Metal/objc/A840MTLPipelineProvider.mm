/* SPDX-License-Identifier: MIT */
/* Apple SDK and arm64e linkage are qualified by Mac CI.
 * End-to-end native Phase 2 pipeline provider:
 *   .metallib -> libnative_metal_translate.a -> libadreno_compile_ir3.a ->
 *   a840_binding_plan -> GPU ISA & bindless descriptor BO upload -> A8XX PM4. */
#import "A840MTLPipelineProvider.h"
#import "A840MTLEncoders.h"
#import "A840MTLResource.h"

#include "AdrenoCompileABI.h"
#include "NativeMetalTranslate.h"
#include "a840_binding_plan.h"
#include "a840_gfx_bind_sds.h"
#include "a840_ir3_io_info.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wprotocol"

namespace {

constexpr uint32_t kDescTypeUniformBuffer = 6u;
constexpr uint32_t kDescTypeStorageBuffer = 7u;
constexpr uint32_t kDescTypeUniformBufferDynamic = 8u;
constexpr uint64_t kKaanapaliChipId = UINT64_C(1141180977); /* 0x44050a31 */

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
    desc.alignment = 256u;
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
    const size_t perItem = alignUp(privateMemBytes, 512u);
    const size_t totalBytes = alignUp(perItem * 128u * 6u, 4096u);
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

@implementation A840MTLFunction {
    NSString *_name;
    MTLFunctionType _type;
}
@synthesize label = _label;

- (instancetype)initWithLibrary:(A840MTLLibrary *)library name:(NSString *)name
                   functionType:(MTLFunctionType)functionType
{
    if (!(self = [super init])) return nil;
    _ownerLibrary = library;
    _name = [name copy];
    _type = functionType;
    return self;
}
- (id<MTLDevice>)device { return _ownerLibrary.device; }
- (NSString *)name { return _name; }
- (MTLFunctionType)functionType { return _type; }
@end

@implementation A840MTLLibrary {
    __weak A840MTLDevice *_device;
    NSDictionary<NSString *, NSNumber *> *_functions;
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

    NmbDevice *dev = nmb_device_create_a840(0u);
    if (!dev) {
        if (error) *error = a840_metal_error(A840_RT_STATE_ERROR, @"Failed to create translator device");
        return nil;
    }
    NmbLibrary *lib = nmb_library_parse(dev, static_cast<const uint8_t *>(_metallibData.bytes),
                                        _metallibData.length);
    if (!lib) {
        const char *msg = nmb_last_error();
        if (error) {
            NSString *desc = msg && msg[0] ? [NSString stringWithUTF8String:msg] : @"Failed to parse metallib";
            *error = a840_metal_error(A840_RT_INVALID_ARG, desc);
        }
        nmb_device_destroy(dev);
        return nil;
    }

    NSMutableDictionary<NSString *, NSNumber *> *map = [NSMutableDictionary new];
    const size_t count = nmb_library_function_count(lib);
    char nameBuf[512];
    for (size_t i = 0u; i < count; ++i) {
        if (!nmb_library_function_name(lib, i, nameBuf, sizeof(nameBuf))) continue;
        uint32_t kind = NMB_FUNCTION_KERNEL;
        (void)nmb_library_function_stage_kind(lib, i, &kind);
        MTLFunctionType ftype = MTLFunctionTypeKernel;
        if (kind == NMB_FUNCTION_VERTEX) ftype = MTLFunctionTypeVertex;
        else if (kind == NMB_FUNCTION_FRAGMENT) ftype = MTLFunctionTypeFragment;
        NSString *fnName = [NSString stringWithUTF8String:nameBuf];
        if (fnName) map[fnName] = @(ftype);
    }
    nmb_library_destroy(lib);
    nmb_device_destroy(dev);
    _functions = [map copy];
    return self;
}

- (id<MTLDevice>)device { return _device; }
- (NSArray<NSString *> *)functionNames { return _functions.allKeys; }

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    NSNumber *kind = name ? _functions[name] : nil;
    if (!kind) return nil;
    return [[A840MTLFunction alloc] initWithLibrary:self name:name
                                       functionType:static_cast<MTLFunctionType>(kind.unsignedIntegerValue)];
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
    NSMutableArray<NSValue *> *transientBuffers;
}
@end

@implementation A840MTLCompiledPlan
- (instancetype)init
{
    if ((self = [super init])) {
        transientBuffers = [NSMutableArray new];
    }
    return self;
}
- (void)dealloc
{
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

- (id)prepareCompute:(id<MTLFunction>)function forDevice:(A840MTLDevice *)device
          descriptor:(A840RtComputePipelineDesc *)descriptor
{
    if (![function isKindOfClass:[A840MTLFunction class]] || !device || !descriptor) return nil;
    auto *fn = static_cast<A840MTLFunction *>(function);
    NSData *data = fn.ownerLibrary.metallibData;
    const char *entry = fn.name.UTF8String;
    if (!data || !entry) return nil;

    NmbLibrary *lib = nmb_library_parse(_translator, static_cast<const uint8_t *>(data.bytes), data.length);
    if (!lib) return nil;

    NmbComputePipelineDesc cdesc{};
    NmbSpirvResult *spv = nmb_compile_compute_spirv_ex(_translator, lib, entry, &cdesc);
    nmb_library_destroy(lib);
    if (!spv) return nil;

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

    const A840CompileTarget target = makeKaanapaliTarget();
    A840StageInput stageIn{};
    stageIn.abi_version = ADRENO_COMPILE_ABI_VERSION;
    stageIn.stage = A840_STAGE_COMPUTE;
    stageIn.spirv_words = spvWords;
    stageIn.spirv_bytes = spvBytes;
    stageIn.entry_point = spvEntry;
    stageIn.sample_count = 1u;
    stageIn.flags = target.flags;

    char errBuf[256] = {};
    const auto cst = a840_compile_stage(&target, &stageIn, &plan->stages[0], errBuf, sizeof(errBuf));
    if (cst != A840_COMPILE_OK || !a840_ir3_get_stage_io_info(&plan->stages[0], &plan->ioInfo[0])) {
        nmb_spirv_result_free(spv);
        return nil;
    }

    const size_t bcount = nmb_spirv_binding_count(spv);
    std::vector<NmbSpirvBinding> bindings(bcount);
    for (size_t i = 0u; i < bcount; ++i)
        (void)nmb_spirv_binding(spv, i, &bindings[i]);
    NmbSpirvPushConstantRange pushRange{};
    (void)nmb_spirv_push_constant_range(spv, 0u, &pushRange);
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
    return plan;
}

- (id)prepareRender:(MTLRenderPipelineDescriptor *)pipeline forDevice:(A840MTLDevice *)device
         descriptor:(A840RtGraphicsProgramDesc *)descriptor
{
    if (!pipeline || !device || !descriptor) return nil;
    if (![pipeline.vertexFunction isKindOfClass:[A840MTLFunction class]] ||
        ![pipeline.fragmentFunction isKindOfClass:[A840MTLFunction class]])
        return nil;

    auto *vsFn = static_cast<A840MTLFunction *>(pipeline.vertexFunction);
    auto *fsFn = static_cast<A840MTLFunction *>(pipeline.fragmentFunction);
    if (vsFn.ownerLibrary != fsFn.ownerLibrary) return nil;
    NSData *data = vsFn.ownerLibrary.metallibData;
    const char *vsName = vsFn.name.UTF8String;
    const char *fsName = fsFn.name.UTF8String;
    if (!data || !vsName || !fsName) return nil;

    NmbLibrary *lib = nmb_library_parse(_translator, static_cast<const uint8_t *>(data.bytes), data.length);
    if (!lib) return nil;

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
    if (rdesc.color_attachment_count == 0u) {
        rdesc.color_attachment_count = 1u;
        rdesc.color_mtl_formats[0] = static_cast<uint32_t>(MTLPixelFormatBGRA8Unorm);
        colorMask = 1u;
    }

    NmbSpirvResult *spv = nmb_compile_render_spirv_ex(_translator, lib, vsName, fsName, &rdesc);
    nmb_library_destroy(lib);
    if (!spv || nmb_spirv_stage_count(spv) < 2u) {
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
    (void)nmb_spirv_push_constant_range(spv, 1u, &pushRanges[1]);
    nmb_spirv_result_free(spv);

    const auto bpst = a840_binding_plan_build(bindings.empty() ? nullptr : bindings.data(),
                                              bindings.size(), plan->stages, 2u,
                                              pushRanges, &plan->bindingPlan);
    if (bpst != A840_BINDING_PLAN_OK) return nil;

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
        if (vsIo.inputs[i].sysval) continue;
        ed.vertex_inputs[ed.vertex_input_count].regid = vsIo.inputs[i].regid;
        ed.vertex_inputs[ed.vertex_input_count].component_mask = vsIo.inputs[i].compmask;
        ++ed.vertex_input_count;
    }

    for (uint32_t i = 0u; i < fsIo.input_count && ed.varying_count < ADRENO_COMPILE_MAX_VARYINGS; ++i) {
        const A840Ir3InputSlot &fin = fsIo.inputs[i];
        if (!fin.bary || fin.compmask == 0u) continue;
        uint8_t vsReg = A840_IR3_REGID_UNUSED;
        for (uint32_t j = 0u; j < vsIo.output_count; ++j) {
            if (vsIo.outputs[j].slot == fin.slot) {
                vsReg = vsIo.outputs[j].regid;
                break;
            }
        }
        if (vsReg >= A840_IR3_REGID_UNUSED) return nil;
        ed.varyings[ed.varying_count].vs_regid = vsReg;
        ed.varyings[ed.varying_count].component_mask = fin.compmask;
        ed.varyings[ed.varying_count].flat = fin.flat != 0u;
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
    ed.fs_inputs.frag_coord = fsIo.fs_frag_coord_regid;
    ed.fs_inputs.frag_coord_mask = fsIo.fs_frag_coord_mask;
    ed.fs_inputs.shading_rate = fsIo.fs_shading_rate_regid;
    ed.depth_output = fsIo.fs_depth_out_regid;
    ed.stencil_output = fsIo.fs_stencil_out_regid;
    ed.sample_mask_output = fsIo.fs_sample_mask_out_regid;
    ed.pixel_lod = fsIo.fs_pixel_lod != 0u;
    ed.full_quad = fsIo.fs_full_quad != 0u;
    ed.fs_thread128 = fsIo.fs_thread128 != 0u;
    for (uint32_t i = 0u; i < A840_MAX_COLOR_TARGETS; ++i) {
        ed.colors[i].regid = fsIo.fs_colors[i].regid;
        ed.colors[i].component_mask = fsIo.fs_colors[i].compmask;
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
    desc.alignment = 256u;
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
    if (!encoder || ![planObj isKindOfClass:[A840MTLCompiledPlan class]]) return A840_RT_INVALID_ARG;
    auto *plan = static_cast<A840MTLCompiledPlan *>(planObj);
    if (!plan->isCompute) return A840_RT_INVALID_ARG;

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
    if (!encoder || ![planObj isKindOfClass:[A840MTLCompiledPlan class]]) return A840_RT_INVALID_ARG;
    auto *plan = static_cast<A840MTLCompiledPlan *>(planObj);
    if (plan->isCompute || plan->stageCount != 2u) return A840_RT_INVALID_ARG;

    const uint32_t stageKinds[2] = {A840_STAGE_VERTEX, A840_STAGE_FRAGMENT};
    A840MTLBindings *stageBindings[2] = {vertex, fragment};
    A840RtBuffer *descBo = [self buildDescriptorTableForPlan:plan stageBindings:stageBindings
                                                  stageKinds:stageKinds stageCount:2u];
    if (!descBo) return A840_RT_OOM;

    uint32_t sdsWords[512];
    A840CmdStream sdsCs{};
    if (a840_cs_init(&sdsCs, sdsWords, 512u, nullptr, 0u) != A840_EMIT_OK)
        return A840_RT_STATE_ERROR;

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
    sdsBoDesc.alignment = 256u;
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
    return [[A840MTLDevice alloc] initWithTransport:transport fallbackFactory:nil
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
    return [[A840MTLDevice alloc] initWithTransport:transport fallbackFactory:nil
                                   pipelineProvider:provider error:error];
}
