// SPDX-License-Identifier: MIT
// Reuses qualified17 USB worker/reset policy and owned GOP snapshot geometry.
// iOS27: OS/SystemOS stay readonly. A pinned persistent Data permits only
// normal XNU shutdown; direct PSCI remains restricted to the RAM-only mode.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/reboot.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/processor.h>
#include <kern/cpu_number.h>
#include <pexpert/pexpert.h>
#include <QcomXnuUsbDebug.h>
extern processor_t thread_bind(processor_t);
extern void thread_terminate_self(void);
extern int (*PE_halt_restart)(unsigned int);
extern volatile uint32_t native_data_persist_state;
extern volatile uint64_t native_data_persist_writes,native_data_persist_errors;
extern int native_ufs_store_ready(void);
extern int native_ufs_store_sync(void);
extern unsigned native_ufs_store_gpt_check(void);
extern uint32_t system_inshutdown,final_shutdown_stage;
extern int nezha_bluetooth_stop_and_wait(uint32_t);
static unsigned normal_shutdown_started;
static thread_t normal_shutdown_thread;
struct nz27_video {unsigned height,width,depth,stride;uintptr_t frame;};
extern struct nz27_video boot_progress_panic_vinfo;
static unsigned mode,prepared,reboot_state;
volatile unsigned nezha27_usb_snapshot_registered,nezha27_usb_reset_owner_ready;
volatile unsigned nezha27_usb_reset_phase;

