// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NezhaDpuTiming.h"
#include <assert.h>
#include <stdio.h>

struct Fake {
    unsigned reads,frame_reads,stamp_reads,cpu_reads,counter_reads;
    unsigned fail_at,change_frame_once,unstable_timestamp,clock_backwards;
    uint32_t words[2][NGP_TIMING_WORDS];
};

static int read32(void *cookie,uint32_t offset,uint32_t *value) {
    struct Fake *f=cookie;
    if(++f->reads==f->fail_at)return 1;
    if(!offset){*value=0xd0000000;return 0;}
    for(unsigned i=0;i<2;++i)for(unsigned k=0;k<NGP_TIMING_WORDS;++k) {
        if(offset!=nezha_dpu_intf_offsets[i]+nezha_dpu_timing_offsets[k])continue;
        if(!i && k==11 && f->change_frame_once && ++f->frame_reads==3)
            f->words[0][11]++;
        *value=f->words[i][k];
        if(!i && k==21 && f->unstable_timestamp)*value=++f->stamp_reads;
        return 0;
    }
    assert(!"Reader left the audited register list");return 1;
}
static uint64_t now(void *cookie) {
    struct Fake *f=cookie;
    ++f->cpu_reads;
    return f->clock_backwards?1000-f->cpu_reads:1000+f->cpu_reads;
}
static uint64_t counter(void *cookie) {
    struct Fake *f=cookie;return 19200000+20*(++f->counter_reads);
}
static struct Fake active(void) {
    struct Fake f={0};
    for(unsigned n=0;n<NGP_TIMING_WORDS;++n)f.words[0][n]=100+n;
    f.words[0][5]=1;f.words[0][6]=1;f.words[0][9]=0x80000001u;
    f.words[0][11]=0x12345678;f.words[0][12]=0x76543210;
    f.words[0][13]=0xabcd0123;f.words[0][14]=2;
    f.words[0][20]=19000000;f.words[0][21]=0;
    f.words[0][22]=19000005;f.words[0][23]=0;
    return f;
}
static uint32_t sample(struct Fake *f,struct NGPTimingReply *out) {
    const struct NezhaDpuTimingIo io={f,read32,now,counter};
    return nezha_dpu_sample(&io,out);
}

int main(void) {
    struct NGPTimingReply out={0};struct Fake f=active();
    out.sequence=99;out.source_cookie=1234;out.surface_handle=42;out.mapping=7;
    out.phase=NGP_TIMING_AFTER_COPY;
    assert(sample(&f,&out)==NGP_TIMING_OK);
    assert(out.flags==7 && out.attempts==1 && out.hardware_version==0xd0000000);
    assert(!memcmp(out.interfaces,f.words,sizeof f.words));
    assert(out.sequence==99 && out.source_cookie==1234 && out.surface_handle==42 && out.mapping==7);
    assert(out.phase==NGP_TIMING_AFTER_COPY && out.cpu_after_us>out.cpu_before_us);
    // Automatic refresh and the video counter remain distinct raw observations.
    assert(out.interfaces[0][11]!=out.interfaces[0][12]);

    f=(struct Fake){0};assert(sample(&f,&out)==NGP_TIMING_OK);
    assert(out.interfaces[0][5]==0 && out.interfaces[0][9]==0 && out.interfaces[0][11]==0);
    // An all-zero inactive interface can be read coherently; the record never
    // sets a physical-presented/active flag or manufactures a timestamp.
    assert(out.flags==NGP_TIMING_READ_COMPLETE+NGP_TIMING_INTF0_STABLE+NGP_TIMING_INTF1_STABLE);

    f=active();f.change_frame_once=1;
    assert(sample(&f,&out)==NGP_TIMING_OK && out.attempts==2);
    assert(out.interfaces[0][11]==0x12345679);

    f=active();f.unstable_timestamp=1;
    assert(sample(&f,&out)==NGP_TIMING_UNSTABLE && out.attempts==NEZHA_DPU_ATTEMPTS);
    assert(!(out.flags&NGP_TIMING_READ_COMPLETE) && f.reads<300);

    f=active();f.fail_at=7;
    assert(sample(&f,&out)==NGP_TIMING_IO && !(out.flags&NGP_TIMING_READ_COMPLETE));
    assert(f.reads==7);

    f=active();f.clock_backwards=1;
    assert(sample(&f,&out)==NGP_TIMING_CLOCK && !(out.flags&NGP_TIMING_READ_COMPLETE));
    assert(nezha_dpu_sample(NULL,&out)==NGP_TIMING_ARGUMENT);
    assert(nezha_dpu_sample(NULL,NULL)==NGP_TIMING_ARGUMENT);
    puts("PASS actual DPU timing reader: raw counter separation, inactive data, frame boundary retry, torn timestamp, IO failure, clocks and source identity");
    return 0;
}
