/* SPDX-License-Identifier: MIT */
/* Execute the actual camera allocator adapter and its page ownership calls.
 * NativeAdrenoMemory is the sole host fixture, not a DMA/cache simulation. */
#include "NezhaCameraNativeMemory.cpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>

struct Publication { NativeAdrenoMemory *owner; size_t begin, bytes; };
static std::vector<Publication> published;
static size_t acquired_bytes;
static unsigned publish_calls,fail_call;

IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned bits,bool,size_t budget)
{
    assert(!mapping_ && bits==40 && bytes<=budget);
    mapping_=std::aligned_alloc(4096,bytes);
    spans_=new NativeAdrenoPhysicalSpan[1]{{0,0x40000000,bytes}};
    bytes_=bytes;span_count_=1;prepared_=true;borrowed_=true;
    return mapping_ ? kIOReturnSuccess:kIOReturnNoMemory;
}
IOReturn NativeAdrenoMemory::publishForDevice(size_t begin,size_t bytes,bool writes,bool,unsigned)
{
    assert(writes || bytes<=32*4096);
    assert(prepared_ && !(begin%4096) && !(bytes%4096) && begin+bytes<=bytes_);
    ++publish_calls;
    if(fail_call && publish_calls==fail_call)return kIOReturnNotReady;
    published.push_back({this,begin,bytes});return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::acquireDeviceWrites(size_t begin,size_t bytes,unsigned)
{ assert(!begin && bytes==bytes_ && device_references_==1);acquired_bytes=bytes;return kIOReturnSuccess; }
IOReturn NativeAdrenoMemory::holdForDevice() { assert(prepared_ && !device_references_);++device_references_;return kIOReturnSuccess; }
IOReturn NativeAdrenoMemory::retireAfterQuiescence() { assert(device_references_==1);--device_references_;return kIOReturnSuccess; }
IOReturn NativeAdrenoMemory::release()
{ assert(!device_references_);std::free(mapping_);delete[]spans_;mapping_=nullptr;spans_=nullptr;bytes_=span_count_=0;prepared_=borrowed_=false;return kIOReturnSuccess; }
extern "C" ArmSmmu4KStatus nezha_camera_build_frame_span_tables(const ArmSmmu4KConfig *,
    const NezhaCameraFrameMapping frames[2],uint32_t,uint32_t,const ArmSmmu4KRange *ranges,
    size_t count,uint64_t *,size_t,NezhaCameraDmaPlan *out)
{
    assert(count==2 && ranges[0].iova==frames[0].iova && ranges[1].iova==frames[1].iova);
    *out={};out->frames[0]=frames[0];out->frames[1]=frames[1];out->tables.table_pages=32;
    return ArmSmmu4KSuccess;
}
int main(int argc,char **)
{
    NezhaCameraNativeMemory *owner=nullptr;
    assert(nezha_camera_native_memory_prepare(&owner,40,4096,3072));
    assert(nezha_camera_native_memory_hold(owner));
    published.clear();publish_calls=0;
    const auto *frame=(const uint8_t *)nezha_camera_native_memory_acquire(owner,0);
    assert(frame && acquired_bytes==15728640);
    fail_call=argc>1 ? 3u:0u;
    assert(nezha_camera_native_memory_publish(owner,0)==!fail_call);
    if(fail_call) {
        assert(!nezha_camera_native_memory_release(&owner,true));
        assert(nezha_camera_native_memory_drop(owner,0));
    } else {
        assert(published.size()==4);
        size_t bytes=0;
        for(unsigned i=0;i<4;++i) {
            const size_t marker=nezha_camera_frame_marker_offset(&memory_storage.layout,i);
            assert(published[i].owner==&memory_storage.frame[0]);
            assert(published[i].begin<=marker && marker+64<=published[i].begin+published[i].bytes);
            bytes+=published[i].bytes;
        }
        assert(bytes==16384);
        assert(nezha_camera_native_memory_acquire(owner,0));
        assert(nezha_camera_native_memory_drop(owner,0));
    }
    assert(!nezha_camera_native_memory_release(&owner,false) && owner);
    assert(nezha_camera_native_memory_release(&owner,true) && !owner);
    puts("Native camera memory: full RAW invalidation; only four dirty marker pages published; failure retains claim and mappings until quiescence");
}
