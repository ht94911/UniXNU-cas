/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef NB_FIRMWARE_H
#define NB_FIRMWARE_H
#include "NbArchive.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    QcomMdtSplit split;
    QcomMdtImage image;
    NbArchiveFile piece[QCOM_MDT_MAX_SEGMENTS];
    uint64_t piece_offset[QCOM_MDT_MAX_SEGMENTS];
    uint64_t bytes;
    uint8_t joined;
} NbFirmware;
/* base is the archive-relative stem. Supports signed .mbn and split .mdt/.bNN.
 * No authentication call or destination write occurs here. */
int nb_firmware_open(NbFirmware *,NbArchive *,const char *base,void *headers,size_t capacity);
int nb_firmware_read(void *,uint64_t,void *,size_t);
#ifdef __cplusplus
}
#endif
#endif
