/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_INPUT_PACK_H
#define NEZHA_AUDIO_INPUT_PACK_H
#include "AdspFirmware.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Existing pandora PANDFW01 container, not a new on-device storage format.
 * Immutable bytes supplied by the sealed private input loader; no filesystem,
 * Android partition or user-client address is accepted by this parser. */
typedef struct {const uint8_t *data;uint64_t bytes;uint32_t count;} NaInputPack;
int na_input_pack(NaInputPack *,const void *,uint64_t bytes);
int na_input_find(const NaInputPack *,const char *name,NaFirmwarePiece *);
/* kind0=adsp,kind1=adsp_dtb. b00 supplies ELF headers; .mdt is the original
 * signed metadata reference. Retain both and every piece until DSP shutdown. */
int na_input_firmware(const NaInputPack *,unsigned kind,NaFirmwarePiece *pieces,
                      uint32_t capacity,uint32_t *count,NaFirmwarePiece *mdt);
#ifdef __cplusplus
}
#endif
#endif
