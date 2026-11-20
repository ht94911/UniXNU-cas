#include <metal_stdlib>
using namespace metal;

kernel void write_planar_codes(texture2d<uint,access::write> y [[texture(0)]],
                               texture2d<uint,access::write> uv [[texture(1)]],
                               uint2 gid [[thread_position_in_grid]]) {
    if(gid.x>=y.get_width() || gid.y>=y.get_height())return;
    y.write(uint4((gid.x*7+gid.y*13+17)&255,0,0,1),gid);
    if((gid.x&1)==0 && (gid.y&1)==0) {
        uint2 c=gid/2;
        uint u=(c.x*5+c.y*11+101)&255,v=(c.x*3+c.y*17+67)&255;
        uv.write(uint4(u|(v<<8),0,0,1),c);
    }
}

kernel void copy_planar_threadgroup(texture2d<half,access::sample> source [[texture(0)]],
                                    texture2d<uint,access::write> y [[texture(1)]],
                                    texture2d<uint,access::write> uv [[texture(2)]],
                                    threadgroup float4 *scratch [[threadgroup(0)]],
                                    uint tid [[thread_index_in_threadgroup]],
                                    uint2 group [[threadgroup_position_in_grid]],
                                    uint2 threads [[threads_per_threadgroup]]) {
    uint2 at=uint2(group.x*threads.x+tid,group.y);
    constexpr sampler s(coord::normalized,filter::nearest,address::clamp_to_edge);
    scratch[tid]=float4(source.sample(s,(float2(at)+0.5f)/float2(source.get_width(),source.get_height())));
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint peer=tid^1;
    uint2 dst=uint2(group.x*threads.x+peer,group.y);
    if(dst.x<y.get_width() && dst.y<y.get_height()) {
        uint4 codes=uint4(scratch[peer]*255.0f+0.5f);
        y.write(uint4(codes.y,0,0,1),dst);
        if((dst.x&1)==0 && (dst.y&1)==0)uv.write(uint4(codes.z|(codes.x<<8),0,0,1),dst/2);
    }
}
