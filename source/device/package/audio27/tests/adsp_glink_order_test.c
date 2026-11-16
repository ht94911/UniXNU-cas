/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production lifecycle and SMEM/GLINK parsers, synthetic peer and PAS/DMA
 * callbacks. Tests FIFO ownership/order; does not authenticate real firmware. */
#include "../platform/AdspLink.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(4096) uint8_t memory[0x200000];
static NaAdsp a;static NaRxQueue queue;static NaPasLoad main_image,dtb_image;
static NaSplitFirmware auth_image;
static unsigned stops,auths,dma,power,power_off,busy,setup_returns;static volatile uint32_t *desc;
static unsigned kick_begin,kick_return,kick_bells,kick_total,last_kick_tx;
static QcomSmp2p peer;
static void wr(uint32_t at,uint32_t v) {memcpy(memory+at,&v,4);}
static int locked(void *v) {(void)v;return busy ? NA_BUSY:NA_OK;}
static void unlocked(void *v) {(void)v;}
static int shared_busy(void *v) {(void)v;return NA_BUSY;}
static void bell(void *v,uint32_t signal) {
    (void)v;assert(signal==0 || signal==2);
    if (!signal) ++kick_total;
    if (!signal && a.ops.observe && a.glink.tx_messages<=16) {
        assert(kick_begin==kick_return+1 && last_kick_tx==a.glink.tx_messages);
        ++kick_bells;
    }
}
static int proxy(void *v,int on) {(void)v;if (on) ++power;else ++power_off;return NA_OK;}
static int pas(void *v,uint32_t command,uint32_t id,uint64_t x,uint64_t y) {
    (void)v;(void)x;(void)y;
    if (command==5 && id==1) {++auths;return NA_OK;}
    assert(command==6 && (id==1 || id==36));++stops;return NA_OK;
}
static int setup_dma(void *v) {(void)v;assert(a.masters_stopped && stops==2);++dma;return NA_OK;}
static void observed(void *v,uint32_t point,int result) {
    (void)v;
    if (point==NaAdspGlinkKickBegin) {
        assert(!result && !busy && a.glink.tx_messages && a.glink.tx_messages<=16);
        assert(kick_begin==kick_return && desc[1]>0);
        ++kick_begin;last_kick_tx=a.glink.tx_messages;
    } else if (point==NaAdspGlinkKickReturn) {
        assert(!result && kick_begin==kick_return+1 && kick_bells==kick_begin);
        assert(last_kick_tx==a.glink.tx_messages);++kick_return;
    }
    if (point==NaAdspGlinkSetupReturn && !result) {
        ++setup_returns;assert(desc[1]==0 && desc[2]==0);
        assert(desc[0]==0 && desc[3]==8); /* Remote-owned indices preserved. */
    }
}
static volatile uint8_t *peer_item(uint16_t id,uint32_t bytes) {
    uint32_t at,n;assert(!qcom_smem_alloc(&a.smem,&a.partition,id,bytes));
    assert(!qcom_smem_find(&a.smem,&a.partition,id,&at,&n) && n==bytes);
    return a.smem.base+at;
}
static void cold(unsigned bad_size) {
    memset(memory,0,sizeof memory);memset(&queue,0,sizeof queue);
    memset(&a,0,sizeof a);memset(&main_image,0,sizeof main_image);memset(&dtb_image,0,sizeof dtb_image);
    stops=auths=dma=power=power_off=busy=setup_returns=0;
    kick_begin=kick_return=kick_bells=kick_total=last_kick_tx=0;
    uint32_t toc=sizeof memory-4096,part=0x10000,size=0x10000;
    wr(toc,0x434f5424);wr(toc+4,1);wr(toc+8,1);
    wr(toc+32,part);wr(toc+36,size);wr(toc+44,2);wr(toc+48,64);
    wr(part,0x54525024);wr(part+4,2);wr(part+8,size);wr(part+12,32);wr(part+16,size);
    main_image.pas=1;dtb_image.pas=36;main_image.stage=dtb_image.stage=NaPasInit;
    main_image.ops.pas=pas;dtb_image.ops.pas=pas;
    NaAdspOps ops={0,locked,unlocked,bell,proxy,setup_dma,0,observed};
    assert(!na_adsp_init(&a,memory,sizeof memory,&main_image,&dtb_image,&queue,&ops));
    for (unsigned n=0;n<5;++n) assert(na_adsp_poll(&a,100+n)==NA_PENDING);
    assert(stops==2 && dma==1 && power==1 && a.stage==NaAdspDtb);
    assert(a.p2p.out && !a.glink_initialized && !a.transport.channel);
    uint32_t at,n;
    assert(qcom_smem_find(&a.smem,&a.partition,478,&at,&n)==QcomSmemMissing);
    assert(qcom_smem_find(&a.smem,&a.partition,479,&at,&n)==QcomSmemMissing);
    /* Model authenticated firmware publishing ready and its GLINK storage. */
    qcom_smp2p_init_out(&peer,(volatile uint32_t *)peer_item(443,QCOM_SMP2P_ITEM_BYTES),2,0);
    assert(qcom_smp2p_add_entry(&peer,"slave-kernel")==0);
    assert(qcom_smp2p_update(&peer,0,2,2));
    desc=(volatile uint32_t *)peer_item(478,32);desc[1]=32;desc[2]=8;desc[3]=8;
    (void)peer_item(479,bad_size ? 0x5000:0x4000);
    volatile uint32_t *rx=(volatile uint32_t *)peer_item(480,0x4000);
    rx[0]=0x00010000;rx[1]=1; /* VERSION v1/reuse. */
    assert(qcom_smp2p_update(&a.p2p,0,0x8000,0x8000));
    a.stage=NaAdspWaitReady;a.masters_stopped=0;a.auth_doorbells=a.doorbells;a.deadline=1000;
    assert(na_adsp_poll(&a,200)==NA_PENDING);
    assert(!a.glink_initialized && desc[1]==32 && desc[2]==8); /* Stale ready is insufficient. */
}
int main(void) {
    cold(0);na_adsp_smp2p_event(&a);busy=1;
    assert(na_adsp_poll(&a,201)==NA_PENDING && a.stage==NaAdspOnline);
    assert(!a.glink_initialized && desc[1]==32 && desc[2]==8 && !setup_returns);
    busy=0;assert(!na_adsp_poll(&a,202));
    assert(a.glink_initialized && a.transport.channel && a.glink.rx_messages==1 && setup_returns==1);
    assert(kick_begin==1 && kick_return==1 && kick_bells==1 && last_kick_tx==2);
    assert(a.mapper.channel && !strcmp(a.mapper.channel->name,"IPCRTR") && !a.mapper.online);
    assert(qcom_smp2p_out_value(&a.p2p,0)==0x8000 && stops==2 && dma==1 && power==1);
    assert(!na_adsp_poll(&a,203) && setup_returns==1 && stops==2 && dma==1);
    /* Observation stops after the first16 messages without suppressing the
     * original notification. A caller without an observer still notifies. */
    a.glink.tx_messages=17;a.glink.kick(a.glink.kick_context);
    assert(kick_begin==1 && kick_return==1 && kick_total==2);
    a.ops.observe=0;a.glink.kick(a.glink.kick_context);
    assert(kick_begin==1 && kick_return==1 && kick_total==3);
    cold(0);a.gpr_initiator=1;na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,201));
    assert(kick_begin==1 && kick_return==1 && kick_bells==1 && last_kick_tx==3);
    cold(1);na_adsp_smp2p_event(&a);
    assert(na_adsp_poll(&a,201)==NA_WIRE && a.stage==NaAdspFailed);
    assert(!a.glink_initialized && desc[1]==32 && desc[2]==8 && a.proxy_possible);
    assert(na_adsp_poll(&a,202)==NA_WIRE && stops==2 && dma==1 && !setup_returns);
    /* During the explicit delay the authenticated DSP remains observable,
     * including real handover. No GLINK FIFO index or packet is touched. */
    cold(0);a.glink_delay_ms=10000;na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,201) && a.stage==NaAdspOnline && a.glink_after==10201);
    assert(!a.glink_initialized && !a.transport.channel && !a.mapper.channel);
    assert(desc[1]==32 && desc[2]==8 && !setup_returns);
    assert(qcom_smp2p_update(&peer,0,4,4));na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,202) && a.handover_seen && a.handed && power_off==1 && !a.proxy_possible);
    assert(!a.glink_initialized && stops==2 && dma==1);
    assert(!na_adsp_poll(&a,10200) && !a.glink_initialized && desc[1]==32 && desc[2]==8);
    assert(!na_adsp_poll(&a,10201) && a.glink_initialized && a.glink.rx_messages==1 && setup_returns==1);
    assert(!na_adsp_poll(&a,10202) && setup_returns==1 && power_off==1 && stops==2 && dma==1);
    /* Fatal SMP2P is still terminal while transport is deferred. */
    cold(0);a.glink_delay_ms=10000;na_adsp_smp2p_event(&a);assert(!na_adsp_poll(&a,201));
    assert(qcom_smp2p_update(&peer,0,1,1));na_adsp_smp2p_event(&a);
    assert(na_adsp_poll(&a,202)==NA_REMOTE && !a.glink_initialized && desc[1]==32 && desc[2]==8);
    assert(na_adsp_poll(&a,203)==NA_REMOTE && stops==2 && dma==1);
    /* Once due, ordinary malformed-storage rejection is unchanged. */
    cold(1);a.glink_delay_ms=10000;na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,201) && !a.glink_initialized);
    assert(na_adsp_poll(&a,10201)==NA_WIRE && !a.glink_initialized && stops==2 && dma==1);
    cold(0);a.glink_delay_ms=20001;na_adsp_smp2p_event(&a);
    assert(na_adsp_poll(&a,201)==NA_ARGUMENT && !a.glink_initialized);
    cold(0);a.glink_delay_ms=20000;na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,201) && a.glink_after==20201);
    assert(!na_adsp_poll(&a,20200) && !a.glink_initialized);
    assert(!na_adsp_poll(&a,20201) && a.glink_initialized && setup_returns==1);
    /* An IRQ from the old firmware is captured by the actual auth baseline.
     * A new dedicated watchdog needs no SMP2P fatal bit or GLINK traffic. */
    cold(0);a.glink_delay_ms=20000;
    na_adsp_watchdog_event(&a);a.stage=NaAdspMain;main_image.stage=NaPasAuthenticate;
    main_image.deadline=1000;main_image.firmware=&auth_image;
    assert(na_adsp_poll(&a,201)==NA_PENDING && a.stage==NaAdspWaitReady);
    assert(a.auth_watchdogs==1 && auths==1 && !a.watchdog_fault);
    na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,202) && a.stage==NaAdspOnline && !a.glink_initialized);
    assert(!a.watchdog_fault && a.watchdogs==1 && a.auth_watchdogs==1);
    na_adsp_watchdog_event(&a);
    assert(na_adsp_poll(&a,203)==NA_REMOTE && a.stage==NaAdspFailed && a.watchdog_fault);
    assert(a.watchdogs==2 && a.slave==2 && !a.glink_initialized && desc[1]==32 && desc[2]==8);
    assert(na_adsp_poll(&a,20202)==NA_REMOTE && stops==2 && dma==1 && auths==1);
    /* Also catch a post-auth watchdog before ready, not only after handover. */
    cold(0);na_adsp_watchdog_event(&a);
    assert(na_adsp_poll(&a,201)==NA_REMOTE && a.watchdog_fault && !a.glink_initialized);
    cold(0);a.ops.poll_shared=shared_busy;a.deadline=201;na_adsp_watchdog_event(&a);
    assert(na_adsp_poll(&a,201)==NA_REMOTE && a.watchdog_fault && !a.glink_initialized);
    /* The existing observer may yield between first GLINK operations. The
     * production worker still handles handover and faults on every poll. */
    cold(0);a.glink_observe_ms=100;na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,201) && a.glink_observe_until==301 && !a.glink_initialized);
    assert(desc[1]==32 && desc[2]==8 && !setup_returns);
    assert(qcom_smp2p_update(&peer,0,4,4));na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,202) && a.handed && power_off==1 && !a.glink_initialized);
    assert(!na_adsp_poll(&a,300) && !a.glink_initialized);
    assert(!na_adsp_poll(&a,301) && a.glink_initialized && setup_returns==1 && a.glink_observe_until==401);
    assert(!a.glink.rx.data && !a.glink.rx_messages);
    assert(!na_adsp_poll(&a,400) && !a.glink.rx.data);
    assert(!na_adsp_poll(&a,401) && !a.glink.rx.data && a.glink_observe_until==501);
    assert(!na_adsp_poll(&a,501) && a.glink.rx.data && a.glink_observe_until==601 && !a.glink.rx_messages);
    assert(!na_adsp_poll(&a,601) && a.glink_observe_until==701 && !a.glink.rx_messages);
    assert(!na_adsp_poll(&a,701) && a.glink.rx_messages==1 && a.glink_observe_until==801);
    assert(!na_adsp_poll(&a,800) && a.glink.rx_messages==1 && setup_returns==1);
    assert(!na_adsp_poll(&a,801) && a.glink.rx_messages==1 && setup_returns==1 && power_off==1 && stops==2 && dma==1);
    cold(0);a.glink_observe_ms=100;na_adsp_smp2p_event(&a);assert(!na_adsp_poll(&a,201));
    na_adsp_watchdog_event(&a);
    assert(na_adsp_poll(&a,250)==NA_REMOTE && a.watchdog_fault && !a.glink_initialized && desc[1]==32 && desc[2]==8);
    cold(0);a.glink_observe_ms=100;na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,201) && !na_adsp_poll(&a,301));
    assert(qcom_smp2p_update(&peer,0,1,1));na_adsp_smp2p_event(&a);
    assert(na_adsp_poll(&a,350)==NA_REMOTE && a.glink_initialized && !a.glink.rx.data && !a.glink.rx_messages);
    cold(0);a.glink_observe_ms=100;na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,201) && !na_adsp_poll(&a,301) && !na_adsp_poll(&a,401) && !na_adsp_poll(&a,501) && !na_adsp_poll(&a,601));
    na_adsp_watchdog_event(&a);
    assert(na_adsp_poll(&a,650)==NA_REMOTE && a.watchdog_fault && !a.glink.rx_messages && setup_returns==1 && stops==2);
    cold(0);a.glink_observe_ms=100;na_adsp_smp2p_event(&a);assert(!na_adsp_poll(&a,201));busy=1;
    assert(na_adsp_poll(&a,301)==NA_PENDING && !a.glink_initialized && !setup_returns);
    busy=0;assert(!na_adsp_poll(&a,302) && a.glink_initialized && setup_returns==1 && a.glink_observe_until==402);
    cold(0);a.glink_observe_ms=101;na_adsp_smp2p_event(&a);
    assert(na_adsp_poll(&a,201)==NA_ARGUMENT && !a.glink_initialized);
    puts("GLINK lifecycle PASS: SMP2P before PAS, FIFO reset only after fresh ready, busy retry without reset, peer indices and SMP2P preserved, bad storage terminal without replay");
    puts("Deferred GLINK PASS: fresh-ready timer once, no FIFO writes before due, independent handover, fatal still terminal, due setup once and bounded delay");
    puts("Dedicated watchdog PASS: actual auth baseline ignores the old event; fresh event is terminal before/after ready without SMP2P fatal, FIFO write or PAS replay");
    puts("Observed GLINK PASS: first setup/RX/poll separated without extra FIFO work; handover/fatal/watchdog remain live during every pause, busy setup not replayed, zero-delay behavior retained");
    return 0;
}
