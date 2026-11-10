/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_STREAM_H
#define NEZHA_AUDIO_STREAM_H
#include "AudioProtocol.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NA_DMA_SLOTS 8u
#define NA_DMA_FLIGHT 4u
enum { NaBufferFree, NaBufferCpu, NaBufferDsp, NaBufferReturned };
enum {NaDoneNoToken=1,NaDoneState,NaDoneGeneration,NaDoneSize,NaDoneAddress,
      NaDoneMetadata,NaDoneBounds};
#define NA_DONE_REJECTION_WORDS 23u
typedef struct {
    uint32_t reason,opcode,token,expected_token,state,generation,payload_bytes,slot;
    uint32_t payload[15]; /* READ_DONE_V2 metadata only; never PCM. */
} NaDoneRejection;
typedef struct {
    uint32_t token, generation, state, offset, length;
    uint64_t sequence, deadline_ms;
} NaBuffer;
typedef struct {
    uint8_t *memory;
    uint64_t dsp_base;        /* Upper word is the verified DSP SID encoding. */
    uint32_t bytes, stride, period_bytes, frame_bytes, handle, generation;
    uint32_t port, endpoint, capture, running, retiring, quarantine, in_flight;
    uint64_t next, returned, frames, completions;
    uint32_t bad_done, remote_errors, underruns, overruns, last_remote_status;
    NaDoneRejection first_bad_done;
    NaBuffer slot[NA_DMA_SLOTS];
} NaStream;
/* No physical allocation here. Platform supplies an exclusively owned, mapped,
 * contiguous low-IOVA range. Cache sync brackets transfer outside this core. */
int na_stream_init(NaStream *s,void *memory,uint64_t dsp_address,uint32_t bytes,
                   uint32_t period_bytes,uint32_t frame_bytes,uint32_t port,
                   uint32_t endpoint,int capture);
int na_stream_mapped(NaStream *s,uint32_t handle,uint32_t generation);
int na_stream_start(NaStream *s);
int na_stream_prepare(NaStream *s,NaSession *session,uint64_t now,uint32_t timeout,
                      unsigned *slot,uint8_t packet[GPR_MAX_PACKET],uint32_t *length);
/* Called BEFORE transport send; undo only if the transport proves not queued. */
int na_stream_commit(NaStream *s,unsigned slot);
int na_stream_unsent(NaStream *s,unsigned slot);
int na_stream_done(NaStream *s,const uint8_t *data,uint32_t length);
void na_stream_rejection_words(const NaStream *s,uint32_t out[NA_DONE_REJECTION_WORDS]);
/* Returned slots are consumed in submission order, even if replies reordered.
 * Capture copies precisely length bytes from offset, never modulo a bad offset. */
int na_stream_take(NaStream *s,void *capture,uint32_t capacity,uint32_t *bytes);
void na_stream_stop(NaStream *s);
void na_stream_expire(NaStream *s,uint64_t now);
void na_stream_lost(NaStream *s);
/* Explicit proofs from graph manager/platform; a GLINK reconnect is NOT proof.
 * Does not free memory: it only permits the owning allocator to reclaim it. */
int na_stream_reclaim(NaStream *s,int close_ack,int unmap_ack,int pas_stopped_and_dma_revoked);
#ifdef __cplusplus
}
#endif
#endif
