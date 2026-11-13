/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_CAMERA_JOURNAL_H
#define NEZHA_CAMERA_JOURNAL_H

/* A fixed path derived from a per-trial 32-bit nonce, in the existing iOS27
 * Data log directory. Only the camera worker appends, in normal context.
 * No new log partition, direct disk writes, panic hook or reboot timer. */
int nezha_camera_journal_begin(unsigned token, unsigned mode);
int nezha_camera_journal_mark(unsigned phase, unsigned status,
    unsigned a, unsigned b, unsigned c);

enum { NezhaCameraPatternBytes = 640 * 480 * 5 / 4, NezhaCameraMainBytes = 4096 * 3072 * 5 / 4 };
/* Persist one complete frame after DMA teardown on existing iOS27 Data. */
int nezha_camera_journal_save_frame(unsigned token, const void *frame, unsigned bytes);

#endif
