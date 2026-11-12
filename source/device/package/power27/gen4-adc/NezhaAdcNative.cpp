// SPDX-License-Identifier: BSD-2-Clause-Patent
// N0 v8 ADC adapter, reusing the kernel's existing device-map entries and
// IOLock convention. GPIO-backed inputs borrow only their EN_CTL bit7;
// regulator, IRQ and ADC channel-table configuration remains inherited.
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
Window windows[7]={{0xc400000,0x1000,0},{0xc403000,0x8000,0},{0xc42a000,0x8000,0},
 {0xc4c0000,0,0},{0xc900000,0,0},{0,0,0},{0,0,0}};
IOLock *lock;
NpAdcSpmiV8 spmi;
NpGen4Adc adc;
bool ready;
bool deferredFailure;
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
 IOLog("NEZHA POWER ADC FAILURE: kind=%s call=%u channel=%u error=%d touched=%u uncertain=%u cleanup_started=%u async_phase=%u last_transfer_valid=%u ppid=%x reg=%x op=%u bytes=%u transfer_error=%d status_valid=%u status=%x first_valid=%u first_error=%d qtm_ppid=%x qtm_before=%x qtm_expected=%x qtm_last=%x qtm_owned=%u qtm_pending=%u qtm_restore_attempted=%u qtm_phase=%u qtm_owner=%u\n",
   kind,(unsigned)f.call,(unsigned)f.channel,f.error,(unsigned)f.touched,(unsigned)f.uncertain,
   (unsigned)f.cleanup_started,(unsigned)f.async_phase,(unsigned)f.transfer_valid,
   (unsigned)f.ppid,(unsigned)f.reg,(unsigned)f.op,(unsigned)f.bytes,f.transfer_error,
   (unsigned)f.status_valid,f.status,(unsigned)adc.first_failure.valid,adc.first_failure.error,
   (unsigned)np_adc_spmi_v8_gpio_ppid(&spmi),spmi.qtm_before,spmi.qtm_expected,spmi.qtm_last,spmi.qtm_owned,spmi.qtm_pending,
   spmi.qtm_restore_attempted,spmi.qtm_phase,spmi.qtm_owner);
}
void sample_failure(unsigned call,unsigned channel,int error) {
 if(adc.first_failure.valid){
  if(deferredFailure){deferredFailure=false;log_failure("sample",adc.first_failure);}
  return;
 }
 if(!error)return;
 adc.first_failure=failure_record(call,channel,error);
 log_failure("sample",adc.first_failure);
}
int dropped_eoc_clear(void *context) {
 const auto &t=spmi.last_transfer;
 if(context!=&spmi||!t.valid||!t.status_valid||t.ppid!=0x90||t.reg!=0x4b||
    t.op!=NP_ADC_SPMI_WRITE||t.bytes!=1||t.error!=-EIO||t.status!=9)return 0;
 if(!adc.first_failure.valid){
  adc.first_failure=failure_record(adc.async_phase?ADC_FAILURE_POLL:ADC_FAILURE_READ,continuousChannel,EIO);
  deferredFailure=true;
 }
 return 1;
}
bool dropped_qtm_restore() {
 return !adc.touched&&!adc.uncertain&&spmi.qtm_owned&&!spmi.qtm_pending&&
  spmi.qtm_restore_attempted==NP_ADC_QTM_RESTORE_DROPPED;
}
int confirm_qtm_restore(unsigned call,unsigned channel) {
 /* The caller holds the existing IOLock and has already retained first_error.
  * Core permits only readback/one original GPIO value, never SDAM replay. */
 const int error=np_adc_spmi_v8_qtm_restore(&spmi);
 log_failure("cleanup",failure_record(call,channel,error));
 return error;
}
void unmap_all() {
 for(unsigned i=sizeof(windows)/sizeof(windows[0]);i;i--)if(windows[i-1].va) {
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
 bool command=false;
 for(unsigned i=3;i<sizeof(windows)/sizeof(windows[0]);++i)
  if(windows[i].va&&pa>=windows[i].pa&&pa-windows[i].pa<windows[i].bytes)command=true;
 if(!command)return EACCES;
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
int map_bus1_gpio() {
 if(windows[5].va&&windows[6].va)return 0;
 uint32_t count=0;int error=read32(nullptr,windows[0].pa+8,&count);
 if(error)return error;
 count&=0x1fff;
 if(!count||spmi.apids+count>8192)return ENODEV;
 /* Bus1 uses global APIDs. The same validated GPIO lease checks the actual
  * write EE before accessing its writer channel; no other bus1 writes exist. */
 windows[5].pa=(windows[3].pa+spmi.apids*0x20u)&~0xfffull;
 const uint64_t end=windows[3].pa+(spmi.apids+count)*0x20u;
 windows[5].bytes=(vm_size_t)(((end+0xfffu)&~0xfffull)-windows[5].pa);
 if(!windows[5].va){error=map(5);if(error)return error;}
 windows[6].pa=(windows[4].pa+spmi.apids*0x200u)&~0xfffull;
 const uint64_t writer_end=windows[4].pa+(spmi.apids+count)*0x200u;
 windows[6].bytes=(vm_size_t)(((writer_end+0xfffu)&~0xfffull)-windows[6].pa);
 return windows[6].va?0:map(6);
}
int prepare_gpio(unsigned channel) {
 if(channel==1)return np_adc_spmi_v8_qtm_prepare(&spmi);
 const uint16_t ppid=channel==2?0x2793:channel==3?0x193:channel==7?0x190:0;
 return ppid?np_adc_spmi_v8_gpio_prepare(&spmi,ppid):0;
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
  NpGen4AdcOps io={&spmi,np_adc_spmi_v8_owns,np_adc_spmi_v8_read,np_adc_spmi_v8_write,pause_core,dropped_eoc_clear};
  error=np_gen4_adc_init(&adc,&io);
 }
 if(error)unmap_all();
 ready=!error;IOLockUnlock(lock);
 return error<0?-error:error;
}
extern "C" int np_adc_native_read(unsigned channel,NpAdcNativeReading *out) {
 if(!out||!np_gen4_adc_channel_supported(channel))return EINVAL;
 *out=NpAdcNativeReading{};
 if(!ready||!lock){out->error=EACCES;return EACCES;}
 IOLockLock(lock);
 if(channel==2){
  if(adc.touched||adc.uncertain||spmi.qtm_owned||spmi.qtm_pending){
   out->error=EBUSY;out->quiescent=0;IOLockUnlock(lock);return EBUSY;
  }
  const int error=map_bus1_gpio();
  if(error){out->error=error;out->quiescent=1;IOLockUnlock(lock);return error;}
 }
 continuousChannel=channel;
 spmi.last_transfer=NpAdcSpmiTransfer{};
 if(!adc.touched&&!adc.uncertain&&!spmi.qtm_owned&&!spmi.qtm_pending)
  {adc.first_failure=NpGen4AdcFailure{};deferredFailure=false;}
 uint16_t raw=0;int error=prepare_gpio(channel);
 if(channel==1)IOLog("NEZHA POWER QTM GPIO: prepare=%d phase=%u owner=%u read_valid=%u before=%x after=%x owned=%u pending=%u\n",
   error,spmi.qtm_phase,spmi.qtm_owner,spmi.qtm_read_valid,spmi.qtm_before,spmi.qtm_last,spmi.qtm_owned,spmi.qtm_pending);
 if(!error)error=np_gen4_adc_read(&adc,channel,&raw);
 sample_failure(ADC_FAILURE_READ,channel,error);
 if(!adc.touched&&!adc.uncertain&&(spmi.qtm_owned||spmi.qtm_pending)){
  int restoreError=np_adc_spmi_v8_qtm_restore(&spmi);
  IOLog("NEZHA POWER QTM GPIO: restore=%d phase=%u before=%x after=%x owned=%u pending=%u\n",
    restoreError,spmi.qtm_phase,spmi.qtm_before,spmi.qtm_last,spmi.qtm_owned,spmi.qtm_pending);
  if(!error){error=restoreError;sample_failure(ADC_FAILURE_READ,channel,error);}
  else if(restoreError)log_failure("cleanup",failure_record(ADC_FAILURE_READ,channel,restoreError));
  if(dropped_qtm_restore())(void)confirm_qtm_restore(ADC_FAILURE_READ,channel);
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
 if(!error&&(adc.touched||adc.uncertain||spmi.qtm_owned||spmi.qtm_pending))error=EIO;
 if(error&&!adc.first_failure.valid)adc.first_failure=failure_record(ADC_FAILURE_STOP,continuousChannel,error);
 if(deferredFailure){deferredFailure=false;log_failure("sample",adc.first_failure);}
 if(error||adc.first_failure.valid)log_failure("cleanup",failure_record(ADC_FAILURE_STOP,continuousChannel,error));
 if(dropped_qtm_restore())error=confirm_qtm_restore(ADC_FAILURE_STOP,continuousChannel);
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
 if(!adc.touched&&!adc.uncertain&&(spmi.qtm_owned||spmi.qtm_pending)) {
  const int restore=np_adc_spmi_v8_qtm_restore(&spmi);
  if(!error){error=restore;sample_failure(call,continuousChannel,error);}
  else if(restore)log_failure("cleanup",failure_record(call,continuousChannel,restore));
  if(dropped_qtm_restore())(void)confirm_qtm_restore(call,continuousChannel);
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
 if(!np_gen4_adc_channel_supported(channel)){out->error=EINVAL;out->quiescent=1;return -1;}
 const int init=np_adc_native_init();
 if(init){out->error=init;out->quiescent=1;return -1;}
 IOLockLock(lock);
 if(adc.touched||adc.uncertain||spmi.qtm_owned||spmi.qtm_pending) {
  out->error=EBUSY;out->quiescent=0;IOLockUnlock(lock);return -1;
 }
 if(channel==2){
  const int error=map_bus1_gpio();
  if(error){out->error=error;out->quiescent=1;IOLockUnlock(lock);return -1;}
 }
 spmi.last_transfer=NpAdcSpmiTransfer{};adc.first_failure=NpGen4AdcFailure{};deferredFailure=false;
 continuousChannel=channel;
 int error=prepare_gpio(channel);
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
