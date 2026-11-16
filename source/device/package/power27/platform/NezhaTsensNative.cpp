// SPDX-License-Identifier: BSD-2-Clause-Patent
// N0's measured TSENS layout with pandora's same-SM8850 status-only access.
#include <IOKit/IOLib.h>
#include <mach/vm_types.h>
#include <sys/errno.h>
#include "NezhaTsensNative.h"
extern "C" vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
namespace {
uintptr_t tm[NP_TSENS_CONTROLLER_COUNT];
bool prepared;
int read_status(void *,uint64_t pa,uint32_t *raw) {
    if(!prepared||!raw||(pa&3u))return EACCES;
    for(unsigned c=0;c<NP_TSENS_CONTROLLER_COUNT;++c) {
        const auto &p=np_tsens_controllers[c];
        const uint64_t first=p.tm_pa+NP_TSENS_STATUS_OFFSET;
        if(pa<first||pa>=first+4u*p.sensors)continue;
        if(!tm[c])return ENXIO;
        *raw=*reinterpret_cast<volatile uint32_t *>(tm[c]+(uintptr_t)(pa-p.tm_pa));
#if defined(__aarch64__)
        __asm__ volatile("dmb oshld" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
        return 0;
    }
    return EACCES;
}
}
extern "C" int np_tsens_native_prepare(void) {
    if(prepared)return EALREADY;
    prepared=true;
    unsigned mask=0;
    for(unsigned c=0;c<NP_TSENS_CONTROLLER_COUNT;++c) {
        tm[c]=(uintptr_t)ml_io_map((vm_offset_t)np_tsens_controllers[c].tm_pa,0x1000);
        if(tm[c])mask|=1u<<c;
    }
    IOLog("NEZHA POWER TSENS: TM mapping mask=%x; seven N0 pages, status words only\n",mask);
    return mask==((1u<<NP_TSENS_CONTROLLER_COUNT)-1u)?0:ENXIO;
}
extern "C" int np_tsens_native_sample(void *,NpTsensSample *out) {
    const NpTsensReadOps ops={nullptr,read_status};
    return np_tsens_sample(&ops,out);
}
