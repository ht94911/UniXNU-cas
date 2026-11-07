/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* MCA wire format from the SM8850 pandora implementation; not classic
 * ShengBattmgr opcode0x30. See SOURCES.md for the stock/reconstruction boundary. */
#include "NezhaMca.h"
#include <string.h>

uint32_t np_le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void np_put32(uint8_t *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
void np_mca_init(NpMca *m) { memset(m, 0, sizeof(*m)); }
void np_mca_cancel(NpMca *m)
{ m->pending = 0; m->deadline_ms = 0; memset(m->reply, 0, sizeof(m->reply)); }
void np_mca_link(NpMca *m, int up)
{
    /* Never reset sequence on reconnect: a delayed packet has no wire epoch. */
    np_mca_cancel(m);
    if (++m->epoch == 0) { m->exhausted = 1; m->up = 0; return; }
    m->up = !!up;
}
int np_mca_begin(NpMca *m, uint32_t property, uint32_t opcode,
                 const void *data, uint32_t size, uint32_t reply_size, uint8_t out[NP_MCA_REQUEST])
{
    if (!m || !out || !m->up || m->pending || m->exhausted ||
        (opcode != NP_MCA_READ && opcode != NP_MCA_WRITE) ||
        size >= NP_MCA_DATA || reply_size >= NP_MCA_DATA || (size && !data) ||
        (opcode == NP_MCA_READ && size) || (opcode == NP_MCA_WRITE && !size)) return -1;
    if (m->sequence == UINT32_MAX) { m->exhausted = 1; return -1; }
    memset(out, 0, NP_MCA_REQUEST);
    np_put32(out, NP_MCA_OWNER); np_put32(out + 4, 1); np_put32(out + 8, opcode);
    np_put32(out + 12, property); np_put32(out + 16, ++m->sequence);
    if (size) memcpy(out + 20, data, size);
    m->property = property; m->opcode = opcode; m->reply_bytes = reply_size;
    m->retcode = UINT32_MAX; m->pending = 1; m->deadline_ms = 0;
    memset(m->reply, 0, sizeof(m->reply));
    return NP_MCA_REQUEST;
}
int np_mca_sent(NpMca *m, uint64_t now, uint32_t timeout)
{
    if (!m || !m->pending) return -1;
    if (!timeout || UINT64_MAX - now < timeout) { np_mca_cancel(m); ++m->errors; return -1; }
    m->sent_ms = now; m->deadline_ms = now + timeout; return 0;
}
int np_mca_receive(NpMca *m, uint64_t epoch, uint64_t now, const uint8_t *p, uint32_t size,
                   uint32_t *note, uint32_t *value)
{
    if (!m) return NP_MCA_ERROR;
    (void)np_mca_timeout(m, now);
    if (!p || size < 12 || !m->up || epoch != m->epoch || np_le32(p) != NP_MCA_OWNER) goto ignore;
    if (np_le32(p + 4) == 2) {
        /* The vendor notification opcode is the property-specific command;
         * it is NOT the read request opcode. Only the exact common envelope
         * is decoded here; the board validates each notification property. */
        if (size != NP_MCA_NOTIFICATION || !note || !value) goto ignore;
        *note = np_le32(p + 12); *value = np_le32(p + 16);
        return NP_MCA_NOTE;
    }
    if (size != NP_MCA_RESPONSE || np_le32(p + 4) != 1 || !m->pending ||
        !m->deadline_ms || now < m->sent_ms || now >= m->deadline_ms || np_le32(p + 8) != m->opcode || np_le32(p + 12) != m->property ||
        np_le32(p + 20) != m->sequence) goto ignore;
    m->retcode = np_le32(p + 16); m->pending = 0; m->deadline_ms = 0;
    if (m->retcode) { ++m->errors; return NP_MCA_ERROR; }
    memcpy(m->reply, p + 24, m->reply_bytes);
    return NP_MCA_REPLY;
ignore:
    ++m->ignored; return NP_MCA_IGNORE;
}
int np_mca_timeout(NpMca *m, uint64_t now)
{
    if (!m->pending || !m->deadline_ms || (now >= m->sent_ms && now < m->deadline_ms)) return 0;
    np_mca_cancel(m); ++m->timeouts; return 1;
}
