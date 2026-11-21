/* SPDX-License-Identifier: BSD-2-Clause */
/* Included after bb_push. The existing lock protects clients and this adapter;
 * only coachz_baseband_poll sends real LOC packets on the baseband worker. */
static int bb_gnss_emit(void *, uint32_t channel, uint32_t cid, const uint8_t *sdu, uint32_t length)
{
    if (channel >= CBQ_CHANNELS || length > sizeof bb_out - CBQ_HDR) return 0;
    cbq_frame_header(bb_out, length, NB_GNSS_PDS_SERVICE, cid);
    memcpy(bb_out + CBQ_HDR, sdu, length);
    const uint32_t index = channel ? CBH_QMUX2 : CBH_QMUX1;
    const uint32_t count = bb_ch[index].count;
    bb_push(index, bb_out, CBQ_HDR + length);
    return bb_ch[index].count > count;
}
static void bb_gnss_revoke(void)
{
    bb_qmux.local_pds = 0;
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
        struct cbq_client *c = &bb_qmux.client[i];
        if (c->used && c->local_service) {
            c->used = 0; c->revoke_pending = 1; nb_gnss_retire(&bb_gnss, i);
        }
    }
}
static void bb_gnss_reconcile(void)
{
    if (!bb_gnss_enabled || !bb_gnss_online) return;
    /* Prefer a real PDS endpoint if the modem ever provides one. */
    if (cbq_server_find(&bb_qmux, NB_GNSS_PDS_SERVICE)) {
        if (bb_gnss.phase != NbGnssAbsent) {
            if (bb_gnss.phase != NbGnssFailed && (bb_gnss.running || bb_gnss.waiting || bb_gnss.sending)) {
                /* End only our LOC session before handing new clients to a
                 * real PDS endpoint. The modem and peer clients keep running. */
                bb_gnss_revoke(); return;
            }
            bb_gnss_release = bb_gnss.local_port; bb_gnss_revoke(); nb_gnss_lost(&bb_gnss);
        }
        return;
    }
    const struct cbq_server *loc = cbq_server_find(&bb_qmux, NB_GNSS_LOC_SERVICE);
    if (!loc || loc->version != 2u || loc->instance != 0u) {
        if (bb_gnss.phase != NbGnssAbsent) {
            bb_gnss_release = bb_gnss.local_port; bb_gnss_revoke(); nb_gnss_lost(&bb_gnss);
        }
        return;
    }
    if (bb_gnss.phase == NbGnssAbsent && !bb_gnss_release && !bb_gnss_failed_closed) {
        /* A fresh local QRTR port separates old indications after reconnect.
         * Never reuse a port during one boot. NAS owns 0x4180..0x4181. */
        if (bb_gnss_epoch >= 256u) return;
        bb_gnss.local_port = BB_GNSS_PORT_BASE + bb_gnss_epoch++;
        bb_gnss_failed_closed = 0;
        nb_gnss_endpoint(&bb_gnss, loc->version, loc->node, loc->port);
    }
    if (bb_gnss.node != loc->node || bb_gnss.port != loc->port) {
        bb_gnss_release = bb_gnss.local_port; bb_gnss_revoke(); nb_gnss_lost(&bb_gnss); return;
    }
    /* Service discovery precedes Apple's client initialization. Defer its
     * PDS commands until the real LOC version/register replies arrive. */
    bb_qmux.local_pds = bb_gnss.phase != NbGnssAbsent && bb_gnss.phase != NbGnssFailed;
    if (bb_gnss.phase == NbGnssFailed) {
        bb_gnss_revoke();
        if (!bb_gnss_failed_closed) {
            bb_gnss_release = bb_gnss.local_port; bb_gnss_failed_closed = 1;
        }
        return;
    }
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
        const struct cbq_client *c = &bb_qmux.client[i];
        const NbGnssClient *g = &bb_gnss.clients[i];
        if (g->used && (!c->used || !c->local_service || c->channel != g->channel || c->cid != g->cid)) nb_gnss_retire(&bb_gnss, i);
        if (c->used && c->local_service) nb_gnss_attach(&bb_gnss, i, c->channel, c->cid);
    }
}
/* Read-only export through the existing baseband diagnostic provider. Its
 * cursor carries the endpoint generation and fix sequence, so individual
 * pages can never silently combine two location reports. */
