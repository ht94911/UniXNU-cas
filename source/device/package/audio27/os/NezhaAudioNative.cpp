// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/kmod.h>
#include <arm/caches_internal.h>
#include "NezhaAudioNative.h"
#include "NezhaAudioService.h"
#ifdef IOKIT_ENABLE_SHARED_PTR
#error Requalify native OSPtr ABI before changing these build flags.
#endif
extern kmod_info_t g_kernel_kmod_info;
// Existing sheng build convention: service object's mod_init_func is removed
// and its native ctor is invoked under the explicit metaclass load bracket.
extern "C" void nezha_audio_service_static_init(void) __asm__("__GLOBAL__sub_I_NezhaAudioService.cpp");
extern "C" int nezha_audio_pas_call(unsigned,unsigned,uint64_t,uint64_t);
extern "C" int nezha_audio_pas_query(unsigned,unsigned,uint64_t [4]);
extern "C" {volatile uint64_t nezha_audio27_stats[16];}
namespace {
NaDriver *worker;
uint32_t started;
bool classesReady;
bool ownedWindow(NaRange r) {
    // Board-specific allowed windows. Audio's SMMU lease restricts exact banks
    // inside its mapped window separately; a mapping alone does not own them.
    const NaRange allowed[]={
        {0x81d00000,0x200000},{0x1f43000,0x1000},{0x1106000,0x1000},
        {0x0c300000,0x1000},{0x18920000,0x10000},{0x81c60000,0x20000},
        {0x01980000,0x4000},{0x019c0000,0x2000},{0x100000,0x100000},
        {0x0f000000,0x1b8000},{0x07760000,0x17000},
        {0x06ae0000,0x1000},{0x07660000,0x1000},{0x07630000,0x10000},
        {0x15000000,0x100000},{0xa1400000,0x5580000},{0xa1380000,0x80000}
    };
    if (!na_range_valid(r) || ((r.base|r.bytes)&4095)) return false;
    for (const auto &a:allowed) if (r.base>=a.base && r.base+r.bytes<=a.base+a.bytes) return true;
    return false;
}
void run(void *v,wait_result_t) {
    auto *d=static_cast<NaDriver *>(v);thread_set_thread_name(current_thread(),"nezha-audio27");
    uint32_t lastStage=~0u;int lastError=0;bool service=false,publicationFailed=false;
    for (;;) {
        uint64_t now=na_native_now_us(nullptr)/1000;
        nezha_audio_owner_lock();
        int r=na_driver_poll(d,now);
        nezha_audio_owner_unlock();
        nezha_audio27_stats[0]=d->stage;nezha_audio27_stats[1]=(uint64_t)(int64_t)r;
        nezha_audio27_stats[2]=d->adsp->stage;nezha_audio27_stats[3]=d->adsp->slave;
        nezha_audio27_stats[4]=d->prm.held;nezha_audio27_stats[5]=d->prm.possible;
        nezha_audio27_stats[6]=d->graph[0].stage;nezha_audio27_stats[7]=d->graph[1].stage;
        nezha_audio27_stats[8]=d->runtime.status.output_periods;nezha_audio27_stats[9]=d->runtime.status.input_periods;
        nezha_audio27_stats[10]=d->runtime.status.underruns;nezha_audio27_stats[11]=d->runtime.status.overruns;
        nezha_audio27_stats[12]=d->speaker->reported_faults[0];nezha_audio27_stats[13]=d->speaker->reported_faults[1];
        nezha_audio27_stats[14]=d->session.generation;nezha_audio27_stats[15]=d->runtime.status.state;
        if (lastStage!=d->stage || lastError!=r) {
            printf("NEZHA AUDIO27: stage=%u error=%d adsp=%u graph=%u/%u prm=%x/%x\n",d->stage,r,d->adsp->stage,d->graph[0].stage,d->graph[1].stage,d->prm.held,d->prm.possible);
            lastStage=d->stage;lastError=r;
        }
        if (!service && !publicationFailed && na_driver_ready(d)) {
            if (!classesReady) {
                void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
                if (handle) {nezha_audio_service_static_init();classesReady=OSMetaClass::postModLoad(handle)==0;}
                if (!classesReady) {na_runtime_disable(&d->runtime);printf("NEZHA AUDIO27: native class registration failed\n");publicationFailed=true;}
            }
            int e=publicationFailed ? NA_MISSING:nezha_audio_publish(&d->runtime);
            if (e) {na_runtime_disable(&d->runtime);printf("NEZHA AUDIO27: service publish failed %d\n",e);publicationFailed=true;}
            service=!publicationFailed;
        }
        // One owner handles ADSP events and audio periods. This is the same
        // bounded kernel-thread wait facility used by sheng/other drivers.
        assert_wait_timeout((event_t)d,THREAD_UNINT,1,kMillisecondScale);
        thread_block(THREAD_CONTINUE_NULL);
    }
    // Publication failed before HAL could request data. Metadata/leases remain
    // retained for diagnosis; they must not be freed under a running DSP.
}
}
extern "C" int na_native_map(NaNativeWindow *w,NaRange range,uint64_t lease) {
    if (!w || w->mapping || !lease || !ownedWindow(range)) return NA_ARGUMENT;
    auto *memory=IODeviceMemory::withRange(range.base,range.bytes);
    if (!memory) return NA_IO;
    IOOptionBits cache=(range.base==na_nezha_facts.firmware.base || range.base==na_nezha_facts.dtb.base) ? kIOMapWriteCombineCache:kIOMapInhibitCache;
    auto *map=memory->map(kIOMapAnywhere|cache);
    if (!map) {memory->release();return NA_IO;}
    w->descriptor=memory;w->mapping=map;w->va=reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());w->physical=range;w->lease=lease;
    return w->va ? NA_OK:NA_IO;
}
extern "C" int na_native_unmap(NaNativeWindow *w,int quiet) {
    if (!w || !quiet) return NA_OWNERSHIP;
    if (w->mapping) static_cast<IOMemoryMap *>(w->mapping)->release();
    if (w->descriptor) static_cast<IODeviceMemory *>(w->descriptor)->release();
    *w={};return NA_OK;
}
extern "C" int na_native_allocate(NaNativeMemory *m,uint32_t bytes) {
    if (!m || m->descriptor || !bytes || bytes>0x1000000 || (bytes&4095)) return NA_ARGUMENT;
    // These bytes feed our explicitly owned SMMU tables/PAS, never a second
    // system mapper. Request and retrieve the actual physical allocation.
    auto *b=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,IOOptionBits(kIOMemoryPhysicallyContiguous)|IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryMapperNone),bytes,4096);
    if (!b) return NA_IO;
    IOByteCount length=0;uint64_t pa=b->getPhysicalSegment(0,&length,kIOMemoryMapperNone);
    auto *va=static_cast<uint8_t *>(b->getBytesNoCopy());
    if (!pa || (pa&4095) || length<bytes || !va || (reinterpret_cast<uintptr_t>(va)&4095)) {b->release();return NA_IO;}
    bzero(va,bytes);m->descriptor=b;m->va=va;m->physical=pa;m->bytes=bytes;return NA_OK;
}
extern "C" int na_native_free(NaNativeMemory *m,int quiet) {
    if (!m || !quiet) return NA_OWNERSHIP;
    if (m->descriptor) static_cast<IOBufferMemoryDescriptor *>(m->descriptor)->release();
    *m={};return NA_OK;
}
extern "C" int na_native_clean(void *,const void *cpu,uint64_t pa,uint64_t bytes) {
    if (!cpu || !pa || !bytes || bytes>0x1000000 || pa>UINT64_MAX-bytes) return NA_ARGUMENT;
    flush_dcache64(pa,(unsigned)bytes,TRUE);__asm__ volatile("dsb sy" :::"memory");return NA_OK;
}
extern "C" void na_native_publish_writes(void) {__asm__ volatile("dsb sy" :::"memory");}
extern "C" int na_native_sync(NaNativeMemory *m,uint32_t offset,uint32_t bytes,int for_device) {
    if (!m || !m->descriptor || !bytes || offset>m->bytes || bytes>m->bytes-offset) return NA_ARGUMENT;
    if (for_device) return na_native_clean(nullptr,m->va+offset,m->physical+offset,bytes);
    uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
    uint64_t line=4ULL<<((ctr>>16)&15);if (line>4096) return NA_MISSING;
    uintptr_t first=(reinterpret_cast<uintptr_t>(m->va)+offset)&~(line-1);
    uintptr_t end=reinterpret_cast<uintptr_t>(m->va)+offset+bytes;
    for (uintptr_t p=first;p<end;p+=line) __asm__ volatile("dc ivac, %0" ::"r"(p):"memory");
    __asm__ volatile("dsb sy" :::"memory");return NA_OK;
}
extern "C" uint64_t na_native_host_time(void *) {return mach_absolute_time();}
extern "C" uint64_t na_native_now_us(void *) {uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;}
extern "C" void na_native_pause_us(void *,uint32_t us) {if (us<=1000) IODelay(us);}
extern "C" int na_native_pas(void *,uint32_t command,uint32_t pas,uint64_t a,uint64_t b) {
    return nezha_audio_pas_call(command,pas,a,b) ? NA_IO:NA_OK;
}
extern "C" int na_native_pas_query(uint32_t command,uint32_t pas,uint64_t result[4]) {
    return nezha_audio_pas_query(command,pas,result) ? NA_IO:NA_OK;
}
extern "C" int nezha_audio_worker_start(NaDriver *d) {
    if (!d || !d->adsp || !d->runtime.shared || !d->feedback_attached) return NA_ARGUMENT;
    uint32_t expected=0;
    if (!__atomic_compare_exchange_n(&started,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return NA_BUSY;
    worker=d;thread_t thread=THREAD_NULL;
    kern_return_t r=kernel_thread_start(run,d,&thread);
    if (r!=KERN_SUCCESS) {worker=nullptr;__atomic_store_n(&started,0,__ATOMIC_RELEASE);return NA_IO;}
    thread_deallocate(thread);return NA_OK;
}
