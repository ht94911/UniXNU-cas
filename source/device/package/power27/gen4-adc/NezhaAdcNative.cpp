// SPDX-License-Identifier: BSD-2-Clause-Patent
// N0 v8 ADC adapter, reusing the kernel's existing device-map entries and
// IOLock convention. No regulator/GPIO/IRQ or channel-table reconfiguration.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <vm/memory_types.h>
#include <sys/errno.h>
#include "NezhaAdcNative.h"
extern "C" {
#include "NezhaGen4Adc.h"
#include "NezhaAdcSpmiV8.h"
#include "NezhaGen4Ntc.h"
vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,uint32_t);
void ml_io_unmap(vm_offset_t,vm_size_t);
}
namespace {
struct Window {uint64_t pa;vm_size_t bytes;vm_offset_t va;};
Window windows[5]={{0xc400000,0x1000,0},{0xc403000,0x8000,0},{0xc42a000,0x8000,0},
 {0xc4c0000,0,0},{0xc900000,0,0}};
IOLock *lock;
NpAdcSpmiV8 spmi;
NpGen4Adc adc;
bool ready;
void unmap_all() {
 for(unsigned i=5;i;i--)if(windows[i-1].va) {
  ml_io_unmap(windows[i-1].va,windows[i-1].bytes);windows[i-1].va=0;
 }
 ready=false;spmi.ready=0;
}
volatile uint32_t *address(uint64_t pa) {
 if(pa&3)return nullptr;
 for(const Window &w:windows)
  if(w.va&&pa>=w.pa&&pa-w.pa<=w.bytes-4)
   return reinterpret_cast<volatile uint32_t *>(w.va+(vm_offset_t)(pa-w.pa));
 return nullptr;
}
int read32(void *,uint64_t pa,uint32_t *out) {
 volatile uint32_t *p=address(pa);if(!p||!out)return EACCES;
 *out=*p;
#if defined(__aarch64__)
 __asm__ volatile("dmb oshld":::"memory");
#endif
 return 0;
}
int write32(void *,uint64_t pa,uint32_t value) {
 volatile uint32_t *p=address(pa);if(!p)return EACCES;
 // Commands/FIFO only in this adapter's observer/writer mappings.
 if(pa<windows[3].pa||pa>=windows[4].pa+windows[4].bytes)return EACCES;
#if defined(__aarch64__)
 __asm__ volatile("dmb oshst":::"memory");
#endif
 *p=value;
#if defined(__aarch64__)
 __asm__ volatile("dsb osh":::"memory");
#endif
 return 0;
}
void pause(void *,unsigned us){delay(us);}
void pause_core(void *,unsigned us){delay(us);}
int map(unsigned i) {
 Window &w=windows[i];
 w.va=ml_io_map_unmappable((vm_offset_t)w.pa,w.bytes,VM_WIMG_IO);
 return w.va?0:ENOMEM;
}
}
extern "C" int np_adc_native_init(void) {
 if(ready)return 0;
 if(!lock)lock=IOLockAlloc();
 if(!lock)return ENOMEM;
 IOLockLock(lock);
 int error=map(0);
 if(!error)error=map(1);
 if(!error)error=map(2);
 if(!error) {
  NpAdcSpmiOps io={nullptr,read32,write32,pause};
  error=np_adc_spmi_v8_init(&spmi,&io);
 }
 if(!error) {
  windows[3].bytes=(vm_size_t)((spmi.apids*0x20u+0xfffu)&~0xfffu);
  windows[4].bytes=(vm_size_t)((spmi.apids*0x200u+0xfffu)&~0xfffu);
  error=map(3);
  if(!error)error=map(4);
  if(!error&&np_adc_spmi_v8_owns(&spmi)!=1)error=EACCES;
 }
 if(!error) {
  NpGen4AdcOps io={&spmi,np_adc_spmi_v8_owns,np_adc_spmi_v8_read,np_adc_spmi_v8_write,pause_core};
  error=np_gen4_adc_init(&adc,&io);
 }
 if(error)unmap_all();
 ready=!error;IOLockUnlock(lock);
 return error<0?-error:error;
}
extern "C" int np_adc_native_read(unsigned channel,NpAdcNativeReading *out) {
 if(!out||channel>1)return EINVAL;
 *out=NpAdcNativeReading{};
 if(!ready||!lock){out->error=EACCES;return EACCES;}
 IOLockLock(lock);
 uint16_t raw=0;int error=np_gen4_adc_read(&adc,channel,&raw);
 out->error=error<0?-error:error;out->quiescent=!adc.touched&&!adc.uncertain;
 if(!error) {
  out->raw=raw;out->c1024=np_gen4_ntc_c1024(raw);
  out->centi_c=np_gen4_ntc_centi(raw);out->converted=1;
 }
 IOLockUnlock(lock);return out->error;
}
extern "C" int np_adc_native_stop(void) {
 if(!lock)return 0;
 IOLockLock(lock);int error=np_gen4_adc_quiesce(&adc);
 if(!error)unmap_all();
 IOLockUnlock(lock);
 return error<0?-error:error;
}
extern "C" int np_adc_native_boot_probe(NpAdcNativeBootProbe *out) {
 if(!out)return EINVAL;
 *out=NpAdcNativeBootProbe{};
 int error=np_adc_native_init();out->init_error=error;
 if(!error) {
  for(unsigned channel=0;channel<2;++channel) {
   out->attempted|=1u<<channel;
   error=np_adc_native_read(channel,&out->reading[channel]);
   if(error)break;
  }
  out->stop_error=np_adc_native_stop();
 }
 return error?error:out->stop_error;
}
