// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "NezhaDpuTiming.h"

// Called by the sole GOP hand-off presenter under its gate, for a delivered
// frame that it still owns. These mappings are read-only and stay retained for
// this boot, like the inherited firmware scanout. No clocks/IRQs are changed.
static IOMemoryMap *nezhaDpuTimingMaps[3];
static int nezhaDpuTimingOpened;

static bool nezhaDpuTimingOpen() {
    if(nezhaDpuTimingOpened)return nezhaDpuTimingOpened>0;
    nezhaDpuTimingOpened=-1;
    unsigned enabled=0;PE_parse_boot_argn("ndt",&enabled,sizeof enabled);
    if(enabled!=1)return false;
    const uint64_t offsets[3]={0,nezha_dpu_intf_offsets[0],nezha_dpu_intf_offsets[1]};
    for(unsigned i=0;i<3;++i) {
        const uint64_t physical=NEZHA_DPU_BASE+offsets[i];
        IODeviceMemory *memory=IODeviceMemory::withRange(physical,0x1000);
        nezhaDpuTimingMaps[i]=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache|kIOMapReadOnly):nullptr;
        OSSafeReleaseNULL(memory);
        IOMemoryMap *map=nezhaDpuTimingMaps[i];
        if(!map || !map->getVirtualAddress() || map->getPhysicalAddress()!=physical || map->getLength()!=0x1000) {
            for(unsigned j=0;j<3;++j)OSSafeReleaseNULL(nezhaDpuTimingMaps[j]);
            return false;
        }
    }
    nezhaDpuTimingOpened=1;return true;
}

static int nezhaDpuTimingRead(void *,uint32_t offset,uint32_t *value) {
    if(!value || (offset&3u))return 1;
    unsigned which=0;uint32_t at=0;
    if(offset) {
        bool found=false;
        for(unsigned i=0;i<2;++i)if(offset>=nezha_dpu_intf_offsets[i] &&
            offset-nezha_dpu_intf_offsets[i]<NEZHA_DPU_INTF_BYTES) {
            which=i+1;at=offset-nezha_dpu_intf_offsets[i];found=true;break;
        }
        if(!found)return 1;
    }
    if(!nezhaDpuTimingMaps[which])return 1;
    const volatile uint32_t *words=reinterpret_cast<const volatile uint32_t *>(
        nezhaDpuTimingMaps[which]->getVirtualAddress());
    *value=words[at/4];return 0;
}
static uint64_t nezhaDpuTimingNow(void *) {
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;
}
static uint64_t nezhaDpuTimingCounter(void *) {
    uint64_t ticks;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(ticks)::"memory");return ticks;
}
extern "C" void native_gop_display_timing(NGPTimingReply *out) {
    if(!out)return;
    out->version=NGP_TIMING_VERSION;
    if(!nezhaDpuTimingOpen()){out->status=NGP_TIMING_UNAVAILABLE;return;}
    const NezhaDpuTimingIo io={nullptr,nezhaDpuTimingRead,nezhaDpuTimingNow,nezhaDpuTimingCounter};
    (void)nezha_dpu_sample(&io,out);
    out->cpu_frames=native_legacy_frame_count;
    out->gpu_fallbacks=native_legacy_gpu_fallbacks;
    unsigned overlay=1;PE_parse_boot_argn("boot-diag-overlay",&overlay,sizeof overlay);
    if(overlay)out->flags|=NGP_TIMING_DIAGNOSTIC_WRITER_POSSIBLE;
}