static int ram_only(void) {
 unsigned persist=~0U;
 return PE_parse_boot_argn("sheng-data-persist",&persist,sizeof(persist)) && persist==0 &&
        native_data_persist_state==0 && native_data_persist_writes==0;
}
static int persistent_selected(void) {
 unsigned persist=0;
 return PE_parse_boot_argn("sheng-data-persist",&persist,sizeof(persist)) && persist==2;
}
static int storage_ready(void) {
 return ram_only() || (mode==1 && persistent_selected() &&
   (native_data_persist_state==2 || native_data_persist_state==3) &&
   native_data_persist_errors==0 && native_ufs_store_ready());
}
__attribute__((noreturn)) static void psci_reset(void) {
 // Existing nezha17/UEFI-qualified PSCI SYSTEM_RESET via SMC. No timer path.
 __asm__ volatile("movz w0,#9\nmovk w0,#0x8400,lsl #16\nmov x1,xzr\nmov x2,xzr\nmov x3,xzr\ndsb sy\nsmc #0"
  :::"x0","x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
 for(;;)__asm__ volatile("wfe" :::"memory");
}
static int restart_owner(unsigned type) {
 if(type!=1 || (mode!=1 && mode!=2) ||
    __atomic_load_n(&PE_halt_restart,__ATOMIC_ACQUIRE)!=restart_owner)return -10;
 if(mode==1) {
  // The session-bound USB ticket was consumed before entering reboot_kernel.
  // That routine sets final_shutdown_stage only after its normal process,
  // filesystem/unmount, buffer and network shutdown work. Never require an
  // active USB controller or take its runtime lock from the final PE callback.
  unsigned expected=1;
  if(current_thread()!=__atomic_load_n(&normal_shutdown_thread,__ATOMIC_ACQUIRE) ||
     __atomic_load_n(&system_inshutdown,__ATOMIC_ACQUIRE)!=1 ||
     __atomic_load_n(&final_shutdown_stage,__ATOMIC_ACQUIRE)!=1 ||
     native_data_persist_errors!=0 ||
     !__atomic_compare_exchange_n(&normal_shutdown_started,&expected,2,0,
       __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -10;
 } else if(!ram_only() || !storage_ready() ||
           !qcom_usb_debug_control_consume_reboot())return -10;
 nezha27_usb_reset_phase=5;
 psci_reset();
}
static int capture_snapshot(void *cookie,uint8_t *rgb,size_t capacity,uint32_t *width,uint32_t *height) {
 (void)cookie;
 if(!rgb||!width||!height)return 0;
 uintptr_t frame=__atomic_load_n(&boot_progress_panic_vinfo.frame,__ATOMIC_ACQUIRE);
 struct nz27_video v=boot_progress_panic_vinfo;
 if(!frame||frame!=v.frame||v.depth!=32||v.width!=1200||v.height!=2608||v.stride<4800||v.stride>8192||
    frame<0xffff000000000000ULL||frame>UINTPTR_MAX-(uint64_t)v.height*v.stride||capacity<600U*1304U*3U)return 0;
 __asm__ volatile("dsb sy" :::"memory");
 for(unsigned y=0;y<1304;++y) {
  const volatile uint8_t *row=(const volatile uint8_t *)(frame+(uint64_t)y*2*v.stride);
  for(unsigned x=0;x<600;++x){const volatile uint8_t *p=row+x*8;*rgb++=p[2];*rgb++=p[1];*rgb++=p[0];}
 }
 if(__atomic_load_n(&boot_progress_panic_vinfo.frame,__ATOMIC_ACQUIRE)!=frame)return 0;
 *width=600;*height=1304;return 1;
}
void nezha27_usb_control_prepare(void) {
 if(__atomic_exchange_n(&prepared,1,__ATOMIC_ACQ_REL))return;
 nezha27_usb_snapshot_registered=qcom_usb_debug_register_screenshot(capture_snapshot,NULL);
 unsigned ctl=0,opt=0;
 if(!PE_parse_boot_argn("qcom-usb-ctl",&ctl,sizeof(ctl))||ctl!=1||
    !PE_parse_boot_argn("nezha-usb-reboot",&opt,sizeof(opt))||(opt!=1&&opt!=2)||
    !(ram_only() || (opt==1 && persistent_selected())))return;
 int (*empty)(unsigned int)=NULL;
 if(!__atomic_compare_exchange_n(&PE_halt_restart,&empty,restart_owner,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)&&empty!=restart_owner)return;
 mode=opt;
 nezha27_usb_reset_owner_ready=qcom_usb_debug_control_use_restart_owner(restart_owner);
 printf("NEZHA27 USB: screenshot=%u restart-owner=%u mode=%u Data=%s;controlled commands only\n",
  nezha27_usb_snapshot_registered,nezha27_usb_reset_owner_ready,mode,persistent_selected()?"persistent-normal-shutdown":"RAM-only");
}
static void reboot_worker(void *arg,wait_result_t wr) {
 (void)arg;(void)wr;int result=-1;processor_t cpu=cpu_to_processor(boot_cpu_id);
 thread_set_thread_name(current_thread(),"nezha27-usb-reboot");
 if(cpu==PROCESSOR_NULL)goto out;
 thread_bind(cpu);thread_block(THREAD_CONTINUE_NULL);
 if(cpu_number()!=boot_cpu_id){result=-2;goto out;}
 if(!storage_ready()){result=-11;goto out;}
 // Retire our UART DMA and shared lease before either existing USB exit mode.
 int bt_stop=nezha_bluetooth_stop_and_wait(30000);
 if(bt_stop){printf("NEZHA27 USB REBOOT: Bluetooth cleanup failed %d; exit refused\n",bt_stop);result=-14;goto out;}
 if(persistent_selected() && (native_ufs_store_gpt_check()!=0 || !native_ufs_store_sync())){result=-12;goto out;}
 nezha27_usb_reset_phase=1;
 printf("NEZHA27 USB REBOOT: mode=%u;ACK/log drain then %s\n",mode,mode==2?"RAM-only direct PSCI":"normal XNU shutdown");
 if(!qcom_usb_debug_wait_drained(2000)){result=-3;goto out;}
 if(!qcom_usb_debug_control_arm_reboot()){result=-4;goto out;}
 nezha27_usb_reset_phase=3;
 if(mode==2)result=restart_owner(1);
 else if(mode==1){
  if(!qcom_usb_debug_control_begin_shutdown()){result=-13;goto out;}
  __atomic_store_n(&normal_shutdown_thread,current_thread(),__ATOMIC_RELEASE);
  __atomic_store_n(&normal_shutdown_started,1,__ATOMIC_RELEASE);
  nezha27_usb_reset_phase=4;
  printf("NEZHA27 USB REBOOT: session ticket consumed;normal XNU shutdown;diagnostic log resumed\n");
  result=reboot_kernel(0,NULL);
  __atomic_store_n(&normal_shutdown_started,0,__ATOMIC_RELEASE);
  __atomic_store_n(&normal_shutdown_thread,THREAD_NULL,__ATOMIC_RELEASE);
 }
 else result=-7;
 out:
 __atomic_store_n(&normal_shutdown_started,0,__ATOMIC_RELEASE);
 __atomic_store_n(&normal_shutdown_thread,THREAD_NULL,__ATOMIC_RELEASE);
 qcom_usb_debug_control_reboot_failed(result?result:-5);
 __atomic_store_n(&reboot_state,4,__ATOMIC_RELEASE);thread_bind(PROCESSOR_NULL);thread_terminate_self();
}
void nezha27_usb_control_service(void) {
 if(!qcom_usb_debug_control_take_reboot())return;
 unsigned state=__atomic_load_n(&reboot_state,__ATOMIC_ACQUIRE);
 for(;;) {
  if(state!=0&&state!=3&&state!=4){qcom_usb_debug_control_reboot_failed(-6);return;}
  if(__atomic_compare_exchange_n(&reboot_state,&state,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))break;
 }
 thread_t thread=THREAD_NULL;unsigned creating=1;int rc=kernel_thread_start(reboot_worker,NULL,&thread);
 if(rc==KERN_SUCCESS){(void)__atomic_compare_exchange_n(&reboot_state,&creating,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);thread_deallocate(thread);}
 else{__atomic_store_n(&reboot_state,3,__ATOMIC_RELEASE);qcom_usb_debug_control_reboot_failed(rc);}
}
