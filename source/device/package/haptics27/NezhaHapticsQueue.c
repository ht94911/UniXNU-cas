/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* From MarbleHapticsRuntime.cpp 7a0d68ff: 96-frame envelope segments.
 * Added protocol/session ownership and absolute playback deadlines. All calls
 * share the runtime's sleepable gate; no hardware call occurs in this file. */
#include "NezhaHaptics.h"
#include <sys/errno.h>
enum { ARGUMENT=-EINVAL,STALE=-ESTALE,BUSY=-EBUSY,RANGE=-ERANGE,OVERFLOW=-EOVERFLOW };
static int advance(uint32_t *value) {if(*value==UINT32_MAX)return OVERFLOW;++*value;return 0;}
static unsigned envelope(unsigned peak) {
    /* Reuse HakiOS redwood's gain_for policy at f128f1d8: iOS's low-level
     * Taptic samples are an envelope for the board's native LRA carrier,
     * rather than a request to drive the motor at that fraction of a volt.
     * Its normalized range is 5/8..1 of the verified board drive, reaching
     * full native strength at 1/3 PCM scale. Keep silence silent and the
     * same 2000mV/500ms executor cap. Only this arithmetic policy is reused;
     * no AW86927 hardware, waveform, voltage or calibration is imported. */
    if(peak<128)return 0;
    if(peak>=32767u/3u)return NZ_HAPTICS_MAX_PERMILLE;
    return 625u+375u*peak/(32767u/3u);
}
void nz_haptics_queue_open(NzHapticsQueue *q,uint32_t session) {
    __builtin_memset(q,0,sizeof *q);q->session=session;q->ready=session!=0;
}
static void cancel(NzHapticsQueue *q) {
    if(q->count || q->active_sequence)++q->cancelled;
    q->head=q->count=0;q->last_end_us=0;q->stopping=1;
}
int nz_haptics_queue_start(NzHapticsQueue *q,uint32_t session,uint32_t *epoch) {
    if(!q || !epoch || !session || session!=q->session || !q->ready)return STALE;
    if(q->running || q->stopping || q->active_sequence)return BUSY;
    int e=advance(&q->epoch);if(e)return e;
    q->running=1;q->error=0;q->next_sequence=1;q->last_end_us=0;*epoch=q->epoch;return 0;
}
int nz_haptics_queue_stop(NzHapticsQueue *q,uint32_t session,uint32_t epoch) {
    if(!q || session!=q->session || epoch!=q->epoch || !q->running)return STALE;
    if(q->epoch==UINT32_MAX)return OVERFLOW;
    cancel(q);q->running=0;++q->epoch;q->next_sequence=0;return 0;
}
void nz_haptics_queue_close(NzHapticsQueue *q) {
    if(!q)return;
    cancel(q);q->running=0;q->ready=0;q->session=0;
}
int nz_haptics_queue_pcm(NzHapticsQueue *q,const NzHapticsPacket *p,const int16_t *pcm,uint64_t now,uint64_t at) {
    if(!q || !p || !pcm || !p->count || p->count>NZ_HAPTICS_MAX_FRAMES || p->value)return ARGUMENT;
    if(!q->ready || !q->running || p->session!=q->session || p->epoch!=q->epoch ||
       p->sequence!=q->next_sequence)return STALE;
    if(q->next_sequence==UINT32_MAX)return OVERFLOW;
    const unsigned needed=(p->count+NZ_HAPTICS_SEGMENT_FRAMES-1)/NZ_HAPTICS_SEGMENT_FRAMES;
    if(q->count>NZ_HAPTICS_QUEUE)return BUSY;
    const uint64_t length=((uint64_t)p->count*1000000+NZ_HAPTICS_RATE-1)/NZ_HAPTICS_RATE;
    if(at>UINT64_MAX-length || (at>now && at-now>250000) ||
       (q->last_end_us>at && q->last_end_us-at>42))return RANGE;
    // CoreAudio may submit a late prewarm/catch-up buffer. Retire its sequence
    // without playing it late or destroying the live Actuator stream.
    if(at+length<=now || now<q->rest_until_us) {
        q->underruns+=needed;
        q->last_end_us=at+length;++q->next_sequence;++q->requests;
        return 0;
    }
    // Reclaim elapsed envelope slots before deciding whether the finite
    // queue can accept this buffer. A full future queue drops one buffer;
    // the next one still belongs to the same stream/session/sequence.
    while(q->count && q->segments[q->head].end_us<=now) {
        q->head=(q->head+1)%NZ_HAPTICS_QUEUE;--q->count;++q->underruns;
    }
    if(needed>NZ_HAPTICS_QUEUE-q->count) {
        q->underruns+=needed;
        q->last_end_us=at+length;++q->next_sequence;++q->requests;
        return 0;
    }
    for(unsigned offset=0;offset<p->count;offset+=NZ_HAPTICS_SEGMENT_FRAMES) {
        const unsigned n=p->count-offset<NZ_HAPTICS_SEGMENT_FRAMES?p->count-offset:NZ_HAPTICS_SEGMENT_FRAMES;
        const uint64_t end=at+((uint64_t)(offset+n)*1000000+NZ_HAPTICS_RATE-1)/NZ_HAPTICS_RATE;
        if(end<=now){++q->underruns;continue;}
        unsigned peak=0;
        for(unsigned i=0;i<n;++i) {int32_t sample=pcm[offset+i];unsigned v=(unsigned)(sample<0?-sample:sample);if(v>peak)peak=v;}
        NzHapticsSegment *s=&q->segments[(q->head+q->count)%NZ_HAPTICS_QUEUE];
        s->begin_us=at+(uint64_t)offset*1000000/NZ_HAPTICS_RATE;
        s->end_us=end;
        s->epoch=q->epoch;s->sequence=p->sequence;s->operation=NzHapticsPCM;s->effect=0;
        s->amplitude=envelope(peak);
        ++q->count;
    }
    q->last_end_us=at+length;++q->next_sequence;++q->requests;return 0;
}
int nz_haptics_queue_next(NzHapticsQueue *q,uint64_t now,NzHapticsSegment *out) {
    if(!q || !out)return ARGUMENT;
    while(q->count) {
        NzHapticsSegment *s=&q->segments[q->head];
        if(s->epoch!=q->epoch || s->end_us<=now) {
            q->head=(q->head+1)%NZ_HAPTICS_QUEUE;--q->count;++q->underruns;continue;
        }
        if(s->begin_us>now)return 0;
        *out=*s;q->head=(q->head+1)%NZ_HAPTICS_QUEUE;--q->count;return 1;
    }
    return 0;
}
void nz_haptics_status(const NzHapticsQueue *q,NzHapticsStatus *out) {
    *out=(NzHapticsStatus){NZ_HAPTICS_ABI,q->session,q->epoch,q->next_sequence,q->ready,
        q->active_sequence!=0,q->count,q->error,q->completed,q->cancelled,q->requests,q->underruns};
}
