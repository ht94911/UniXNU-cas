/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tas25xxProgram.h"
#include <string.h>
#include <limits.h>
static uint32_t rd(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static int string64(const uint8_t *p) {
    for (unsigned i=0;i<64;++i) if (!p[i]) return i!=0;
    return 0;
}
int na_tas_image(NaTasImage *image,const void *data,uint32_t size) {
    if (!image || !data || size<127 || size>262144) return NA_ARGUMENT;
    const uint8_t *b=data;
    if (memcmp(b,"HEADR",5) || rd(b+5)!=118 || rd(b+9)!=7 || rd(b+85)!=2 || !string64(b+13)) return NA_WIRE;
    NaTasImage parsed={0};
    for (unsigned i=0;i<2;++i) {
        parsed.device_type[i]=b[89+2*i];parsed.declared_address[i]=b[90+2*i];
        if (!parsed.device_type[i] || parsed.declared_address[i]<8 || parsed.declared_address[i]>0x77) return NA_WIRE;
    }
    parsed.iv_width=b[93];parsed.vbat_monitor=b[94];memcpy(parsed.default_hw,b+99,8);
    if (rd(b+95) || parsed.vbat_monitor>1) return NA_MISSING; /* unsupported features */
    for (unsigned i=0;i<8;++i) if (parsed.default_hw[i]>=(i==1 || i==2 ? 4:3)) return NA_WIRE;
    uint32_t at=127,channels=0;
    static const char tag[NaTasSections][6]={"INITP","HWPRM","PROFP","INTRP","BLKOP"};
    while (at<size) {
        if (size-at<9) return NA_WIRE;
        uint32_t bytes=rd(b+at+5);
        if (bytes>size-at-9) return NA_WIRE;
        NaTasBytes view={b+at+5,bytes+4};
        if (!memcmp(b+at,"KCNTR",5)) {
            if (parsed.controls.data) return NA_WIRE;
            parsed.controls=view;
        } else {
            unsigned kind=0;
            while (kind<NaTasSections && memcmp(b+at,tag[kind],5)) ++kind;
            if (kind==NaTasSections) return NA_MISSING;
            if (kind==NaTasInit && ++channels>2) return NA_WIRE;
            if (!channels || parsed.section[channels-1][kind].data) return NA_WIRE;
            parsed.section[channels-1][kind]=view;
        }
        at+=9+bytes;
    }
    if (channels!=2 || !parsed.controls.data) return NA_MISSING;
    for (unsigned c=0;c<2;++c) for (unsigned k=0;k<NaTasSections;++k)
        if (!parsed.section[c][k].data) return NA_MISSING;
    *image=parsed;return NA_OK;
}
int na_tas_validate(NaTasBytes b,NaTasProgramInfo *info) {
    if (!b.data || !info || b.bytes<4 || b.bytes>65540 || rd(b.data)!=b.bytes-4) return NA_ARGUMENT;
    NaTasProgramInfo sum={0};uint32_t pos=4;
    while (pos<b.bytes) {
        uint8_t op=b.data[pos++];uint32_t reg=0,n=0;
        if (++sum.commands>4096 || b.bytes-pos<4) return NA_WIRE;
        if (op==3) {
            uint32_t delay=rd(b.data+pos);pos+=4;
            if (delay>1000 || sum.delay_ms>2000-delay) return NA_WIRE;
            sum.delay_ms+=delay;continue;
        }
        if (op>2) return NA_WIRE;
        reg=rd(b.data+pos);pos+=4;
        if (reg>=0x800000) return NA_WIRE;
        if (op==0) n=1;
        else if (op==2) n=2;
        else {
            if (b.bytes-pos<4) return NA_WIRE;
            n=rd(b.data+pos);pos+=4;
            if (!n || n>128-(reg&127)) return NA_WIRE;
        }
        if (n>b.bytes-pos) return NA_WIRE;
        if (op==2 && !b.data[pos]) return NA_WIRE;
        pos+=n;sum.written_bytes+=op==2 ? 1:n;
    }
    *info=sum;return NA_OK;
}
int na_tas_profile(const NaTasImage *image,uint32_t channel,const char *name,NaTasProfile *out) {
    if (!image || channel>1 || !name || !name[0] || !out) return NA_ARGUMENT;
    unsigned name_length=0;while (name_length<64 && name[name_length]) ++name_length;
    if (name_length==64) return NA_ARGUMENT;
    NaTasBytes b=image->section[channel][NaTasProfiles];
    if (!b.data || b.bytes<5 || rd(b.data)!=b.bytes-4) return NA_WIRE;
    uint32_t count=b.data[4],pos=5,found=0;NaTasProfile selected={0};
    if (!count || count>32) return NA_WIRE;
    for (uint32_t i=0;i<count;++i) {
        if (b.bytes-pos<68 || !string64(b.data+pos)) return NA_WIRE;
        int match=!memcmp(b.data+pos,name,name_length+1);
        NaTasProfile p={0};p.misc=rd(b.data+pos+64);pos+=68;
        for (unsigned k=0;k<4;++k) {
            if (b.bytes-pos<4) return NA_WIRE;
            uint32_t bytes=rd(b.data+pos);
            if (bytes>b.bytes-pos-4) return NA_WIRE;
            p.phase[k]=(NaTasBytes){b.data+pos,bytes+4};
            NaTasProgramInfo info;
            if (na_tas_validate(p.phase[k],&info)) return NA_WIRE;
            pos+=4+bytes;
        }
        if (match) {if (found++) return NA_WIRE;selected=p;}
    }
    if (pos!=b.bytes) return NA_WIRE;
    if (!found) return NA_MISSING;
    *out=selected;return NA_OK;
}
int na_tas_begin(NaTasRun *run,NaTasBytes program,const NaTasIo *io,uint64_t lease,uint32_t channel) {
    if (!run || !io || !io->read || !io->write || !lease || channel>1) return NA_ARGUMENT;
    if (run->error) return NA_OWNERSHIP;
    if (run->lease && !run->finished) return NA_BUSY;
    NaTasProgramInfo info;int r=na_tas_validate(program,&info);if (r) return r;
    memset(run,0,sizeof *run);run->program=program;run->io=*io;run->lease=lease;
    run->address=(uint8_t)na_nezha_facts.amp_address[channel];run->offset=4;return NA_OK;
}
void na_tas_abort(NaTasRun *run) {if (run) run->error=NA_IO;}
int na_tas_poll(NaTasRun *run,uint64_t now) {
    if (!run || !run->lease || now>UINT64_MAX-1000) return NA_ARGUMENT;
    if (run->error) return run->error;
    if (now<run->wait_until) return NA_PENDING;
    if (run->offset==run->program.bytes) {run->finished=1;return NA_OK;}
    /* begin has validated this immutable, retained byte range. Advance once;
     * a failed transfer is terminal and is never automatically replayed. */
    const uint8_t *b=run->program.data+run->offset;
    uint8_t op=*b++;uint32_t value=rd(b);b+=4;int result=0;
    if (op==3) run->wait_until=now+value;
    else if (op==0) {result=run->io.write(run->io.context,run->lease,run->address,value,b,1);++b;}
    else if (op==1) {
        uint32_t n=rd(b);b+=4;result=run->io.write(run->io.context,run->lease,run->address,value,b,n);b+=n;
    } else {
        uint8_t v=0;
        result=run->io.read(run->io.context,run->lease,run->address,value,&v);
        if (!result) {v=(uint8_t)((v&~b[0])|(b[1]&b[0]));result=run->io.write(run->io.context,run->lease,run->address,value,&v,1);}
        b+=2;
    }
    run->offset=(uint32_t)(b-run->program.data);++run->commands;
    if (result) {run->error=result<0 ? result : NA_IO;return run->error;}
    if (run->offset==run->program.bytes && now>=run->wait_until) {run->finished=1;return NA_OK;}
    return NA_PENDING;
}
