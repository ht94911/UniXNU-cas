// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <machine/machine_routines.h>
#include "Sm8850ResourcesNative.h"
// Same selected XNU declaration used by the existing native resource probe;
// this KPI is hidden by the public IOKit compile recipe's feature guards.
extern "C" vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
namespace {
IOLock *gate;
Sm8850Resources resources;
vm_offset_t window[5];
unsigned usbReady,attempted;
constexpr uint64_t address[]={0x15000000,0x18920000,0x00100000,0x01f43000,0x01106000};
constexpr uint32_t bytes[]={0x100000,0x10000,0x100000,0x1000,0x1000};
int lock(void *) {return sm8850_apps_config_lock();}
void unlock(void *) {sm8850_apps_config_unlock();}
int barrier(void *) {__asm__ volatile("dsb sy" ::: "memory");return 0;}
bool fits(unsigned domain,uint32_t offset,unsigned width) {
    return domain<5 && window[domain] && !(offset&(width-1)) && offset<=bytes[domain]-width;
}
int read32(void *,unsigned d,uint32_t o,uint32_t *v) {
    if (!v || !fits(d,o,4)) return -1;
    barrier(nullptr);*v=*reinterpret_cast<volatile uint32_t *>(window[d]+o);barrier(nullptr);return 0;
}
int read64(void *,unsigned d,uint32_t o,uint64_t *v) {
    if (!v || d!=Sm8850Smmu || !fits(d,o,8)) return -1;
    barrier(nullptr);*v=*reinterpret_cast<volatile uint64_t *>(window[d]+o);barrier(nullptr);return 0;
}
int write32(void *,unsigned d,uint32_t o,uint32_t v) {
    if (!fits(d,o,4)) return -1;
    *reinterpret_cast<volatile uint32_t *>(window[d]+o)=v;barrier(nullptr);return 0;
}
}
extern "C" int sm8850_apps_config_lock(void) {
    if (!ml_get_interrupts_enabled()) return -1;
    IOLock *value=__atomic_load_n(&gate,__ATOMIC_ACQUIRE);
    if (!value) {
        IOLock *fresh=IOLockAlloc();if (!fresh) return -1;
        IOLock *empty=nullptr;
        if (!__atomic_compare_exchange_n(&gate,&empty,fresh,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) IOLockFree(fresh);
        value=__atomic_load_n(&gate,__ATOMIC_ACQUIRE);
    }
    IOLockLock(value);return 0;
}
extern "C" void sm8850_apps_config_unlock(void) {IOLockUnlock(__atomic_load_n(&gate,__ATOMIC_ACQUIRE));}
extern "C" void sm8850_usb_domain_publish(unsigned ready) {__atomic_store_n(&usbReady,!!ready,__ATOMIC_RELEASE);}
extern "C" unsigned sm8850_usb_domain_ready(void) {return __atomic_load_n(&usbReady,__ATOMIC_ACQUIRE);}
extern "C" Sm8850Resources *sm8850_native_resources(void) {
    if (sm8850_apps_config_lock()) return nullptr;
    if (!attempted) {
        attempted=1;bool mapped=true;
        for (unsigned i=0;i<5;++i) {window[i]=ml_io_map(address[i],bytes[i]);if (!window[i]) {mapped=false;break;}}
        if (mapped) {
            Sm8850ResourceIo io={nullptr,lock,unlock,read32,write32,read64,barrier};
            (void)sm8850_resources_init(&resources,&io);
        }
    }
    Sm8850Resources *r=resources.initialized ? &resources:nullptr;
    sm8850_apps_config_unlock();return r;
}
