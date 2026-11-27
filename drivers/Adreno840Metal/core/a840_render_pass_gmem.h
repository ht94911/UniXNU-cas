/* SPDX-License-Identifier: MIT */
#ifndef A840_RENDER_PASS_GMEM_H
#define A840_RENDER_PASS_GMEM_H
#include "a840_emit_gmem.h"
#include "a840_runtime_cmd.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct A840RtGmemPass {
    A840GmemPass desc;
    A840GmemBinding bindings[A840_GMEM_PLANES];
    const A840GmemVsc *vsc;
    /* Optional immutable primary draw state/draw IB for each phase.
     * Each must establish its program, MRT/ZS formats, sample and draw
     * state. A clear-only pass needs neither. Draw IBs are trusted runtime
     * products. Include their transitive BOs in the submission residency. */
    A840GmemRange bin_draws, tile_draws;
} A840RtGmemPass;

/* All supplied storage is caller-owned. On failure no words/relocs are
 * published. Sources and destination must be disjoint. */
A840EmitStatus a840_rt_gmem_encode(A840CmdStream *dst, const A840RtGmemPass *pass,
                                    A840GmemPlan *out_plan);

/* A normal W03 device/queue backed by a submit adapter. Its GMEM prefix
 * and W03 completion IB travel in one ordered transport submission.
 * Keep this object at a stable address until close. */
typedef struct A840RtGmemQueue {
    A840BackendTransport downstream;
    A840BackendTransportOps ops;
    A840RtDevice *device;
    A840RtQueue *queue;
} A840RtGmemQueue;

enum { A840_GMEM_MAX_RESIDENT = 64 };
typedef struct A840RtGmemFlight {
    A840RtCmdBuffer *cb;
    A840RtBuffer *resident[A840_GMEM_MAX_RESIDENT + 1];
    size_t count;
} A840RtGmemFlight;
A840RtStatus a840_rt_gmem_open(A840RtGmemQueue *, A840BackendTransport);
void a840_rt_gmem_close(A840RtGmemQueue *);
/* flight starts zeroed; ib is exclusive and CPU mapped, scratch is temporary
 * relocation storage. Data/flags/VSC/draw resources and indirect draw IB
 * dependencies must be supplied in resident. Keep IB contents immutable
 * until retirement. A successful submit retains all of these buffers. */
A840RtStatus a840_rt_gmem_submit(A840RtGmemQueue *, const A840RtGmemPass *,
    A840RtBuffer *ib, A840Reloc *scratch, size_t capacity,
    A840RtBuffer *const resident[], size_t count, A840RtGmemFlight *flight);
A840RtStatus a840_rt_gmem_retire(A840RtGmemFlight *, uint64_t timeout_ns);
#ifdef __cplusplus
}
#endif
#endif
