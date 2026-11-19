/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioRing.h"
#include <limits.h>
#include <string.h>
static int valid(const void *ring,uint64_t frame) {return ring && !((uintptr_t)ring&7) && frame<=NA_AUDIO_MAX_FRAME;}
static int16_t clip(int32_t v) {return (int16_t)(v>32767 ? 32767 : v< -32768 ? -32768:v);}
int na_ring_mix(uint64_t *ring,uint64_t frame,int16_t l,int16_t r) {
    if (!valid(ring,frame)) return NA_ARGUMENT;
    uint64_t *slot=ring+frame%NA_AUDIO_RING_FRAMES,old=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    for (unsigned attempt=0;attempt<16;++attempt) {
        int32_t left=l,right=r;uint32_t raw_tag=(uint32_t)(old>>32),tag=raw_tag&0x7fffffff;
        if (tag==(uint32_t)frame+1) {
            if (raw_tag&0x80000000u) return NA_OWNERSHIP;
            left+=(int16_t)old;right+=(int16_t)(old>>16);
        }
        else if (tag>(uint32_t)frame+1) return NA_OWNERSHIP; /* late writer cannot overwrite the future */
        uint64_t next=((frame+1)<<32)|(uint32_t)(uint16_t)clip(left)|((uint32_t)(uint16_t)clip(right)<<16);
        if (__atomic_compare_exchange_n(slot,&old,next,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return NA_OK;
    }
    return NA_BUSY;
}
int na_ring_take(uint64_t *ring,uint64_t frame,int16_t *l,int16_t *r) {
    if (!valid(ring,frame) || !l || !r) return NA_ARGUMENT;
    uint64_t *slot=ring+frame%NA_AUDIO_RING_FRAMES,old=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    *l=*r=0;
    for (unsigned attempt=0;attempt<16;++attempt) {
        uint32_t tag=(uint32_t)(old>>32),wanted=(uint32_t)frame+1;
        if ((tag&0x7fffffffu)>wanted || tag==(wanted|0x80000000u)) return NA_MISSING;
        /* Tombstone this exact frame: late writers cannot recreate it. A
         * later wrap has a strictly greater tag and may replace this slot. */
        uint64_t cleared=(0x80000000ULL|((uint32_t)frame+1))<<32;
        if (__atomic_compare_exchange_n(slot,&old,cleared,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            if (tag==wanted) {*l=(int16_t)old;*r=(int16_t)(old>>16);return NA_OK;}
            /* Even a missing frame is consumed. A late writer must not make
             * it appear valid after the device already played silence. */
            return NA_MISSING;
        }
    }
    return NA_BUSY;
}
void na_ring_silence(uint64_t *ring) {
    if (!ring || ((uintptr_t)ring&7)) return;
    for (unsigned i=0;i<NA_AUDIO_RING_FRAMES;++i)
        __atomic_fetch_and(ring+i,0xffffffff00000000ULL,__ATOMIC_ACQ_REL);
}
int na_ring_capture(uint64_t *ring,uint64_t frame,int16_t value) {
    if (!valid(ring,frame)) return NA_ARGUMENT;
    __atomic_store_n(ring+frame%NA_AUDIO_RING_FRAMES,((frame+1)<<32)|(uint16_t)value,__ATOMIC_RELEASE);return NA_OK;
}
int na_ring_read(const uint64_t *ring,uint64_t frame,int16_t *value) {
    if (!valid(ring,frame) || !value) return NA_ARGUMENT;
    uint64_t word=__atomic_load_n(ring+frame%NA_AUDIO_RING_FRAMES,__ATOMIC_ACQUIRE);*value=0;
    if ((uint32_t)(word>>32)!=(uint32_t)frame+1) return NA_MISSING;
    *value=(int16_t)word;return NA_OK;
}
int na_audio_status_snapshot(const NaAudioStatus *shared,NaAudioStatus *out) {
    if (!shared || !out || shared==out) return NA_ARGUMENT;
    for (unsigned i=0;i<8;++i) {
        uint32_t sequence=__atomic_load_n(&shared->sequence,__ATOMIC_ACQUIRE);
        if (sequence&1) continue;
        /* Naturally aligned, correctly typed atomics avoid both races and
         * strict-aliasing violations when copying the mixed-width status. */
        out->magic=__atomic_load_n(&shared->magic,__ATOMIC_RELAXED);
        out->version=__atomic_load_n(&shared->version,__ATOMIC_RELAXED);
        out->bytes=__atomic_load_n(&shared->bytes,__ATOMIC_RELAXED);
        out->rate=__atomic_load_n(&shared->rate,__ATOMIC_RELAXED);
        out->ring_frames=__atomic_load_n(&shared->ring_frames,__ATOMIC_RELAXED);
        out->period_frames=__atomic_load_n(&shared->period_frames,__ATOMIC_RELAXED);
        out->lead_frames=__atomic_load_n(&shared->lead_frames,__ATOMIC_RELAXED);
        out->state=__atomic_load_n(&shared->state,__ATOMIC_RELAXED);
        out->epoch=__atomic_load_n(&shared->epoch,__ATOMIC_RELAXED);
        out->out_channels=__atomic_load_n(&shared->out_channels,__ATOMIC_RELAXED);
        out->in_channels=__atomic_load_n(&shared->in_channels,__ATOMIC_RELAXED);
        out->host_hz=__atomic_load_n(&shared->host_hz,__ATOMIC_RELAXED);
        out->reserved=__atomic_load_n(&shared->reserved,__ATOMIC_RELAXED);
        out->underruns=__atomic_load_n(&shared->underruns,__ATOMIC_RELAXED);
        out->overruns=__atomic_load_n(&shared->overruns,__ATOMIC_RELAXED);
        out->last_error=__atomic_load_n(&shared->last_error,__ATOMIC_RELAXED);
        out->clock_result=__atomic_load_n(&shared->clock_result,__ATOMIC_RELAXED);
        out->ticks_per_frame_q32=__atomic_load_n(&shared->ticks_per_frame_q32,__ATOMIC_RELAXED);
        out->anchor_host=__atomic_load_n(&shared->anchor_host,__ATOMIC_RELAXED);
        out->out_consumed=__atomic_load_n(&shared->out_consumed,__ATOMIC_RELAXED);
        out->in_produced=__atomic_load_n(&shared->in_produced,__ATOMIC_RELAXED);
        out->output_periods=__atomic_load_n(&shared->output_periods,__ATOMIC_RELAXED);
        out->input_periods=__atomic_load_n(&shared->input_periods,__ATOMIC_RELAXED);
        out->sequence=sequence;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (sequence==__atomic_load_n(&shared->sequence,__ATOMIC_ACQUIRE)) {
            if (out->magic!=NA_AUDIO_MAGIC || out->version!=NA_AUDIO_ABI_VERSION || out->bytes!=sizeof *out ||
                out->rate!=NA_AUDIO_RATE || out->ring_frames!=NA_AUDIO_RING_FRAMES ||
                out->period_frames!=NA_AUDIO_PERIOD_FRAMES || out->lead_frames!=NA_AUDIO_LEAD_FRAMES ||
                out->out_channels!=2 || out->in_channels>1 || !out->ticks_per_frame_q32) return NA_WIRE;
            return NA_OK;
        }
    }
    return NA_BUSY;
}
void na_audio_status_publish(NaAudioStatus *shared,const NaAudioStatus *private_state) {
    if (!shared || !private_state || shared==private_state) return;
    uint32_t old=__atomic_load_n(&shared->sequence,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->sequence,old+1,__ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&shared->magic,private_state->magic,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->version,private_state->version,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->bytes,private_state->bytes,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->rate,private_state->rate,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->ring_frames,private_state->ring_frames,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->period_frames,private_state->period_frames,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->lead_frames,private_state->lead_frames,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->state,private_state->state,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->epoch,private_state->epoch,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->out_channels,private_state->out_channels,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->in_channels,private_state->in_channels,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->host_hz,private_state->host_hz,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->reserved,private_state->reserved,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->underruns,private_state->underruns,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->overruns,private_state->overruns,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->last_error,private_state->last_error,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->clock_result,private_state->clock_result,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->ticks_per_frame_q32,private_state->ticks_per_frame_q32,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->anchor_host,private_state->anchor_host,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->out_consumed,private_state->out_consumed,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->in_produced,private_state->in_produced,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->output_periods,private_state->output_periods,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->input_periods,private_state->input_periods,__ATOMIC_RELAXED);
    __atomic_store_n(&shared->sequence,old+2,__ATOMIC_RELEASE);
}
