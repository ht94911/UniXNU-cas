/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* sheng SMEM/SMP2P/GLINK lifecycle with the SM8850 PAS and proxy sequence. */
#include "AdspLink.h"
#include <string.h>
#include <limits.h>
static volatile uint8_t *item(NaAdsp *a,uint16_t id,uint32_t minimum,uint32_t *bytes) {
    uint32_t at=0,n=0;
    if (qcom_smem_find(&a->smem,&a->partition,id,&at,&n) || n<minimum || at>a->smem.size || n>a->smem.size-at || (at&3)) return 0;
    if (bytes) *bytes=n;return a->smem.base+at;
}
static int alloc_item(NaAdsp *a,uint16_t id,uint32_t bytes) {
    int r=a->ops.smem_lock(a->ops.context);if (r) return r;
    r=qcom_smem_alloc(&a->smem,&a->partition,id,bytes);a->ops.smem_unlock(a->ops.context);
    return r==0 || r==QcomSmemExists ? NA_OK:NA_IO;
}
static void kick(void *v) {NaAdsp *a=v;a->ops.doorbell(a->ops.context,0);}
int na_adsp_init(NaAdsp *a,volatile void *smem,uint32_t bytes,NaPasLoad *main_image,NaPasLoad *dtb_image,NaRxQueue *rx,const NaAdspOps *ops) {
    if (!a || !smem || ((uintptr_t)smem&3) || bytes!=0x200000 || !main_image || !dtb_image ||
        main_image->pas!=1 || dtb_image->pas!=36 || main_image->stage!=NaPasInit || dtb_image->stage!=NaPasInit ||
        !rx || !ops || !ops->smem_lock || !ops->smem_unlock || !ops->doorbell || !ops->proxy || !ops->dma_setup) return NA_ARGUMENT;
    memset(a,0,sizeof *a);a->smem=(QcomSmem){smem,bytes};a->main_image=main_image;a->dtb_image=dtb_image;a->rx=rx;a->ops=*ops;
    if (qcom_smem_partition(&a->smem,QCOM_SMEM_HOST_APPS,2,&a->partition)) return NA_MISSING;
    a->stage=NaAdspStopMain;return NA_OK;
}
void na_adsp_smp2p_event(NaAdsp *a) {if (a) __atomic_fetch_add(&a->doorbells,1,__ATOMIC_RELEASE);}
static int failed(NaAdsp *a,int r) {
    r=r<0 ? r:NA_IO;a->stage=NaAdspFailed;a->error=r;
    if (a->transport.channel) na_transport_lost(&a->transport);
    /* PAS/firmware/proxy resources remain owned. Generic failure is never a
     * proof that the DSP stopped doing DMA. Recovery requires PAS+IOMMU audit. */
    return r;
}
static int setup(NaAdsp *a) {
    int r=alloc_item(a,429,QCOM_SMP2P_ITEM_BYTES);if (r) return r;
    r=alloc_item(a,478,32);if (r) return r;
    r=alloc_item(a,479,0x4000);if (r) return r;
    volatile uint8_t *out=item(a,429,QCOM_SMP2P_ITEM_BYTES,0),*desc=item(a,478,32,0);
    uint32_t txbytes=0;volatile uint8_t *tx=item(a,479,0x4000,&txbytes);
    if (!out || !desc || !tx || txbytes!=0x4000) return NA_WIRE;
    qcom_smp2p_init_out(&a->p2p,(volatile uint32_t *)out,0,2);
    if (qcom_smp2p_add_entry(&a->p2p,"master-kernel")<0) return NA_WIRE;
    a->ops.doorbell(a->ops.context,2);
    volatile uint32_t *d=(volatile uint32_t *)desc;
    QcomGlinkFifo tf={&d[1],&d[0],tx,txbytes},rf={&d[3],&d[2],0,0};
    qcom_glink_init(&a->glink,tf,rf,kick,a);a->glink_initialized=1;
    return na_transport_init(&a->transport,&a->glink,a->rx);
}
static int smp2p(NaAdsp *a) {
    if (!a->p2p.out) return NA_OK;
    if (!a->p2p.in) a->p2p.in=(volatile uint32_t *)item(a,443,QCOM_SMP2P_ITEM_BYTES,0);
    if (!a->p2p.in) return NA_PENDING;
    if (qcom_smp2p_check_in(a->p2p.in,0,2)) return NA_PENDING;
    uint32_t changed=0;
    int r=qcom_smp2p_service(&a->p2p,"slave-kernel",&a->slave,&changed);
    if (r<0) return NA_WIRE;
    if (r) a->ops.doorbell(a->ops.context,2);
    return NA_OK;
}
int na_adsp_poll(NaAdsp *a,uint64_t now) {
    if (!a || a->stage==NaAdspCold || now>UINT64_MAX-30000) return NA_ARGUMENT;
    if (a->stage==NaAdspFailed) return a->error;
    if (!a->deadline) a->deadline=now+30000;
    if (a->stage!=NaAdspOnline && now>=a->deadline) return failed(a,NA_TIMEOUT);
    int r=0;
    switch (a->stage) {
    case NaAdspStopMain:
        r=a->main_image->ops.pas(a->main_image->ops.context,6,1,0,0);
        if (r) return failed(a,r);a->stage=NaAdspStopDtb;break;
    case NaAdspStopDtb:
        r=a->dtb_image->ops.pas(a->dtb_image->ops.context,6,36,0,0);
        if (r) return failed(a,r);a->masters_stopped=1;a->stage=NaAdspDma;break;
    case NaAdspDma:
        r=a->ops.dma_setup(a->ops.context);
        if (!r) {a->dma_ready=1;a->stage=NaAdspSetup;}break;
    case NaAdspSetup:
        r=setup(a);if (!r) a->stage=NaAdspPower;break;
    case NaAdspPower:
        a->proxy_possible=1;r=a->ops.proxy(a->ops.context,1);
        if (!r) {a->stage=NaAdspDtb;a->dtb_image->deadline=now+30000;}break;
    case NaAdspDtb:
        if (!a->masters_stopped || !a->dma_ready) return failed(a,NA_OWNERSHIP);
        r=na_pas_poll(a->dtb_image,now);
        if (!r) {a->stage=NaAdspMain;a->main_image->deadline=now+30000;}break;
    case NaAdspMain:
        /* Establish the event baseline immediately BEFORE auth_and_reset.
         * A stale SMEM ready bit alone must not accept the previous firmware. */
        if (a->main_image->stage==NaPasAuthenticate) a->auth_doorbells=__atomic_load_n(&a->doorbells,__ATOMIC_ACQUIRE);
        if (a->main_image->stage==NaPasAuthenticate) a->masters_stopped=0;
        r=na_pas_poll(a->main_image,now);
        if (!r) {a->stage=NaAdspWaitReady;a->deadline=now+5000;}break;
    case NaAdspWaitReady:
    case NaAdspOnline:
        r=smp2p(a);
        if (r<0) break;
        if (__atomic_load_n(&a->doorbells,__ATOMIC_ACQUIRE)!=a->auth_doorbells) {
            if (a->slave&1) return failed(a,NA_REMOTE);
            if (a->slave&2) a->stage=NaAdspOnline;
        }
        if (a->stage==NaAdspOnline) {
            if (!a->glink.rx.data) {
                uint32_t bytes=0;volatile uint8_t *rx=item(a,480,64,&bytes);
                if (rx) {if (bytes>0x100000 || (bytes&7)) return failed(a,NA_WIRE);qcom_glink_set_rx(&a->glink,rx,bytes);}
            }
            if ((a->slave&4) && !a->handed) {
                r=a->ops.proxy(a->ops.context,0);
                if (!r) {a->handed=1;a->proxy_possible=0;}
                else if (r<0) {a->cleanup_error=r;return failed(a,r);}
            }
            int tr=na_transport_poll(&a->transport);if (tr<0) return failed(a,tr);
        }
        break;
    default:return failed(a,NA_OWNERSHIP);
    }
    if (r<0 && r!=NA_BUSY) return failed(a,r);
    return a->stage==NaAdspOnline ? NA_OK:NA_PENDING;
}
