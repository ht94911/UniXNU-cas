/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SM8850 GENI13 clock/pin adapter. Register encodings verified against pinned
 * Linux gcc/pinctrl-kaanapali and i2c-qcom-geni; tuning stays private. */
#include "NezhaAudioBoard.h"
#include <string.h>
#define GCC 0x00100000ULL
#define TLMM 0x0f000000ULL
#define SE 0x01980000ULL
static const uint32_t pins[9]={54,56,64,65,174,122,123,124,125};
static int read_reg(NaGeniBoard *b,uint64_t address,uint32_t *v) {
    int r=b->io.read(b->io.context,b->lease,address,v);if (r) b->error=1;return r;
}
static int update(NaGeniBoard *b,uint64_t address,uint32_t mask,uint32_t v) {
    int r=b->io.update(b->io.context,b->lease,address,mask,v);if (r) b->error=1;return r;
}
static uint32_t read_se(void *v,uint32_t reg) {NaGeniBoard *b=v;uint32_t data=0;read_reg(b,SE+reg,&data);return data;}
static void write_se(void *v,uint32_t reg,uint32_t data) {NaGeniBoard *b=v;update(b,SE+reg,~0u,data);}
static uint64_t now_us(void *v) {NaGeniBoard *b=v;return b->io.now_us(b->io.context);}
static void pause_us(void *v,uint32_t n) {NaGeniBoard *b=v;b->io.pause_us(b->io.context,n);}
static int claim(void *v,uint64_t *lease) {
    NaGeniBoard *b=v;if (b->lease) return NA_OWNERSHIP;
    int r=b->io.claim(b->io.context,NaBoardTas,&b->lease);
    *lease=b->lease;if (r || !b->lease) return r ? r:NA_OWNERSHIP;
    for (unsigned i=0;i<9;++i) if ((r=read_reg(b,TLMM+4096*pins[i],&b->saved_pins[i]))) return r;
    if ((r=read_reg(b,TLMM+4096*174+4,&b->saved_receiver_output))) return r;
    return read_reg(b,GCC+0x52018,&b->saved_vote);
}
static int reset(void *v,uint64_t lease,int asserted) {
    NaGeniBoard *b=v;if (!lease || lease!=b->lease) return NA_OWNERSHIP;
    for (unsigned i=0;i<2;++i) {
        uint64_t p=TLMM+4096*pins[i];
        int r=update(b,p+4,2,asserted ? 0:2);if (r) return r;
        r=update(b,p,0x3ff,1u<<9);if (r) return r; /* GPIO out,2mA */
    }
    return NA_OK;
}
static int power(void *v,uint64_t lease,int on) {
    NaGeniBoard *b=v;if (!lease || lease!=b->lease) return NA_OWNERSHIP;
    const uint32_t votes=(1u<<12)|(1u<<8)|(1u<<9);int r;
    if (!on) {
        b->bus.ready=0;
        /* This source uses polling; disable its local SE IRQ emission before
         * dropping the engine vote, without touching GIC/global IRQ state. */
        if (b->clocks_on && b->configured) {
            if ((r=update(b,SE+GENI_SE_IRQ_EN,GENI_SE_M_IRQ_EN,0)) ||
                (r=update(b,SE+0x7c,~0u,b->saved_clock_index)) ||
                (r=update(b,SE+GENI_CLK_CFG,~0u,b->saved_config)) ||
                (r=update(b,SE+GENI_SCL_COUNTERS,~0u,b->saved_scl))) return r;
        }
        b->clocks_on=b->configured=0;
        if ((r=update(b,GCC+0x52018,votes,b->saved_vote&votes))) return r;
        b->powered=0;return NA_OK;
    }
    if (b->powered) return b->error ? NA_IO:NA_OK;
    b->error=0;
    for (unsigned i=2;i<4;++i) if ((r=update(b,TLMM+4096*pins[i],0x3ff,7))) return r; /*mux1,2mA,pullup*/
    for (unsigned i=5;i<9;++i) if ((r=update(b,TLMM+4096*pins[i],0x3ff,0xc4))) return r; /*TDM0,8mA,mux1*/
    if ((r=update(b,TLMM+4096*174+4,2,2)) || (r=update(b,TLMM+4096*174,0x3ff,0x203))) return r;
    if ((r=update(b,GCC+0x52018,votes,votes))) return r;
    uint64_t start=now_us(b);uint32_t a=0,c=0,d=0;
    for (unsigned i=0;i<200;++i) {
        if ((r=read_reg(b,GCC+0xa8004,&a)) || (r=read_reg(b,GCC+0x233f0,&c)) || (r=read_reg(b,GCC+0x233f4,&d))) return r;
        if (!((a|c|d)&(1u<<31))) break;
        if (i==199 || now_us(b)-start>=10000) return NA_TIMEOUT;pause_us(b,50);
    }
    b->clocks_on=1;
    uint32_t dfs=0;int found=0;
    if ((r=read_reg(b,GCC+0xa8028,&dfs))) return r;
    if (dfs&1) {
        /* Select an existing DFS entry that is exactly XO19.2MHz /1. Never
         * rewrite shared GPLL tables or assume DFS index0 is this frequency. */
        for (unsigned i=0;i<16;++i) {
            uint32_t cfg;if ((r=read_reg(b,GCC+0xa8030+4*i,&cfg))) return r;
            if ((cfg&0x371f)==1) {b->clock_index=i;found=1;break;}
        }
    } else {
        uint32_t cfg;if ((r=read_reg(b,GCC+0xa8018,&cfg))) return r;
        found=(cfg&0x371f)==1;b->clock_index=0;
    }
    if (!found) return NA_MISSING; /* no unverified root-clock retuning */
    uint32_t protocol;if ((r=read_reg(b,SE+GENI_FW_REVISION,&protocol))) return r;
    if (((protocol>>8)&255)!=GENI_PROTO_I2C) return NA_MISSING;
    if ((r=read_reg(b,SE+0x7c,&b->saved_clock_index)) || (r=read_reg(b,SE+GENI_CLK_CFG,&b->saved_config)) ||
        (r=read_reg(b,SE+GENI_SCL_COUNTERS,&b->saved_scl))) return r;
    b->configured=1;
    /* Linux19.2MHz standard-mode timing:divider7,high10,low12,cycle26. */
    if ((r=update(b,SE+0x7c,~0u,b->clock_index)) || (r=update(b,SE+GENI_CLK_CFG,~0u,(7u<<4)|1)) ||
        (r=update(b,SE+GENI_SCL_COUNTERS,~0u,(10u<<20)|(12u<<10)|26))) return r;
    GeniIo io={read_se,write_se,b};
    r=na_tas_bus_prepare(&b->bus,&io,b,now_us,pause_us,b->lease);
    if (r || b->error) return r ? r:NA_IO;
    /* The core polls status. Avoid creating an unowned IRQ373 handler. */
    if ((r=update(b,SE+GENI_SE_IRQ_EN,GENI_SE_M_IRQ_EN,0))) return r;
    b->powered=1;return NA_OK;
}
static int release(void *v,uint64_t lease) {
    NaGeniBoard *b=v;if (!lease || lease!=b->lease || b->powered) return NA_OWNERSHIP;
    for (unsigned i=2;i<9;++i) {
        int r=update(b,TLMM+4096*pins[i],0x3ff,b->saved_pins[i]&0x3ff);if (r) return r;
    }
    int restored=update(b,TLMM+4096*174+4,2,b->saved_receiver_output&2);if (restored) return restored;
    /* Keep amplifier reset asserted on release. Restoring an old high output
     * would power the speakers with no stream/clock owner. */
    int r=b->io.release(b->io.context,NaBoardTas,lease);if (!r) b->lease=0;return r;
}
static int read_amp(void *v,uint64_t lease,uint8_t address,uint32_t reg,uint8_t *value) {
    NaGeniBoard *b=v;int r=na_tas_bus_read(&b->bus,lease,address,reg,value);return r ? r:b->error ? NA_IO:NA_OK;
}
static int write_amp(void *v,uint64_t lease,uint8_t address,uint32_t reg,const uint8_t *data,uint32_t n) {
    NaGeniBoard *b=v;int r=na_tas_bus_write(&b->bus,lease,address,reg,data,n);return r ? r:b->error ? NA_IO:NA_OK;
}
int na_geni_board_init(NaGeniBoard *b,const NaBoardIo *io,NaTasPowerOps *ops) {
    if (!b || !io || !ops || !io->claim || !io->release || !io->read || !io->update || !io->now_us || !io->pause_us) return NA_ARGUMENT;
    memset(b,0,sizeof *b);b->io=*io;
    *ops=(NaTasPowerOps){b,claim,power,reset,release,{b,read_amp,write_amp}};return NA_OK;
}
