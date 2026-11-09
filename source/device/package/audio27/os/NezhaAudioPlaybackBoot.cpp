// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit service-mode startup using the existing playback/native worker.
#include "NezhaAudioPlayback.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
extern "C" {
#include <QcomUsbDiagnostic.h>
}
extern "C" kern_return_t thread_terminate(thread_t);
namespace {
NaNativePlaybackStart playbackStart;
uint32_t started;
uint32_t startupSequence,startupValues[6];
void publish_start(const NaNativePlaybackStart *s) {
    const uint32_t values[]={s->phase,(uint32_t)s->error,s->identity.phase,
        (uint32_t)s->identity.cleanup_error,s->identity.revision[0],s->identity.revision[1]};
    __atomic_fetch_add(&startupSequence,1,__ATOMIC_ACQ_REL);
    for (unsigned i=0;i<6;++i) __atomic_store_n(&startupValues[i],values[i],__ATOMIC_RELAXED);
    __atomic_fetch_add(&startupSequence,1,__ATOMIC_RELEASE);
}
int snapshot(void *,char *out,size_t capacity,size_t *bytes,unsigned *truncated) {
    uint32_t s[6];uint64_t v[24];bool valid=false;
    *bytes=0;*truncated=0;
    for (unsigned attempt=0;attempt<4;++attempt) {
        uint32_t seq=__atomic_load_n(&startupSequence,__ATOMIC_ACQUIRE);if (seq&1) continue;
        for (unsigned i=0;i<6;++i) s[i]=__atomic_load_n(&startupValues[i],__ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq==__atomic_load_n(&startupSequence,__ATOMIC_RELAXED)) {valid=true;break;}
    }
    if (!valid || nezha_audio27_copy_stats(v)) return QUD_BUSY;
    // Startup and worker are independently sampled; neither reader takes a
    // lock that a GENI/trace callback could be waiting for USB to service.
    int n=snprintf(out,capacity,"{\"v\":3,\"mode\":\"playback\",\"startup_phase\":%u,\"startup_error\":%d,"
        "\"identity_phase\":%u,\"identity_cleanup\":%d,\"revision\":[%u,%u],"
        "\"driver_stage\":%u,\"driver_result\":%d,\"adsp_stage\":%u,\"slave\":%u,\"gpr_online\":%u,"
        "\"prm_held\":%u,\"prm_possible\":%u,\"prm_error\":%d,"
        "\"graph_output\":%u,\"graph_input\":%u,\"graph_vi\":%u,"
        "\"graph_output_error\":%d,\"graph_vi_error\":%d,"
        "\"output_periods\":%llu,\"input_periods\":%llu,\"underruns\":%u,\"overruns\":%u,"
        "\"faults\":[%u,%u],\"generation\":%u,\"runtime_state\":%u,\"runtime_request\":%u,"
        "\"service_published\":%s,\"capture_enabled\":%s,\"HAL_load_verified\":false,\"automatic_pcm\":false}",
        s[0],(int32_t)s[1],s[2],(int32_t)s[3],s[4],s[5],
        (unsigned)v[0],(int)(int64_t)v[1],(unsigned)v[2],(unsigned)v[3],(unsigned)v[16],
        (unsigned)v[4],(unsigned)v[5],(int)(int64_t)v[20],(unsigned)v[6],(unsigned)v[7],(unsigned)v[17],
        (int)(int64_t)v[18],(int)(int64_t)v[19],(unsigned long long)v[8],(unsigned long long)v[9],
        (unsigned)v[10],(unsigned)v[11],(unsigned)v[12],(unsigned)v[13],(unsigned)v[14],(unsigned)v[15],
        (unsigned)v[22],v[21] ? "true":"false",v[23] ? "true":"false");
    if (n<0 || (size_t)n>=capacity) {*truncated=1;return QUD_FAILED;}
    *bytes=(size_t)n;return QUD_OK;
}
const qud_diag_provider provider={8,3,1u<<QUD_SNAPSHOT,0,0,"nezhaaudio",nullptr,snapshot,nullptr};
void run_start(void *v,wait_result_t) {
    auto *s=static_cast<NaNativePlaybackStart *>(v);
    thread_set_thread_name(current_thread(),"nezha-audio27-start");
    mach_timebase_info_data_t timebase={};clock_timebase_info(&timebase);
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
