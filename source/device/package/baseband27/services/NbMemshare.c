/* SPDX-License-Identifier: BSD-2-Clause */
/* See NbMemshare.h for the wire format source and the behaviour followed. */
#include "NbMemshare.h"
#include "NbQmi.h"

#define PROC_MODEM 0u
#define OUT_BYTES 64u

const NbMemshareClient nb_nezha_memshare_clients[] = {
    {0, 0, NB_MEMSHARE_BOOT_TIME, "gps"},
    {1, 0x500000, NB_MEMSHARE_DYNAMIC, "diag"},
    {2, 0, 0, "ftm"},
    {5, 0x1000000, NB_MEMSHARE_SHARED, "qmc_dma"},
    {6, 0x400000, NB_MEMSHARE_SHARED, "qmc_cma"},
    {7, 0x100000, NB_MEMSHARE_SHARED, "ims_1"},
    {8, 0x100000, 0, "ims_2"},
};
const unsigned nb_nezha_memshare_client_count = sizeof nb_nezha_memshare_clients / sizeof nb_nezha_memshare_clients[0];

static int find(const NbMemshare *m, uint32_t id, uint32_t processor)
{
    if (processor != PROC_MODEM) return -1;
    for (uint32_t i = 0; i < m->count; ++i) if (m->client[i].id == id) return (int)i;
    return -1;
}

static uint32_t alloc_answer(NbMemshare *m, uint32_t i, uint16_t transaction, int ok, uint8_t *out)
{
    NbQmiWriter w;
    if (nb_qmi_begin(&w, out, OUT_BYTES, NB_QMI_RESPONSE, transaction, NB_MEMSHARE_ALLOC)) return 0;
    nb_qmi_result(&w, ok ? 0 : 1, ok ? NB_QMI_ERR_NONE : NB_QMI_ERR_NO_MEMORY);
    nb_qmi_u32(&w, 0x10, m->block[i].sequence);
    uint8_t *p = nb_qmi_put(&w, 0x11, 0, 13);
    if (p) { p[0] = 1; nb_put64(p + 1, m->block[i].physical); nb_put32(p + 9, m->block[i].bytes); }
    return nb_qmi_end(&w);
}

static void send(NbMemshare *m, uint32_t node, uint32_t from, const uint8_t *out, uint32_t length)
{
    if (length) nb_qrtr_send_data(m->qrtr, m->port, node, from, out, length);
}

/* Assign an allotted block to the modem: 0 done, 1 busy, < 0 failed. */
static int assign(NbMemshare *m, uint32_t i)
{
    if (m->block[i].assigned || !m->block[i].allotted || !m->block[i].bytes) return 0;
    const int r = m->ops.assign(m->ops.context, m->block[i].physical, m->block[i].bytes, 1,
                                (m->client[i].flags & NB_MEMSHARE_SHARED) != 0);
    if (!r) m->block[i].assigned = 1;
    return r;
}

static void handle_alloc(NbMemshare *m, const NbQmiMessage *q, uint32_t node, uint32_t from)
{
    uint8_t out[OUT_BYTES];
    uint32_t size = 0, want = 0, id = ~0u, processor = ~0u, sequence = 0;
    const uint8_t *v;
    if ((v = nb_qmi_tlv(q, 0x01, &size)) && size == 4) want = nb_le32(v);
    if ((v = nb_qmi_tlv(q, 0x02, &size)) && size == 4) id = nb_le32(v);
    if ((v = nb_qmi_tlv(q, 0x03, &size)) && size == 4) processor = nb_le32(v);
    if ((v = nb_qmi_tlv(q, 0x04, &size)) && size == 4) sequence = nb_le32(v);
    m->allocs++;
    const int i = find(m, id, processor);
    if (i < 0) {  /* msm_memshare answers an unknown client with its default failure */
        NbQmiWriter w;
        m->failures++;
        if (!nb_qmi_begin(&w, out, OUT_BYTES, NB_QMI_RESPONSE, q->transaction, NB_MEMSHARE_ALLOC)) {
            nb_qmi_result(&w, 1, NB_QMI_ERR_INTERNAL);
            send(m, node, from, out, nb_qmi_end(&w));
        }
        return;
    }
    int ok = 1;
    if (!m->block[i].allotted && want) {
        uint32_t bytes = want > m->client[i].bytes ? m->client[i].bytes : want;
        if (m->client[i].flags & NB_MEMSHARE_DYNAMIC) bytes = m->client[i].bytes;
        uint64_t physical = 0;
        if (!bytes || m->ops.alloc(m->ops.context, (uint32_t)i, bytes, &physical) || !physical || (physical & 4095)) ok = 0;
        else { m->block[i].allotted = 1; m->block[i].physical = physical; m->block[i].bytes = bytes; }
    }
    m->block[i].sequence = sequence;
    if (ok) {
        const int r = assign(m, (uint32_t)i);
        if (r > 0) {  /* answer once the assignment is done */
            m->block[i].pending = 1; m->block[i].node = node; m->block[i].from = from; m->block[i].transaction = q->transaction;
            return;
        }
        if (r < 0) ok = 0;
    }
    if (!ok) m->failures++;
    send(m, node, from, out, alloc_answer(m, (uint32_t)i, q->transaction, ok, out));
}

