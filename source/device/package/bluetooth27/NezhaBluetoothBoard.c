/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuses QcaBluetooth's executor, H4/IBS and GENI engine. Power ordering and
 * TX-drain/cancel rules follow sheng BluetoothProbe.cpp; hardware ownership
 * remains with the chosen SM8850 platform provider. See SOURCE.md. */
#include "NezhaBluetoothBoard.h"
#include "qbt_firmware.h"
#include "qbt_geni_regs.h"
#ifdef KERNEL
#include <sys/errno.h>
#include <sys/systm.h>
#include <kern/cpu_data.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#else
#include <errno.h>
#include <stdio.h>
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
static void log_line(void *p,int level,const char *line) {
    struct nezha_bt_board *b=p;
    if(b->ops.log && (b->boot_attempts<=3 || level<=4))b->ops.log(b->ops.context,level,line);
}
static void rate_trace(struct nezha_bt_board *b,const char *stage,int error) {
    if(!b->ops.log || b->boot_attempts>3)return;
    char line[256];
    snprintf(line,sizeof(line),"baud %s error=%d rate=%u source=%u sample=%u select=%u state=%u status=%x rfr=%x txcfg=%x clocks=%x/%x rx_busy=%u rx_ready=%u rx_len=%u rx_dma=%llu",
        stage,error,b->geni.baud,b->geni.source_hz,b->geni.oversampling,b->geni.clock_selector,b->geni.state,
        read_reg(b,G_STATUS),read_reg(b,G_MANUAL_RFR),read_reg(b,G_TX_CFG),read_reg(b,G_M_CLK),read_reg(b,G_S_CLK),
        b->geni.rx_busy,b->geni.rx_ready,read_reg(b,G_RX_LEN_IN),(unsigned long long)b->geni.stats.rx_dma);
    b->ops.log(b->ops.context,6,line);
}
static void failure_trace(struct nezha_bt_board *b,int error) {
    if(!b->ops.log || b->boot_attempts>3 || !b->clocked || b->geni.state==QBT_GENI_OFF)return;
    rate_trace(b,"init-error",error);
    char line[256];
    snprintf(line,sizeof(line),"UART failure error=%d ios=%x dma=%x fifo=%x irq=%x/%x/%x/%x rx_len=%u busy=%u/%u",
        error,read_reg(b,G_IOS),read_reg(b,G_DMA_DEBUG),read_reg(b,G_RX_FIFO_STATUS),
        read_reg(b,G_M_IRQ),read_reg(b,G_S_IRQ),read_reg(b,G_TX_IRQ),read_reg(b,G_RX_IRQ),
        read_reg(b,G_RX_LEN_IN),b->geni.tx_busy,b->geni.rx_busy);
    b->ops.log(b->ops.context,6,line);
    snprintf(line,sizeof(line),"UART failure counters tx=%llu rx=%llu tx_dma=%llu rx_dma=%llu fault=%x/%x/%x/%x",
        (unsigned long long)b->geni.stats.tx_bytes,(unsigned long long)b->geni.stats.rx_bytes,
        (unsigned long long)b->geni.stats.tx_dma,(unsigned long long)b->geni.stats.rx_dma,
        b->geni.fault_irq[0],b->geni.fault_irq[1],b->geni.fault_irq[2],b->geni.fault_irq[3]);
    b->ops.log(b->ops.context,6,line);
    if(b->ops.serial_snapshot)b->ops.serial_snapshot(b->ops.context);
}
static size_t from_uart(void *p,const uint8_t *data,size_t n) {
    struct nezha_bt_board *b=p;
    return b->peri_rx?qbt_peri_receive(b->peri,data,n):qbt_receive(b->core,data,n);
}
static size_t from_uart_at(void *p,const uint8_t *data,size_t n,uint64_t position) {
    struct nezha_bt_board *b=p;
    return b->peri_rx?qbt_peri_receive_at(b->peri,data,n,position):qbt_receive(b->core,data,n);
}
static size_t peri_hci(void *p,const uint8_t *data,size_t n) {
    struct nezha_bt_board *b=p;return qbt_receive(b->core,data,n);
}
static size_t to_stack(void *p,const uint8_t *data,size_t n) {struct nezha_bt_board *b=p;return b->ops.receive(b->ops.context,data,n);}
static size_t to_uart(void *p,const uint8_t *data,size_t n) {struct nezha_bt_board *b=p;return qbt_geni_write(&b->geni,data,n);}
static bool drained(void *p) {struct nezha_bt_board *b=p;return qbt_geni_drained(&b->geni);}
static int flow(void *p,bool enable) {
    /* Actual Ganges USERIAL_OP_FLOW_OFF/ON only changes TIOCM_RTS. */
    struct nezha_bt_board *b=p;qbt_geni_set_rts(&b->geni,enable);
    rate_trace(b,enable?"flow-on":"flow-off",0);return 0;
}
static void pump(void *p) {
    struct nezha_bt_board *b=p;
    struct qbt_info info;qbt_get_info(b->core,&info);
    int error=0;
    if (__atomic_load_n(&b->cancelled,__ATOMIC_ACQUIRE) ||
        (b->phase!=NEZHA_BT_STOPPING && __atomic_load_n(&b->closing,__ATOMIC_ACQUIRE))) error=-ECANCELED;
    else if(b->boot_deadline && now(b)>=b->boot_deadline)error=-ETIMEDOUT;
    if(error) {
        if(info.state!=3)qbt_fail(b->core,error);
        if(b->peri_rx)qbt_peri_fail(b->peri,error);
    }
    if (b->clocked && b->phase!=NEZHA_BT_QUARANTINED) {
        if(qbt_geni_service(&b->geni))wake(b);
        /* Space can return with no new UART bytes after the demux consumes
         * a complete H4 frame. Step drains only cached data/control; it never
         * reenters this platform pump or creates another RX executor. */
        if(b->peri_rx) {
            qbt_peri_step(b->peri);
            qbt_peri_get_info(b->peri,&b->peri_info);
            if(b->peri_info.error && b->peri_hci && info.state!=3)qbt_fail(b->core,b->peri_info.error);
        }
        qbt_get_info(b->core,&info);
        if(b->geni.state==QBT_GENI_FAILED) {
            if(info.state!=3)qbt_fail(b->core,b->geni.error);
            if(b->peri_rx)qbt_peri_fail(b->peri,b->geni.error);
        }
    }
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
    int e=quiesce(b);uint32_t hz=0,selector=0;
    rate_trace(b,"quiesce",e);
    if(!e)e=b->ops.serial_source(b->ops.context,baud,&hz,&selector);
    if(!e)e=qbt_geni_set_rate_index(&b->geni,baud,hz,16,selector);
    rate_trace(b,"retime",e);
    if(!e)e=qbt_geni_resume(&b->geni);
    rate_trace(b,"resume",e);
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
        char selected[64];
        /* The actual board0 response selects .bin in the locked Linux
         * initializer. A missing nonzero board must not retry that file. */
        if(info.board==UINT16_MAX)return -ENODEV;
        int count=info.board?
            snprintf(selected,sizeof(selected),"%s.b%02x",b->nvm_name,info.board):
            snprintf(selected,sizeof(selected),"%s.bin",b->nvm_name);
        if(count<0 || count>=(int)sizeof(selected) || strcmp(base,selected))return -ENODEV;
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
    if(!in || !*in || strlen(in)>=64 || in[0]=='.')return -EINVAL;
    for(const char *p=in;*p;p++)if((*p=='.' && p[1]=='.') ||
        !((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='.' || *p=='_' || *p=='-'))return -EINVAL;
    memcpy(out,in,strlen(in)+1);return 0;
}
static int ganges_config_valid(const struct nezha_bt_ganges_config *c) {
    return c && c->enabled &&
        !strncmp(c->patch,"gngperifw20.mbn",sizeof(c->patch)) &&
        !strncmp(c->nvm,"gngperinv20",sizeof(c->nvm)) &&
        !strncmp(c->tme,"tmel_peach_20.elf",sizeof(c->tme)) &&
        (c->elf_config==QBT_PERI_ELF_EXPLICIT ||
         (c->elf_config==QBT_PERI_ELF_NO_VALUE && !c->elf_value));
}
static void peri_trace(struct nezha_bt_board *b,const char *stage,int error) {
    if(b->peri)qbt_peri_get_info(b->peri,&b->peri_info);
    if(!b->ops.log || b->boot_attempts>3)return;
    char line[256];
    snprintf(line,sizeof(line),"PERI %s error=%d baud=%u access=%u awake=%u state=%02x/%02x board=%04x tx=%llu rx=%llu matched=%llu malformed=%llu activation=%u/%u",
        stage,error,b->peri_info.host_baud,b->peri_info.access_held,b->peri_info.awake,
        b->peri_arbitration,b->tme_arbitration,b->peri_board,
        (unsigned long long)b->peri_info.tx_bytes,(unsigned long long)b->peri_info.rx_bytes,
        (unsigned long long)b->peri_info.matched,(unsigned long long)b->peri_info.malformed,
        b->peri_info.activation_stage,b->peri_info.bt_activated);
    b->ops.log(b->ops.context,6,line);
}
static void peri_version_trace(struct nezha_bt_board *b,const struct qbt_peri_version *v,int error) {
    if(!b->ops.log || b->boot_attempts>3)return;
    char line[160];
    snprintf(line,sizeof(line),"PERI version error=%d subsystem=%u product=%x SoC=%x ROM=%x patch=%x security=%x",
        error,v->subsystem,v->product,v->soc,v->rom,v->patch,v->security);
    b->ops.log(b->ops.context,6,line);
}
static int peri_access(void *context,bool acquire,uint64_t deadline) {
    struct nezha_bt_board *b=context;
    if(now(b)>=deadline)return -ETIMEDOUT;
    /* This kernel has one PERI caller. Its existing BT/UWB reset lease and
     * UART resource cookie establish exclusive SoC access; reserve that
     * initialization phase without acquiring another power owner or touching
     * Android's userspace ioctl. Release preserves the live UART for HCI. */
    if(!b->power_lease || !b->dma.cookie)return -EIO;
    if(acquire) {
        if(b->peri_access || b->powered || b->phase!=NEZHA_BT_FIRMWARE ||
            !b->clocked || !b->irq_attached || b->geni.state!=QBT_GENI_RUN)return -EBUSY;
        b->peri_access=true;
    } else {
        if(!b->peri_access)return -EPERM;
        b->peri_access=false;
    }
    return 0;
}
static int peri_verify_bt(void *context,const struct qbt_peri_bt_identity *id,uint64_t deadline) {
    struct nezha_bt_board *b=context;
    if(now(b)>=deadline)return -ETIMEDOUT;
    if(id->product!=b->boot.expected_product || id->soc!=b->boot.expected_soc ||
        id->rom!=b->boot.expected_rom)return -ENODEV;
    return 0;
}
static int peri_rx_produced(void *context,uint64_t *count) {
    struct nezha_bt_board *b=context;
    if(!b->clocked || !b->peri_rx)return -EIO;
    int error=qbt_geni_rx_produced(&b->geni,count);
    if(b->ops.log && b->boot_attempts<=3) {
        char line[192];
        snprintf(line,sizeof(line),"PERI RX producer error=%d committed=%llu DMA=%u FIFO=%x total=%llu",
            error,(unsigned long long)b->geni.rx_produced,read_reg(b,G_RX_LEN_IN),
            read_reg(b,G_RX_FIFO_STATUS),(unsigned long long)(error?0:*count));
        b->ops.log(b->ops.context,6,line);
    }
    return error;
}
static int peri_write_fenced(void *context,const uint8_t *data,size_t length,size_t *consumed,uint64_t *mark) {
    struct nezha_bt_board *b=context;
    if(!consumed || !mark)return -EINVAL;
    *consumed=0;
    if(!b->clocked || !b->peri_rx)return -EIO;
    /* Reuse the selected kernel's short preemption guard around the RX
     * snapshot and TX admission. MMIO/cache callbacks here never block. */
#ifdef KERNEL
    disable_preemption();
#endif
    int error=qbt_geni_write_fenced(&b->geni,data,length,consumed,mark);
#ifdef KERNEL
    enable_preemption();
#endif
    return error;
}
static int peri_blob(struct nezha_bt_board *b,uint8_t subsystem,const char *base,enum qbt_firmware_kind kind) {
    char path[64];const uint8_t *data=NULL;uint8_t *prepared=NULL;size_t length=0;
    if(snprintf(path,sizeof(path),"qca/%s",base)>=(int)sizeof(path))return -EINVAL;
    int error=b->ops.firmware_get(b->ops.context,path,&data,&length);
    if(error)return error;
    error=qbt_firmware_validate(kind,data,length);
    if(!error && kind==QBT_FW_NVM && subsystem==QBT_PERI_SS) {
        /* ReadTlvInfo prepares tag17 after loading the verified stock NVM.
         * Change only the owned temporary copy; retain its original SHA/file. */
        prepared=b->ops.allocate(b->ops.context,length);
        if(!prepared)error=-ENOMEM;
        else {
            memcpy(prepared,data,length);
            error=qbt_firmware_prepare_peri_nvm(prepared,length,b->boot.operating_baud);
        }
    }
    if(!error) {
        struct qbt_peri_blob blob={prepared?prepared:data,length,b->ganges.elf_config,b->ganges.elf_value};
        error=qbt_peri_download(b->peri,subsystem,&blob,b->boot_deadline);
    }
    if(prepared)b->ops.release(b->ops.context,prepared,length);
    b->ops.firmware_put(b->ops.context,data,length);
    return error;
}
static int peri_nvm(struct nezha_bt_board *b,bool board_valid) {
    if(!board_valid || b->peri_board==UINT16_MAX)return -ENODEV;
    char nvm[60];
    if(snprintf(nvm,sizeof(nvm),b->peri_board>0xff?"%s.b%04x":"%s.b%02x",
        b->ganges.nvm,b->peri_board)>=(int)sizeof(nvm))return -EINVAL;
    int error=peri_blob(b,QBT_PERI_SS,nvm,QBT_FW_NVM);
    /* N0 HandleEdlCmdResEvt encodes a successful zero as "00" and
     * DownloadTlvFile tries .b00 before its base .bin. Permit only the
     * zero-ID/absent-variant case with an explicitly locked base image;
     * query/read/validation failures retain their error. */
    if(error==-ENOENT && b->peri_board==0) {
        if(snprintf(nvm,sizeof(nvm),"%s.bin",b->ganges.nvm)>=(int)sizeof(nvm))return -EINVAL;
        error=peri_blob(b,QBT_PERI_SS,nvm,QBT_FW_NVM);
    }
    return error;
}
static int ganges_prepare(struct nezha_bt_board *b) {
    /* The HAL's LoadPatchMaptable maps these real Ganges2.0 keys to the same
     * PERI images. TME filenames are independently selected from target3's
     * version; the H4 probe must still match this board's actual BT identity. */
    struct qbt_platform uart={b,allocate,release,now_cb,to_uart,drained,rate,flow,
        clock_vote,pump,wait_cb,NULL,firmware,firmware_put,log_line};
    struct qbt_peri_ops ops={.uart=uart,.access=peri_access,.verify_bt=peri_verify_bt,
        .rx_produced=peri_rx_produced,.uart_write_fenced=peri_write_fenced};
    b->peri=qbt_peri_create(&ops,b->boot.initial_baud);
    if(!b->peri)return -ENOMEM;
    b->peri_rx=true;b->peri_hci=false;b->tme_skipped=false;b->peri_arbitration=b->tme_arbitration=0xff;
    b->peri_board=UINT16_MAX;memset(&b->peri_info,0,sizeof(b->peri_info));
    memset(&b->peri_version,0,sizeof(b->peri_version));memset(&b->tme_version,0,sizeof(b->tme_version));
    int error=qbt_peri_acquire(b->peri,b->boot_deadline);
    if(!error)error=qbt_peri_wake(b->peri,b->boot_deadline);
    if(!error)error=qbt_peri_read_version(b->peri,QBT_PERI_SS,b->boot_deadline,&b->peri_version);
    peri_version_trace(b,&b->peri_version,error);
    if(!error && ((b->peri_version.product!=0x1e && b->peri_version.product!=0x21) ||
        b->peri_version.soc!=0x40210200 || b->peri_version.rom!=0x200))error=-ENODEV;
    peri_trace(b,"version",error);
    if(!error)error=qbt_peri_set_baud(b->peri,b->boot.operating_baud,b->boot_deadline);
    peri_trace(b,"baud",error);
    uint64_t floor=qbt_peri_sequence(b->peri);
    if(!error)error=qbt_peri_arbitrate(b->peri,QBT_PERI_SS,b->boot_deadline,&b->peri_arbitration);
    peri_trace(b,"arbitrate",error);
    if(!error && b->peri_arbitration==0) {
        error=peri_blob(b,QBT_PERI_SS,b->ganges.patch,QBT_FW_ELF_PATCH);
        peri_trace(b,"patch",error);
        bool board_valid=false;
        if(!error) {
            error=qbt_peri_read_board_id(b->peri,b->boot_deadline,&b->peri_board);
            board_valid=!error;
        }
        if(!error)error=peri_nvm(b,board_valid);
        peri_trace(b,"NVM",error);
        char build[128];
        if(!error)error=qbt_peri_read_build_info(b->peri,QBT_PERI_SS,b->boot_deadline,build,sizeof(build));
        if(!error)error=qbt_peri_reset(b->peri,b->boot_deadline);
        peri_trace(b,"reset-ready",error);
    } else if(!error && b->peri_arbitration==0x0b) {
        error=qbt_peri_wait_ready(b->peri,QBT_PERI_SS,floor,b->boot_deadline);
        peri_trace(b,"external-ready",error);
    }
    if(!error)error=qbt_peri_wake(b->peri,b->boot_deadline);
    floor=qbt_peri_sequence(b->peri);
    if(!error)error=qbt_peri_arbitrate(b->peri,QBT_PERI_TME,b->boot_deadline,&b->tme_arbitration);
    peri_trace(b,"TME-arbitrate",error);
    if(!error && b->tme_arbitration==0) {
        const char *selected=NULL;
        error=qbt_peri_read_version(b->peri,QBT_PERI_TME,b->boot_deadline,&b->tme_version);
        peri_version_trace(b,&b->tme_version,error);
        if(!error)error=qbt_peri_tme_filename(&b->tme_version,&selected);
        if(!error && strcmp(selected,b->ganges.tme))error=-ENODEV;
        peri_trace(b,"TME-version",error);
        floor=qbt_peri_sequence(b->peri);
        if(!error)error=peri_blob(b,QBT_PERI_TME,selected,QBT_FW_ELF_PATCH);
        if(!error)error=qbt_peri_wait_ready(b->peri,QBT_PERI_TME,floor,b->boot_deadline);
        peri_trace(b,"TME-patch-ready",error);
    } else if(!error && b->tme_arbitration==0x0b) {
        error=qbt_peri_wait_ready(b->peri,QBT_PERI_TME,floor,b->boot_deadline);
        peri_trace(b,"TME-external-ready",error);
    } else if(!error && (b->tme_arbitration==5 || b->tme_arbitration==6)) {
        b->tme_skipped=true;peri_trace(b,"TME-HAL-skip-not-patch-success",0);
    }
    if(!error)error=qbt_peri_wake(b->peri,b->boot_deadline);
    if(!error)error=qbt_peri_activate_bt(b->peri,b->boot_deadline);
    peri_trace(b,"BT-activation-and-H4-probe",error);
    /* The original BT initializer still receives34 packets. Keep this demux
     * through patch/NVM/Reset/runtime and relay H4 plus IBS to the existing
     * core. No UART close/reopen or second RX owner is introduced. */
    if(!error)error=qbt_peri_enter_hci(b->peri,peri_hci);
    if(!error)b->peri_hci=true;
    peri_trace(b,"mixed-H4-handoff",error);
    return error;
}
int nezha_bt_board_init(struct nezha_bt_board *b,const struct nezha_bt_board_ops *o,
    const struct qbt_boot_options *boot,const uint8_t address[6]) {
    if(!b || !o || !boot || !address || boot->controller!=QBT_WCN7860 ||
       !o->allocate || !o->release || !o->now_ns || !o->wait_until || !o->wake ||
       !o->resource_acquire || !o->resource_dma_map || !o->resource_release || !o->power_acquire || !o->power_release ||
       !o->reset_enable || !o->pins_default || !o->pins_active || !o->serial_clock || !o->serial_retire || !o->serial_source ||
       !o->irq_attach || !o->irq_detach || !o->firmware_get || !o->firmware_put || !o->receive ||
       !o->geni.read || !o->geni.write || !o->geni.barrier || !o->geni.sync || !o->geni.irq_mask) return -EINVAL;
    if(b->core || b->peri || b->peri_access || b->dma.cookie || b->power_lease || b->irq_attached || b->clocked || b->pins_set || b->reset_set)return -EBUSY;
    if (boot->baud_method!=QBT_BAUD_FC48 && boot->baud_method!=QBT_BAUD_CURRENT && boot->baud_method!=QBT_BAUD_PERI)return -EINVAL;
    if ((boot->initial_baud!=115200 && boot->initial_baud!=3000000 && boot->initial_baud!=3200000 && boot->initial_baud!=4000000) ||
        (boot->operating_baud!=115200 && boot->operating_baud!=3000000 && boot->operating_baud!=3200000 && boot->operating_baud!=4000000))return -EINVAL;
    if ((boot->baud_method==QBT_BAUD_FC48 && boot->initial_baud!=115200) ||
        (boot->baud_method==QBT_BAUD_CURRENT && boot->initial_baud!=boot->operating_baud))return -EOPNOTSUPP;
    if (boot->baud_method==QBT_BAUD_PERI &&
        (boot->initial_baud!=115200 || boot->operating_baud!=3200000 || boot->expected_product!=0x1e ||
         boot->expected_soc!=0x40210200 || boot->expected_rom!=0x200 || !ganges_config_valid(o->ganges)))return -EOPNOTSUPP;
    if (o->ganges && o->ganges->enabled && boot->baud_method!=QBT_BAUD_PERI)return -EINVAL;
    if(!(address[0]|address[1]|address[2]|address[3]|address[4]|address[5]) || (address[0]&1))return -EINVAL;
    memset(b,0,sizeof(*b));b->ops=*o;b->boot=*boot;memcpy(b->address,address,6);
    if(boot->baud_method==QBT_BAUD_PERI)b->ganges=*o->ganges;
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
uint64_t nezha_bt_board_boot_budget_ns(const struct qbt_boot_options *boot) {
    if(boot && boot->controller==QBT_WCN7860 && boot->baud_method==QBT_BAUD_CURRENT &&
       boot->initial_baud==115200 && boot->operating_baud==115200)return 60000000000ULL;
    return 25000000000ULL;
}
int nezha_bt_board_power_on(struct nezha_bt_board *b) {
    if(!b || !b->core)return -EINVAL;
    if(__atomic_load_n(&b->closing,__ATOMIC_ACQUIRE))return -ESHUTDOWN;
    if(b->phase==NEZHA_BT_QUARANTINED)return b->cleanup_error?b->cleanup_error:-EIO;
    if(b->powered)return 0;
    __atomic_store_n(&b->cancelled,0,__ATOMIC_RELEASE);b->error=b->cleanup_error=0;
    if(b->boot_attempts!=UINT32_MAX)b->boot_attempts++;
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
    struct qbt_geni_ops io={b,read_reg,write_reg,barrier,sync_dma,now_cb,mask_irq,wake_worker,from_uart,from_uart_at};
    e=qbt_geni_prepare(&b->geni,&io,&b->dma.tx,b->dma.rx);if(e)goto fail;
    e=b->ops.resource_dma_map(b->ops.context,&b->dma);if(e)goto fail;
    uint32_t hz=0,selector=0;e=b->ops.serial_source(b->ops.context,b->boot.initial_baud,&hz,&selector);if(e)goto fail;
    e=qbt_geni_set_rate_index(&b->geni,b->boot.initial_baud,hz,16,selector);if(e)goto fail;
    b->irq_attached=true;e=b->ops.irq_attach(b->ops.context,b->dma.intid,interrupt,b);if(e)goto fail;
    mask_irq(b,true);
    e=b->ops.reset_enable(b->ops.context,b->power_lease,true);if(e)goto fail;
    e=settle(b,60);if(e)goto fail;
    b->pins_set=true;e=b->ops.pins_active(b->ops.context,true);if(e)goto fail;
    e=qbt_geni_start(&b->geni);if(e)goto fail;
    b->phase=NEZHA_BT_FIRMWARE;
    b->boot_deadline=now(b)+nezha_bt_board_boot_budget_ns(&b->boot);
    struct qbt_boot_options hci=b->boot;
    if(b->boot.baud_method==QBT_BAUD_PERI) {
        e=ganges_prepare(b);if(e)goto fail;
        hci.baud_method=QBT_BAUD_CURRENT;hci.initial_baud=hci.operating_baud;
    }
    e=qbt_boot_controller(b->core,&hci,b->address);if(e)goto fail;
    b->boot_deadline=0;b->powered=true;b->phase=NEZHA_BT_READY;return 0;
fail:
    failure_trace(b,e);
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
            if(b->peri_rx && !b->peri_hci)qbt_peri_fail(b->peri,-ECANCELED);
            else first=qbt_shutdown(b->core);
            e=quiesce(b);if(!first)first=e;
        }
        /* prepare may have returned EBUSY with state OFF. Even then, retire
         * the inherited engines before removing clocks or any DMA route.
         * A failed quiesce is releasable only with this independent proof. */
        e=b->ops.serial_retire(b->ops.context);if(e)goto quarantine;
        b->geni.state=QBT_GENI_OFF;b->geni.tx_busy=b->geni.rx_busy=false;
    }
    if(b->peri) {
        e=qbt_peri_release(b->peri,now(b)+40000000ULL);
        peri_trace(b,"retired-release",e);if(e)goto quarantine;
        e=qbt_peri_destroy(b->peri);if(e)goto quarantine;
        b->peri=NULL;b->peri_rx=false;b->peri_hci=false;
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
