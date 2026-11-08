/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuses QcaBluetooth's executor, H4/IBS and GENI engine. Power ordering and
 * TX-drain/cancel rules follow sheng BluetoothProbe.cpp; hardware ownership
 * remains with the chosen SM8850 platform provider. See SOURCE.md. */
#include "NezhaBluetoothBoard.h"
#include "qbt_firmware.h"
#ifdef KERNEL
#include <sys/errno.h>
#include <sys/systm.h>
#else
#include <errno.h>
#include <string.h>
#endif
static uint64_t now(struct nezha_bt_board *b) {return b->ops.now_ns(b->ops.context);}
static void wake(struct nezha_bt_board *b) {b->ops.wake(b->ops.context);}
static uint32_t read_reg(void *p,uint32_t r) {struct nezha_bt_board *b=p;return b->ops.geni.read(b->ops.geni.context,r);}
static void write_reg(void *p,uint32_t r,uint32_t v) {struct nezha_bt_board *b=p;b->ops.geni.write(b->ops.geni.context,r,v);}
static void barrier(void *p) {struct nezha_bt_board *b=p;b->ops.geni.barrier(b->ops.geni.context);}
static void sync_dma(void *p,const struct qbt_dma_buffer *d,size_t n,bool device,bool tx) {
    struct nezha_bt_board *b=p;b->ops.geni.sync(b->ops.geni.context,d,n,device,tx);
}
static void mask_irq(void *p,bool masked) {struct nezha_bt_board *b=p;b->ops.geni.irq_mask(b->ops.geni.context,masked);}
static void wake_worker(void *p) {wake(p);}
static uint64_t now_cb(void *p) {return now(p);}
static void wait_cb(void *p,uint64_t deadline) {struct nezha_bt_board *b=p;b->ops.wait_until(b->ops.context,deadline);}
static void *allocate(void *p,size_t n) {struct nezha_bt_board *b=p;return b->ops.allocate(b->ops.context,n);}
static void release(void *p,void *v,size_t n) {struct nezha_bt_board *b=p;b->ops.release(b->ops.context,v,n);}
static void log_line(void *p,int level,const char *line) {struct nezha_bt_board *b=p;if(b->ops.log)b->ops.log(b->ops.context,level,line);}
static size_t from_uart(void *p,const uint8_t *data,size_t n) {
    struct nezha_bt_board *b=p;
    size_t used=qbt_receive(b->core,data,n);
    struct qbt_info info;qbt_get_info(b->core,&info);
    /* Stop the raw bootstrap cache before NVM and the BD_ADDR readback.
     * Complete-event opcode/status counters cover the remaining startup. */
    if(b->phase==NEZHA_BT_FIRMWARE && !info.board_valid && used && b->ops.bootstrap_rx)
        b->ops.bootstrap_rx(b->ops.context,data,used);
    return used;
}
static size_t to_stack(void *p,const uint8_t *data,size_t n) {struct nezha_bt_board *b=p;return b->ops.receive(b->ops.context,data,n);}
static size_t to_uart(void *p,const uint8_t *data,size_t n) {struct nezha_bt_board *b=p;return qbt_geni_write(&b->geni,data,n);}
static bool drained(void *p) {struct nezha_bt_board *b=p;return qbt_geni_drained(&b->geni);}
static int flow(void *p,bool enable) {struct nezha_bt_board *b=p;qbt_geni_set_flow(&b->geni,enable);return 0;}
static void pump(void *p) {
    struct nezha_bt_board *b=p;
    struct qbt_info info;qbt_get_info(b->core,&info);
    if(info.state!=3) {
        if (__atomic_load_n(&b->cancelled,__ATOMIC_ACQUIRE) ||
            (b->phase!=NEZHA_BT_STOPPING && __atomic_load_n(&b->closing,__ATOMIC_ACQUIRE))) qbt_fail(b->core,-ECANCELED);
        else if(b->boot_deadline && now(b)>=b->boot_deadline)qbt_fail(b->core,-ETIMEDOUT);
    }
    if (b->clocked && b->phase!=NEZHA_BT_QUARANTINED) {
        if(qbt_geni_service(&b->geni))wake(b);
        qbt_get_info(b->core,&info);
        if(b->geni.state==QBT_GENI_FAILED && info.state!=3)qbt_fail(b->core,b->geni.error);
    }
    if(b->ops.bootstrap_tick)b->ops.bootstrap_tick(b->ops.context);
}
/* GPIO27 is a real DT wake source, but its selected-package IRQ/pin ownership
 * and restore sequence have not been validated. Keep the serial clock running
 * instead of borrowing sheng GPIO79/INTID240 or claiming sleep coverage. IBS
 * wake/ack/sleep bytes still run in the reused protocol core. */
