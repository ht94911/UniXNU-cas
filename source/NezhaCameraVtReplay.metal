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
    if(gid<64) {
        const uint2 size=uint2(input.get_width(),input.get_height());
        const float2 at=float2(uint2(gid%8,gid/8)*(size-1)/7)+0.5f;
        output[gid]=float4(input.sample(s,at));
    }
}

// The original downscale packs four byte codes into each R32Uint texel.
// A separate writer checks those same plane views before replaying the transfer.
kernel void replay_packed_store(constant uint *codes [[buffer(0)]],
                               texture2d<uint,access::write> y [[texture(1)]],
                               texture2d<uint,access::write> uv [[texture(2)]],
                               uint2 gid [[thread_position_in_grid]]) {
    if(gid.x<y.get_width()&&gid.y<y.get_height())
        y.write(uint4(codes[0]*0x01010101u,0,0,0),gid);
    if(gid.x<uv.get_width()&&gid.y<uv.get_height())
        uv.write(uint4((codes[1]|(codes[2]<<8))*0x00010001u,0,0,0),gid);
}
