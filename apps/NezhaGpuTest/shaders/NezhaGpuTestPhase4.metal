/* SPDX-License-Identifier: MIT */
#include <metal_stdlib>
using namespace metal;

/* Metal 3.0 source. Resource creation, alias destruction/rebinding, compute
 * producer -> indirect consumer barriers and resetCommandAtIndex live in the
 * downstream runner. The kernels do not emulate those driver operations. */
struct Phase4Varying {
    float4 position [[position]];
    float4 color;
};
struct BindlessIndex {
    uint buffer_slots[4];
    uint element_indices[4];
    uint texture_slots[2];
};
struct Phase4ArgumentBuffer {
    array<device const uint *, 4> buffers [[id(0)]];
    array<texture2d<float, access::sample>, 2> textures [[id(4)]];
    sampler sampling [[id(6)]];
    device const BindlessIndex *indices [[id(7)]];
    float weight0 [[id(8)]];
    float weight1 [[id(9)]];
    float weight2 [[id(10)]];
    float weight3 [[id(11)]];
    float weight4 [[id(12)]];
    float weight5 [[id(13)]];
    uint nearest [[id(14)]];
};
struct DrawArguments { uint vertex_count, instance_count, vertex_start, base_instance; };
struct DrawIndexedArguments {
    uint index_count, instance_count, index_start;
    int base_vertex;
    uint base_instance;
};
struct DispatchArguments { uint x, y, z; };
struct Phase4IndirectArguments {
    DrawArguments draw;
    DrawIndexedArguments indexed;
    DispatchArguments dispatch;
};
struct IntVertex { int x, y; };
struct IcbDrawRecord {
    uint slot, generation, base_instance;
    float x, y, width, height;
    uint bgra;
};

static float4 bgra_codes(uint value) {
    return float4((value >> 16) & 255u, (value >> 8) & 255u,
                  value & 255u, value >> 24);
}

kernel void heap_linear_buffer_kernel(device uchar *target [[buffer(0)]],
                                      constant uint &byte_count [[buffer(1)]],
                                      uint gid [[thread_position_in_grid]]) {
    if (gid < byte_count) target[gid] = uchar((gid & 255u) * 37u + 19u);
}

vertex Phase4Varying heap_alias_vertex(uint index [[vertex_id]]) {
    const float2 corners[3] = {float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)};
    Phase4Varying output;
    output.position = float4(corners[index], 0.0f, 1.0f);
    output.color = float4(1.0f);
    return output;
}

fragment float4 heap_alias_fragment(Phase4Varying input [[stage_in]]) {
    const uint2 p = uint2(input.position.xy);
    const uint b = (p.x * 17u + p.y * 3u + 41u) & 255u;
    const uint g = (p.x * 5u + p.y * 29u + 67u) & 255u;
    const uint r = (p.x * 11u + p.y * 7u + 113u) & 255u;
    return float4(float(r), float(g), float(b), 255.0f) / 255.0f;
}

kernel void heap_rgba16f_kernel(texture2d<half, access::write> output [[texture(0)]],
                                uint2 gid [[thread_position_in_grid]]) {
    if (gid.x < output.get_width() && gid.y < output.get_height()) {
        const half4 value = half4(half(gid.x & 15u) / half(16.0f),
                                 half(gid.y & 15u) / half(16.0f),
                                 half((gid.x + 3u * gid.y) & 15u) / half(16.0f),
                                 half(1.0f));
        output.write(value, gid);
    }
}

kernel void argbuf_composite_kernel(constant Phase4ArgumentBuffer &args [[buffer(0)]],
                                    texture2d<float, access::write> output [[texture(0)]],
                                    uint2 gid [[thread_position_in_grid]]) {
    const uint2 extent(output.get_width(), output.get_height());
    if (gid.x >= extent.x || gid.y >= extent.y) return;
    const BindlessIndex index = args.indices[gid.y * extent.x + gid.x];
    const float weights[6] = {args.weight0, args.weight1, args.weight2,
                              args.weight3, args.weight4, args.weight5};
    float4 value(0.0f);
    for (uint k = 0; k < 4; ++k)
        value += bgra_codes(args.buffers[index.buffer_slots[k]][index.element_indices[k]]) * weights[k];
    const float2 uv = (float2(gid) + 0.5f) / float2(extent);
    for (uint k = 0; k < 2; ++k) {
        const float2 coordinate = k == 0 ? uv : float2(1.0f - uv.x, uv.y);
        float4 codes = args.textures[index.texture_slots[k]].sample(args.sampling, coordinate, level(0.0f)) * 255.0f;
        // Recover exact texel codes for nearest; keep interpolation for linear.
        if (args.nearest != 0) codes = floor(codes + 0.5f);
        value += codes * weights[k + 4];
    }
    // Explicit rounding leaves BGRA8 conversion away from half-code ties.
    output.write(floor(clamp(value, 0.0f, 255.0f) + 0.5f) / 255.0f, gid);
}

