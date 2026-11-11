/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Peripheral authentication service (PAS) loading of one signed image, as a bounded state machine.
 *
 * Derived from audio27 platform/AdspFirmware.c (na_pas_begin/na_pas_poll, BSD-2-Clause-Patent), which follows Linux
 * qcom_mdt_pas_init + qcom_mdt_load_no_init + qcom_scm_pas_auth_and_reset. Changes for the modem: the image is a joined
 * .mbn read through a callback from the board's firmware store (the modem image is ~64 MiB and not linked into the
 * kernel), the allowed carveout is a parameter instead of the audio facts, and every step is bounded to one 64 KiB copy.
 *
 * Secure calls go through the board's shared NezhaScm owner (one lock and WAITQ loop for GPU, Wi-Fi, audio and modem).
 * A failed secure call may have changed secure state, so nothing here replays one: a fault is terminal for this boot. */
#ifndef NB_PAS_H
#define NB_PAS_H
#include "NbCommon.h"
#include "../upstream/QcomMdt.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NB_PAS_STEP_BYTES 65536u

enum { NB_PAS_INIT_IMAGE = 1, NB_PAS_MEM_SETUP = 2, NB_PAS_AUTH_AND_RESET = 5, NB_PAS_SHUTDOWN = 6 };
/* What the board returns when the secure world answers a call with -EINVAL: for a modem shutdown that means the image is still
 * being decrypted (Linux qcom_pas_decrypt_shutdown retries 50 times, 100 ms apart). */
#define NB_PAS_EINVAL (-22)
enum { NbPasIdle, NbPasInit, NbPasMemory, NbPasCopy, NbPasAuthenticate, NbPasReady, NbPasFault };

typedef struct {
    void *context;
    /* command is one of NB_PAS_*; 0 on success, NB_BUSY when the shared owner could not take the call now (nothing sent). */
    int (*pas)(void *context, uint32_t command, uint32_t pas, uint64_t arg1, uint64_t arg2);
    /* Clean the CPU cache over bytes written for the secure world or the modem. */
    int (*sync)(void *context, void *cpu, uint64_t physical, uint64_t bytes);
} NbPasOps;

typedef struct {
    QcomMdtImage image;
    QcomMdtRead read;
    void *read_context;
    NbPasOps ops;
    NbRange region;
    uint8_t *mapped;                   /* CPU mapping of region, used only while loading */
    uint8_t *metadata;
    uint64_t metadata_pa, metadata_capacity, span, offset, deadline, timeout;
    uint32_t pas, stage, segment;
    uint8_t secure_touched;            /* a secure call was issued: the region now belongs to PAS until shutdown */
    int error;
} NbPas;

/* Parses the image and writes its metadata into the caller's physically contiguous, page aligned buffer. Checks every
 * loadable segment against region (and against each other) before any secure call. Touches neither region nor PAS. */
int nb_pas_begin(NbPas *l, uint32_t pas, NbRange region, void *mapped, uint64_t file_size, QcomMdtRead read, void *read_context,
                 void *metadata, uint64_t metadata_pa, uint64_t metadata_capacity, const NbPasOps *ops, uint64_t now_ms,
                 uint64_t timeout_ms);
/* One bounded step: init image, memory setup (relocatable images), one copy of at most NB_PAS_STEP_BYTES, or auth and reset.
 * NB_OK once authenticated, NB_PENDING while working, an error once (and from then on). */
int nb_pas_poll(NbPas *l, uint64_t now_ms);
/* Restarts the step deadline (timeout_ms from begin) when the owner reaches this image; loading only starts then. */
void nb_pas_arm(NbPas *l, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
