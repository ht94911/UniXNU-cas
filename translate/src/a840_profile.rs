// SPDX-License-Identifier: MIT

use metal2vulkan::device::{ApiVersion, Device, DeviceDescription, DevicePolicy, FormatProperties};
use metal2vulkan::device::{VkExtension, VkFeature};
use metal2vulkan::error::Error;
use metal2vulkan::metal::{vertex, VertexFormat};
use metal2vulkan::vk::{self, FormatFeatureFlags2 as FormatFlags};

const CHIP_ID: u32 = 0x4405_0a31;
const THREADSIZE_BASE: u32 = 64;
const MAX_WAVES: u32 = 16;
pub(crate) const MAX_WORKGROUP_THREADS: u32 = THREADSIZE_BASE * MAX_WAVES;
const SHARED_MEMORY_BYTES: u32 = 32_768;
const VERTEX_INPUTS: u32 = 32;
const BIND_SETS: u32 = 8;

pub(crate) fn create(extended: bool) -> Result<Device, Error> {
    use VkFeature::*;

    let mut desc = DeviceDescription::new(ApiVersion::V1_2);
    desc.vendor_id = 0x5143;
    desc.device_id = CHIP_ID;
    desc.device_name = "Adreno 840 (Kaanapali)".into();
    // Native paths are selected from the described capabilities. The frozen
    // DevicePolicy selects the address and handle domains only.
    desc.extensions.insert(VkExtension::ExtRobustness2);
    desc.features.extend([
        BufferDeviceAddress,
        ShaderInt64,
        ShaderInt16,
        ShaderFloat16,
        StorageBuffer16BitAccess,
        UniformBufferStandardLayout,
        ScalarBlockLayout,
        ShaderUniformBufferArrayDynamicIndexing,
        ShaderSampledImageArrayDynamicIndexing,
        ShaderStorageBufferArrayDynamicIndexing,
        ShaderStorageImageArrayDynamicIndexing,
        RuntimeDescriptorArray,
        DescriptorBindingPartiallyBound,
        DescriptorBindingSampledImageUpdateAfterBind,
        DescriptorBindingStorageImageUpdateAfterBind,
        DescriptorBindingUniformTexelBufferUpdateAfterBind,
        DescriptorBindingStorageTexelBufferUpdateAfterBind,
        ShaderSampledImageArrayNonUniformIndexing,
        ShaderStorageImageArrayNonUniformIndexing,
        ShaderUniformTexelBufferArrayNonUniformIndexing,
        ShaderStorageTexelBufferArrayNonUniformIndexing,
        ShaderImageGatherExtended,
        ShaderStorageImageExtendedFormats,
        ShaderStorageImageReadWithoutFormat,
        ShaderStorageImageWriteWithoutFormat,
        RobustImageAccess2,
        SamplerAnisotropy,
        SamplerMirrorClampToEdge,
    ]);

    // Core geometry comes from kits/Adreno840/chip_facts_a840.h and its
    // Kaanapali defaults. Remaining limits describe the Wave 1 AOT subset,
    // using the applicable pinned Mesa limits and a 128-byte push window.
    let limits = &mut desc.limits;
    limits.max_compute_work_group_invocations = MAX_WORKGROUP_THREADS;
    limits.max_compute_work_group_size = [limits.max_compute_work_group_invocations; 3];
    limits.max_compute_work_group_count = [65_535; 3];
    limits.max_compute_shared_memory_size = SHARED_MEMORY_BYTES;
    limits.max_push_constants_size = 128;
    limits.max_bound_descriptor_sets = BIND_SETS;
    limits.max_vertex_input_attributes = VERTEX_INPUTS;
    limits.max_vertex_input_bindings = VERTEX_INPUTS;
    limits.max_vertex_output_components = 128;
    limits.max_fragment_input_components = 124;
    limits.max_fragment_output_attachments = 8;
    limits.max_color_attachments = 8;
    limits.max_vertex_input_attribute_offset = 4095;
    limits.max_vertex_input_binding_stride = 2048;
    limits.max_per_stage_descriptor_samplers = 128;
    limits.max_per_stage_descriptor_uniform_buffers = 128;
    limits.max_per_stage_descriptor_storage_buffers = 128;
    limits.max_per_stage_descriptor_sampled_images = 128;
    limits.max_per_stage_descriptor_storage_images = 128;
    limits.max_per_stage_resources = 256;
    limits.max_descriptor_set_samplers = 256;
    limits.max_descriptor_set_uniform_buffers = 256;
    limits.max_descriptor_set_storage_buffers = 256;
    limits.max_descriptor_set_sampled_images = 256;
    limits.max_descriptor_set_storage_images = 256;
    limits.max_descriptor_set_update_after_bind_samplers = 65_536;
    limits.max_descriptor_set_update_after_bind_sampled_images = 65_536;
    limits.max_descriptor_set_update_after_bind_storage_images = 65_536;
    limits.max_per_stage_descriptor_update_after_bind_samplers = 65_536;
    limits.max_per_stage_descriptor_update_after_bind_sampled_images = 65_536;
    limits.max_per_stage_descriptor_update_after_bind_storage_images = 65_536;
    limits.max_per_stage_update_after_bind_resources = 65_536;
    limits.max_uniform_buffer_range = 65_536;
    limits.max_storage_buffer_range = (1 << 31) - 1;
    limits.min_uniform_buffer_offset_alignment = 64;
    limits.min_storage_buffer_offset_alignment = 4;
    limits.min_texel_buffer_offset_alignment = 64;
    limits.max_texel_buffer_elements = (1 << 29) - 1;
    limits.max_image_dimension_1d = 16_384;
    limits.max_image_dimension_2d = 16_384;
    limits.max_image_dimension_3d = 2048;
    limits.max_image_dimension_cube = 16_384;
    limits.max_image_array_layers = 2048;
    limits.max_sampler_lod_bias_bits = (4095.0_f32 / 256.0).to_bits();
    limits.max_sampler_anisotropy_bits = 16.0_f32.to_bits();
    limits.min_texel_offset = -16;
    limits.max_texel_offset = 15;
    limits.min_texel_gather_offset = -32;
    limits.max_texel_gather_offset = 31;

    desc.subgroup.size = THREADSIZE_BASE;
    desc.subgroup.supported_stages = vk::ShaderStageFlags::COMPUTE
        | vk::ShaderStageFlags::VERTEX
        | vk::ShaderStageFlags::FRAGMENT;
    desc.subgroup.supported_operations = vk::SubgroupFeatureFlags::BASIC
        | vk::SubgroupFeatureFlags::VOTE
        | vk::SubgroupFeatureFlags::ARITHMETIC
        | vk::SubgroupFeatureFlags::BALLOT
        | vk::SubgroupFeatureFlags::SHUFFLE
        | vk::SubgroupFeatureFlags::SHUFFLE_RELATIVE
        | vk::SubgroupFeatureFlags::CLUSTERED
        | vk::SubgroupFeatureFlags::QUAD;
    desc.float_controls.f16.rounding_rte = true;
    desc.float_controls.f32.rounding_rte = true;

    let sampling = FormatFlags::SAMPLED_IMAGE | FormatFlags::SAMPLED_IMAGE_FILTER_LINEAR;
    let color = FormatFlags::COLOR_ATTACHMENT | FormatFlags::COLOR_ATTACHMENT_BLEND;
    let storage = FormatFlags::STORAGE_IMAGE;
    for format in [
        vk::Format::R8_UNORM,
        vk::Format::R16_SFLOAT,
        vk::Format::R8G8B8A8_UNORM,
        vk::Format::R16G16B16A16_SFLOAT,
    ] {
        let mut props = FormatProperties::default();
        props.optimal_tiling = sampling | color | storage;
        props.buffer = FormatFlags::UNIFORM_TEXEL_BUFFER | FormatFlags::STORAGE_TEXEL_BUFFER;
        props.color_attachment_sample_counts = vk::SampleCountFlags::COUNT_1;
        desc.formats.formats.insert(format, props);
    }
    for format in [
        vk::Format::B8G8R8A8_UNORM,
        vk::Format::B8G8R8A8_SRGB,
        vk::Format::R8G8B8A8_SRGB,
    ] {
        let mut props = FormatProperties::default();
        props.optimal_tiling = sampling | color;
        props.color_attachment_sample_counts = vk::SampleCountFlags::COUNT_1;
        desc.formats.formats.insert(format, props);
    }
    if extended {
        describe_extended_formats(&mut desc);
        // The translator marks Workgroup initializers. The AOT bridge lowers them
        // to explicit stores and a barrier before handing SPIR-V to IR3.
        desc.extensions.extend([
            VkExtension::KhrZeroInitializeWorkgroupMemory,
            VkExtension::KhrVertexAttributeDivisor,
            VkExtension::KhrDynamicRendering,
            VkExtension::KhrMaintenance5,
            VkExtension::ExtShaderDemoteToHelperInvocation,
        ]);
        desc.features.extend([
            ShaderZeroInitializeWorkgroupMemory,
            VertexAttributeInstanceRateDivisor,
            IndependentBlend,
            SampleRateShading,
            DepthClamp,
            ShaderClipDistance,
            ShaderOutputLayer,
            ShaderOutputViewportIndex,
            FragmentStoresAndAtomics,
            VertexPipelineStoresAndAtomics,
            ImageCubeArray,
            SamplerFilterMinmax,
            ShaderDemoteToHelperInvocation,
            ShaderInt8,
            StorageBuffer8BitAccess,
            UniformAndStorageBuffer8BitAccess,
            UniformAndStorageBuffer16BitAccess,
            ShaderSubgroupExtendedTypes,
            NullDescriptor,
        ]);
        desc.limits.max_vertex_attrib_divisor = u32::MAX;
        desc.limits.supports_non_zero_first_instance = true;
    }
    Device::from_queries(desc, DevicePolicy::default())
}

