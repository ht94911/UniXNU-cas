/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Real lifecycle and SMEM/SMP2P/GLINK with synthetic PAS/proxy callbacks. */
#include "../../package/baseband27/platform/NbMss.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static _Alignas(4096) uint8_t memory[0x200000], original[0x10000];
static NbMss m; static NbPas main_image, dtb_image; static QcomSmp2p peer;
static unsigned lock_busy, locked, stops, assigns, powers, power_off, auths, busy_stop, decrypt;
static int fail_main, fail_dtb; static volatile uint32_t *desc;
static void wr(unsigned at,unsigned v) { memcpy(memory+at,&v,4); }
static int lock(void *v) { (void)v; if(lock_busy)return NB_BUSY; assert(!locked);locked=1;return 0; }
static void unlock(void *v) { (void)v; assert(locked);locked=0; }
static void bell(void *v,unsigned sig) { (void)v; assert(sig==0 || sig==2); assert(!locked); }
static int proxy(void *v,int on) { (void)v;if(on)++powers;else ++power_off;return 0; }
static int assign(void *v,int on) { (void)v; assert(on && m.masters_stopped && m.initial_stop_mask==3);++assigns;return 0; }
static int pas(void *v,unsigned cmd,unsigned id,uint64_t a,uint64_t b)
{
    (void)v;(void)a;(void)b; assert(!locked && powers && (id==4 || id==0x26));
    if(cmd==5) { ++auths;assert(m.masters_stopped==0);return 0; }
    assert(cmd==6);
    if(busy_stop) { --busy_stop;return NB_BUSY; }
    if(id==4 && decrypt) { --decrypt;return NB_PAS_EINVAL; }
    ++stops;return id==4 ? fail_main : fail_dtb;
}
static volatile uint8_t *allocate(unsigned id,unsigned bytes)
{
    unsigned at,n; assert(!qcom_smem_alloc(&m.smem,&m.partition,(uint16_t)id,bytes));
    assert(!qcom_smem_find(&m.smem,&m.partition,(uint16_t)id,&at,&n) && n==bytes);
    return m.smem.base+at;
}
static void fixture(unsigned bad_out,unsigned bad_fifo)
{
    memset(memory,0,sizeof memory);memset(&m,0,sizeof m);memset(&main_image,0,sizeof main_image);memset(&dtb_image,0,sizeof dtb_image);
    lock_busy=locked=stops=assigns=powers=power_off=auths=busy_stop=decrypt=0;fail_main=fail_dtb=0;
    unsigned toc=sizeof memory-4096,part=0x10000,size=0x10000;
    wr(toc,0x434f5424);wr(toc+4,1);wr(toc+8,1);wr(toc+32,part);wr(toc+36,size);wr(toc+44,1u<<16);wr(toc+48,64);
    wr(part,0x54525024);wr(part+4,1u<<16);wr(part+8,size);wr(part+12,32);wr(part+16,size);
    main_image.pas=4;dtb_image.pas=0x26;main_image.stage=dtb_image.stage=NbPasInit;
    main_image.ops.pas=pas;dtb_image.ops.pas=pas;main_image.timeout=dtb_image.timeout=30000;
    NbMssOps ops={0,lock,unlock,bell,proxy,assign,0};
    assert(!nb_mss_init(&m,memory,sizeof memory,&main_image,&dtb_image,&ops));
    memset((void *)allocate(428,bad_out ? 344:340),0xaa,bad_out ? 344:340);
    qcom_smp2p_init_out(&peer,(volatile uint32_t *)allocate(435,340),1,0);
    assert(qcom_smp2p_add_entry(&peer,"slave-kernel")==0 && qcom_smp2p_update(&peer,0,6,6));
    desc=(volatile uint32_t *)allocate(478,32);desc[0]=0;desc[1]=32;desc[2]=8;desc[3]=8;
    (void)allocate(479,bad_fifo ? 0x5000:0x4000);
    volatile uint32_t *rx=(volatile uint32_t *)allocate(480,0x4000);rx[0]=0x00010000;rx[1]=1;
    memcpy(original,memory+part,sizeof original);
}
static void to_dtb(void)
{
    for(unsigned t=0;t<7;++t) assert(nb_mss_poll(&m,t)==NB_PENDING);
    assert(m.stage==NbMssDtb && stops==2 && powers==1 && assigns==1 && m.masters_stopped);
    assert(m.p2p.out && !m.glink_ready && desc[1]==32 && desc[2]==8);
}
static void authenticate(void)
{
    dtb_image.stage=NbPasReady;assert(nb_mss_poll(&m,10)==NB_PENDING && m.stage==NbMssMain);
    main_image.stage=NbPasAuthenticate;main_image.deadline=30000;
    nb_mss_wdog_event(&m); /* Before the new authentication baseline: old event. */
    assert(nb_mss_poll(&m,11)==NB_PENDING && m.stage==NbMssWaitReady && auths==1);
    assert(nb_mss_poll(&m,12)==NB_PENDING && !m.glink_ready && desc[1]==32 && desc[2]==8);
}
int main(void)
{
    fixture(0,0);to_dtb();authenticate();nb_mss_smp2p_event(&m);lock_busy=1;
    assert(nb_mss_poll(&m,13)==NB_PENDING && m.stage==NbMssOnline && !m.glink_ready);
    lock_busy=0;assert(nb_mss_poll(&m,14)==NB_PENDING && m.glink_ready && !m.glink_polled);
    assert(desc[0]==0 && desc[1]==0 && desc[2]==0 && desc[3]==8);
    assert(nb_mss_poll(&m,15)==NB_OK && m.glink_polled && m.handed && power_off==1);
    assert(nb_mss_poll(&m,16)==NB_OK && stops==2 && powers==1 && assigns==1 && auths==1);
    /* Stale SMEM ready and shared lock contention cannot publish transport. */
    fixture(0,0);to_dtb();authenticate();assert(!m.glink_ready && !m.handed);
    assert(nb_mss_poll(&m,6000)==NB_TIMEOUT && !m.glink_ready);
    /* Both independent stops are observed; any failure prevents all writes. */
    for(unsigned which=0;which<2;++which) {
        fixture(0,0);if(which)fail_dtb=NB_IO;else fail_main=NB_IO;
        assert(nb_mss_poll(&m,0)==NB_PENDING);assert(nb_mss_poll(&m,1)==NB_PENDING);
        assert(nb_mss_poll(&m,2)==NB_IO && m.initial_stop_mask==3 && stops==2 && !assigns && !m.p2p.out);
        assert(!memcmp(original,memory+0x10000,sizeof original));
        assert(nb_mss_poll(&m,3)==NB_IO && stops==2);
    }
    /* Busy submits nothing; EINVAL uses the existing decrypt stop schedule. */
    fixture(0,0);busy_stop=1;decrypt=1;assert(nb_mss_poll(&m,0)==NB_PENDING);
    assert(nb_mss_poll(&m,1)==NB_PENDING && !stops);assert(nb_mss_poll(&m,2)==NB_PENDING && m.retry_at==102);
    assert(nb_mss_poll(&m,101)==NB_PENDING && !stops);assert(nb_mss_poll(&m,102)==NB_PENDING && stops==1);
    assert(nb_mss_poll(&m,103)==NB_PENDING && stops==2 && m.masters_stopped);
    assert(nb_mss_poll(&m,104)==NB_PENDING && m.stage==NbMssAssign);
    /* Existing storage with the wrong size remains intact and is rejected. */
    fixture(1,0);for(unsigned t=0;t<5;++t)assert(nb_mss_poll(&m,t)==NB_PENDING);
    assert(nb_mss_poll(&m,5)==NB_WIRE && !m.p2p.out && desc[1]==32 && desc[2]==8);
    fixture(0,1);to_dtb();authenticate();nb_mss_smp2p_event(&m);
    assert(nb_mss_poll(&m,13)==NB_WIRE && !m.glink_ready && desc[1]==32 && desc[2]==8);
    /* Fresh watchdog after authentication remains fatal. */
    fixture(0,0);to_dtb();authenticate();nb_mss_wdog_event(&m);
    assert(nb_mss_poll(&m,13)==NB_REMOTE && !m.glink_ready);
    puts("PASS MSS reuse: proxy/two stops before assign/SMEM, same-size reused, fresh-ready GLINK, remote indices, busy/decrypt/errors/watchdog");
}
