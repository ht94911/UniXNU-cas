/* SPDX-License-Identifier: BSD-2-Clause */
/* Included after bb_push; all map and completion calls use the existing lock. */
static uint8_t *bb_nas_allocate(void *, uint32_t bytes)
{
    return static_cast<uint8_t *>(IOMallocData(bytes));
}
static void bb_nas_release(void *, uint8_t *data, uint32_t bytes)
{
    IOFreeData(data, bytes);
}
static void bb_nas_emit(void *, uint32_t, uint32_t channel, uint32_t cid,
                        const uint8_t *data, uint32_t bytes)
{
    cbq_frame_header(bb_out, bytes, NB_NAS_SERVICE, cid);
    memcpy(bb_out + CBQ_HDR, data, bytes);
    bb_push(channel ? CBH_QMUX2 : CBH_QMUX1, bb_out, CBQ_HDR + bytes);
}
static void bb_nas_reconcile(void)
{
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
        const auto *c = &bb_qmux.client[i];
        const auto *n = &bb_nas.client[i];
        if (n->used && (!c->used || c->service != NB_NAS_SERVICE
            || c->channel != n->channel || c->cid != n->cid
            || c->node != n->node || c->port != n->port)) nb_nas_retire(&bb_nas, i);
    }
}
static void bb_nas_request(const struct cbq_result &r, const uint8_t *sdu)
{
    const unsigned client = r.local_port - CBQ_PORT_BASE;
    nb_nas_attach(&bb_nas, client, r.channel, r.cid, r.node, r.port);
    const uint32_t error = nb_nas_submit(&bb_nas, client, sdu, r.len);
    if (error) nb_nas_emit_error(&bb_nas, client, cbq_rd16(sdu + 1), cbq_rd16(sdu + 3), error);
}
