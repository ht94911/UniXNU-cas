/* SPDX-License-Identifier: BSD-3-Clause */
/* The TFTP server the modem reads its configuration tree from (QRTR service 4096, version 1, instance 0): the protocol
 * of Linaro's tqftpserv (linux-msm/tqftpserv, BSD-3-Clause), rewritten for a kernel worker with no sockets and no
 * allocation. Same wire format and option semantics:
 *   RRQ 1, WRQ 2, DATA 3, ACK 4, ERROR 5, OACK 6; only "octet" mode;
 *   options blksize (8..65464), timeoutms (1..255000), tsize, rsize (1..100 MB), wsize (1..65535), seek;
 *   with options the answer is an OACK (blksize, timeoutms, tsize when asked, wsize, rsize and seek when set), otherwise
 *   DATA block 1 at once; ACK n is answered with blocks n+1 .. n+wsize; block b holds bytes seek + (b-1)*blksize;
 *   with rsize the transfer covers exactly rsize bytes (the last block trimmed), without it the usual short final block;
 *   each transfer runs on its own local port, connected to the requesting node/port; packets from anyone else are dropped;
 *   ERROR 9 from the client is a normal end of transfer.
 * Paths: "/readonly/firmware/image/..." is the device's read-only modem configuration tree (modem_pr, modem_oem and the
 * domain JSON files, supplied by the board from its read-only firmware store, the same files Android's tftp_server serves
 * from its firmware mount); "/readwrite/..." is a small RAM store that starts empty and is lost at reboot (Linux tqftpserv
 * also keeps it in a scratch directory). Nothing here writes any partition or file on the phone.
 * Differences from tqftpserv, all bounded-resource choices: blksize is capped (OACK carries the accepted value, as
 * RFC 2348 allows), a refused send is retried on the next poll instead of being lost, block numbers past 65535 wrap as in
 * RFC 1350 implementations that support large files, idle transfers are closed after their timeout. */
#ifndef NB_TFTP_H
#define NB_TFTP_H
#include <stdint.h>
#include "../transport/NbQrtr.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NB_TFTP_SERVICE 4096u
#define NB_TFTP_INSTANCE 1u        /* version 1, instance 0 */
#define NB_TFTP_SESSIONS 8u
#define NB_TFTP_PATH 128u
#define NB_TFTP_RW_FILES 32u
#define NB_TFTP_RW_FILE_BYTES 65536u
#define NB_TFTP_IDLE_MS 30000u

enum { NB_TFTP_RRQ = 1, NB_TFTP_WRQ = 2, NB_TFTP_DATA = 3, NB_TFTP_ACK = 4, NB_TFTP_ERROR = 5, NB_TFTP_OACK = 6 };
enum { NB_TFTP_EUNDEF = 0, NB_TFTP_ENOENT = 1, NB_TFTP_EACCESS = 2, NB_TFTP_ENOSPACE = 3, NB_TFTP_EBADOP = 4,
       NB_TFTP_EBADID = 5, NB_TFTP_EEXISTS = 6, NB_TFTP_ENOUSER = 7, NB_TFTP_EOPTNEG = 8, NB_TFTP_END = 9 };

typedef struct {
    void *context;
    /* path is relative to the image directory ("modem_pr/mcfg/..."). 0 and the size when the file exists. */
    int (*lookup)(void *context, const char *path, uint64_t *size, uint32_t *handle);
    /* Bytes read (may be short only at the end of the file) or a negative error. */
    int64_t (*read)(void *context, uint32_t handle, uint64_t offset, void *to, uint32_t bytes);
} NbTftpStore;

typedef struct {
    uint8_t used, writing, oack_due, finished;
    uint32_t port, node, peer;                 /* our port; the client */
    uint8_t ram;                               /* the file is in the RAM store (index in handle) */
    uint32_t handle;
    uint64_t size, seek, limit;                /* limit: bytes this transfer covers (reads) */
    uint32_t blksize, wsize, timeout_ms;
    int64_t tsize;                             /* -1 when not asked */
    uint8_t has_rsize, has_seek, has_options;
    uint64_t acked, next, window_end, last;    /* unwrapped block numbers; last: final block (0 = not yet known) */
    uint64_t idle_at;
} NbTftpSession;

typedef struct {
    NbQrtr *qrtr;
    uint32_t service_port;
    uint32_t first_port;                       /* sessions use first_port .. first_port + NB_TFTP_SESSIONS - 1 */
    uint32_t max_blksize;
    NbTftpStore store;
    uint8_t *packet;                           /* 4 + max_blksize bytes, owned by the board */
    uint8_t *rw_pool;                          /* NB_TFTP_RW_FILES * NB_TFTP_RW_FILE_BYTES bytes */
    struct { char path[NB_TFTP_PATH]; uint32_t bytes; uint8_t used; } rw[NB_TFTP_RW_FILES];
    NbTftpSession session[NB_TFTP_SESSIONS];
    uint64_t now;
    uint64_t reads, writes, bytes_out, bytes_in, not_found, refused, errors_sent, spoofed, timeouts;
    char last_missing[NB_TFTP_PATH];
} NbTftp;

/* Registers service 4096 on service_port. */
int nb_tftp_init(NbTftp *t, NbQrtr *q, uint32_t service_port, uint32_t first_port, uint32_t max_blksize, const NbTftpStore *store,
                 uint8_t *packet, uint8_t *rw_pool);
/* NbQrtrEvents.unrouted calls this first: 1 when the port is one of our transfers. */
int nb_tftp_session_packet(NbTftp *t, uint32_t port, uint32_t node, uint32_t from_port, const uint8_t *data, uint32_t length);
/* A client went away: its transfers end. */
void nb_tftp_client_gone(NbTftp *t, uint32_t node, uint32_t port);
/* Sends what is due (blocks refused earlier) and closes idle transfers. */
void nb_tftp_poll(NbTftp *t, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
