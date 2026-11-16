// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#ifndef NEZHA_ALS_CONTROL_ONLY
#include <mach/kmod.h>
#endif
#include <arm/caches_internal.h>
#include "NezhaAudioNative.h"
#include "../platform/NezhaAudioBoard.h"
#ifndef NEZHA_ALS_CONTROL_ONLY
#include "NezhaAudioService.h"
#endif
#include "NezhaAudioXpuTrace.h"
#ifdef IOKIT_ENABLE_SHARED_PTR
#error Requalify native OSPtr ABI before changing these build flags.
#endif
#ifndef NEZHA_ALS_CONTROL_ONLY
extern kmod_info_t g_kernel_kmod_info;
// Existing sheng build convention: service object's mod_init_func is removed
// and its native ctor is invoked under the explicit metaclass load bracket.
extern "C" void nezha_audio_service_static_init(void) __asm__("__GLOBAL__sub_I_NezhaAudioService.cpp");
#endif
extern "C" int nezha_audio_pas_call(unsigned,unsigned,uint64_t,uint64_t);
extern "C" int nezha_audio_pas_query(unsigned,unsigned,uint64_t [4]);
#ifndef NEZHA_ALS_CONTROL_ONLY
extern "C" {volatile uint64_t nezha_audio27_stats[32];}
static uint32_t audioStatsSequence;
#endif
namespace {
#ifndef NEZHA_ALS_CONTROL_ONLY
NaDriver *worker;
uint32_t started;
bool classesReady;
#endif
bool ownedWindow(NaRange r) {
    // Board-specific allowed windows. Audio's SMMU lease restricts exact banks
    // inside its mapped window separately; a mapping alone does not own them.
    const NaRange allowed[]={
        {0x81d00000,0x200000},{0x1f43000,0x1000},{0x1106000,0x1000},
        {0x0c300000,0x1000},{0x18920000,0x10000},{0x81c60000,0x20000},
        {0x01980000,0x4000},{0x019c0000,0x2000},{0x100000,0x100000},
        {NA_TLMM_GPIO_BASE,NA_TLMM_GPIO_BYTES},{0x07760000,0x17000},
        {0x06ae0000,0x1000},{0x07660000,0x1000},{0x07630000,0x10000},
        {0x0b220000,0x2000}, // PDC v5 identity/CFG observation only; no PDC writes.
        {0x17960000,0x1000}, // Only SPI751 type at1796014c/bit15 is configured.
        {0x15000000,0x100000},{0xa1400000,0x5580000},{0xa1380000,0x80000}
    };
    if (!na_range_valid(r) || ((r.base|r.bytes)&4095)) return false;
    for (const auto &a:allowed) if (r.base>=a.base && r.base+r.bytes<=a.base+a.bytes) return true;
    return false;
}
#ifndef NEZHA_ALS_CONTROL_ONLY
void run(void *v,wait_result_t) {
    auto *d=static_cast<NaDriver *>(v);thread_set_thread_name(current_thread(),"nezha-audio27");
    uint32_t lastStage=~0u,lastTasPhase=~0u,lastTasIndex=~0u,pilQueries=0;uint64_t lastOutput=0,lastInput=0,nextPeriods=0;
    int lastError=0;bool service=false,publicationFailed=false;
    int captureFrames=0;PE_parse_boot_argn("na-capture-probe",&captureFrames,sizeof captureFrames);
    bool probeSubmitted=false;uint32_t lastProbe=~0u;
    for (;;) {
        uint64_t now=na_native_now_us(nullptr)/1000;
        nezha_audio_owner_lock();
        int r=na_driver_poll(d,now);
        if (captureFrames>0 && !probeSubmitted && na_driver_ready(d)) {
            int requested=na_runtime_probe_capture(&d->runtime,(uint32_t)captureFrames,now);
            if (requested!=NA_BUSY) {
                probeSubmitted=true;
                if (requested) {d->runtime.probe.error=requested;__atomic_store_n(&d->runtime.probe.state,NaProbeFailed,__ATOMIC_RELEASE);}
            }
        }
        const uint64_t outputPeriods=d->runtime.status.output_periods,inputPeriods=d->runtime.status.input_periods;
        const uint32_t underruns=d->runtime.status.underruns,overruns=d->runtime.status.overruns;
        const uint32_t topFaults=d->speaker->reported_faults[0],bottomFaults=d->speaker->reported_faults[1];
        __atomic_fetch_add(&audioStatsSequence,1,__ATOMIC_ACQ_REL);
        __atomic_store_n(&nezha_audio27_stats[0],(uint64_t)(d->stage),__ATOMIC_RELAXED);__atomic_store_n(&nezha_audio27_stats[1],(uint64_t)((uint64_t)(int64_t)r),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[2],(uint64_t)(d->adsp->stage),__ATOMIC_RELAXED);__atomic_store_n(&nezha_audio27_stats[3],(uint64_t)(d->adsp->slave),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[4],(uint64_t)(d->prm.held),__ATOMIC_RELAXED);__atomic_store_n(&nezha_audio27_stats[5],(uint64_t)(d->prm.possible),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[6],(uint64_t)(d->graph[0].stage),__ATOMIC_RELAXED);__atomic_store_n(&nezha_audio27_stats[7],(uint64_t)(d->graph[1].stage),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[8],(uint64_t)(d->runtime.status.output_periods),__ATOMIC_RELAXED);__atomic_store_n(&nezha_audio27_stats[9],(uint64_t)(d->runtime.status.input_periods),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[10],(uint64_t)(d->runtime.status.underruns),__ATOMIC_RELAXED);__atomic_store_n(&nezha_audio27_stats[11],(uint64_t)(d->runtime.status.overruns),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[12],(uint64_t)(d->speaker->reported_faults[0]),__ATOMIC_RELAXED);__atomic_store_n(&nezha_audio27_stats[13],(uint64_t)(d->speaker->reported_faults[1]),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[14],(uint64_t)(d->session.generation),__ATOMIC_RELAXED);__atomic_store_n(&nezha_audio27_stats[15],(uint64_t)(d->runtime.status.state),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[16],(uint64_t)(d->adsp->transport.online),__ATOMIC_RELAXED);__atomic_store_n(&nezha_audio27_stats[17],(uint64_t)(d->graph[2].stage),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[18],(uint64_t)((uint64_t)(int64_t)d->graph[0].error),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[19],(uint64_t)((uint64_t)(int64_t)d->graph[2].error),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[20],(uint64_t)((uint64_t)(int64_t)d->prm.error),__ATOMIC_RELAXED);__atomic_store_n(&nezha_audio27_stats[21],(uint64_t)(service),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[22],(uint64_t)(__atomic_load_n(&d->runtime.request,__ATOMIC_ACQUIRE)),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[23],(uint64_t)(d->input_enabled),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[24],(uint64_t)(d->runtime.probe.state),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[25],(uint64_t)(d->runtime.probe.frames),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[26],(uint64_t)(d->runtime.probe.periods),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[27],(uint64_t)(d->runtime.probe.nonzero),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[28],(uint64_t)(d->runtime.probe.peak),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[29],(uint64_t)(d->runtime.probe.squares),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[30],(uint64_t)((int64_t)d->runtime.probe.error),__ATOMIC_RELAXED);
        __atomic_store_n(&nezha_audio27_stats[31],(uint64_t)(d->runtime.probe.pcm_frames),__ATOMIC_RELAXED);
        __atomic_fetch_add(&audioStatsSequence,1,__ATOMIC_RELEASE);
        nezha_audio_owner_unlock();
        // Exact037 PIL preflight, omitted by038's playback worker. Like the
        // control probe, run after the proxy poll reaches StopMain and before
        // the next poll issues PAS6. No audio gate is held across the SMCs.
        na_native_pil_preflight(d->adsp,&pilQueries);
        if (lastStage!=d->stage || lastError!=r || lastTasPhase!=d->speaker->phase || lastTasIndex!=d->speaker->index) {
            const uint64_t trace=d->speaker->ops.trace_wait_us ? *d->speaker->ops.trace_wait_us:0;
            printf("NEZHA AUDIO27: stage=%u error=%d adsp=%u graph=%u/%u prm=%x/%x tas=%u:%u/%u trace_us=%llu deadline_ms=%llu\n",d->stage,r,d->adsp->stage,d->graph[0].stage,d->graph[1].stage,d->prm.held,d->prm.possible,
                d->speaker->phase,d->speaker->index,d->speaker->count[d->speaker->phase],
                (unsigned long long)(trace>=d->speaker->trace_base_us ? trace-d->speaker->trace_base_us:0),
                (unsigned long long)d->speaker->deadline);
            lastStage=d->stage;lastError=r;lastTasPhase=d->speaker->phase;lastTasIndex=d->speaker->index;
        }
        if (now>=nextPeriods && (lastOutput!=outputPeriods || lastInput!=inputPeriods)) {
            printf("NEZHA AUDIO27 PCM: output=%llu input=%llu underruns=%u overruns=%u faults=%x/%x\n",
                (unsigned long long)outputPeriods,(unsigned long long)inputPeriods,
                underruns,overruns,topFaults,bottomFaults);
            lastOutput=outputPeriods;lastInput=inputPeriods;nextPeriods=now+1000;
        }
        if (lastProbe!=d->runtime.probe.state) {
            printf("NEZHA AUDIO27 CAPTURE PROBE: state=%u frames=%u periods=%u nonzero=%u peak=%u pcm_frames=%u error=%d\n",
                d->runtime.probe.state,d->runtime.probe.frames,d->runtime.probe.periods,d->runtime.probe.nonzero,d->runtime.probe.peak,d->runtime.probe.pcm_frames,d->runtime.probe.error);
            lastProbe=d->runtime.probe.state;
        }
        if (!service && !publicationFailed && na_driver_ready(d) &&
            (!captureFrames || d->runtime.probe.state==NaProbeComplete)) {
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
#endif
}
#ifndef NEZHA_ALS_CONTROL_ONLY
extern "C" int nezha_audio27_copy_stats(uint64_t out[32]) {
    if (!out) return NA_ARGUMENT;
    for (unsigned attempt=0;attempt<4;++attempt) {
        uint32_t seq=__atomic_load_n(&audioStatsSequence,__ATOMIC_ACQUIRE);if (seq&1) continue;
        for (unsigned i=0;i<32;++i) out[i]=__atomic_load_n(&nezha_audio27_stats[i],__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq==__atomic_load_n(&audioStatsSequence,__ATOMIC_RELAXED)) return NA_OK;
    }
    return NA_BUSY;
}
extern "C" int nezha_audio27_capture_pcm(uint32_t offset,uint32_t count,int16_t *out,uint32_t *copied) {
    if (!out || !copied || !count || count>64 || offset>=NA_CAPTURE_PROBE_PCM_FRAMES) return NA_ARGUMENT;
    *copied=0;
    if (!nezha_audio_owner_try_lock()) return NA_BUSY;
    NaDriver *d=__atomic_load_n(&worker,__ATOMIC_ACQUIRE);int result=NA_BUSY;
    if (d && (d->runtime.probe.state==NaProbeComplete || d->runtime.probe.state==NaProbeFailed)) {
        const NaCaptureProbe *p=&d->runtime.probe;
        if (offset>=p->pcm_frames) result=NA_ARGUMENT;
        else {
            uint32_t available=p->pcm_frames-offset;if (count>available) count=available;
            memcpy(out,p->pcm+offset,count*sizeof *out);*copied=count;result=NA_OK;
        }
    }
    nezha_audio_owner_unlock();return result;
}
#endif
extern "C" int na_native_map(NaNativeWindow *w,NaRange range,uint64_t lease) {
    if (!w || w->mapping || !lease || !ownedWindow(range)) return NA_ARGUMENT;
    if (!na_xpu_trace_before("descriptor-new",range.base,range.bytes)) return NA_IO;
    auto *memory=IODeviceMemory::withRange(range.base,range.bytes);
    na_xpu_trace_after("descriptor-new",range.base,memory ? NA_OK:NA_IO,0,(uintptr_t)memory);
    if (!memory) return NA_IO;
    IOOptionBits cache=(range.base==na_nezha_facts.firmware.base || range.base==na_nezha_facts.dtb.base) ? kIOMapWriteCombineCache:kIOMapInhibitCache;
    if (!na_xpu_trace_before("descriptor-map",range.base,range.bytes)) {memory->release();return NA_IO;}
    auto *map=memory->map(kIOMapAnywhere|cache);
    na_xpu_trace_after("descriptor-map",range.base,map ? NA_OK:NA_IO,0,(uintptr_t)map);
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
#ifndef NEZHA_ALS_CONTROL_ONLY
extern "C" int nezha_audio_worker_start(NaDriver *d) {
    if (!d || !d->adsp || !d->runtime.shared || !d->feedback_attached) return NA_ARGUMENT;
    uint32_t expected=0;
    if (!__atomic_compare_exchange_n(&started,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return NA_BUSY;
    __atomic_store_n(&worker,d,__ATOMIC_RELEASE);thread_t thread=THREAD_NULL;
    kern_return_t r=kernel_thread_start(run,d,&thread);
    if (r!=KERN_SUCCESS) {__atomic_store_n(&worker,(NaDriver *)nullptr,__ATOMIC_RELEASE);__atomic_store_n(&started,0,__ATOMIC_RELEASE);return NA_IO;}
    thread_deallocate(thread);return NA_OK;
}
#endif