static int clock_vote(void *p,bool enable) {struct nezha_bt_board *b=p;return enable?(b->clocked?0:-EIO):-EOPNOTSUPP;}
static int quiesce(struct nezha_bt_board *b) {
    uint64_t deadline=now(b)+40000000ULL;
    for (;;) {
        int e=qbt_geni_quiesce(&b->geni);
        if(!e || b->geni.state==QBT_GENI_STOPPED)return 0;
        if(e!=-EBUSY && e!=-EINPROGRESS)return e;
        pump(b);
        uint64_t t=now(b);if(t>=deadline)return -ETIMEDOUT;
        uint64_t next=qbt_geni_deadline(&b->geni);
        if(!next || next>t+1000000ULL)next=t+1000000ULL;
        if(next>deadline)next=deadline;
        wait_cb(b,next);
    }
}
static int rate(void *p,uint32_t baud) {
    struct nezha_bt_board *b=p;
    int e=quiesce(b);uint32_t hz=0;
    if(!e)e=b->ops.serial_source(b->ops.context,baud,&hz);
    if(!e)e=qbt_geni_set_rate(&b->geni,baud,hz,16);
    if(!e)e=qbt_geni_resume(&b->geni);
    return e;
}
static int firmware(void *p,const char *name,const uint8_t **data,size_t *size) {
    struct nezha_bt_board *b=p;
    *data=NULL;*size=0;
    if(!name || strncmp(name,"qca/",4))return -ENOENT;
    const char *base=name+4;enum qbt_firmware_kind kind;
    if(!strcmp(base,b->patch_name)) {
        const char *dot=strrchr(base,'.');kind=dot && !strcmp(dot,".mbn")?QBT_FW_ELF_PATCH:QBT_FW_TLV_PATCH;
    } else if(!strcmp(base,b->nvm_name))kind=QBT_FW_NVM;
    else if(b->boot.nvm_by_board && !strncmp(base,b->nvm_name,strlen(b->nvm_name)) &&
            base[strlen(b->nvm_name)]=='.') {
        struct qbt_info info; qbt_get_info(b->core,&info);
        if(!info.board_valid)return -ENODEV;
        kind=QBT_FW_NVM;
    }
    else return -ENOENT;
    int e=b->ops.firmware_get(b->ops.context,name,data,size);
    if(e)return e;
    e=qbt_firmware_validate(kind,*data,*size);
    if(e){b->ops.firmware_put(b->ops.context,*data,*size);*data=NULL;*size=0;}
    return e;
}
static void firmware_put(void *p,const uint8_t *data,size_t n) {struct nezha_bt_board *b=p;b->ops.firmware_put(b->ops.context,data,n);}
static void interrupt(void *p) {struct nezha_bt_board *b=p;qbt_geni_irq(&b->geni);}
static int name(char out[64],const char *in) {
    if(!in || !*in || strlen(in)>=64 || in[0]=='.' || strstr(in,".."))return -EINVAL;
    for(const char *p=in;*p;p++)if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='.' || *p=='_' || *p=='-'))return -EINVAL;
    memcpy(out,in,strlen(in)+1);return 0;
}
int nezha_bt_board_init(struct nezha_bt_board *b,const struct nezha_bt_board_ops *o,
    const struct qbt_boot_options *boot,const uint8_t address[6]) {
    if(!b || !o || !boot || !address || boot->controller!=QBT_WCN7860 ||
       !o->allocate || !o->release || !o->now_ns || !o->wait_until || !o->wake ||
       !o->resource_acquire || !o->resource_dma_map || !o->resource_release || !o->power_acquire || !o->power_release ||
       !o->reset_enable || !o->pins_default || !o->pins_active || !o->serial_clock || !o->serial_retire || !o->serial_source ||
       !o->irq_attach || !o->irq_detach || !o->firmware_get || !o->firmware_put || !o->receive ||
       !o->geni.read || !o->geni.write || !o->geni.barrier || !o->geni.sync || !o->geni.irq_mask) return -EINVAL;
    if(b->core || b->dma.cookie || b->power_lease || b->irq_attached || b->clocked || b->pins_set || b->reset_set)return -EBUSY;
    if (boot->baud_method!=QBT_BAUD_FC48 && boot->baud_method!=QBT_BAUD_CURRENT)return -EINVAL;
    if ((boot->initial_baud!=115200 && boot->initial_baud!=3000000 && boot->initial_baud!=3200000 && boot->initial_baud!=4000000) ||
        (boot->operating_baud!=115200 && boot->operating_baud!=3000000 && boot->operating_baud!=3200000 && boot->operating_baud!=4000000))return -EINVAL;
    if ((boot->baud_method==QBT_BAUD_FC48 && boot->initial_baud!=115200) ||
        (boot->baud_method==QBT_BAUD_CURRENT && boot->initial_baud!=boot->operating_baud))return -EOPNOTSUPP;
    if(!(address[0]|address[1]|address[2]|address[3]|address[4]|address[5]) || (address[0]&1))return -EINVAL;
    memset(b,0,sizeof(*b));b->ops=*o;b->boot=*boot;memcpy(b->address,address,6);
    int e=name(b->patch_name,boot->rampatch);if(!e)e=name(b->nvm_name,boot->nvm);if(e)return e;
    if(!strcmp(b->patch_name,b->nvm_name) || strlen(b->patch_name)>=60 || strlen(b->nvm_name)>=60)return -EINVAL;
    b->boot.rampatch=b->patch_name;b->boot.nvm=b->nvm_name;
    struct qbt_platform platform={b,allocate,release,now_cb,to_uart,drained,rate,flow,
        clock_vote,pump,wait_cb,to_stack,firmware,firmware_put,log_line};
    b->core=qbt_create(&platform);if(!b->core)return -ENOMEM;
    return 0;
}
static int dma_valid(const struct nezha_bt_dma *d) {
    if(!d->cookie || d->uart_base!=NEZHA_BT_UART_BASE || d->qup_base!=NEZHA_BT_QUP_BASE ||
       d->intid!=NEZHA_BT_UART_INTID || d->sid!=NEZHA_BT_QUP_SID)return 0;
    const struct qbt_dma_buffer *all[5]={&d->tx,&d->rx[0],&d->rx[1],&d->rx[2],&d->rx[3]};
    for(unsigned i=0;i<5;i++) {
        const struct qbt_dma_buffer *a=all[i];size_t need=i?QBT_GENI_RX_SIZE:QBT_GENI_TX_SIZE;
        uintptr_t aa=(uintptr_t)a->bytes;
        if(!a->bytes || a->capacity<need || !a->address || (a->address&63u) || (aa&63u) ||
           a->address>UINT64_MAX-need || aa>UINTPTR_MAX-need)return 0;
        for(unsigned j=0;j<i;j++) {
            const struct qbt_dma_buffer *v=all[j];size_t vn=j?QBT_GENI_RX_SIZE:QBT_GENI_TX_SIZE;
            if(a->address<v->address+vn && v->address<a->address+need)return 0;
            uintptr_t vv=(uintptr_t)v->bytes;
            if(aa<vv+vn && vv<aa+need)return 0;
        }
    }
    return 1;
}
static int settle(struct nezha_bt_board *b,unsigned ms) {
    uint64_t end=now(b)+(uint64_t)ms*1000000ULL;
    while(now(b)<end) {
        if(__atomic_load_n(&b->cancelled,__ATOMIC_ACQUIRE) || __atomic_load_n(&b->closing,__ATOMIC_ACQUIRE))return -ECANCELED;
        wait_cb(b,end);
    }
    return 0;
}
int nezha_bt_board_power_on(struct nezha_bt_board *b) {
    if(!b || !b->core)return -EINVAL;
    if(__atomic_load_n(&b->closing,__ATOMIC_ACQUIRE))return -ESHUTDOWN;
    if(b->phase==NEZHA_BT_QUARANTINED)return b->cleanup_error?b->cleanup_error:-EIO;
    if(b->powered)return 0;
    __atomic_store_n(&b->cancelled,0,__ATOMIC_RELEASE);b->error=b->cleanup_error=0;
    b->phase=NEZHA_BT_RESOURCES;
    int e=b->ops.resource_acquire(b->ops.context,&b->dma);
    if(!e && !dma_valid(&b->dma))e=-EINVAL;
    if(e)goto fail;
    b->phase=NEZHA_BT_POWER;
    e=b->ops.power_acquire(b->ops.context,NEZHA_BT_POWER_OWNER,&b->power_lease);if(e)goto fail;
    if(!b->power_lease){e=-EIO;b->phase=NEZHA_BT_QUARANTINED;b->cleanup_error=e;goto fail;}
    b->reset_set=true;e=b->ops.reset_enable(b->ops.context,b->power_lease,false);if(e)goto fail;
    b->pins_set=true;e=b->ops.pins_default(b->ops.context);if(e)goto fail;
    e=settle(b,100);if(e)goto fail;
    e=b->ops.serial_clock(b->ops.context,true);
    if(e){b->phase=NEZHA_BT_QUARANTINED;b->cleanup_error=e;goto fail;}
    b->clocked=true;
    b->phase=NEZHA_BT_UART;
    e=b->ops.serial_retire(b->ops.context);if(e)goto fail;
    struct qbt_geni_ops io={b,read_reg,write_reg,barrier,sync_dma,now_cb,mask_irq,wake_worker,from_uart};
    e=qbt_geni_prepare(&b->geni,&io,&b->dma.tx,b->dma.rx);if(e)goto fail;
    e=b->ops.resource_dma_map(b->ops.context,&b->dma);if(e)goto fail;
    uint32_t hz=0;e=b->ops.serial_source(b->ops.context,b->boot.initial_baud,&hz);if(e)goto fail;
    e=qbt_geni_set_rate(&b->geni,b->boot.initial_baud,hz,16);if(e)goto fail;
    b->irq_attached=true;e=b->ops.irq_attach(b->ops.context,b->dma.intid,interrupt,b);if(e)goto fail;
    mask_irq(b,true);
    e=b->ops.reset_enable(b->ops.context,b->power_lease,true);if(e)goto fail;
    e=settle(b,60);if(e)goto fail;
    b->pins_set=true;e=b->ops.pins_active(b->ops.context,true);if(e)goto fail;
    e=qbt_geni_start(&b->geni);if(e)goto fail;
    b->phase=NEZHA_BT_FIRMWARE;
    b->boot_deadline=now(b)+25000000000ULL;
    e=qbt_boot_controller(b->core,&b->boot,b->address);if(e)goto fail;
    b->boot_deadline=0;b->powered=true;b->phase=NEZHA_BT_READY;return 0;
fail:
    b->error=e;b->cleanup_error=nezha_bt_board_power_off(b);
    if(b->phase!=NEZHA_BT_QUARANTINED)b->phase=NEZHA_BT_FAILED;
    return e;
}
int nezha_bt_board_power_off(struct nezha_bt_board *b) {
    if(!b || !b->core)return -EINVAL;
    if(b->phase==NEZHA_BT_QUARANTINED)return b->cleanup_error?b->cleanup_error:-EIO;
    b->boot_deadline=0;__atomic_store_n(&b->cancelled,0,__ATOMIC_RELEASE);
    b->phase=NEZHA_BT_STOPPING;
    int first=0,e=0;
    if(b->clocked) {
        if(b->geni.state!=QBT_GENI_OFF) {
            first=qbt_shutdown(b->core);
            e=quiesce(b);if(!first)first=e;
        }
        /* prepare may have returned EBUSY with state OFF. Even then, retire
         * the inherited engines before removing clocks or any DMA route.
         * A failed quiesce is releasable only with this independent proof. */
        e=b->ops.serial_retire(b->ops.context);if(e)goto quarantine;
        b->geni.state=QBT_GENI_OFF;b->geni.tx_busy=b->geni.rx_busy=false;
    }
    if(b->irq_attached) {
        mask_irq(b,true);
        e=b->ops.irq_detach(b->ops.context,b->dma.intid,b);if(e)goto quarantine;
        b->irq_attached=false;
    }
    if(b->reset_set) {e=b->ops.reset_enable(b->ops.context,b->power_lease,false);if(e)goto quarantine;b->reset_set=false;}
    if(b->pins_set) {e=b->ops.pins_active(b->ops.context,false);if(e)goto quarantine;b->pins_set=false;}
    if(b->clocked) {e=b->ops.serial_clock(b->ops.context,false);if(e)goto quarantine;b->clocked=false;}
    if(b->dma.cookie) {e=b->ops.resource_release(b->ops.context,&b->dma);if(e)goto quarantine;memset(&b->dma,0,sizeof(b->dma));}
    if(b->power_lease) {e=b->ops.power_release(b->ops.context,NEZHA_BT_POWER_OWNER,b->power_lease);if(e)goto quarantine;b->power_lease=0;}
    b->powered=false;b->phase=NEZHA_BT_OFF;b->cleanup_error=first;
    return first;
quarantine:
    if(b->irq_attached)mask_irq(b,true);
    qbt_fail(b->core,e);b->powered=false;b->phase=NEZHA_BT_QUARANTINED;b->cleanup_error=e;
    /* Callback/engine ownership is uncertain. Keep the remaining DMA, lease
     * and clock state rather than freeing memory under a live bus master. */
    return e;
}
int nezha_bt_board_destroy(struct nezha_bt_board *b) {
    if(!b || !b->core)return -EINVAL;
    int e=nezha_bt_board_power_off(b);
    if(b->phase==NEZHA_BT_QUARANTINED)return e;
    qbt_destroy(b->core);b->core=NULL;return e;
}
void nezha_bt_board_cancel(struct nezha_bt_board *b) {if(b && b->ops.wake){__atomic_store_n(&b->cancelled,1,__ATOMIC_RELEASE);wake(b);}}
void nezha_bt_board_stop(struct nezha_bt_board *b) {if(b && b->ops.wake){__atomic_store_n(&b->closing,1,__ATOMIC_RELEASE);wake(b);}}
void nezha_bt_board_step(struct nezha_bt_board *b) {if(b && b->core && b->powered){pump(b);qbt_step(b->core);}}
uint64_t nezha_bt_board_deadline(const struct nezha_bt_board *b) {
    if(!b || !b->core || !b->powered)return UINT64_MAX;
    uint64_t a=qbt_deadline(b->core),g=qbt_geni_deadline(&b->geni);return a<g?a:g;
}
