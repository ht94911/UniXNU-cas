/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Lifecycle hardened around sheng's four in-flight, eight-slot period engine. */
#include "AudioStream.h"
#include <string.h>
#include <limits.h>
static uint32_t rd(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void rejected_done(NaStream *s,const GprPacket *p,uint32_t reason) {
    if (s->first_bad_done.reason) return;
    NaDoneRejection r={0};r.reason=reason;r.opcode=p->opcode;r.token=p->token;
    r.generation=s->generation;r.payload_bytes=p->payload_bytes;r.slot=NA_DMA_SLOTS;
    for (unsigned i=0;i<15 && (i+1)*4<=p->payload_bytes;++i) r.payload[i]=rd(p->payload+i*4);
    if (s->capture ? (p->payload_bytes==56u || p->payload_bytes==60u) : p->payload_bytes==32u) {
        const uint8_t *a=p->payload+(s->capture ? 4:0);
        uint64_t address=rd(a)|((uint64_t)rd(a+4)<<32);
        if (s->stride && address>=s->dsp_base && (address-s->dsp_base)%s->stride==0 &&
            (address-s->dsp_base)/s->stride<NA_DMA_SLOTS) {
            r.slot=(uint32_t)((address-s->dsp_base)/s->stride);
            r.expected_token=s->slot[r.slot].token;r.state=s->slot[r.slot].state;
        }
    }
    s->first_bad_done=r;
}
void na_stream_rejection_words(const NaStream *s,uint32_t out[NA_DONE_REJECTION_WORDS]) {
    if (!s || !out) return;
    const NaDoneRejection *r=&s->first_bad_done;
    out[0]=r->reason;out[1]=r->opcode;out[2]=r->token;out[3]=r->expected_token;
    out[4]=r->state;out[5]=r->generation;out[6]=r->payload_bytes;out[7]=r->slot;
    memcpy(out+8,r->payload,sizeof r->payload);
}
int na_stream_init(NaStream *s,void *mem,uint64_t address,uint32_t bytes,
                   uint32_t period,uint32_t frame,uint32_t port,uint32_t endpoint,int capture) {
    if (!s || !mem || ((uintptr_t)mem&63) || (address&4095) || !period || period>4096 ||
        !frame || frame>8 || period%frame || bytes<4096*NA_DMA_SLOTS || (bytes&4095) ||
        (uint64_t)(uint32_t)address+bytes>0x100000000ULL || !port || !endpoint) return NA_ARGUMENT;
    memset(s,0,sizeof *s);s->memory=mem;s->dsp_base=address;s->bytes=bytes;s->stride=4096;
    s->period_bytes=period;s->frame_bytes=frame;s->port=port;s->endpoint=endpoint;s->capture=!!capture;
    return NA_OK;
}
int na_stream_mapped(NaStream *s,uint32_t handle,uint32_t generation) {
    if (!s || !handle || !generation) return NA_ARGUMENT;
    if (s->handle || s->quarantine || s->running) return NA_OWNERSHIP;
    s->handle=handle;s->generation=generation;return NA_OK;
}
int na_stream_start(NaStream *s) {
    if (!s || !s->handle || s->quarantine || s->retiring || s->in_flight) return NA_OWNERSHIP;
    if (s->running) return NA_BUSY;
    s->running=1;return NA_OK;
}
int na_stream_prepare(NaStream *s,NaSession *session,uint64_t now,uint32_t timeout,
                      unsigned *index,uint8_t packet[GPR_MAX_PACKET],uint32_t *length) {
    if (!s || !session || !index || !packet || !length || !timeout || timeout>10000 ||
        now>UINT64_MAX-timeout) return NA_ARGUMENT;
    if (!s->running || s->retiring || s->quarantine || !session->online ||
        s->generation!=session->generation) return NA_OFFLINE;
    if (s->next==UINT64_MAX || s->next-s->returned>=NA_DMA_FLIGHT) return NA_BUSY;
    unsigned i=(unsigned)(s->next%NA_DMA_SLOTS);
    NaBuffer *b=&s->slot[i];
    if (b->state!=NaBufferFree) return NA_OWNERSHIP;
    uint32_t token=na_token(session);if (!token) return NA_OVERFLOW;
    *length=na_buffer_packet(packet,s->port,s->endpoint,token,s->capture,
        s->dsp_base+(uint64_t)i*s->stride,s->handle,s->period_bytes);
    if (!*length) return NA_ARGUMENT;
    *b=(NaBuffer){token,s->generation,NaBufferCpu,0,s->period_bytes,s->next,now+timeout};
    *index=i;return NA_OK;
}
int na_stream_commit(NaStream *s,unsigned i) {
    if (!s || i>=NA_DMA_SLOTS || !s->running || s->retiring || s->quarantine || s->slot[i].state!=NaBufferCpu ||
        s->slot[i].sequence!=s->next) return NA_OWNERSHIP;
    s->slot[i].state=NaBufferDsp;++s->in_flight;++s->next;return NA_OK;
}
int na_stream_unsent(NaStream *s,unsigned i) {
    if (!s || i>=NA_DMA_SLOTS) return NA_ARGUMENT;
    NaBuffer *b=&s->slot[i];
    if (b->state==NaBufferDsp && b->sequence+1==s->next) {
        --s->next;--s->in_flight;
    } else if (b->state!=NaBufferCpu) return NA_OWNERSHIP;
    memset(b,0,sizeof *b);return NA_OK;
}
int na_stream_done(NaStream *s,const uint8_t *data,uint32_t length) {
    if (!s) return NA_ARGUMENT;
    GprPacket p;
    if (na_packet(data,length,&p)) return NA_WIRE;
    if (p.dst_port!=s->port || p.src_port!=s->endpoint) return NA_IGNORED;
    int basic=p.opcode==GPR_OP_BASIC_RESULT;
    uint32_t reason=NaDoneSize;
    if (!basic && p.opcode!=(s->capture ? AR_OP_READ_DONE : AR_OP_WRITE_DONE)) return NA_IGNORED;
    NaBuffer *b=0;unsigned i;
    for (i=0;i<NA_DMA_SLOTS;++i)
        if (s->slot[i].token==p.token && p.token) {b=&s->slot[i];break;}
    if (!b || b->state!=NaBufferDsp || b->generation!=s->generation) {
        if (basic) return NA_IGNORED; /* belongs to another control transaction */
        rejected_done(s,&p,!b ? NaDoneNoToken:b->state!=NaBufferDsp ? NaDoneState:NaDoneGeneration);
        ++s->bad_done;return NA_IGNORED; /* duplicate or retired generation */
    }
    const uint8_t *d=p.payload;
    if (basic) {
        if (p.payload_bytes!=8) goto malformed;
        if (rd(d)!=(s->capture ? AR_OP_READ:AR_OP_WRITE)) return NA_IGNORED;
        uint32_t status=rd(d+4);
        /* A successful command receipt is not a DMA completion. Error
         * receipts retain their actual DSP status and require CLOSE/UNMAP. */
        if (!status) return NA_IGNORED;
        if (!s->remote_errors) s->last_remote_status=status;
        ++s->remote_errors;s->quarantine=1;s->running=0;return NA_REMOTE;
    }
    /* AudioReach's published V2 reply is56 bytes, without an offset; the
     * existing cached Linux layout is60 bytes. Both mirror the same submitted
     * data address/handle. Only these complete, metadata-free headers apply. */
    if (s->capture ? (p.payload_bytes!=56u && p.payload_bytes!=60u) : p.payload_bytes!=32u) goto malformed;
    uint32_t status=rd(d+(s->capture ? 0:12));
    const uint8_t *a=d+(s->capture ? 4:0);
    uint64_t address=rd(a)|((uint64_t)rd(a+4)<<32);
    reason=NaDoneAddress;
    if (address!=s->dsp_base+(uint64_t)i*s->stride || rd(a+8)!=s->handle) goto malformed;
    /* No metadata buffer was supplied. Never accept an unsolicited DMA target. */
    if (s->capture) {
        reason=NaDoneMetadata;
        uint32_t metadata=p.payload_bytes==56u ? 36u:40u;
        for (unsigned word=0;word<5;++word)
            if (rd(d+metadata+word*4)) goto malformed;
        uint32_t bytes=rd(d+16),offset=p.payload_bytes==60u ? rd(d+20):0;
        reason=NaDoneBounds;
        if (offset>s->period_bytes || bytes>s->period_bytes-offset ||
            offset%s->frame_bytes || bytes%s->frame_bytes) goto malformed;
        b->length=bytes;b->offset=offset;
    } else {
        reason=NaDoneMetadata;
        if (rd(d+16) || rd(d+20) || rd(d+24) || rd(d+28)) goto malformed;
    }
    if (status) {
        if (!s->remote_errors) s->last_remote_status=status;
        ++s->remote_errors;s->quarantine=1;s->running=0;return NA_REMOTE;
    }
    if (s->capture && !s->first_read_done) {
        s->first_read_done=((uint64_t)p.token<<32)|((uint64_t)!!s->quarantine<<26)|
            ((uint64_t)!!s->retiring<<25)|((uint64_t)!!s->running<<24)|
            ((uint64_t)b->length<<8)|p.payload_bytes;
    }
    ++s->completions;b->state=NaBufferReturned;--s->in_flight;return NA_OK;
malformed:
    if (!basic) rejected_done(s,&p,reason);
    ++s->bad_done;s->quarantine=1;s->running=0;return NA_WIRE;
}
int na_stream_take(NaStream *s,void *capture,uint32_t capacity,uint32_t *bytes) {
    if (!s || !bytes) return NA_ARGUMENT;
    if (s->quarantine) return NA_OWNERSHIP;
    NaBuffer *b=&s->slot[s->returned%NA_DMA_SLOTS];
    if (b->state!=NaBufferReturned || b->sequence!=s->returned) return NA_PENDING;
    if (s->capture) {
        if (!capture || capacity<b->length) return NA_ARGUMENT;
        /* Platform invalidates this slot only AFTER authenticated completion. */
        memcpy(capture,s->memory+(s->returned%NA_DMA_SLOTS)*s->stride+b->offset,b->length);
    }
    *bytes=b->length;s->frames+=b->length/s->frame_bytes;++s->returned;
    memset(b,0,sizeof *b);return NA_OK;
}
void na_stream_stop(NaStream *s) { if (s) {s->running=0;s->retiring=1;} }
void na_stream_expire(NaStream *s,uint64_t now) {
    if (!s) return;
    for (unsigned i=0;i<NA_DMA_SLOTS;++i)
        if (s->slot[i].state==NaBufferDsp && now>=s->slot[i].deadline_ms) {
            s->quarantine=1;s->running=0;
        }
}
void na_stream_lost(NaStream *s) { if (s) {s->quarantine=1;s->running=0;s->retiring=1;} }
int na_stream_reclaim(NaStream *s,int close_ack,int unmap_ack,int revoked) {
    if (!s || s->running) return NA_OWNERSHIP;
    /* Successful CLOSE then UNMAP on the original session also proves no
     * outstanding DMA, including a command that timed out locally. */
    if (!revoked && !(close_ack && unmap_ack)) return NA_OWNERSHIP;
    memset(s->slot,0,sizeof s->slot);s->handle=0;s->in_flight=0;s->quarantine=0;
    s->retiring=0;s->next=0;s->returned=0;s->frames=0;return NA_OK;
}
