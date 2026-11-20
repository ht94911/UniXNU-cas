#include <metal_stdlib>
using namespace metal;

// These are independent readers of the original replay's bound resources.
kernel void replay_uniform(constant uint *input [[buffer(0)]],device uint *output [[buffer(1)]],
                           uint gid [[thread_position_in_grid]]) {
    if(gid<44)output[gid]=input[gid];
}

kernel void replay_filter(texture2d<float,access::read> input [[texture(3)]],device float4 *output [[buffer(1)]],
                          uint gid [[thread_position_in_grid]]) {
    if(gid<64)output[gid]=input.read(uint2(gid%2,gid/2));
}

kernel void replay_sampler(texture2d<half,access::sample> input [[texture(0)]],sampler s [[sampler(0)]],
                           device float4 *output [[buffer(1)]],uint gid [[thread_position_in_grid]]) {
    if(gid<64)output[gid]=float4(input.sample(s,float2(gid%16,gid/16)+0.5f));
}