fn describe_extended_formats(desc: &mut DeviceDescription) {
    use vk::Format as F;
    let samples = vk::SampleCountFlags::COUNT_1
        | vk::SampleCountFlags::COUNT_2
        | vk::SampleCountFlags::COUNT_4;
    desc.limits.framebuffer_color_sample_counts = samples;
    desc.limits.framebuffer_integer_color_sample_counts = samples;
    desc.limits.framebuffer_depth_sample_counts = samples;
    desc.limits.framebuffer_stencil_sample_counts = samples;
    desc.limits.framebuffer_no_attachments_sample_counts = samples;
    for props in desc.formats.formats.values_mut() {
        props.color_attachment_sample_counts = samples;
    }

    let color_blend_sample = FormatFlags::COLOR_ATTACHMENT
        | FormatFlags::COLOR_ATTACHMENT_BLEND
        | FormatFlags::SAMPLED_IMAGE
        | FormatFlags::SAMPLED_IMAGE_FILTER_LINEAR;
    let storage = FormatFlags::STORAGE_IMAGE;

    // Full Adreno 840 hardware color+blend+sample+storage formats (kits/Adreno840/caps/formats.tsv).
    for format in [
        F::R8_UNORM,
        F::R8_SNORM,
        F::R8G8_UNORM,
        F::R8G8_SNORM,
        F::R8G8B8A8_UNORM,
        F::R8G8B8A8_SNORM,
        F::B8G8R8A8_UNORM,
        F::R16_UNORM,
        F::R16_SNORM,
        F::R16_SFLOAT,
        F::R16G16_UNORM,
        F::R16G16_SNORM,
        F::R16G16_SFLOAT,
        F::R16G16B16A16_UNORM,
        F::R16G16B16A16_SNORM,
        F::R16G16B16A16_SFLOAT,
        F::R32_SFLOAT,
        F::R32G32_SFLOAT,
        F::R32G32B32A32_SFLOAT,
        F::A2B10G10R10_UNORM_PACK32,
        F::A2R10G10B10_UNORM_PACK32,
        F::B10G11R11_UFLOAT_PACK32,
    ] {
        let props = desc.formats.formats.entry(format).or_default();
        props.optimal_tiling |= color_blend_sample | storage;
        props.color_attachment_sample_counts = samples;
    }

    // Color+blend+sample formats (sRGB, A8_UNORM via KHR_maintenance5, packed 16-bit).
    for format in [
        F::A8_UNORM,
        F::R8_SRGB,
        F::R8G8_SRGB,
        F::R8G8B8A8_SRGB,
        F::B8G8R8A8_SRGB,
        F::R5G6B5_UNORM_PACK16,
        F::B5G6R5_UNORM_PACK16,
        F::R5G5B5A1_UNORM_PACK16,
        F::B5G5R5A1_UNORM_PACK16,
        F::A1R5G5B5_UNORM_PACK16,
        F::R4G4B4A4_UNORM_PACK16,
        F::B4G4R4A4_UNORM_PACK16,
        F::E5B9G9R9_UFLOAT_PACK32,
    ] {
        let props = desc.formats.formats.entry(format).or_default();
        props.optimal_tiling |= color_blend_sample;
        props.color_attachment_sample_counts = samples;
    }

    // Integer color+sample+storage formats (including R16G16B16A16_UINT/SINT & A2B10G10R10_UINT).
    for format in [
        F::R8_UINT,
        F::R8_SINT,
        F::R8G8_UINT,
        F::R8G8_SINT,
        F::R8G8B8A8_UINT,
        F::R8G8B8A8_SINT,
        F::R16_UINT,
        F::R16_SINT,
        F::R16G16_UINT,
        F::R16G16_SINT,
        F::R16G16B16A16_UINT,
        F::R16G16B16A16_SINT,
        F::R32_UINT,
        F::R32_SINT,
        F::R32G32_UINT,
        F::R32G32_SINT,
        F::R32G32B32A32_UINT,
        F::R32G32B32A32_SINT,
        F::A2B10G10R10_UINT_PACK32,
        F::A2R10G10B10_UINT_PACK32,
    ] {
        let props = desc.formats.formats.entry(format).or_default();
        props.optimal_tiling |= FormatFlags::COLOR_ATTACHMENT | FormatFlags::SAMPLED_IMAGE | storage;
        props.color_attachment_sample_counts = samples;
    }

    // Sampled ASTC LDR + sRGB formats.
    for format in [
        F::ASTC_4X4_UNORM_BLOCK,
        F::ASTC_4X4_SRGB_BLOCK,
        F::ASTC_5X4_UNORM_BLOCK,
        F::ASTC_5X4_SRGB_BLOCK,
        F::ASTC_5X5_UNORM_BLOCK,
        F::ASTC_5X5_SRGB_BLOCK,
        F::ASTC_6X5_UNORM_BLOCK,
        F::ASTC_6X5_SRGB_BLOCK,
        F::ASTC_6X6_UNORM_BLOCK,
        F::ASTC_6X6_SRGB_BLOCK,
        F::ASTC_8X5_UNORM_BLOCK,
        F::ASTC_8X5_SRGB_BLOCK,
        F::ASTC_8X6_UNORM_BLOCK,
        F::ASTC_8X6_SRGB_BLOCK,
        F::ASTC_8X8_UNORM_BLOCK,
        F::ASTC_8X8_SRGB_BLOCK,
        F::ASTC_10X5_UNORM_BLOCK,
        F::ASTC_10X5_SRGB_BLOCK,
        F::ASTC_10X6_UNORM_BLOCK,
        F::ASTC_10X6_SRGB_BLOCK,
        F::ASTC_10X8_UNORM_BLOCK,
        F::ASTC_10X8_SRGB_BLOCK,
        F::ASTC_10X10_UNORM_BLOCK,
        F::ASTC_10X10_SRGB_BLOCK,
        F::ASTC_12X10_UNORM_BLOCK,
        F::ASTC_12X10_SRGB_BLOCK,
        F::ASTC_12X12_UNORM_BLOCK,
        F::ASTC_12X12_SRGB_BLOCK,
    ] {
        desc.formats.formats.entry(format).or_default().optimal_tiling |=
            FormatFlags::SAMPLED_IMAGE | FormatFlags::SAMPLED_IMAGE_FILTER_LINEAR;
    }

    // Depth and stencil attachments (with sampling support).
    for format in [
        F::D16_UNORM,
        F::D32_SFLOAT,
        F::D32_SFLOAT_S8_UINT,
        F::D24_UNORM_S8_UINT,
    ] {
        desc.formats
            .formats
            .entry(format)
            .or_default()
            .optimal_tiling |= FormatFlags::DEPTH_STENCIL_ATTACHMENT
            | FormatFlags::SAMPLED_IMAGE
            | FormatFlags::SAMPLED_IMAGE_FILTER_LINEAR;
    }
    desc.formats
        .formats
        .entry(F::S8_UINT)
        .or_default()
        .optimal_tiling |= FormatFlags::DEPTH_STENCIL_ATTACHMENT | FormatFlags::SAMPLED_IMAGE;

    // These Metal fetch formats map to native vertex pipe formats in the pinned
    // Mesa fd6 format table. Metal FloatRGB9E5 has no exact fixed-function fetch.
    for raw in 1..=VertexFormat::FloatRG11B10.as_raw() {
        if let Ok(format) = VertexFormat::try_from(raw) {
            if let Ok(vk_format) = vertex::vk_vertex_format(format) {
                desc.formats.formats.entry(vk_format).or_default().buffer |=
                    FormatFlags::VERTEX_BUFFER;
            }
        }
    }

    for format in [
        F::R8_USCALED,
        F::R8_SSCALED,
        F::R8G8_USCALED,
        F::R8G8_SSCALED,
        F::R8G8B8_USCALED,
        F::R8G8B8_SSCALED,
        F::R8G8B8A8_USCALED,
        F::R8G8B8A8_SSCALED,
        F::R16_USCALED,
        F::R16_SSCALED,
        F::R16G16_USCALED,
        F::R16G16_SSCALED,
        F::R16G16B16_USCALED,
        F::R16G16B16_SSCALED,
        F::R16G16B16A16_USCALED,
        F::R16G16B16A16_SSCALED,
    ] {
        desc.formats.formats.entry(format).or_default().buffer |= FormatFlags::VERTEX_BUFFER;
    }
}
