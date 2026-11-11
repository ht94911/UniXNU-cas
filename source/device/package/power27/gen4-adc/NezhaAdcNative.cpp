// SPDX-License-Identifier: BSD-2-Clause-Patent
// N0 v8 ADC adapter, reusing the kernel's existing device-map entries and
// IOLock convention. No regulator/GPIO/IRQ or channel-table reconfiguration.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
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
unsigned continuousChannel;
enum { ADC_FAILURE_BEGIN=1,ADC_FAILURE_POLL=2,ADC_FAILURE_READ=3,ADC_FAILURE_STOP=4 };
NpGen4AdcFailure failure_record(unsigned call,unsigned channel,int error) {
 NpGen4AdcFailure f={};const auto &t=spmi.last_transfer;
 f.valid=1;f.call=(uint8_t)call;f.channel=(uint8_t)channel;
 f.error=error<0?-error:error;
 f.touched=adc.touched;f.uncertain=adc.uncertain;
 f.cleanup_started=adc.cleanup_started;f.async_phase=adc.async_phase;
 f.transfer_valid=t.valid;f.ppid=t.ppid;f.reg=t.reg;f.op=t.op;f.bytes=t.bytes;
 f.transfer_error=t.error;f.status_valid=t.status_valid;f.status=t.status;
 return f;
}
void log_failure(const char *kind,const NpGen4AdcFailure &f) {
 IOLog("NEZHA POWER ADC FAILURE: kind=%s call=%u channel=%u error=%d touched=%u uncertain=%u cleanup_started=%u async_phase=%u last_transfer_valid=%u ppid=%x reg=%x op=%u bytes=%u transfer_error=%d status_valid=%u status=%x first_valid=%u first_error=%d\n",
   kind,(unsigned)f.call,(unsigned)f.channel,f.error,(unsigned)f.touched,(unsigned)f.uncertain,
   (unsigned)f.cleanup_started,(unsigned)f.async_phase,(unsigned)f.transfer_valid,
   (unsigned)f.ppid,(unsigned)f.reg,(unsigned)f.op,(unsigned)f.bytes,f.transfer_error,
   (unsigned)f.status_valid,f.status,(unsigned)adc.first_failure.valid,adc.first_failure.error);
}
void sample_failure(unsigned call,unsigned channel,int error) {
 if(!error||adc.first_failure.valid)return;
 adc.first_failure=failure_record(call,channel,error);
 log_failure("sample",adc.first_failure);
}
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
 spmi.last_transfer=NpAdcSpmiTransfer{};
 if(!adc.touched&&!adc.uncertain&&!spmi.qtm_owned&&!spmi.qtm_pending)
  adc.first_failure=NpGen4AdcFailure{};
 uint16_t raw=0;int error=channel==1?np_adc_spmi_v8_qtm_prepare(&spmi):0;
 if(channel==1)IOLog("NEZHA POWER QTM GPIO: prepare=%d phase=%u owner=%u read_valid=%u before=%x after=%x owned=%u pending=%u\n",
   error,spmi.qtm_phase,spmi.qtm_owner,spmi.qtm_read_valid,spmi.qtm_before,spmi.qtm_last,spmi.qtm_owned,spmi.qtm_pending);
 if(!error)error=np_gen4_adc_read(&adc,channel,&raw);
 sample_failure(ADC_FAILURE_READ,channel,error);
 if(channel==1&&!adc.touched&&!adc.uncertain){
  int restoreError=np_adc_spmi_v8_qtm_restore(&spmi);
  IOLog("NEZHA POWER QTM GPIO: restore=%d phase=%u before=%x after=%x owned=%u pending=%u\n",
    restoreError,spmi.qtm_phase,spmi.qtm_before,spmi.qtm_last,spmi.qtm_owned,spmi.qtm_pending);
  if(!error){error=restoreError;sample_failure(ADC_FAILURE_READ,channel,error);}
  else if(restoreError)log_failure("cleanup",failure_record(ADC_FAILURE_READ,channel,restoreError));
 }
 out->error=error<0?-error:error;out->quiescent=!adc.touched&&!adc.uncertain&&!spmi.qtm_owned&&!spmi.qtm_pending;
 if(!error) {
  out->raw=raw;out->c1024=np_gen4_ntc_c1024(raw);
  out->centi_c=np_gen4_ntc_centi(raw);out->converted=1;
 }
 IOLockUnlock(lock);return out->error;
}
extern "C" int np_adc_native_stop(void) {
 if(!lock)return 0;
 IOLockLock(lock);spmi.last_transfer=NpAdcSpmiTransfer{};
 int error=np_gen4_adc_quiesce(&adc);
 if(!error)error=np_adc_spmi_v8_qtm_restore(&spmi);
 if(error||adc.first_failure.valid)log_failure("cleanup",failure_record(ADC_FAILURE_STOP,continuousChannel,error));
 if(!error)unmap_all();
 IOLockUnlock(lock);
 return error<0?-error:error;
}
namespace {
uint64_t now_ms() {
 uint64_t ns=0;absolutetime_to_nanoseconds(mach_continuous_time(),&ns);
 return ns/1000000;
}
int continuous_result(int error,uint16_t raw,NpAdcNativeReading *out,unsigned call) {
 /* Keep the existing GPIO2 lease until SDAM is proven quiet. A restore
  * failure is a failed conversion even when its ADC code was available. */
 sample_failure(call,continuousChannel,error);
 /* Preserve the failed sample while quiescing only this existing owner. */
 if(error&&(adc.touched||adc.uncertain)) {
  const int cleanup=np_gen4_adc_quiesce(&adc);
  log_failure("cleanup",failure_record(call,continuousChannel,cleanup));
 }
 if(continuousChannel==1&&!adc.touched&&!adc.uncertain) {
  const int restore=np_adc_spmi_v8_qtm_restore(&spmi);
  if(!error){error=restore;sample_failure(call,continuousChannel,error);}
  else if(restore)log_failure("cleanup",failure_record(call,continuousChannel,restore));
 }
 out->error=error<0?-error:error;
 out->quiescent=!adc.touched&&!adc.uncertain&&!spmi.qtm_owned&&!spmi.qtm_pending;
 if(!error&&!out->quiescent)out->error=error=EIO;
 sample_failure(call,continuousChannel,error);
 if(!error&&out->quiescent) {
  out->raw=raw;out->c1024=np_gen4_ntc_c1024(raw);
  out->centi_c=np_gen4_ntc_centi(raw);out->converted=1;
 }
 return error?-1:0;
}
}
extern "C" int np_adc_native_begin(unsigned channel,NpAdcNativeReading *out) {
 if(!out)return -1;
 *out=NpAdcNativeReading{};
 if(channel>1){out->error=EINVAL;out->quiescent=1;return -1;}
 const int init=np_adc_native_init();
 if(init){out->error=init;out->quiescent=1;return -1;}
 IOLockLock(lock);
 if(adc.touched||adc.uncertain||spmi.qtm_owned||spmi.qtm_pending) {
  out->error=EBUSY;out->quiescent=0;IOLockUnlock(lock);return -1;
 }
 spmi.last_transfer=NpAdcSpmiTransfer{};adc.first_failure=NpGen4AdcFailure{};
 continuousChannel=channel;
 int error=channel==1?np_adc_spmi_v8_qtm_prepare(&spmi):0;
 if(!error)error=np_gen4_adc_begin(&adc,channel,now_ms());
 const int result=error?continuous_result(error,0,out,ADC_FAILURE_BEGIN):1;
 IOLockUnlock(lock);return result;
}
extern "C" int np_adc_native_poll(NpAdcNativeReading *out) {
 if(!out)return -1;
 *out=NpAdcNativeReading{};
 if(!ready||!lock){out->error=EACCES;out->quiescent=1;return -1;}
 IOLockLock(lock);spmi.last_transfer=NpAdcSpmiTransfer{};uint16_t raw=0;
 const int error=np_gen4_adc_poll(&adc,now_ms(),&raw);
 const int result=error==1?1:continuous_result(error,raw,out,ADC_FAILURE_POLL);
 IOLockUnlock(lock);return result;
}
static int boot_probe(NpAdcNativeBootProbe *out,unsigned first) {
 if(!out)return EINVAL;
 *out=NpAdcNativeBootProbe{};
 int error=np_adc_native_init();out->init_error=error;
 if(!error) {
  for(unsigned channel=first;channel<2;++channel) {
   out->attempted|=1u<<channel;
   error=np_adc_native_read(channel,&out->reading[channel]);
   if(error)break;
  }
  out->stop_error=np_adc_native_stop();
 }
 return error?error:out->stop_error;
}
extern "C" int np_adc_native_boot_probe(NpAdcNativeBootProbe *out){return boot_probe(out,0);}
extern "C" int np_adc_native_boot_probe_qtm(NpAdcNativeBootProbe *out){return boot_probe(out,1);}
