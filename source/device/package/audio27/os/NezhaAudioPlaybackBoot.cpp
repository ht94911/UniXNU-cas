// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit service-mode startup using the existing playback/native worker.
#include "NezhaAudioPlayback.h"
#include "NezhaAudioService.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include <QcomUsbDiagnostic.h>
}
extern "C" kern_return_t thread_terminate(thread_t);
namespace {
NaNativePlaybackStart playbackStart;
uint32_t started;
uint32_t startupSequence,startupValues[7];
void publish_start(const NaNativePlaybackStart *s) {
    const uint32_t values[]={s->phase,(uint32_t)s->error,s->identity.phase,
        (uint32_t)s->identity.cleanup_error,s->identity.revision[0],s->identity.revision[1],s->capture_frames};
    __atomic_fetch_add(&startupSequence,1,__ATOMIC_ACQ_REL);
    for (unsigned i=0;i<7;++i) __atomic_store_n(&startupValues[i],values[i],__ATOMIC_RELAXED);
    __atomic_fetch_add(&startupSequence,1,__ATOMIC_RELEASE);
}
int snapshot(void *,char *out,size_t capacity,size_t *bytes,unsigned *truncated) {
    uint32_t s[7],h[16];uint64_t v[64];bool valid=false;
    *bytes=0;*truncated=0;
    for (unsigned attempt=0;attempt<4;++attempt) {
        uint32_t seq=__atomic_load_n(&startupSequence,__ATOMIC_ACQUIRE);if (seq&1) continue;
        for (unsigned i=0;i<7;++i) s[i]=__atomic_load_n(&startupValues[i],__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq==__atomic_load_n(&startupSequence,__ATOMIC_RELAXED)) {valid=true;break;}
    }
    if (!valid || nezha_audio27_copy_stats(v)) return QUD_BUSY;
    nezha_audio_hal_stats(h);
    // Startup and worker are independently sampled; neither reader takes a
    // lock that a GENI/trace callback could be waiting for USB to service.
    // first_read_done: payload_bytes,data_size,graph_stage,running,retiring,quarantine,token.
    int n=snprintf(out,capacity,"{\"v\":7,\"mode\":\"playback\",\"startup_phase\":%u,\"startup_error\":%d,"
        "\"identity_phase\":%u,\"identity_cleanup\":%d,\"revision\":[%u,%u],"
        "\"driver_stage\":%u,\"driver_result\":%d,\"adsp_stage\":%u,\"slave\":%u,\"gpr_online\":%u,"
        "\"prm_held\":%u,\"prm_possible\":%u,\"prm_error\":%d,"
        "\"graph_output\":%u,\"graph_input\":%u,\"graph_vi\":%u,"
        "\"graph_output_error\":%d,\"graph_vi_error\":%d,"
        "\"output_periods\":%llu,\"input_periods\":%llu,\"underruns\":%u,\"overruns\":%u,"
        "\"faults\":[%u,%u],\"generation\":%u,\"runtime_state\":%u,\"runtime_request\":%u,"
        "\"service_published\":%s,\"capture_enabled\":%s,\"HAL_load_verified\":%s,\"automatic_pcm\":false,"
        "\"probe_requested_frames\":%u,\"probe_state\":%u,\"probe_frames\":%u,\"probe_periods\":%u,"
        "\"probe_nonzero\":%u,\"probe_peak\":%u,\"probe_squares\":%llu,\"probe_error\":%d,"
        "\"probe_pcm_frames\":%u,\"HAL_input_published\":false,"
        "\"input_flow\":[%llu,%llu,%u,%u,%u,%u,%u,%d],"
        "\"rejected_done\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],"
        "\"first_read_done\":[%u,%u,%u,%u,%u,%u,%u],"
        "\"HAL\":{\"open\":[%u,%u,%u],\"maps\":[%u,%u,%u],\"clock\":[%u,%d],"
        "\"start_output\":[%u,%d],\"start_input\":[%u,%d],\"connected\":%u,\"device_lists\":%u,\"property_errors\":%u,\"records\":%u}}",
        s[0],(int32_t)s[1],s[2],(int32_t)s[3],s[4],s[5],
        (unsigned)v[0],(int)(int64_t)v[1],(unsigned)v[2],(unsigned)v[3],(unsigned)v[16],
        (unsigned)v[4],(unsigned)v[5],(int)(int64_t)v[20],(unsigned)v[6],(unsigned)v[7],(unsigned)v[17],
        (int)(int64_t)v[18],(int)(int64_t)v[19],(unsigned long long)v[8],(unsigned long long)v[9],
        (unsigned)v[10],(unsigned)v[11],(unsigned)v[12],(unsigned)v[13],(unsigned)v[14],(unsigned)v[15],
        (unsigned)v[22],v[21] ? "true":"false",v[23] ? "true":"false",h[12] ? "true":"false",s[6],
        (unsigned)v[24],(unsigned)v[25],(unsigned)v[26],(unsigned)v[27],(unsigned)v[28],
        (unsigned long long)v[29],(int)(int64_t)v[30],(unsigned)v[31],
        (unsigned long long)v[32],(unsigned long long)v[33],(unsigned)v[34],(unsigned)v[35],
        (unsigned)v[36],(unsigned)v[37],(unsigned)v[38],(int)(int64_t)v[39],
        (unsigned)v[40],(unsigned)v[41],(unsigned)v[42],(unsigned)v[43],(unsigned)v[44],(unsigned)v[45],(unsigned)v[46],(unsigned)v[47],(unsigned)v[48],(unsigned)v[49],(unsigned)v[50],(unsigned)v[51],(unsigned)v[52],(unsigned)v[53],(unsigned)v[54],(unsigned)v[55],(unsigned)v[56],(unsigned)v[57],(unsigned)v[58],(unsigned)v[59],(unsigned)v[60],(unsigned)v[61],(unsigned)v[62],
        (unsigned)(v[63]&255u),(unsigned)((v[63]>>8)&65535u),(unsigned)((v[63]>>27)&31u),
        (unsigned)((v[63]>>24)&1u),(unsigned)((v[63]>>25)&1u),(unsigned)((v[63]>>26)&1u),(unsigned)(v[63]>>32),
        h[0],h[1],h[2],h[3],h[4],h[5],h[6],(int32_t)h[7],h[8],(int32_t)h[9],h[10],(int32_t)h[11],h[12],h[13],h[14],h[15]);
    if (n<0 || (size_t)n>=capacity) {*truncated=1;return QUD_FAILED;}
    *bytes=(size_t)n;return QUD_OK;
}
int submit(void *,const qud_diag_request *request,char *out,size_t capacity,size_t *bytes,unsigned *truncated) {
    *bytes=0;*truncated=0;
    if (!request || request->op!=QUD_HISTORY) return QUD_INVALID;
    if (request->arg1==1) {
        if (request->arg0>8 || request->cursor>=NA_HAL_HISTORY_CAPACITY) return QUD_INVALID;
        NaHalObservation events[8];uint32_t copied=0;
        int r=nezha_audio_hal_history((uint32_t)request->cursor,request->arg0 ? request->arg0:8,events,&copied);
        if (r==NA_BUSY) return QUD_BUSY;
        if (r) return QUD_INVALID;
        int n=snprintf(out,capacity,"{\"kind\":\"hal_events\",\"version\":1,\"offset\":%u,\"count\":%u,\"records\":[",(unsigned)request->cursor,copied);
        if (n<0 || (size_t)n>=capacity) {*truncated=1;return QUD_FAILED;}
        size_t used=(size_t)n;
        for (uint32_t i=0;i<copied;++i) {
            const NaHalObservation *event=&events[i];
            n=snprintf(out+used,capacity-used,"%s[%u,%u,%u,%u,%u,%d,%u]",i ? ",":"",event->kind,event->object,event->selector,event->scope,event->element,event->result,event->value);
            if (n<0 || (size_t)n>=capacity-used) {*truncated=1;return QUD_FAILED;}
            used+=(size_t)n;
        }
        n=snprintf(out+used,capacity-used,"]}");
        if (n<0 || (size_t)n>=capacity-used) {*truncated=1;return QUD_FAILED;}
        *bytes=used+(size_t)n;return QUD_OK;
    }
    const bool encoded=request->arg1==2;
    if (request->arg0>(encoded ? NA_CAPTURE_PROBE_PAGE_FRAMES:64u) ||
        (request->arg1 && !encoded) ||
        request->cursor>=NA_CAPTURE_PROBE_PCM_FRAMES) return QUD_INVALID;
    int16_t pcm[NA_CAPTURE_PROBE_PAGE_FRAMES];uint32_t copied=0;
    int r=nezha_audio27_capture_pcm((uint32_t)request->cursor,request->arg0 ? request->arg0:64,pcm,&copied);
    if (r==NA_BUSY) return QUD_BUSY;
    if (r) return r==NA_ARGUMENT ? QUD_INVALID:QUD_FAILED;
    if (encoded) {
        int n=snprintf(out,capacity,"{\"kind\":\"capture_pcm\",\"rate\":48000,\"channels\":1,\"bits\":16,\"offset\":%u,\"frames\":%u,\"encoding\":\"s16le-base64\",\"data\":\"",
            (unsigned)request->cursor,copied);
        if (n<0 || (size_t)n>=capacity) {*truncated=1;return QUD_FAILED;}
        size_t used=(size_t)n,count=copied*sizeof *pcm;
        const uint8_t *data=reinterpret_cast<const uint8_t *>(pcm);
        static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        if (4*((count+2)/3)+3>=capacity-used) {*truncated=1;return QUD_FAILED;}
        for (size_t i=0;i<count;i+=3) {
            uint32_t word=(uint32_t)data[i]<<16;
            if (i+1<count) word|=(uint32_t)data[i+1]<<8;
            if (i+2<count) word|=data[i+2];
            out[used++]=alphabet[word>>18];out[used++]=alphabet[(word>>12)&63];
            out[used++]=i+1<count ? alphabet[(word>>6)&63]:'=';
            out[used++]=i+2<count ? alphabet[word&63]:'=';
        }
        memcpy(out+used,"\"}",3);*bytes=used+2;return QUD_OK;
    }
    int n=snprintf(out,capacity,"{\"kind\":\"capture_pcm\",\"rate\":48000,\"channels\":1,\"bits\":16,\"offset\":%u,\"frames\":%u,\"samples\":[",
        (unsigned)request->cursor,copied);
    if (n<0 || (size_t)n>=capacity) {*truncated=1;return QUD_FAILED;}
    size_t used=(size_t)n;
    for (uint32_t i=0;i<copied;++i) {
        n=snprintf(out+used,capacity-used,"%s%d",i ? ",":"",(int)pcm[i]);
        if (n<0 || (size_t)n>=capacity-used) {*truncated=1;return QUD_FAILED;}
        used+=(size_t)n;
    }
    n=snprintf(out+used,capacity-used,"]}");
    if (n<0 || (size_t)n>=capacity-used) {*truncated=1;return QUD_FAILED;}
    *bytes=used+(size_t)n;return QUD_OK;
}
const qud_diag_provider provider={8,7,(1u<<QUD_SNAPSHOT)|(1u<<QUD_HISTORY),NA_CAPTURE_PROBE_PAGE_FRAMES,2,"nezhaaudio",nullptr,snapshot,submit};
void run_start(void *v,wait_result_t) {
    auto *s=static_cast<NaNativePlaybackStart *>(v);
    thread_set_thread_name(current_thread(),"nezha-audio27-start");
    mach_timebase_info_data_t timebase={};clock_timebase_info(&timebase);
    int requested=0;PE_parse_boot_argn("na-capture-probe",&requested,sizeof requested);
    if (requested<0 || requested>(int)(2*NA_AUDIO_RATE)) {s->error=NA_ARGUMENT;s->phase=5;}
    else s->capture_frames=(uint32_t)requested;
    unsigned last=UINT32_MAX;int r=NA_PENDING;
    for (;;) {
        r=na_native_playback_start_step(s,na_native_now_us(nullptr)/1000,timebase.numer,timebase.denom);
        if (r<0 && !s->error) {s->error=r;s->phase=5;}
        publish_start(s);
        if (last!=s->phase || r<0) {
            printf("NEZHA AUDIO27 START: phase=%u error=%d identity=%u cleanup=%d revisions=%u/%u\n",
                s->phase,r,s->identity.phase,s->identity.cleanup_error,s->identity.revision[0],s->identity.revision[1]);
            last=s->phase;
        }
        if (r!=NA_PENDING) break;
        assert_wait_timeout((event_t)s,THREAD_UNINT,1,kMillisecondScale);
        thread_block(THREAD_CONTINUE_NULL);
    }
    // The initialized driver now owns its own existing audio worker. Failed
    // startup stays retained; no timed reset or initialization replay follows.
    thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);
}
}
extern "C" void nezha_audio27_playback_start(void) {
    if (__atomic_exchange_n(&started,1,__ATOMIC_ACQ_REL)) return;
    if (!qcom_usb_diag_register(&provider)) {
        playbackStart.error=NA_BUSY;playbackStart.phase=5;publish_start(&playbackStart);
        printf("NEZHA AUDIO27 START: audio provider registration failed\n");return;
    }
    thread_t thread=THREAD_NULL;
    if (kernel_thread_start(run_start,&playbackStart,&thread)!=KERN_SUCCESS) {
        playbackStart.error=NA_IO;playbackStart.phase=5;
        publish_start(&playbackStart);
        printf("NEZHA AUDIO27 START: kernel thread creation failed\n");return;
    }
    thread_deallocate(thread);
}