kernel void fill_indirect_args_kernel(device Phase4IndirectArguments *output [[buffer(0)]],
                                      constant uint &instances [[buffer(1)]],
                                      uint gid [[thread_position_in_grid]]) {
    if (gid != 0) return;
    output->draw.vertex_count = 6;
    output->draw.instance_count = instances;
    output->draw.vertex_start = 6;
    output->draw.base_instance = 2;
    output->indexed.index_count = 6;
    output->indexed.instance_count = instances;
    output->indexed.index_start = 3;
    output->indexed.base_vertex = -4;
    output->indexed.base_instance = 1;
    output->dispatch.x = 3;
    output->dispatch.y = 2;
    output->dispatch.z = 1;
}

kernel void indirect_dispatch_kernel(device uint *output [[buffer(0)]],
                                      constant uint3 &extent [[buffer(1)]],
                                      uint3 gid [[thread_position_in_grid]]) {
    if (all(gid < extent))
        output[8u + (gid.z * extent.y + gid.y) * extent.x + gid.x] =
            0x10000000u + (gid.x + 1u) * 17u + (gid.y + 1u) * 257u + (gid.z + 1u) * 65537u;
}

vertex Phase4Varying indirect_vertex(device const IntVertex *vertices [[buffer(0)]],
                                     constant uint2 &extent [[buffer(1)]],
                                     constant uint &indexed [[buffer(2)]],
                                     uint index [[vertex_id]],
                                     uint instance [[instance_id]]) {
    const IntVertex vertex = vertices[index];
    const float2 p(float(vertex.x) + float(instance * 5u + (indexed != 0 ? 32u : 0u)),
                   float(vertex.y) + (indexed != 0 ? 16.0f : 0.0f));
    Phase4Varying output;
    output.position = float4(p.x * 2.0f / float(extent.x) - 1.0f,
                             1.0f - p.y * 2.0f / float(extent.y), 0.0f, 1.0f);
    output.color = float4(float(indexed != 0 ? 77u : 201u),
                          float((23u + instance * 5u) & 255u),
                          float((13u + instance * 3u) & 255u), 255.0f) / 255.0f;
    return output;
}

fragment float4 indirect_fragment(Phase4Varying input [[stage_in]]) {
    return input.color;
}

vertex Phase4Varying icb_vertex(constant IcbDrawRecord &draw [[buffer(0)]],
                               constant uint2 &extent [[buffer(1)]],
                               device atomic_uint *slot_execution_count [[buffer(2)]],
                               uint index [[vertex_id]],
                               uint instance [[instance_id]]) {
    const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(1, 1),
                               float2(0, 0), float2(1, 1), float2(0, 1)};
    const float2 p = float2(draw.x + float(instance * 5u), draw.y) +
                     corners[index] * float2(draw.width, draw.height);
    if (index == 0 && instance == draw.base_instance && draw.slot < 16)
        atomic_fetch_add_explicit(&slot_execution_count[draw.slot], 1u, memory_order_relaxed);
    Phase4Varying output;
    output.position = float4(p.x * 2.0f / float(extent.x) - 1.0f,
                             1.0f - p.y * 2.0f / float(extent.y), 0.0f, 1.0f);
    const uint red = (((draw.bgra >> 16) & 255u) + draw.generation * 37u + instance * 7u) & 255u;
    output.color = float4(float(red), float((draw.bgra >> 8) & 255u),
                          float(draw.bgra & 255u), float(draw.bgra >> 24)) / 255.0f;
    return output;
}

fragment float4 icb_fragment(Phase4Varying input [[stage_in]],
                             device atomic_uint *pixel_execution_count [[buffer(0)]],
                             constant uint2 &extent [[buffer(1)]]) {
    const uint2 p = uint2(input.position.xy);
    if (all(p < extent))
        atomic_fetch_add_explicit(&pixel_execution_count[p.y * extent.x + p.x], 1u, memory_order_relaxed);
    return input.color;
}
