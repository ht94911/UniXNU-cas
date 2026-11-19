#include <metal_stdlib>
using namespace metal;
kernel void sample_nv12(texture2d<float,access::sample> source [[texture(0)]],
                        device float4 *output [[buffer(0)]],uint2 gid [[thread_position_in_grid]]) {
    constexpr sampler s(coord::pixel,filter::nearest,address::clamp_to_edge);
    output[gid.y*64+gid.x]=source.sample(s,float2(gid)+0.5f);
}
