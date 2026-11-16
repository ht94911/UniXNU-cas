/* SPDX-License-Identifier: BSD-2-Clause */
/* The remote file system the modem keeps its EFS on: QMI service 14, version 1, instance 0, 512-byte sectors copied
 * between an image and the shared rmtfs buffer.
 *
 * Message ids and TLVs follow the Coachz implementation (Trogdor-ipadOS coachz-prelink-wifi.patch wr_request,
 * BSD-2-Clause, HoshinoYumeka), which follows linux-msm rmtfs: 1 OPEN (0x01 path -> 0x10 caller id), 2 CLOSE,
 * 3 RW_IOVEC (0x01 caller, 0x02 direction, 0x03 count byte + {sector, physical, sectors} entries), 4 ALLOC_BUFF
 * (0x02 size -> 0x10 u64 address), 5 GET_DEV_ERROR (-> 0x10 status byte). Every reply carries 0x02 {result, error}.
 *
 * nezha difference: the images are the device's own modemst1/modemst2/fsg/fsc read-only copies supplied by the
 * board; Android's rmt_storage maps the same four names. Writes land in RAM only and are lost at reboot, exactly the
 * behaviour of "rmtfs -r". Nothing here writes a partition or a pinned file. Images and the shared buffer are owned by
 * the board; this file only checks bounds and copies. */
#ifndef NB_RMTFS_H
#define NB_RMTFS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NB_RMTFS_SERVICE 14u
#define NB_RMTFS_INSTANCE 1u   /* version 1 | instance 0 << 8 */
#define NB_RMTFS_SECTOR 512u
#define NB_RMTFS_IMAGES 4u

enum { NB_RMTFS_OPEN = 1, NB_RMTFS_CLOSE = 2, NB_RMTFS_RW_IOVEC = 3, NB_RMTFS_ALLOC_BUFF = 4, NB_RMTFS_GET_DEV_ERROR = 5 };

typedef struct {
    const char *path;          /* "/boot/modem_fs1", "/boot/modem_fs2", "/boot/modem_fsg", "/boot/modem_fsc" */
    uint8_t *data;             /* RAM copy, writable; never flushed anywhere */
    uint64_t bytes;            /* multiple of 512 */
} NbRmtfsImage;

typedef struct {
    NbRmtfsImage image[NB_RMTFS_IMAGES];
    uint32_t count;
    uint8_t *shared;           /* CPU mapping of the rmtfs buffer the modem can access */
    uint64_t shared_phys, shared_bytes;
    /* Cache maintenance around the copy: before reading what the modem wrote, after writing what it will read. */
    void (*sync)(void *context, void *cpu, uint64_t bytes, int to_device);
    void *context;
    uint8_t alloc_done;
    uint64_t opens[NB_RMTFS_IMAGES], reads, writes, sectors, rejects, unknown;
    uint32_t last_error;
} NbRmtfs;

/* Validates image sizes (non-zero multiples of 512) and the buffer; 0 on success. */
int nb_rmtfs_init(NbRmtfs *r);
/* One request message (QMI header + TLVs). Writes the reply into out and returns its length, 0 = no reply (not a
 * request or unknown id, which rmtfs also leaves unanswered). */
uint32_t nb_rmtfs_handle(NbRmtfs *r, const uint8_t *msg, uint32_t length, uint8_t *out, uint32_t capacity);

#ifdef __cplusplus
}
#endif
#endif
