/* SPDX-License-Identifier: BSD-2-Clause */
#include "NbGnss.h"
static int nb_gnss_wants_fix(const NbGnssClient *);

static NbGnssClient nb_gnss_defaults(uint32_t generation, uint32_t channel, uint32_t cid)
{
    NbGnssClient c;
    __builtin_memset(&c, 0, sizeof c);
    c.used = 1; c.generation = generation; c.channel = (uint8_t)channel; c.cid = (uint8_t)cid;
    /* This is the exact 24A446 standalone default: 1 s / 255 s / 400 m.
     * It is configuration, never a substituted accuracy in a position. */
    c.interval_seconds = 1; c.timeout_seconds = 255; c.accuracy_metres = 400;
    return c;
}
void nb_gnss_init(NbGnss *s, uint32_t local_port, void *context, NbGnssEmit emit, NbGnssFixReceived received)
{
    __builtin_memset(s, 0, sizeof *s);
    s->local_port = local_port; s->context = context; s->emit = emit; s->fix_received = received;
}
static int nb_gnss_current_owned(const NbGnss *s)
{
    const uint32_t i = s->current.client;
    return s->active && !s->cancelled && i < CBQ_MAX_CLIENTS && s->clients[i].used &&
        s->clients[i].generation == s->current.generation;
}
static void nb_gnss_emit(NbGnss *s, const NbGnssClient *c, const uint8_t *data, uint32_t length)
{
    if (length && (!s->emit || !s->emit(s->context, c->channel, c->cid, data, length))) ++s->output_drops;
}
static void nb_gnss_answer(NbGnss *s, const NbGnssClient *c, uint16_t transaction,
                            uint16_t message, uint32_t error, uint8_t tag, const void *value, uint32_t length)
{
    uint8_t out[64]; NbQmiWriter w;
    nb_qmi_begin(&w, out, sizeof out, NB_QMI_RESPONSE, transaction, message);
    nb_qmi_result(&w, error ? 1 : 0, (uint16_t)error);
    if (value && !error) nb_qmi_put(&w, tag, value, length);
    nb_gnss_emit(s, c, out, nb_qmi_end(&w));
}
static void nb_gnss_finish(NbGnss *s, uint32_t error)
{
    if (nb_gnss_current_owned(s)) {
        NbGnssClient *c = &s->clients[s->current.client];
        if (!error) {
            s->current.after.last_utc_ms = s->current.change & NbGnssChangeReset ? 0 : c->last_utc_ms;
            *c = s->current.after;
        }
        nb_gnss_answer(s, c, s->current.transaction, s->current.message, error, 0, 0, 0);
        if (!error && (s->current.change & (NbGnssChangeState | NbGnssChangeAuto | NbGnssChangeForce | NbGnssChangeReset))) {
            uint8_t out[16], state[2] = {c->enabled, (uint8_t)(nb_gnss_wants_fix(c) && s->running == 1u ? 2u : 1u)};
            NbQmiWriter w; nb_qmi_begin(&w, out, sizeof out, NB_QMI_INDICATION, 0, NB_PDS_GET_STATE);
            nb_qmi_put(&w, 1, state, sizeof state); nb_gnss_emit(s, c, out, nb_qmi_end(&w));
        }
    }
    if (error) { s->last_error = error; ++s->errors; s->inhibited = 1; }
    s->active = 0; s->cancelled = 0; s->action = NbGnssIdle;
}
static void nb_gnss_pending_error(NbGnss *s, uint32_t error)
{
    while (s->count) {
        const NbGnssRequest *r = &s->requests[s->head];
        if (r->client < CBQ_MAX_CLIENTS && s->clients[r->client].used && s->clients[r->client].generation == r->generation)
            nb_gnss_answer(s, &s->clients[r->client], r->transaction, r->message, error, 0, 0, 0);
        s->head = (s->head + 1u) % NB_GNSS_REQUESTS; --s->count;
    }
}
void nb_gnss_retire(NbGnss *s, uint32_t i)
{
    if (i >= CBQ_MAX_CLIENTS) return;
    s->clients[i].used = 0;
    if (s->active && s->current.client == i) s->cancelled = 1;
    /* Removed queued requests are discarded when consumed. The next worker
     * poll recomputes the remaining users and stops our session if necessary. */
    s->inhibited = 0;
}
void nb_gnss_attach(NbGnss *s, uint32_t i, uint32_t channel, uint32_t cid)
{
    if (i >= CBQ_MAX_CLIENTS || channel >= CBQ_CHANNELS || !cid || cid >= 255u) return;
    NbGnssClient *c = &s->clients[i];
    if (c->used && c->channel == channel && c->cid == cid) return;
    nb_gnss_retire(s, i);
    *c = nb_gnss_defaults(c->generation + 1u, channel, cid);
}
void nb_gnss_lost(NbGnss *s)
{
    ++s->generation;
    if (s->active) nb_gnss_finish(s, NB_QMI_ERR_INTERNAL);
    nb_gnss_pending_error(s, NB_QMI_ERR_INTERNAL);
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) nb_gnss_retire(s, i);
    s->phase = NbGnssAbsent; s->running = 0; s->active = 0; s->cancelled = 0;
    s->head = s->count = 0; s->waiting = s->sending = 0;
    s->node = s->port = s->revision = 0; s->action = NbGnssIdle; s->inhibited = 0;
    s->fault_cleanup = 0;
    s->last_fix_utc_ms = 0;
    s->anchor_utc_ms = s->anchor_received_ms = 0;
    s->next_transaction = 0; s->next_session = 0;
    s->command = s->failed_command = 0;
    __builtin_memset(&s->fix, 0, sizeof s->fix); __builtin_memset(&s->sky, 0, sizeof s->sky);
    __builtin_memset(&s->incoming, 0, sizeof s->incoming);
    __builtin_memset(s->me_version, 0, sizeof s->me_version);
    __builtin_memset(s->host_version, 0, sizeof s->host_version);
    __builtin_memset(s->full_version, 0, sizeof s->full_version);
}
void nb_gnss_endpoint(NbGnss *s, uint32_t version, uint32_t node, uint32_t port)
{
    if (version != 2u || !port) return;
    if (s->phase != NbGnssAbsent && s->node == node && s->port == port) return;
    nb_gnss_lost(s); s->node = node; s->port = port; s->phase = NbGnssQuery;
}

