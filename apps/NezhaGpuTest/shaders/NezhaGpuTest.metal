/* SPDX-License-Identifier: MIT */
#include <metal_stdlib>
using namespace metal;

struct TestVertex { float x, y, r, g, b, a, u, v; };
struct TestVarying {
    float4 position [[position]];
    float4 color;
    float2 uv;
};

kernel void add_vec(device const float *a [[buffer(0)]],
                    device const float *b [[buffer(1)]],
                    device float *output [[buffer(2)]],
                    constant uint &count [[buffer(3)]],
                    constant float &multiplier [[buffer(4)]],
                    uint gid [[thread_position_in_grid]]) {
    if (gid < count) output[gid] = a[gid] + b[gid] * multiplier;
}

kernel void nonuniform_grid(device uint *output [[buffer(0)]],
                            constant uint2 &extent [[buffer(1)]],
                            uint2 gid [[thread_position_in_grid]]) {
    if (gid.x < extent.x && gid.y < extent.y)
        output[gid.y * extent.x + gid.x] = (gid.x + 1) * 17 + (gid.y + 1) * 257;
}

kernel void clear_bgra8(texture2d<float, access::write> target [[texture(0)]],
                        constant uint4 &rectangle [[buffer(0)]],
                        constant float4 &color [[buffer(1)]],
                        uint2 gid [[thread_position_in_grid]]) {
    if (gid.x < rectangle.z && gid.y < rectangle.w)
        target.write(color, gid + rectangle.xy);
}

kernel void scale_texture(texture2d<float, access::sample> source [[texture(0)]],
                          texture2d<float, access::write> destination [[texture(1)]],
                          sampler sampling [[sampler(0)]],
                          uint2 gid [[thread_position_in_grid]]) {
    uint2 extent(destination.get_width(), destination.get_height());
    if (gid.x < extent.x && gid.y < extent.y) {
        float2 uv = (float2(gid) + 0.5f) / float2(extent);
        destination.write(source.sample(sampling, uv, level(0.0f)), gid);
    }
}

kernel void fence_step(device uint *state [[buffer(0)]],
                       device uint *observed [[buffer(1)]],
                       constant uint &index [[buffer(2)]],
                       uint gid [[thread_position_in_grid]]) {
    if (gid == 0) {
        observed[index] = state[index];
        state[index + 1] = state[index] + 1;
    }
}

kernel void fence_verify(device const uint *state [[buffer(0)]],
                         device uint *observed_after [[buffer(1)]],
                         constant uint &index [[buffer(2)]],
                         uint gid [[thread_position_in_grid]]) {
    if (gid == 0) observed_after[index] = state[index + 1];
}

vertex TestVarying test_vertex(device const TestVertex *vertices [[buffer(0)]],
                               constant float2 &extent [[buffer(1)]],
                               uint index [[vertex_id]]) {
    TestVertex value = vertices[index];
    TestVarying output;
    output.position = float4(value.x * 2.0f / extent.x - 1.0f,
                             1.0f - value.y * 2.0f / extent.y, 0.0f, 1.0f);
    output.color = float4(value.r, value.g, value.b, value.a);
    output.uv = float2(value.u, value.v);
    return output;
}

fragment float4 triangle_fragment(TestVarying input [[stage_in]]) {
    return input.color;
}

fragment float4 textured_fragment(TestVarying input [[stage_in]],
                                   texture2d<float, access::sample> texture [[texture(0)]],
                                   sampler sampling [[sampler(0)]]) {
    return texture.sample(sampling, input.uv) * input.color;
}