static void handle_free(NbMemshare *m, const NbQmiMessage *q, uint32_t node, uint32_t from)
{
    uint8_t out[OUT_BYTES];
    uint32_t size = 0, id = ~0u, processor = PROC_MODEM;
    const uint8_t *v;
    if ((v = nb_qmi_tlv(q, 0x10, &size)) && size == 4) id = nb_le32(v);
    if ((v = nb_qmi_tlv(q, 0x11, &size)) && size == 4) processor = nb_le32(v);
    m->frees++;
    const int i = find(m, id, processor);
    int ok = 0;
    if (i >= 0 && m->block[i].allotted && !(m->client[i].flags & NB_MEMSHARE_BOOT_TIME) && !m->block[i].pending) {
        int r = 0;
        if (m->block[i].assigned)
            r = m->ops.assign(m->ops.context, m->block[i].physical, m->block[i].bytes, 0,
                              (m->client[i].flags & NB_MEMSHARE_SHARED) != 0);
        if (!r) {
            m->block[i].assigned = 0;
            m->ops.release(m->ops.context, (uint32_t)i, m->block[i].physical, m->block[i].bytes);
            m->block[i].allotted = 0; m->block[i].physical = 0; m->block[i].bytes = 0;
            ok = 1;
        }
        /* A failed or busy return assignment keeps the block owned by the modem: never released while it may use it. */
    }
    if (!ok) m->failures++;
    NbQmiWriter w;
    if (nb_qmi_begin(&w, out, OUT_BYTES, NB_QMI_RESPONSE, q->transaction, NB_MEMSHARE_FREE)) return;
    nb_qmi_result(&w, ok ? 0 : 1, ok ? NB_QMI_ERR_NONE : NB_QMI_ERR_INTERNAL);
    send(m, node, from, out, nb_qmi_end(&w));
}

static void handle_query(NbMemshare *m, const NbQmiMessage *q, uint32_t node, uint32_t from)
{
    uint8_t out[OUT_BYTES];
    uint32_t size = 0, id = ~0u, processor = PROC_MODEM;
    const uint8_t *v;
    if ((v = nb_qmi_tlv(q, 0x01, &size)) && size == 4) id = nb_le32(v);
    if ((v = nb_qmi_tlv(q, 0x10, &size)) && size == 4) processor = nb_le32(v);
    m->queries++;
    const int i = find(m, id, processor);
    NbQmiWriter w;
    if (nb_qmi_begin(&w, out, OUT_BYTES, NB_QMI_RESPONSE, q->transaction, NB_MEMSHARE_QUERY)) return;
    if (i < 0) nb_qmi_result(&w, 1, NB_QMI_ERR_INTERNAL);
    else { nb_qmi_result(&w, 0, NB_QMI_ERR_NONE); nb_qmi_u32(&w, 0x10, m->client[i].bytes); }
    send(m, node, from, out, nb_qmi_end(&w));
}

static void receive(void *context, NbQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)q; (void)port;
    NbMemshare *m = context;
    NbQmiMessage msg;
    if (nb_qmi_parse(data, length, &msg) || msg.type != NB_QMI_REQUEST) return;
    switch (msg.message) {
    case NB_MEMSHARE_ALLOC: handle_alloc(m, &msg, node, from); break;
    case NB_MEMSHARE_FREE: handle_free(m, &msg, node, from); break;
    case NB_MEMSHARE_QUERY: handle_query(m, &msg, node, from); break;
    default: m->unknown++; break;
    }
}

int nb_memshare_init(NbMemshare *m, NbQrtr *q, uint32_t port, const NbMemshareClient *clients, uint32_t count,
                     const NbMemshareOps *ops)
{
    if (!m || !q || !clients || !count || count > NB_MEMSHARE_CLIENTS || !ops || !ops->alloc || !ops->release || !ops->assign)
        return -1;
    *m = (NbMemshare){0};
    m->qrtr = q; m->port = port; m->ops = *ops; m->client = clients; m->count = count;
    return nb_qrtr_add_service(q, NB_MEMSHARE_SERVICE, NB_MEMSHARE_INSTANCE, port, receive, m) ? -1 : 0;
}

void nb_memshare_poll(NbMemshare *m)
{
    uint8_t out[OUT_BYTES];
    for (uint32_t i = 0; m && i < m->count; ++i) {
        if (!m->block[i].pending) continue;
        const int r = assign(m, i);
        if (r > 0) continue;
        m->block[i].pending = 0;
        if (r < 0) m->failures++;
        send(m, m->block[i].node, m->block[i].from, out, alloc_answer(m, i, m->block[i].transaction, r == 0, out));
    }
}

void nb_memshare_modem_gone(NbMemshare *m)
{
    for (uint32_t i = 0; m && i < m->count; ++i) {
        m->block[i].pending = 0;
        /* Only after the modem is proven stopped (PAS shutdown) may a block come back to HLOS. */
        if (m->block[i].assigned &&
            !m->ops.assign(m->ops.context, m->block[i].physical, m->block[i].bytes, 0, (m->client[i].flags & NB_MEMSHARE_SHARED) != 0))
            m->block[i].assigned = 0;
    }
}