static int nb_gnss_wants_fix(const NbGnssClient *c)
{
    return c->used && c->enabled && c->automatic && !c->forced_off && !c->failed;
}
static NbGnssCriteria nb_gnss_criteria(const NbGnss *s, int proposed)
{
    NbGnssCriteria result = {0, 0};
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
        const NbGnssClient *c = proposed && nb_gnss_current_owned(s) && s->current.client == i ?
                                &s->current.after : &s->clients[i];
        if (!nb_gnss_wants_fix(c)) continue;
        if (!result.interval_seconds || c->interval_seconds < result.interval_seconds) result.interval_seconds = c->interval_seconds;
        if (c->timeout_seconds > result.timeout_seconds) result.timeout_seconds = c->timeout_seconds;
    }
    return result;
}
static int nb_gnss_same_criteria(NbGnssCriteria a, NbGnssCriteria b)
{
    return a.interval_seconds == b.interval_seconds && a.timeout_seconds == b.timeout_seconds;
}
static int nb_gnss_duplicate(const NbGnss *s, uint32_t i, uint16_t transaction)
{
    if (s->active && s->current.client == i && !s->cancelled && s->current.transaction == transaction) return 1;
    for (unsigned n = 0; n < s->count; ++n) {
        const NbGnssRequest *r = &s->requests[(s->head + n) % NB_GNSS_REQUESTS];
        if (r->client == i && r->generation == s->clients[i].generation && r->transaction == transaction) return 1;
    }
    return 0;
}
static void nb_gnss_query(NbGnss *s, const NbGnssClient *c, uint16_t transaction, uint16_t message)
{
    if (message == NB_PDS_GET_STATE) {
        const uint8_t value[] = {c->enabled, (uint8_t)(nb_gnss_wants_fix(c) && s->running == 1u ? 2u : 1u)};
        nb_gnss_answer(s, c, transaction, message, 0, 1, value, sizeof value);
    } else if (message == NB_PDS_GET_AUTO) {
        nb_gnss_answer(s, c, transaction, message, 0, 1, &c->automatic, 1);
    } else {
        uint8_t value[10] = {c->mode, c->timeout_seconds};
        nb_put32(value + 2, c->interval_seconds); nb_put32(value + 6, c->accuracy_metres);
        nb_gnss_answer(s, c, transaction, message, 0, 1, value, sizeof value);
    }
}
void nb_gnss_request(NbGnss *s, uint32_t i, const uint8_t *data, uint32_t length)
{
    if (i >= CBQ_MAX_CLIENTS || !s->clients[i].used) return;
    NbQmiMessage m;
    if (!nb_gnss_message(data, length, &m) || m.type != NB_QMI_REQUEST) {
        ++s->rejected; return;
    }
    NbGnssClient *c = &s->clients[i];
    if (s->phase == NbGnssAbsent || s->phase == NbGnssFailed) {
        nb_gnss_answer(s, c, m.transaction, m.message, NB_QMI_ERR_INTERNAL, 0, 0, 0); return;
    }
    if ((m.message == NB_PDS_GET_STATE || m.message == NB_PDS_GET_AUTO || m.message == NB_PDS_GET_TRACKING) && m.length) {
        nb_gnss_answer(s, c, m.transaction, m.message, NB_QMI_ERR_INVALID_ARG, 0, 0, 0); return;
    }
    if (nb_gnss_duplicate(s, i, m.transaction) || s->count == NB_GNSS_REQUESTS) {
        nb_gnss_answer(s, c, m.transaction, m.message, NB_GNSS_ERROR_BUSY, 0, 0, 0); return;
    }
    NbGnssClient after = *c; uint32_t error = 0; uint8_t change = 0;
    const uint8_t *p;
    switch (m.message) {
    case NB_PDS_GET_STATE: case NB_PDS_GET_AUTO: case NB_PDS_GET_TRACKING:
        /* Preserve request ordering during asynchronous LOC initialization
         * and when a query follows an in-flight state change. */
        break;
    case NB_PDS_RESET:
        if (m.length) error = NB_QMI_ERR_INVALID_ARG;
        else { after = nb_gnss_defaults(c->generation, c->channel, c->cid); change = NbGnssChangeReset; }
        break;
    case NB_PDS_REPORT:
        for (uint32_t at = 0; at < m.length;) {
            const uint8_t type = m.tlvs[at]; const uint32_t n = nb_le16(m.tlvs + at + 1);
            const uint8_t value = n ? m.tlvs[at + 3] : 2;
            if (n != 1u || value > 1u) { error = NB_QMI_ERR_INVALID_ARG; break; }
            if (type == 0x12) { after.parsed_report = value; change |= NbGnssChangeParsed; }
            else if (type == 0x10) { after.nmea_report = value; change |= NbGnssChangeNmea; }
            else if (value) { error = NB_QMI_ERR_NOT_SUPPORTED; break; }
            /* Disabling a report that this provider never emits is effective. */
            at += 3u + n;
        }
        break;
    case NB_PDS_SET_TRACKING:
        p = nb_gnss_tlv(&m, 1, 10);
        if (!p || m.length != 13u || !p[1] || !nb_le32(p + 2) || nb_le32(p + 2) > UINT32_MAX / 1000u || !nb_le32(p + 6)) error = NB_QMI_ERR_INVALID_ARG;
        else if (p[0]) error = NB_QMI_ERR_NOT_SUPPORTED; /* standalone only; no unimplemented SUPL success */
        else { after.mode = p[0]; after.timeout_seconds = p[1]; after.interval_seconds = nb_le32(p + 2); after.accuracy_metres = nb_le32(p + 6); change = NbGnssChangeTracking; }
        break;
    case NB_PDS_SET_STATE: case NB_PDS_SET_AUTO: case NB_PDS_FORCE_OFF:
        p = nb_gnss_tlv(&m, 1, 1);
        if (!p || m.length != 4u || *p > 1u) error = NB_QMI_ERR_INVALID_ARG;
        else if (m.message == NB_PDS_SET_STATE) { after.enabled = *p; change = NbGnssChangeState; }
        else if (m.message == NB_PDS_SET_AUTO) { after.automatic = *p; change = NbGnssChangeAuto; }
        else { after.forced_off = *p; change = NbGnssChangeForce; }
        break;
    default: error = NB_QMI_ERR_NOT_SUPPORTED; break;
    }
    if (error) { nb_gnss_answer(s, c, m.transaction, m.message, error, 0, 0, 0); return; }
    s->requests[(s->head + s->count++) % NB_GNSS_REQUESTS] = (NbGnssRequest){i, c->generation, m.transaction, m.message, change, after};
    s->inhibited = 0;
}

