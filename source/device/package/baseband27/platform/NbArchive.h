/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef NB_ARCHIVE_H
#define NB_ARCHIVE_H
#include "NbCommon.h"
#include "../upstream/QcomMdt.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NB_ARCHIVE_HEADER 64u
#define NB_ARCHIVE_ENTRY 256u
#define NB_ARCHIVE_NAME 192u
#define NB_ARCHIVE_FILES 4096u
#define NB_ARCHIVE_BLOCK 4096u
#define NB_ARCHIVE_EXTENTS 4096u
/* Hash provider uses the package's existing SHA-256 implementation. */
typedef int (*NbArchiveHash)(void *, QcomMdtRead, void *, uint64_t, uint64_t, uint8_t[32]);
typedef struct {
    QcomMdtRead read;
    void *context;
    NbArchiveHash hash;
    void *hash_context;
    uint8_t *index;                 /* caller storage, NB_ARCHIVE_ENTRY * count */
    uint32_t count;
    uint64_t bytes;
} NbArchive;
typedef struct { NbArchive *archive; uint64_t offset, bytes; } NbArchiveFile;
/* Validates the ADT whole-image digest before parsing its index. */
int nb_archive_open(NbArchive *, QcomMdtRead, void *, uint64_t, const uint8_t[32],
                    NbArchiveHash, void *, void *, size_t);
int nb_archive_find(NbArchive *, const char *, NbArchiveFile *);
int nb_archive_read(void *, uint64_t, void *, size_t);
/* Validate one entry's digest (whole image was already checked). */
int nb_archive_verify(NbArchive *, const char *);
#ifdef __cplusplus
}
#endif
#endif
