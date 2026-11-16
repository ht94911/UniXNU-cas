/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Original bounded FIFO implementation of sheng/downstream v2 register
 * protocol; no Linux retries returning success after a mismatched response. */
#include "SoundWire.h"
#include <string.h>
#include <limits.h>
static int fail(NaSwr *s,int error) {s->failed=1;return error;}
static int wait_fifo(NaSwr *s,int reading) {
    uint64_t now=s->io.now_us(s->io.context);if (now>UINT64_MAX-10000) return fail(s,NA_OVERFLOW);
    for (unsigned i=0;i<200;++i) {
        uint32_t status=s->io.read(s->io.context,0x5000);s->last_status=status;
        if (status&0xbf8u) return fail(s,NA_IO); /* clash, FIFO, command/enum errors */
        uint32_t fifo=s->io.read(s->io.context,0x5050);
        if (s->io.error(s->io.context)) return fail(s,NA_IO);
        if (reading ? ((fifo>>16)&31)!=0:((fifo>>8)&31)==0) return NA_OK;
        if (s->io.now_us(s->io.context)-now>=10000) break;
        s->io.pause_us(s->io.context,50);
    }
    return fail(s,NA_TIMEOUT);
}
int na_swr_version_supported(uint32_t version) {
    return version==0x02000000u || version==0x02020000u;
}
int na_swr_bind(NaSwr *s,const NaSwrIo *io,uint64_t lease) {
    if (!s || !io || !lease || !io->read || !io->write || !io->now_us || !io->pause_us || !io->error) return NA_ARGUMENT;
    memset(s,0,sizeof *s);s->io=*io;s->lease=lease;
    if (!na_swr_version_supported(io->read(io->context,0)) || !(io->read(io->context,0x5064)&1)) return NA_MISSING;
    uint32_t status=io->read(io->context,0x1090);unsigned found=0;
    for (unsigned dev=1;dev<=11;++dev) if (((status>>(2*dev))&3)==1) {
        uint32_t lo=io->read(io->context,0x530+8*dev),hi=io->read(io->context,0x534+8*dev);
        if (lo==na_nezha_facts.codec_tx_address[1] && hi==na_nezha_facts.codec_tx_address[0]) {s->device=(uint8_t)dev;++found;}
    }
    if (found!=1 || ((io->read(io->context,0x5050)>>16)&31)) return fail(s,NA_OWNERSHIP);
    if (io->error(io->context)) return fail(s,NA_IO);
    return NA_OK;
}
static uint32_t command(NaSwr *s,uint16_t reg,uint8_t data) {
    s->next_id=s->next_id<14 ? s->next_id+1:0;
    return reg|(uint32_t)s->next_id<<16|(uint32_t)s->device<<20|(uint32_t)data<<24;
}
int na_swr_read(NaSwr *s,uint16_t reg,uint8_t *out) {
    if (!s || !out || !s->lease || !s->device || s->failed) return NA_OWNERSHIP;
    int r=wait_fifo(s,0);if (r) return r;
    if ((s->io.read(s->io.context,0x5050)>>16)&31) return fail(s,NA_WIRE);
    if (s->io.error(s->io.context)) return fail(s,NA_IO);
    s->io.write(s->io.context,0x5024,command(s,reg,1));
    if (s->io.error(s->io.context)) return fail(s,NA_IO);
    s->io.pause_us(s->io.context,250);
    r=wait_fifo(s,1);if (r) return r;
    uint32_t word=s->io.read(s->io.context,0x5040);
    if (s->io.error(s->io.context)) return fail(s,NA_IO);
    if (((word>>8)&15)!=s->next_id) return fail(s,NA_WIRE);
    *out=(uint8_t)word;++s->reads;return NA_OK;
}
int na_swr_write(NaSwr *s,uint16_t reg,uint8_t value) {
    if (!s || !s->lease || !s->device || s->failed) return NA_OWNERSHIP;
    int r=wait_fifo(s,0);if (r) return r;
    s->io.write(s->io.context,0x5020,command(s,reg,value));
    if (s->io.error(s->io.context)) return fail(s,NA_IO);
    s->io.pause_us(s->io.context,150);
    r=wait_fifo(s,0);if (!r) ++s->writes;return r;
}
int na_swr_update(NaSwr *s,uint16_t reg,uint8_t mask,uint8_t value) {
    if (!mask || (value&~mask)) return NA_ARGUMENT;
    uint8_t old;int r=na_swr_read(s,reg,&old);
    return r ? r:na_swr_write(s,reg,(uint8_t)((old&~mask)|value));
}
int na_swr_select_tx_bank(NaSwr *s,unsigned bank) {
    if (!s || !s->lease || !s->device || s->failed || bank>1) return NA_ARGUMENT;
    /* swrm_apply_port_config sets the legacy host DIV2 before switching.
     * Command id15 is reserved by the controller for FRAME_CTRL broadcast. */
    int r=wait_fifo(s,0);if (r) return r;
    uint32_t word=command(s,(uint16_t)(0xe0+bank*0x10),1);
    s->io.write(s->io.context,0x5020,(word&~0xf00000u)|0xf00000u);
    if (s->io.error(s->io.context)) return fail(s,NA_IO);
    s->io.pause_us(s->io.context,150);
    r=wait_fifo(s,0);if (r) return r;++s->writes;
    s->io.write(s->io.context,0x5020,(NA_SWR_TX_FRAME_SHAPE<<24)|0xff0000u|0x60u|bank*0x10);
    if (s->io.error(s->io.context)) return fail(s,NA_IO);
    s->io.pause_us(s->io.context,150);
    r=wait_fifo(s,0);if (r) return r;++s->writes;
    uint64_t now=s->io.now_us(s->io.context);
    for (unsigned i=0;i<200;++i) {
        uint32_t active=s->io.read(s->io.context,0x104c);
        if (s->io.error(s->io.context)) return fail(s,NA_IO);
        if ((active&1)==bank) return NA_OK;
        if (s->io.now_us(s->io.context)-now>=10000) break;
        s->io.pause_us(s->io.context,50);
    }
    return fail(s,NA_TIMEOUT);
}