static void nb_gnss_fail_command(NbGnss *s, uint32_t error)
{
    s->waiting = s->sending = 0; s->last_error = error; s->failed_command = s->command;
    if (s->phase != NbGnssReady || s->action == NbGnssStop) s->phase = NbGnssFailed;
    if (s->active) nb_gnss_finish(s, error);
    else { ++s->errors; s->inhibited = 1; }
    if (s->phase == NbGnssFailed) nb_gnss_pending_error(s, error);
}
static void nb_gnss_command_done(NbGnss *s)
{
    s->waiting = 0;
    /* The stock minor177 client opens the service and registers events
     * directly. INFORM_CLIENT_REVISION is an optional legacy request;
     * making its acceptance mandatory prevented GNSS002 initialization. */
    if (s->phase == NbGnssQuery) { s->phase = NbGnssRegister; return; }
    if (s->phase == NbGnssInform) { s->phase = NbGnssRegister; return; }
    if (s->phase == NbGnssRegister) { s->phase = NbGnssReady; return; }
    switch (s->action) {
    case NbGnssStop:
        s->running = 0; ++s->stops; s->action = NbGnssIdle; s->fault_cleanup = 0;
        __builtin_memset(&s->fix, 0, sizeof s->fix);
        break;
    case NbGnssMode: s->action = NbGnssStart; break;
    case NbGnssStart:
        s->running = 1; s->applied = s->desired; ++s->starts;
        s->action = NbGnssIdle;
        if (s->active) nb_gnss_finish(s, 0);
        break;
    default: break;
    }
}
int nb_gnss_next(NbGnss *s, uint64_t now_ms, NbGnssPacket *packet)
{
    (void)now_ms;
    if (s->phase == NbGnssAbsent || s->phase == NbGnssFailed || s->waiting || s->sending) return 0;
    uint16_t command;
    if (s->fault_cleanup) { s->action = NbGnssStop; command = NB_LOC_STOP; }
    else if (s->phase == NbGnssQuery) command = NB_LOC_SERVICE_REVISION;
    else if (s->phase == NbGnssInform) command = NB_LOC_REVISION;
    else if (s->phase == NbGnssRegister) command = NB_LOC_REGISTER;
    else {
        if (!s->active) while (s->count) {
            s->current = s->requests[s->head]; s->head = (s->head + 1u) % NB_GNSS_REQUESTS; --s->count;
            s->active = 1; s->cancelled = 0;
            if (nb_gnss_current_owned(s)) {
                const NbGnssClient *c = &s->clients[s->current.client];
                if (s->current.message == NB_PDS_GET_STATE || s->current.message == NB_PDS_GET_AUTO || s->current.message == NB_PDS_GET_TRACKING) {
                    nb_gnss_query(s, c, s->current.transaction, s->current.message); s->active = 0; return 0;
                }
                const NbGnssClient supplied = s->current.after;
                const uint8_t change = s->current.change;
                s->current.after = change & NbGnssChangeReset ? nb_gnss_defaults(c->generation, c->channel, c->cid) : *c;
                s->current.after.ready_notified = c->ready_notified;
                NbGnssClient *after = &s->current.after;
                if (change & NbGnssChangeState) after->enabled = supplied.enabled;
                if (change & NbGnssChangeAuto) after->automatic = supplied.automatic;
                if (change & NbGnssChangeForce) after->forced_off = supplied.forced_off;
                if (change & NbGnssChangeParsed) after->parsed_report = supplied.parsed_report;
                if (change & NbGnssChangeNmea) after->nmea_report = supplied.nmea_report;
                if (change & NbGnssChangeTracking) {
                    after->mode = supplied.mode; after->timeout_seconds = supplied.timeout_seconds;
                    after->interval_seconds = supplied.interval_seconds; after->accuracy_metres = supplied.accuracy_metres;
                }
                if (change & (NbGnssChangeAuto | NbGnssChangeState | NbGnssChangeForce | NbGnssChangeReset)) after->failed = 0;
                break;
            }
            s->active = 0;
        }
        s->desired = nb_gnss_criteria(s, 1);
        if (s->cancelled && !s->waiting) { s->active = 0; s->cancelled = 0; }
        if (s->inhibited && !s->active && s->running != 2u) return 0;
        if (s->action == NbGnssIdle) {
            if (s->running && (s->running == 2u || !s->desired.interval_seconds || !nb_gnss_same_criteria(s->desired, s->applied))) s->action = NbGnssStop;
            else if (!s->running && s->desired.interval_seconds) s->action = NbGnssMode;
            else { if (s->active) nb_gnss_finish(s, 0); return 0; }
        }
        if (s->action == NbGnssStart && !s->desired.interval_seconds) {
            s->action = NbGnssIdle; if (s->active) nb_gnss_finish(s, 0); return 0;
        }
        command = s->action == NbGnssStop ? NB_LOC_STOP : s->action == NbGnssMode ? NB_LOC_SET_MODE : NB_LOC_START;
    }
    /* Neither wire transaction nor session id is reused on this live LOC
     * port. Reconnection creates a new source port in the bridge wrapper. */
    if (s->next_transaction == UINT16_MAX || (command == NB_LOC_START && s->next_session == UINT8_MAX)) {
        nb_gnss_fail_command(s, NB_QMI_ERR_INTERNAL); s->phase = NbGnssFailed; return 0;
    }
    __builtin_memset(packet, 0, sizeof *packet);
    packet->generation = s->generation; packet->node = s->node; packet->port = s->port; packet->local_port = s->local_port;
    packet->transaction = ++s->next_transaction; packet->message = command;
    NbQmiWriter w;
    nb_qmi_begin(&w, packet->data, sizeof packet->data, NB_QMI_REQUEST, packet->transaction, command);
    switch (command) {
    case NB_LOC_REVISION: nb_qmi_u32(&w, 1, s->revision < NB_GNSS_LOC_REVISION ? s->revision : NB_GNSS_LOC_REVISION); break;
    case NB_LOC_REGISTER:
        /* Position, satellites, NMEA, engine, session. Normal subscription
         * only; NV, constellation enable masks and factory data are untouched. */
        nb_qmi_u64(&w, 1, UINT64_C(0x187));
        /* Original LOC IDL and the actual minor177 HAL supply client
         * metadata. AFW identifies the normal application framework;
         * requests remain subject to its ordinary location authorization. */
        /* QMI_LOC_MAX_CLIENT_ID_STRING_LENGTH is four on both the
         * inspected IDL and this phone's minor177 service. */
        nb_qmi_put(&w, 0x10, "HAL", 3);
        nb_qmi_u32(&w, 0x11, 1); /* QMI_LOC_CLIENT_AFW */
        /* locClientRegisterEventMask writes valid=1/value=0 on minor177.
         * A nonzero value is ignored for AFW in the public IDL, but this
         * board's real service rejected the former request. Match its HAL. */
        nb_qmi_u8(&w, 0x12, 0);
        break;
    case NB_LOC_SET_MODE: nb_qmi_u32(&w, 1, 4); break; /* QMI LOC standalone */
    case NB_LOC_START:
        s->session = ++s->next_session;
        nb_qmi_u8(&w, 1, s->session); nb_qmi_u32(&w, 0x10, 1); /* periodic */
        nb_qmi_u32(&w, 0x11, 3); /* QMI LOC ACCURACY_HIGH */
        nb_qmi_u32(&w, 0x12, 2); /* QMI LOC INTERMEDIATE_REPORTS_OFF */
        nb_qmi_u32(&w, 0x13, s->desired.interval_seconds * 1000u);
        nb_qmi_u32(&w, 0x17, (uint32_t)s->desired.timeout_seconds * 1000u);
        break;
    case NB_LOC_STOP: nb_qmi_u8(&w, 1, s->session); break;
    default: break;
    }
    packet->length = nb_qmi_end(&w);
    if (!packet->length) { nb_gnss_fail_command(s, NB_QMI_ERR_INTERNAL); return 0; }
    s->command = command; s->transaction = packet->transaction;
    s->sending = 1; s->ack_seen = s->indication_seen = 0;
    return 1;
}
void nb_gnss_sent(NbGnss *s, const NbGnssPacket *packet, int result, uint64_t now_ms)
{
    if (packet->generation != s->generation || packet->transaction != s->transaction || !s->sending) return;
    s->sending = 0;
    if (result < 0) { nb_gnss_fail_command(s, NB_QMI_ERR_INTERNAL); return; }
    s->waiting = 1; s->sent_ms = now_ms; ++s->commands;
}

