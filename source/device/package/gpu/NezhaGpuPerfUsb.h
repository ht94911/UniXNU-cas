/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "NativeAdrenoPerf.h"
#include "NativeGpuTrace.h"
extern "C" {
#include <QcomUsbDiagnostic.h>
}
#include <stdarg.h>
extern "C" {
extern volatile uint32_t native_gpu_coherence[8],native_adreno_io_coherent,native_adreno_coherent_skips[4];
extern volatile uint32_t native_adreno840_coherence_attempts,native_adreno840_coherence_retained;
void native_legacy_vsync_snapshot(uint64_t out[8]);
bool nezha_cpu27_snapshot(int32_t out[20]);
}
struct NezhaPerfWriter {
    char *out;size_t cap,used=0;bool failed=false;
    void add(const char *format,...) {
        if(failed)return;va_list args;va_start(args,format);
        int n=vsnprintf(out+used,cap-used,format,args);va_end(args);
        if(n<0 || size_t(n)>=cap-used){failed=true;return;}used+=size_t(n);
    }
    void array(const uint64_t *values,unsigned count) {
        add("[");for(unsigned i=0;i<count;++i)add("%s%llu",i?",":"",values[i]);add("]");
    }
    void hex(const uint8_t *values,size_t count) {
        if(failed)return;
        if(used>=cap || count>(cap-used-1)/2){failed=true;return;}
        static const char digits[]="0123456789abcdef";
        for(size_t i=0;i<count;++i){out[used++]=digits[values[i]>>4];out[used++]=digits[values[i]&15];}
        out[used]=0;
    }
    int finish(size_t *bytes,unsigned *trunc) {*bytes=failed?0:used;*trunc=failed?1:0;return failed?QUD_FAILED:QUD_OK;}
};
static int nezhaPerfSnapshot(void *,char *out,size_t cap,size_t *bytes,unsigned *trunc)
{
    NativeAdrenoPerfSample s{};uint64_t power[16]={};
    if(!native_adreno_perf_sample(&s) || nezha_gpu_platform_perf_copy(power))return QUD_BUSY;
    NezhaPerfWriter w{out,cap};
    w.add("{\"v\":1,\"power\":");w.array(power,16);
    w.add(",\"jobs\":");w.array(s.jobs,14);w.add(",\"cache\":");w.array(s.cache,5);
    uint64_t cache_ns=0,reassert_ns=0;absolutetime_to_nanoseconds(s.cache[0],&cache_ns);absolutetime_to_nanoseconds(s.reassert_ticks,&reassert_ns);
    w.add(",\"cache_ns\":%llu,\"reassert_ns\":%llu,\"cpu_us\":%llu,\"cpu_mask\":%u,\"cpu\":[",cache_ns,reassert_ns,s.cpu_us,s.cpu_valid);
    for(unsigned n=0;n<8;++n)w.add("%s[%u,%u,%u,%u]",n?",":"",s.cpu[n][0],s.cpu[n][1],s.cpu[n][2],s.cpu[n][3]);
    uint64_t coherence[15]={},display[8]={};
    for(unsigned i=0;i<8;++i)coherence[i]=native_gpu_coherence[i];
    coherence[8]=native_adreno_io_coherent;coherence[9]=native_adreno840_coherence_attempts;
    coherence[10]=native_adreno840_coherence_retained;
    for(unsigned i=0;i<4;++i)coherence[11+i]=native_adreno_coherent_skips[i];
    native_legacy_vsync_snapshot(display);
    w.add("],\"coherence\":");w.array(coherence,15);
    w.add(",\"display_pacing\":");w.array(display,8);
    w.add(",\"physical_vblank\":false,\"clock_readback\":false,\"gpu_timestamp_hz\":19200000}");return w.finish(bytes,trunc);
}
static int nezhaPerfRequest(void *,const qud_diag_request *r,char *out,size_t cap,size_t *bytes,unsigned *trunc)
{
    if(r->op==QUD_CONFIG) {
        if(nezha_gpu_platform_perf_control(r->arg0,r->arg1))return QUD_INVALID;
        NezhaPerfWriter w{out,cap};w.add("{\"v\":1,\"queued\":true,\"mode\":%u,\"lease_ms\":%u}",r->arg0,r->arg1);return w.finish(bytes,trunc);
    }
    if(r->op!=QUD_HISTORY)return QUD_UNSUPPORTED;
    NezhaPerfWriter w{out,cap};
    if(r->arg0==1) {
        uint64_t calls[18][4];native_adreno_perf_calls(calls);w.add("{\"v\":1,\"calls\":[");
        for(unsigned i=0;i<18;++i){if(i)w.add(",");w.array(calls[i],4);}w.add("]}");return w.finish(bytes,trunc);
    }
    if(r->arg0==2) {
        int32_t cpu[20];if(!nezha_cpu27_snapshot(cpu))return QUD_BUSY;
        w.add("{\"v\":1,\"cpu_policy\":[");
        for(unsigned i=0;i<20;++i)w.add("%s%d",i?",":"",cpu[i]);
        w.add("],\"physical_clock_readback\":false}");return w.finish(bytes,trunc);
    }
    if(r->arg0==3) {
        if(!native_gpu_cpu_map_available())return QUD_UNSUPPORTED;
        NativeGpuTraceRecord records[6];uint64_t next=0,latest=0,missed=0;
        const unsigned count=native_gpu_cpu_map_page(r->cursor,records,6,&next,&latest,&missed);
        w.add("{\"v\":1,\"stream\":\"cpu-map\",\"record_bytes\":120,\"count\":%u,\"next\":%llu,\"latest\":%llu,\"missed\":%llu,\"hex\":\"",count,next,latest,missed);
        w.hex(reinterpret_cast<const uint8_t *>(records),count*sizeof(records[0]));
        w.add("\"}");return w.finish(bytes,trunc);
    }
    if(r->arg0)return QUD_INVALID;
    NativeGpuTraceRecord records[6];uint64_t next=0,latest=0,missed=0;
    const unsigned count=native_gpu_trace_page(r->cursor,records,6,&next,&latest,&missed);
    w.add("{\"v\":1,\"record_bytes\":120,\"count\":%u,\"next\":%llu,\"latest\":%llu,\"missed\":%llu,\"hex\":\"",count,next,latest,missed);
    const uint8_t *raw=reinterpret_cast<const uint8_t *>(records);
    w.hex(raw,count*sizeof(records[0]));
    w.add("\"}");return w.finish(bytes,trunc);
}
/* CONFIG mode 3 is the bounded bandwidth lease. HISTORY still validates its
 * smaller selector set in nezhaPerfRequest after the shared dispatch bounds. */
static const qud_diag_provider nezhaPerfProvider={7,1,(1u<<QUD_SNAPSHOT)|(1u<<QUD_HISTORY)|(1u<<QUD_CONFIG),5,10000,"nezhagpu",nullptr,nezhaPerfSnapshot,nezhaPerfRequest};