extern "C" int nezha_gnss_diagnostic(uint64_t cursor, uint32_t page, char *out,
                                      size_t capacity, size_t *used, unsigned *truncated)
{
    if (!bb_lock || !bb_gnss_enabled) return 1;
    if (!IOLockTryLock(bb_lock)) return 2;
    NbGnss *s = &bb_gnss;
    const int sky_page = (page & 0x100u) != 0;
    const uint64_t sequence = sky_page ? s->sky.sequence : s->fix.sequence;
    const uint64_t identity = ((uint64_t)s->generation << 32) | (uint32_t)sequence;
    if (cursor && cursor != identity) { IOLockUnlock(bb_lock); return 6; }
    int n;
    if (!page) {
        n = snprintf(out, capacity,
            "{\"cursor\":%llu,\"generation\":%u,\"phase\":%u,\"session\":%u,\"running\":%u,\"loc_revision\":%u,"
            "\"sequence\":%llu,\"utc_ms\":%llu,\"received_ms\":%llu,\"valid\":%u,\"raw_valid\":%u,\"status\":%u,\"technology\":%u,"
            "\"lat_bits\":\"%016llx\",\"lon_bits\":\"%016llx\",\"accuracy_bits\":\"%08x\","
            "\"fix_deliveries\":%u,\"fix_id_present\":%u,\"fix_id\":%u,\"system_tick_present\":%u,\"system_tick\":%llu,"
            "\"used_present\":%u,\"signals_present\":%u,\"used_count\":%u,\"gps_used\":%u,\"bds_used\":%u,\"bds_signals\":%llu,"
            "\"positions\":%llu,\"published\":%llu,\"rejected\":%llu,\"stale\":%llu,\"errors\":%llu,\"drops\":%llu,\"last_error\":%u,"
            "\"loc_command\":%u,\"failed_command\":%u,\"waiting\":%u,\"ack_seen\":%u,\"indication_seen\":%u,\"commands\":%llu,\"sky_sequence\":%llu,\"bds_seen\":%u,"
            "\"reference_commands\":%llu,\"reference_completed\":%llu,\"reference_errors\":%llu,\"reference_status\":%u,"
            "\"measurement_reports\":%llu,\"measurement_published\":%llu,\"measurement_rejected\":%llu,"
            "\"measurement_configured\":%u,\"measurement_config_error\":%u,"
            "\"measurement_sequence\":%u,\"measurement_maximum\":%u,\"measurement_system\":%u,"
            "\"measurement_signals\":%llu,\"measurement_count\":%u,\"measurement_received_ms\":%llu,"
            "\"measurement_tick_present\":%u,\"measurement_tick\":%u,"
            "\"measurement_epochs\":%llu,\"measurement_empty_slots\":%llu,"
            "\"measurement_incomplete_epochs\":%llu,\"measurement_output_blocks\":%u}",
            (unsigned long long)identity, s->generation, (unsigned)s->phase, s->session, s->running, s->revision,
            (unsigned long long)s->fix.sequence, (unsigned long long)s->fix.utc_ms, (unsigned long long)s->fix.received_ms,
            s->fix.valid, s->fix.raw_valid, s->fix.status, s->fix.technology,
            (unsigned long long)nb_le64(s->fix.parsed + 27), (unsigned long long)nb_le64(s->fix.parsed + 35),
            nb_le32(s->fix.parsed + (s->fix.valid & NB_GNSS_CIRCULAR ? 63u : 67u)),
            s->fix.deliveries, s->fix.fix_id_present, s->fix.fix_id, s->fix.system_tick_present, (unsigned long long)s->fix.system_tick,
            s->fix.used_present, s->fix.signals_present, s->fix.used_count, s->fix.gps_used, s->fix.bds_used,
            (unsigned long long)s->fix.bds_signals, (unsigned long long)s->positions, (unsigned long long)s->published,
            (unsigned long long)s->rejected, (unsigned long long)s->stale, (unsigned long long)s->errors,
            (unsigned long long)s->output_drops, s->last_error, s->command, s->failed_command,
            s->waiting, s->ack_seen, s->indication_seen, (unsigned long long)s->commands,
            (unsigned long long)s->sky.sequence, s->sky.bds_seen,
            (unsigned long long)s->reference_commands, (unsigned long long)s->reference_completed,
            (unsigned long long)s->reference_errors, s->reference_status,
            (unsigned long long)s->measurement_reports, (unsigned long long)s->measurement_published,
            (unsigned long long)s->measurement_rejected, s->measurement_configured, s->measurement_config_error,
            s->measurement.sequence, s->measurement.maximum,
            s->measurement.system, (unsigned long long)s->measurement.signals, s->measurement.count,
            (unsigned long long)s->measurement_received_ms, s->measurement.tick_present, s->measurement.tick,
            (unsigned long long)s->measurement_epochs, (unsigned long long)s->measurement_empty_slots,
            (unsigned long long)s->measurement_incomplete_epochs, s->measurement_last_blocks);
    } else if (sky_page) {
        const uint32_t index = (page & 0xffu) - 1u;
        if (!(page & 0xffu)) {
            n = snprintf(out, capacity, "{\"cursor\":%llu,\"sequence\":%llu,\"received_ms\":%llu,\"count\":%u,\"bds_seen\":%u,\"signals_present\":%u,\"list_present\":%u,\"altitude_present\":%u,\"altitude_assumed\":%u,\"usage_evidence\":false}",
                (unsigned long long)identity, (unsigned long long)s->sky.sequence, (unsigned long long)s->sky.received_ms,
                s->sky.count, s->sky.bds_seen, s->sky.signals_present,
                s->sky.list_present, s->sky.altitude_present, s->sky.altitude_assumed);
        } else {
            if (!cursor || index >= s->sky.count) { IOLockUnlock(bb_lock); return 3; }
            const NbGnssSatellite *sv = &s->sky.sv[index];
            n = snprintf(out, capacity, "{\"cursor\":%llu,\"index\":%u,\"valid\":%u,\"system\":%u,\"svid\":%u,\"signals\":%llu,\"tracking\":%u,\"info\":%u,\"health\":%u,\"cn0_bits\":\"%08x\",\"elevation_bits\":\"%08x\",\"azimuth_bits\":\"%08x\"}",
                (unsigned long long)identity, index, sv->valid, sv->system, sv->id, (unsigned long long)sv->signals,
                sv->tracking, sv->info, sv->health, sv->cn0, sv->elevation, sv->azimuth);
        }
    } else {
        const uint32_t index = page - 1u;
        if (!cursor || index >= s->fix.used_count) { IOLockUnlock(bb_lock); return 3; }
        const NbGnssUsedSv *sv = &s->fix.used[index];
        n = snprintf(out, capacity, "{\"cursor\":%llu,\"index\":%u,\"system\":%u,\"svid\":%u,\"signals\":%llu,\"used_in_fix\":true}",
            (unsigned long long)identity, index, sv->system, sv->id, (unsigned long long)sv->signals);
    }
    IOLockUnlock(bb_lock);
    if (n < 0 || (size_t)n >= capacity) { *truncated = 1; return 4; }
    *used = (size_t)n; *truncated = 0; return 0;
}
static void bb_gnss_request(const struct cbq_result &r, const uint8_t *sdu)
{
    bb_gnss_reconcile();
    nb_gnss_request(&bb_gnss, r.local_port - CBQ_PORT_BASE, sdu, r.len);
}
