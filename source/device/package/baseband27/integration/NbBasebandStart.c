/* SPDX-License-Identifier: BSD-2-Clause */
/* Package late initializer. The one ADT switch gates allocation, owners,
 * worker creation and class registration. No timer or reboot action. */
#ifdef NB_HOST_TEST
#include "NbStartTestShim.h"
#else
#include <stdint.h>
#include <string.h>
#include <sys/systm.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <pexpert/device_tree.h>
#include <IOKit/IOLib.h>
#endif
#include "../platform/NbBaseband.h"
int nezha_baseband_enabled(void)
{
    DTEntry chosen = NULL;const void *value = NULL;unsigned size = 0;uint32_t enabled = 0;
    if (SecureDTLookupEntry(NULL,"/chosen",&chosen) != kSuccess ||
        SecureDTGetProperty(chosen,"nezha-baseband",&value,&size) != kSuccess || size != sizeof enabled || !value) return 0;
    memcpy(&enabled,value,sizeof enabled);return enabled == 1;
}
extern int nezha_bb_bind(NbBasebandConfig *);
extern int nezha_bb_events(void (*)(void),void (*)(void));
extern int nezha_bb_dispatch(void);
extern int nezha_bb_owners_ready(void);
extern int nezha_bb_diag_register(void);
extern NbBasebandBridge nb_bridge_hooks(NbBaseband *);
extern void nb_bridge_poll(void);
extern void nb_bridge_state(int);
extern void nb_bridge_invalidate(void);
extern void nb_bridge_report(unsigned);
static NbBaseband nb_worker;
static NbBasebandConfig nb_config;
static uint32_t nb_started;
volatile int nezha_baseband_status = -1;
/* Existing USB provider reads only these atomically published scalar words. */
static uint64_t nb_stats[16];
static uint32_t nb_stats_sequence;
int nezha_bb_snapshot(uint64_t out[16])
{
    for(unsigned attempt=0;attempt<3;attempt++){
        uint32_t before=__atomic_load_n(&nb_stats_sequence,__ATOMIC_ACQUIRE);if(before&1)continue;
        for(unsigned i=0;i<16;i++)out[i]=__atomic_load_n(&nb_stats[i],__ATOMIC_ACQUIRE);
        if(before==__atomic_load_n(&nb_stats_sequence,__ATOMIC_ACQUIRE))return 0;
    }
    return NB_BUSY;
}
static void publish(int r)
{
    uint64_t words[16]={nb_worker.stage,(uint64_t)(int64_t)(r<0?r:0),nb_worker.mss.stage,nb_worker.mss.slave,
        nb_worker.qrtr.rx_packets,nb_worker.qrtr.tx_packets,nb_worker.qrtr.remote_count,
        nb_worker.tx_dropped,nb_worker.mss.wdog_events,nb_worker.mss.doorbells,
        nb_worker.c.rmtfs?nb_worker.c.rmtfs->reads:0,nb_worker.c.rmtfs?nb_worker.c.rmtfs->writes:0,
        nb_worker.tftp.reads,nb_worker.tftp.writes,nb_worker.qrtr.node_count,nb_worker.qrtr.rx_bad};
    __atomic_fetch_add(&nb_stats_sequence,1,__ATOMIC_ACQ_REL);
    for(unsigned i=0;i<16;i++)__atomic_store_n(&nb_stats[i],words[i],__ATOMIC_RELEASE);
    __atomic_fetch_add(&nb_stats_sequence,1,__ATOMIC_RELEASE);
}
static void on_smp2p(void){nb_baseband_smp2p_event(&nb_worker);}
static void on_watchdog(void){nb_bridge_invalidate();nb_baseband_wdog_event(&nb_worker);}
static uint64_t now_ms(void){uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000;}
static void *storage(size_t n){return kalloc_data(n,Z_WAITOK|Z_ZERO);}
static void trace_stages(uint32_t last[5],int result)
{
    const uint32_t stage[5]={nb_worker.stage,nb_worker.mss.stage,nb_config.ipa_fws->stage,nb_config.modem_dtb->stage,nb_config.modem->stage};
    const char *names[5]={"worker","mss","ipa-image","dtb-image","main-image"};
    for(unsigned i=0;i<5;i++)if(last[i]!=stage[i]){
        printf("NEZHA27 BB stage=%s-%u result=%d\n",names[i],stage[i],result);last[i]=stage[i];
    }
}
static int setup(void)
{
    int r=nezha_bb_bind(&nb_config);if(r)return r;
    nb_config.bridge=nb_bridge_hooks(&nb_worker);
    nb_config.qrtr_tx=storage(NB_QRTR_MAX_PACKET);nb_config.tftp_packet=storage(4+NB_TFTP_MAX_BLKSIZE);
    nb_config.tftp_files=storage((size_t)NB_TFTP_RW_FILES*NB_TFTP_RW_FILE_BYTES);
    nb_config.ipcrtr_intents=storage((size_t)NB_IPCRTR_INTENTS*NB_IPCRTR_INTENT_BYTES);
    nb_config.ds_intents=storage((size_t)NB_DS_INTENTS*NB_DS_INTENT_BYTES);nb_config.tx_queue=storage((size_t)NB_TX_QUEUE*NB_TX_SLOT_BYTES);
    if(!nb_config.qrtr_tx||!nb_config.tftp_packet||!nb_config.tftp_files||!nb_config.ipcrtr_intents||!nb_config.ds_intents||!nb_config.tx_queue)return NB_IO;
    r=nb_baseband_init(&nb_worker,&nb_config);if(r)return r;
    return nezha_bb_events(on_smp2p,on_watchdog);
}
static void worker(void *arg,wait_result_t reason)
{
    (void)arg;(void)reason;int r=NB_PENDING;uint64_t deadline=now_ms()+90000;
    printf("NEZHA27 BB stage=owners-wait result=%d\n",r);
    while((r=nezha_bb_owners_ready())==NB_PENDING&&now_ms()<deadline)IOSleep(10);
    if(r==NB_PENDING)r=NB_TIMEOUT;
    if(!r)r=setup();
    if(r){nezha_baseband_status=r;publish(r);printf("NEZHA27 BB stage=setup result=%d\n",r);thread_terminate(current_thread());return;}
    uint32_t stages[5]={~0u,~0u,~0u,~0u,~0u};unsigned last_second=0;
    for(;;){
        trace_stages(stages,NB_PENDING);
        r=nezha_bb_dispatch();if(!r)r=nb_baseband_poll(&nb_worker,now_ms());
        trace_stages(stages,r);publish(r);nezha_baseband_status=r<0?r:0;
        nb_bridge_state(r);
        if(r<0){printf("NEZHA27 BB stage=worker-stop result=%d\n",r);break;}
        nb_bridge_poll();unsigned second=(unsigned)(now_ms()/1000);
        if(second!=last_second){last_second=second;if(!(second%10))nb_bridge_report(second);}
        IOSleep(1);
    }
    thread_terminate(current_thread());
}
int nezha_baseband_start(void)
{
    if(!nezha_baseband_enabled())return NB_OK;
    uint32_t expected=0;if(!__atomic_compare_exchange_n(&nb_started,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return NB_BUSY;
    if(nezha_bb_diag_register()){nezha_baseband_status=NB_IO;publish(NB_IO);return NB_IO;}
    publish(NB_PENDING);nezha_baseband_status=0;
    thread_t thread=THREAD_NULL;
    if(kernel_thread_start(worker,NULL,&thread)!=KERN_SUCCESS){nezha_baseband_status=NB_IO;publish(NB_IO);printf("NEZHA27 BB stage=thread result=%d\n",NB_IO);return NB_IO;}
    thread_deallocate(thread);return NB_OK;
}
/* Called by the package after source storage/IOKit startup; all heavy work is
 * on the normal kernel thread, after the inherited shared owners are ready. */
void nezha_baseband_late_init(void){(void)nezha_baseband_start();}