static void nb_gnss_version(char *out, uint32_t room, const NbQmiMessage *m, uint8_t tag)
{
    uint32_t bytes = 0; const uint8_t *p = nb_qmi_tlv(m, tag, &bytes);
    if (!p || bytes >= room) return;
    __builtin_memcpy(out, p, bytes); out[bytes] = 0;
}
static void nb_gnss_publish_fix(NbGnss *s, const NbQmiMessage *m, uint64_t now_ms)
{
    ++s->positions;
    if (s->running != 1u || !nb_gnss_decode_fix(m, now_ms, &s->incoming) || s->incoming.session != s->session) { ++s->rejected; return; }
    NbGnssFix *fix = &s->incoming;
    fix->sequence = s->positions;
    if (!nb_gnss_fix_usable(fix)) { ++s->rejected; return; }
    if (fix->utc_ms <= s->last_fix_utc_ms) { ++s->stale; return; }
    /* Compare the receiver's UTC advance with host monotonic advance. This
     * catches delayed bursts without borrowing a possibly unsynced OS wall
     * clock. The first fix relies on the current unique LOC session. */
    if (s->anchor_utc_ms && now_ms >= s->anchor_received_ms) {
        const uint64_t utc_advance = fix->utc_ms - s->anchor_utc_ms;
        const uint64_t host_advance = now_ms - s->anchor_received_ms;
        if (host_advance > utc_advance && host_advance - utc_advance > NB_GNSS_FIX_MAX_DELAY_MS) { ++s->stale; return; }
        if (utc_advance > host_advance) {
            s->anchor_utc_ms = fix->utc_ms; s->anchor_received_ms = now_ms;
        }
    } else { s->anchor_utc_ms = fix->utc_ms; s->anchor_received_ms = now_ms; }
    s->fix = *fix;
    uint8_t out[128]; const uint32_t length = nb_gnss_pds_fix(&s->fix, out, sizeof out);
    if (!length) { ++s->rejected; return; }
    s->last_fix_utc_ms = s->fix.utc_ms; ++s->valid_fixes;
    if (s->fix_received) s->fix_received(s->context, &s->fix);
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
        NbGnssClient *c = &s->clients[i];
        if (!nb_gnss_wants_fix(c) || !c->parsed_report) continue;
        if (c->last_utc_ms && (s->fix.utc_ms <= c->last_utc_ms || s->fix.utc_ms - c->last_utc_ms < (uint64_t)c->interval_seconds * 1000u)) continue;
        const unsigned offset = s->fix.valid & NB_GNSS_CIRCULAR ? 63u : 67u;
        uint32_t accuracy;
        if (!nb_gnss_uncertainty(nb_le32(s->fix.parsed + offset), &accuracy) || accuracy > c->accuracy_metres) continue;
        const uint64_t dropped = s->output_drops;
        nb_gnss_emit(s, c, out, length);
        if (s->output_drops == dropped) { c->last_utc_ms = s->fix.utc_ms; ++s->published; ++s->fix.deliveries; }
    }
}
static void nb_gnss_publish_nmea(NbGnss *s, const NbQmiMessage *m)
{
    uint32_t bytes = 0; const uint8_t *p = nb_qmi_tlv(m, 1, &bytes);
    if (s->running != 1u || !p || !bytes || bytes > 200u || p[0] != '$') return;
    uint8_t out[216]; NbQmiWriter w;
    nb_qmi_begin(&w, out, sizeof out, NB_QMI_INDICATION, 0, NB_PDS_REPORT);
    uint8_t *value = nb_qmi_put(&w, 0x10, 0, bytes + 1u);
    if (!value) return;
    value[0] = 0; /* PDS NMEA sequence starts with standalone operation mode. */
    __builtin_memcpy(value + 1, p, bytes);
    const uint32_t length = nb_qmi_end(&w);
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i)
        if (nb_gnss_wants_fix(&s->clients[i]) && s->clients[i].nmea_report) nb_gnss_emit(s, &s->clients[i], out, length);
}
void nb_gnss_receive(NbGnss *s, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length, uint64_t now_ms)
{
    NbQmiMessage m;
    if (s->phase == NbGnssAbsent || s->phase == NbGnssFailed || node != s->node || port != s->port ||
        !nb_gnss_message(data, length, &m)) { ++s->rejected; return; }
    if (m.type == NB_QMI_RESPONSE) {
        if (!s->waiting || m.transaction != s->transaction || m.message != s->command) { ++s->rejected; return; }
        const uint32_t error = nb_gnss_result(&m);
        if (error) { nb_gnss_fail_command(s, error); return; }
        s->ack_seen = 1;
        if ((s->command != NB_LOC_SET_MODE && s->command != NB_LOC_SERVICE_REVISION) || s->indication_seen) nb_gnss_command_done(s);
        return;
    }
    if (m.type != NB_QMI_INDICATION) { ++s->rejected; return; }
    if (s->waiting && m.message == s->command && (m.message == NB_LOC_SET_MODE || m.message == NB_LOC_SERVICE_REVISION)) {
        const uint8_t *status = nb_gnss_tlv(&m, 1, 4);
        if (!status || nb_le32(status)) { nb_gnss_fail_command(s, NB_QMI_ERR_INTERNAL); return; }
        if (m.message == NB_LOC_SERVICE_REVISION) {
            const uint8_t *revision = nb_gnss_tlv(&m, 2, 4);
            if (!revision || !nb_le32(revision)) { nb_gnss_fail_command(s, NB_QMI_ERR_MALFORMED_MSG); return; }
            s->revision = nb_le32(revision);
            nb_gnss_version(s->me_version, sizeof s->me_version, &m, 0x10);
            nb_gnss_version(s->host_version, sizeof s->host_version, &m, 0x11);
            nb_gnss_version(s->full_version, sizeof s->full_version, &m, 0x12);
        }
        s->indication_seen = 1; if (s->ack_seen) nb_gnss_command_done(s); return;
    }
    if (s->phase != NbGnssReady) return;
    if (m.message == NB_LOC_POSITION) nb_gnss_publish_fix(s, &m, now_ms);
    else if (m.message == NB_LOC_SATELLITES) {
        if (nb_gnss_decode_sky(&m, now_ms, &s->sky)) { s->sky.sequence = ++s->sky_reports; }
        else ++s->rejected;
    } else if (m.message == NB_LOC_NMEA) nb_gnss_publish_nmea(s, &m);
    else if (m.message == NB_LOC_SESSION) {
        const uint8_t *state = nb_gnss_tlv(&m, 1, 4), *id = nb_gnss_tlv(&m, 0x10, 1);
        if (state && id && *id == s->session && nb_le32(state) == 2u && s->running == 1u) {
            s->running = 0; s->inhibited = 1; /* no automatic replay of an ended request */
            for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i)
                if (nb_gnss_wants_fix(&s->clients[i])) s->clients[i].failed = 1;
        }
    }
}
void nb_gnss_poll(NbGnss *s, uint64_t now_ms)
{
    if (s->waiting && now_ms >= s->sent_ms && now_ms - s->sent_ms >= NB_GNSS_WAIT_MS) {
        /* START may have executed despite a missing reply. One STOP of that
         * same owned session resolves it; a failed STOP disables the provider. */
        if (s->command == NB_LOC_START || s->command == NB_LOC_STOP) {
            s->running = 2; s->fault_cleanup = 1;
            for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i)
                if (nb_gnss_wants_fix(&s->clients[i])) s->clients[i].failed = 1;
        }
        nb_gnss_fail_command(s, NB_GNSS_ERROR_TIMEOUT);
    }
    if (s->phase == NbGnssReady) {
        uint8_t out[NB_QMI_HEADER]; NbQmiWriter w;
        nb_qmi_begin(&w, out, sizeof out, NB_QMI_INDICATION, 0, NB_PDS_READY);
        const uint32_t length = nb_qmi_end(&w);
        for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
            NbGnssClient *c = &s->clients[i];
            if (c->used && !c->ready_notified) {
                const uint64_t dropped = s->output_drops;
                nb_gnss_emit(s, c, out, length);
                c->ready_notified = s->output_drops == dropped;
            }
        }
    }
}
