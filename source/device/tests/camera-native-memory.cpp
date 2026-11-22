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
static unsigned publish_calls,fail_call,allocate_calls,fail_allocate,hold_calls,fail_hold;
static unsigned retire_calls,fail_retire,release_calls;

IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned bits,bool,size_t budget)
{
    assert(!mapping_ && bits==40 && bytes<=budget);
    ++allocate_calls;
    if (fail_allocate && allocate_calls==fail_allocate) return kIOReturnNoMemory;
    mapping_=std::aligned_alloc(4096,bytes);
    if(this==&memory_storage.tables) {
        spans_=new NativeAdrenoPhysicalSpan[1]{{0,0x40000000,bytes}};
        span_count_=1;
    } else {
        const uint64_t start=this==&memory_storage.frame[0] ? 0x50000000:0x52000000;
        spans_=new NativeAdrenoPhysicalSpan[2]{{0,start,bytes/2},{bytes/2,start+0x1000000,bytes/2}};
        span_count_=2;
    }
    bytes_=bytes;prepared_=true;borrowed_=true;
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
IOReturn NativeAdrenoMemory::holdForDevice() {
    assert(prepared_ && !device_references_);++hold_calls;
    if(fail_hold && hold_calls==fail_hold)return kIOReturnNotReady;
    ++device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::retireAfterQuiescence() {
    assert(device_references_==1);++retire_calls;
    if(fail_retire && retire_calls==fail_retire)return kIOReturnNotReady;
    --device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::release()
{ assert(!device_references_);++release_calls;std::free(mapping_);delete[]spans_;mapping_=nullptr;spans_=nullptr;bytes_=span_count_=0;prepared_=borrowed_=false;return kIOReturnSuccess; }
static uint64_t translated_page(uint64_t iova)
{
    const auto *tables=(const uint64_t *)memory_storage.tables.mapping();
    const auto &result=memory_storage.plan.tables;
    size_t table=0;
    for(unsigned level=result.start_level;level<4;++level) {
        const size_t entry=(iova>>(39-9*level))&511;
        const uint64_t descriptor=tables[table*512+entry];
        if(!(descriptor&1))return 0;
        const uint64_t page=descriptor&UINT64_C(0x0000fffffffff000);
        if(level==3)return page;
        assert(page>=result.root_physical);
        table=size_t((page-result.root_physical)/4096);
        assert(table<result.table_pages);
    }
    assert(false);return 0;
}
static void check_tables()
{
    assert(memory_storage.plan.tables.leaf_pages==7680);
    assert(memory_storage.plan.tables.table_pages<=32);
    for(unsigned frame=0;frame<2;++frame) {
        const auto &plan=memory_storage.plan.frames[frame];
        const auto &part=memory_storage.frame[frame];
        for(size_t i=0;i<part.spanCount();++i) {
            const auto &span=part.spans()[i];
            for(size_t at=0;at<span.bytes;at+=4096)
                assert(translated_page(plan.iova+span.offset+at)==span.physical+at);
        }
        assert(!translated_page(plan.iova-4096));
        assert(!translated_page(plan.iova+plan.mapped_bytes));
    }
}
static void dispose_idle_pool()
{
    assert(!memory_in_use && !memory_storage.held && !memory_storage.cpu);
    for(auto *part:{&memory_storage.tables,&memory_storage.frame[0],&memory_storage.frame[1]})
        if(part->backed())assert(part->release()==kIOReturnSuccess);
    memory_storage.pool_ready=false;
}
static void failure_is(unsigned step,unsigned object,uint32_t status)
{
    NezhaCameraNativeMemoryFailure detail{};
    nezha_camera_native_memory_failure(&detail);
    assert(detail.step==step && detail.object==object && detail.status==status);
}
int main(int argc,char **argv)
{
    NezhaCameraNativeMemory *owner=nullptr;
    const char *mode=argc>1 ? argv[1]:"normal";
    if(!strcmp(mode,"table-allocation"))fail_allocate=1;
    if(!strcmp(mode,"frame0-allocation"))fail_allocate=2;
    if(!strcmp(mode,"frame1-allocation"))fail_allocate=3;
    if(fail_allocate) {
        assert(!nezha_camera_native_memory_prepare(&owner,40,4096,3072));
        failure_is(fail_allocate==1 ? NezhaMemoryTableAllocate:NezhaMemoryFrameAllocate,
            fail_allocate-1,kIOReturnNoMemory);
        assert(owner && !memory_storage.pool_ready);
        assert(nezha_camera_native_memory_release(&owner,true) && !owner && !memory_in_use);
        assert(release_calls==fail_allocate-1);
        fail_allocate=0;
        assert(nezha_camera_native_memory_prepare(&owner,40,4096,3072));
        assert(nezha_camera_native_memory_hold(owner));
        assert(nezha_camera_native_memory_release(&owner,true) && !owner);
        dispose_idle_pool();
        puts("Partial allocation reports the actual first return and releases only acquired backing; next preparation succeeds");
        return 0;
    }
    assert(nezha_camera_native_memory_prepare(&owner,40,4096,3072));
    check_tables();
    assert(allocate_calls==3);
    if(!strcmp(mode,"hold1-failure"))fail_hold=2;
    if(!strcmp(mode,"hold2-failure"))fail_hold=3;
    if(fail_hold) {
        assert(!nezha_camera_native_memory_hold(owner));
        failure_is(NezhaMemoryHold,fail_hold-1,kIOReturnNotReady);
        assert(memory_storage.held==(1u<<(fail_hold-1))-1);
        assert(!nezha_camera_native_memory_release(&owner,false) && owner);
        assert(nezha_camera_native_memory_release(&owner,true) && !owner);
        assert(retire_calls==fail_hold-1 && release_calls==0);
        fail_hold=0;
        assert(nezha_camera_native_memory_prepare(&owner,40,4096,3072));
        assert(allocate_calls==3 && memory_failure.reused);
        assert(nezha_camera_native_memory_hold(owner));
        assert(nezha_camera_native_memory_release(&owner,true));
        dispose_idle_pool();
        puts("Partial device holds retain references until detached, then safely reuse the same prepared pool");
        return 0;
    }
    assert(nezha_camera_native_memory_hold(owner));
    published.clear();publish_calls=0;
    const auto *frame=(const uint8_t *)nezha_camera_native_memory_acquire(owner,0);
    assert(frame && acquired_bytes==15728640);
    fail_call=!strcmp(mode,"marker-failure") ? 3u:0u;
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
    if(!strcmp(mode,"retire-failure")) {
        fail_retire=1;
        assert(!nezha_camera_native_memory_release(&owner,true) && owner);
        assert(memory_storage.held==7 && !release_calls);
        fail_retire=0;
    }
    assert(nezha_camera_native_memory_release(&owner,true) && !owner);
    assert(!release_calls && !memory_storage.held && memory_storage.pool_ready);
    const void *saved_frames[]={memory_storage.frame[0].mapping(),memory_storage.frame[1].mapping()};
    const auto saved_tables=memory_storage.tables.mapping();
    fail_allocate=allocate_calls+1; // Allocator now refuses any fresh request.
    fail_call=0;
    for(unsigned session=0;session<4;++session) {
        assert(nezha_camera_native_memory_prepare(&owner,40,4096,3072));
        assert(memory_failure.reused && !memory_failure.step && allocate_calls==3);
        check_tables();
        assert(memory_storage.tables.mapping()==saved_tables);
        for(unsigned i=0;i<2;++i) {
            const auto *bytes=(const uint8_t *)memory_storage.frame[i].mapping();
            assert(bytes==saved_frames[i] && bytes[0]==0xa5 && bytes[15728639]==0xa5);
        }
        assert(nezha_camera_native_memory_hold(owner));
        assert(nezha_camera_native_memory_acquire(owner,session%2));
        assert(!nezha_camera_native_memory_release(&owner,true) && owner);
        assert(nezha_camera_native_memory_drop(owner,session%2));
        assert(nezha_camera_native_memory_release(&owner,true) && !owner && !release_calls);
    }
    dispose_idle_pool();
    puts("Actual camera adapter: full RAW invalidation and four marker publications retained; four detached reopens reuse the same three allocations even when further allocations fail; uncertain teardown retains ownership");
}
