/* SPDX-License-Identifier: BSD-2-Clause */
/* Behaviour from Trogdor-ipadOS coachz-prelink-wifi.patch wr_request (BSD-2-Clause, HoshinoYumeka), rewritten
 * on the shared QMI writer with full bounds checks; error codes kept (1/1 not found or out of range, 1/2 bad iovec). */
#include "NbRmtfs.h"
#include "NbQmi.h"

int nb_rmtfs_init(NbRmtfs *r)
{
    if (!r || !r->count || r->count > NB_RMTFS_IMAGES || !r->shared || !r->shared_phys || !r->shared_bytes ||
        (r->shared_phys & 0xfffu) || (r->shared_bytes & (NB_RMTFS_SECTOR - 1u))) return -1;
    for (uint32_t i = 0; i < r->count; ++i)
        if (!r->image[i].path || !r->image[i].data || !r->image[i].bytes || (r->image[i].bytes & (NB_RMTFS_SECTOR - 1u))) return -1;
    return 0;
}

static int same_path(const char *want, const uint8_t *got, uint32_t n)
{
    while (n && !got[n - 1]) --n;  /* a sender may count the terminating zero */
    uint32_t i = 0;
    for (; i < n && want[i]; ++i) if ((uint8_t)want[i] != got[i]) return 0;
    return i == n && !want[i];
}

static void copy(uint8_t *to, const uint8_t *from, uint32_t n) { for (uint32_t i = 0; i < n; ++i) to[i] = from[i]; }

/* RW_IOVEC: every entry is checked before any byte moves, so a bad request changes nothing. */
static uint16_t iovec(NbRmtfs *r, const NbQmiMessage *m)
{
    uint32_t size = 0, caller = 0;
    const uint8_t *v = nb_qmi_tlv(m, 0x01, &size);
    if (v && size >= 4) caller = nb_le32(v);
    v = nb_qmi_tlv(m, 0x02, &size);
    const int to_image = v && size >= 1 && v[0];
    const uint8_t *list = nb_qmi_tlv(m, 0x03, &size);
    if (!caller || caller > r->count || !list || !size || 1u + (uint32_t)list[0] * 12u > size) return 2;
    const NbRmtfsImage *img = &r->image[caller - 1];
    for (uint32_t e = 0; e < list[0]; ++e) {
        const uint8_t *p = list + 1 + 12u * e;
        const uint64_t sector = nb_le32(p), phys = nb_le32(p + 4), count = nb_le32(p + 8);
        const uint64_t bytes = count * NB_RMTFS_SECTOR;
        if (phys < r->shared_phys || phys - r->shared_phys > r->shared_bytes || bytes > r->shared_bytes - (phys - r->shared_phys) ||
            sector * NB_RMTFS_SECTOR > img->bytes || bytes > img->bytes - sector * NB_RMTFS_SECTOR) return 1;
    }
    for (uint32_t e = 0; e < list[0]; ++e) {
        const uint8_t *p = list + 1 + 12u * e;
        const uint64_t at = (uint64_t)nb_le32(p) * NB_RMTFS_SECTOR, off = nb_le32(p + 4) - r->shared_phys;
        const uint64_t bytes = (uint64_t)nb_le32(p + 8) * NB_RMTFS_SECTOR;
        uint8_t *shared = r->shared + off;
        if (!bytes) continue;
        if (to_image) {
            if (r->sync) r->sync(r->context, shared, bytes, 0);
            copy(img->data + at, shared, (uint32_t)bytes);
            r->writes++;
        } else {
            copy(shared, img->data + at, (uint32_t)bytes);
            if (r->sync) r->sync(r->context, shared, bytes, 1);
            r->reads++;
        }
        r->sectors += bytes / NB_RMTFS_SECTOR;
    }
    return 0;
}

uint32_t nb_rmtfs_handle(NbRmtfs *r, const uint8_t *msg, uint32_t length, uint8_t *out, uint32_t capacity)
{
    NbQmiMessage m;
    NbQmiWriter w;
    if (!r || nb_qmi_parse(msg, length, &m) || m.type != NB_QMI_REQUEST ||
        nb_qmi_begin(&w, out, capacity, NB_QMI_RESPONSE, m.transaction, m.message)) return 0;
    uint32_t size = 0;
    const uint8_t *v;
    switch (m.message) {
    case NB_RMTFS_OPEN: {
        v = nb_qmi_tlv(&m, 0x01, &size);
        uint32_t found = 0;
        for (uint32_t i = 0; v && i < r->count && !found; ++i) if (same_path(r->image[i].path, v, size)) found = i + 1;
        if (!found) { r->rejects++; r->last_error = 1; nb_qmi_result(&w, 1, 1); break; }
        r->opens[found - 1]++;
        nb_qmi_result(&w, 0, 0);
        nb_qmi_u32(&w, 0x10, found);
        break;
    }
    case NB_RMTFS_CLOSE:
        nb_qmi_result(&w, 0, 0);
        break;
    case NB_RMTFS_RW_IOVEC: {
        const uint16_t error = iovec(r, &m);
        if (error) { r->rejects++; r->last_error = error; }
        nb_qmi_result(&w, error ? 1 : 0, error);
        break;
    }
    case NB_RMTFS_ALLOC_BUFF:
        v = nb_qmi_tlv(&m, 0x02, &size);
        if (!v || size < 4 || nb_le32(v) > r->shared_bytes) { r->rejects++; r->last_error = 1; nb_qmi_result(&w, 1, 1); break; }
        r->alloc_done = 1;
        nb_qmi_result(&w, 0, 0);
        nb_qmi_u64(&w, 0x10, r->shared_phys);
        break;
    case NB_RMTFS_GET_DEV_ERROR:
        nb_qmi_result(&w, 0, 0);
        nb_qmi_u8(&w, 0x10, 0);
        break;
    default:
        r->unknown++;
        return 0;
    }
    return nb_qmi_end(&w);
}
