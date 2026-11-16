/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tas25xxTables.h"
#include <string.h>
static uint32_t rd(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void wr(uint8_t *p,uint32_t x) {p[0]=x;p[1]=x>>8;p[2]=x>>16;p[3]=x>>24;}
static int name_ok(const uint8_t *p) {for (unsigned i=0;i<64;++i) if (!p[i]) return i!=0;return 0;}
static int same(const uint8_t *p,const char *s) {
    if (!s) return 0;
    for (unsigned i=0;i<64;++i) {if (p[i]!=(uint8_t)s[i]) return 0;if (!p[i]) return i!=0;}
    return 0;
}
static int sized(NaTasBytes parent,uint32_t *pos,NaTasBytes *child) {
    if (*pos>parent.bytes || parent.bytes-*pos<4) return NA_WIRE;
    uint32_t n=rd(parent.data+*pos);
    if (n>parent.bytes-*pos-4) return NA_WIRE;
    *child=(NaTasBytes){parent.data+*pos,n+4};*pos+=n+4;return NA_OK;
}
int na_tas_control(const NaTasImage *image,const char *name,NaTasControl *result) {
    if (!image || !name || !result) return NA_ARGUMENT;
    NaTasBytes b=image->controls;
    if (!b.data || b.bytes<8 || rd(b.data)!=b.bytes-4) return NA_WIRE;
    uint32_t n=rd(b.data+4),pos=8,found=0;NaTasControl selected={0};
    if (n>128) return NA_WIRE;
    for (uint32_t i=0;i<n;++i) {
        if (b.bytes-pos<67) return NA_WIRE;
        NaTasControl c={0};c.type=b.data[pos++];const uint8_t *label=b.data+pos;
        if (!name_ok(label)) return NA_WIRE;
        pos+=64;c.channel=b.data[pos++];if (c.channel>1) return NA_WIRE;
        if (c.type==0) {
            if (b.bytes-pos<5) return NA_WIRE;
            c.reg=rd(b.data+pos);pos+=4;c.reg_type=b.data[pos++];
            if (c.reg>=0x800000 || c.reg_type>2) return NA_WIRE;
            if (c.reg_type==2) {if (b.bytes-pos<4) return NA_WIRE;c.mask=rd(b.data+pos);pos+=4;if (!c.mask || c.mask>255) return NA_WIRE;}
            if (b.bytes-pos<24) return NA_WIRE;
            /* min/max/step describe the UI; the index selects the encoded
             * table. Never turn the UI index itself into an amplifier gain. */
            c.default_index=rd(b.data+pos+12);c.misc=rd(b.data+pos+16);c.count=rd(b.data+pos+20);pos+=24;
            uint32_t width=c.reg_type==1 ? 4:1;
            if (!c.count || c.count>512 || c.count>(b.bytes-pos)/width || c.default_index>=c.count) return NA_WIRE;
            c.values=(NaTasBytes){b.data+pos,c.count*width};pos+=c.values.bytes;
        } else if (c.type==1) {
            if (b.bytes-pos<9) return NA_WIRE;
            c.default_index=b.data[pos++];c.misc=rd(b.data+pos);c.count=rd(b.data+pos+4);pos+=8;
            if (!c.count || c.count>128 || c.default_index>=c.count) return NA_WIRE;
            uint32_t start=pos;
            for (uint32_t j=0;j<c.count;++j) {
                if (b.bytes-pos<64 || !name_ok(b.data+pos)) return NA_WIRE;pos+=64;
                NaTasBytes program;NaTasProgramInfo info;
                if (sized(b,&pos,&program) || na_tas_validate(program,&info)) return NA_WIRE;
            }
            c.values=(NaTasBytes){b.data+start,pos-start};
        } else return NA_WIRE;
        if ((c.misc&15)>2) return NA_MISSING;
        if (same(label,name)) {if (found++) return NA_WIRE;selected=c;}
    }
    if (pos!=b.bytes) return NA_WIRE;
    if (!found) return NA_MISSING;
    *result=selected;return NA_OK;
}
int na_tas_enum(const NaTasControl *control,const char *name,NaTasBytes *result) {
    if (!control || !name || !result || control->type!=1) return NA_ARGUMENT;
    NaTasBytes b=control->values,selected={0};uint32_t pos=0,found=0;
    for (uint32_t i=0;i<control->count;++i) {
        if (b.bytes-pos<64 || !name_ok(b.data+pos)) return NA_WIRE;
        int match=same(b.data+pos,name);pos+=64;NaTasBytes program;NaTasProgramInfo info;
        if (sized(b,&pos,&program) || na_tas_validate(program,&info)) return NA_WIRE;
        if (match) {if (found++) return NA_WIRE;selected=program;}
    }
    if (pos!=b.bytes) return NA_WIRE;
    if (!found) return NA_MISSING;
    *result=selected;return NA_OK;
}
int na_tas_integer(const NaTasControl *c,uint32_t index,uint8_t out[17],NaTasBytes *result) {
    if (!c || !out || !result || c->type!=0 || c->reg_type>2 || index>=c->count) return NA_ARGUMENT;
    uint32_t width=c->reg_type==1 ? 4:1;
    if (c->values.bytes!=c->count*width || c->reg>=0x800000) return NA_WIRE;
    out[4]=(uint8_t)c->reg_type;wr(out+5,c->reg);uint32_t n;
    if (c->reg_type==1) {
        if ((c->reg&127)>124) return NA_WIRE;
        wr(out+9,4);const uint8_t *p=c->values.data+index*4;
        for (unsigned i=0;i<4;++i) out[13+i]=p[3-i];n=17;
    } else if (c->reg_type==2) {if (!c->mask || c->mask>255) return NA_WIRE;out[9]=(uint8_t)c->mask;out[10]=c->values.data[index];n=11;}
    else {out[9]=c->values.data[index];n=10;}
    wr(out,n-4);*result=(NaTasBytes){out,n};return NA_OK;
}
int na_tas_enum_index(const NaTasControl *c,uint32_t index,NaTasBytes *out) {
    if (!c || !out || c->type!=1 || index>=c->count) return NA_ARGUMENT;
    uint32_t pos=0;NaTasBytes selected={0};
    for (unsigned i=0;i<c->count;++i) {
        if (pos>c->values.bytes || c->values.bytes-pos<64 || !name_ok(c->values.data+pos)) return NA_WIRE;
        pos+=64;NaTasBytes b;NaTasProgramInfo info;
        if (sized(c->values,&pos,&b) || na_tas_validate(b,&info)) return NA_WIRE;
        if (i==index) selected=b;
    }
    if (pos!=c->values.bytes) return NA_WIRE;
    *out=selected;return NA_OK;
}
int na_tas_format(const NaTasImage *image,uint32_t ch,int tx,const char *name,NaTasBytes *out) {
    if (!name || !out || (tx!=0 && tx!=1)) return NA_ARGUMENT;
    uint32_t width=tx ? 14:5;
    /* Both table label widths are fixed by v7, not strlen of arbitrary input. */
    for (unsigned i=0;i<width;++i) if (!name[i]) return NA_ARGUMENT;
    if (name[width]) return NA_ARGUMENT;
    NaTasBytes b,selected={0};int r=na_tas_block(image,ch,tx ? "TXFMT":"RXFMT",&b);
    if (r) return r;
    if (b.bytes<8 || rd(b.data+4)>64) return NA_WIRE;
    uint32_t pos=8,found=0;
    for (unsigned i=0;i<rd(b.data+4);++i) {
        if (pos>b.bytes || b.bytes-pos<width) return NA_WIRE;
        int match=!memcmp(b.data+pos,name,width);pos+=width;NaTasBytes p;NaTasProgramInfo info;
        if (sized(b,&pos,&p) || na_tas_validate(p,&info)) return NA_WIRE;
        if (match) {if (found++) return NA_WIRE;selected=p;}
    }
    if (pos!=b.bytes) return NA_WIRE;
    if (!found) return NA_MISSING;
    *out=selected;return NA_OK;
}
int na_tas_fault_severity(const NaTasFaultTable *t,uint32_t channel,uint32_t flags,uint32_t *fatal) {
    if (!t || !fatal || channel>1 || !t->count || t->count>32 || t->entries.bytes!=t->count*92) return NA_ARGUMENT;
    uint32_t ignored=0;
    for (unsigned i=0;i<t->count;++i) {
        const uint8_t *p=t->entries.data+i*92;
        /* action=0 includes overtemperature/overcurrent; it is not severity.
         * Only this exact normal completion indication is non-fatal. */
        if (rd(p+64)==0x64 && rd(p+68)==8 &&
            same(p,channel ? "Isense-CM-Trim-done_1":"Isense-CM-Trim-done_0")) ignored|=1u<<i;
    }
    *fatal=flags&~ignored;return NA_OK;
}
int na_tas_block(const NaTasImage *image,uint32_t channel,const char tag[5],NaTasBytes *result) {
    if (!image || channel>1 || !tag || !result) return NA_ARGUMENT;
    NaTasBytes b=image->section[channel][NaTasBlockOps],selected={0};uint32_t pos=8,found=0;
    if (!b.data || b.bytes<8 || rd(b.data)!=b.bytes-4 || rd(b.data+4)>32) return NA_WIRE;
    for (uint32_t i=0;i<rd(b.data+4);++i) {
        if (b.bytes-pos<5) return NA_WIRE;
        int match=!memcmp(b.data+pos,tag,5);pos+=5;NaTasBytes program;
        if (sized(b,&pos,&program)) return NA_WIRE;
        if (match) {if (found++) return NA_WIRE;selected=program;}
    }
    if (pos!=b.bytes) return NA_WIRE;
    if (!found) return NA_MISSING;
    *result=selected;return NA_OK;
}
int na_tas_hardware(const NaTasImage *image,uint32_t channel,uint32_t index,NaTasBytes *result) {
    if (!image || channel>1 || !result) return NA_ARGUMENT;
    NaTasBytes b=image->section[channel][NaTasHw],selected={0};uint32_t pos=8;
    if (!b.data || b.bytes<8 || rd(b.data)!=b.bytes-4 || rd(b.data+4)!=26 || index>=26) return NA_WIRE;
    for (unsigned i=0;i<26;++i) {
        NaTasBytes p;NaTasProgramInfo info;
        if (sized(b,&pos,&p) || na_tas_validate(p,&info)) return NA_WIRE;
        if (i==index) selected=p;
    }
    if (pos!=b.bytes) return NA_WIRE;
    *result=selected;return NA_OK;
}
int na_tas_fault_table(const NaTasImage *image,uint32_t channel,NaTasFaultTable *result) {
    if (!image || channel>1 || !result) return NA_ARGUMENT;
    NaTasBytes b=image->section[channel][NaTasInterrupts];NaTasFaultTable table={0};uint32_t pos=5;
    if (!b.data || b.bytes<5 || rd(b.data)!=b.bytes-4) return NA_WIRE;
    table.count=b.data[4];if (!table.count || table.count>32) return NA_WIRE;
    if (sized(b,&pos,&table.enable) || sized(b,&pos,&table.disable) || sized(b,&pos,&table.clear)) return NA_WIRE;
    NaTasProgramInfo info;
    if (na_tas_validate(table.enable,&info) || na_tas_validate(table.disable,&info) || na_tas_validate(table.clear,&info)) return NA_WIRE;
    if (b.bytes-pos!=table.count*92+24) return NA_WIRE;
    table.entries=(NaTasBytes){b.data+pos,table.count*92};pos+=table.entries.bytes;table.settle_ms=rd(b.data+pos);
    if (table.settle_ms>1000) return NA_WIRE;
    for (unsigned i=0;i<table.count;++i) {
        const uint8_t *p=table.entries.data+i*92;
        if (!name_ok(p) || rd(p+64)>=0x800000 || !rd(p+68) || rd(p+68)>255 || rd(p+76)>1) return NA_WIRE;
    }
    *result=table;return NA_OK;
}
int na_tas_power_check(NaTasBytes b,const NaTasIo *io,uint64_t lease,uint32_t channel) {
    if (!io || !io->read || !lease || channel>1) return NA_ARGUMENT;
    NaTasProgramInfo info;if (na_tas_validate(b,&info)) return NA_WIRE;
    /* First ensure there are only comparisons; no partial I/O on malformed input. */
    for (uint32_t p=4;p<b.bytes;) {uint8_t op=b.data[p];if (op!=0 && op!=2) return NA_WIRE;p+=op==0 ? 6:7;}
    for (uint32_t p=4;p<b.bytes;) {
        uint8_t op=b.data[p++],value=0;uint32_t reg=rd(b.data+p);p+=4;
        uint8_t mask=op==0 ? 255:b.data[p++],expected=b.data[p++];
        int r=io->read(io->context,lease,(uint8_t)na_nezha_facts.amp_address[channel],reg,&value);
        if (r) return r;if ((value&mask)!=(expected&mask)) return NA_IO;
    }
    return NA_OK;
}
int na_tas_faults(const NaTasFaultTable *t,const NaTasIo *io,uint64_t lease,uint32_t channel,int clock,uint32_t *faults) {
    if (!t || !io || !io->read || !lease || channel>1 || !faults || t->count>32 || t->entries.bytes!=t->count*92) return NA_ARGUMENT;
    uint32_t flags=0,registers[32],count=0;uint8_t values[32];
    /* Read a status register once: entries sharing clear-on-read status must
     * see the same snapshot, including non-adjacent entries. */
    for (unsigned i=0;i<t->count;++i) {
        const uint8_t *p=t->entries.data+i*92;uint32_t reg=rd(p+64),mask=rd(p+68);
        if (!clock && rd(p+76)) continue;
        unsigned j=0;while (j<count && registers[j]!=reg) ++j;
        if (j==count) {int r=io->read(io->context,lease,(uint8_t)na_nezha_facts.amp_address[channel],reg,&values[j]);if (r) return r;registers[count++]=reg;}
        if (values[j]&mask) flags|=1u<<i;
    }
    *faults=flags;return NA_OK;
}
