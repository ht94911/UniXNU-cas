/* SPDX-License-Identifier: BSD-3-Clause */
/* Protocol behaviour from linux-msm tqftpserv (tqftpserv.c, BSD-3-Clause, Copyright (c) 2018 Linaro Ltd.); see NbTftp.h. */
#include "NbTftp.h"

#define RO_PREFIX "/readonly/firmware/image/"
#define RW_PREFIX "/readwrite/"
#define MAX_RSIZE (100u * 1024u * 1024u)

static void put16be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t text_length(const char *s, uint32_t max) { uint32_t n = 0; while (n < max && s[n]) ++n; return n; }

/* Option names and the mode compare without case (RFC 1350/2347); paths compare exactly. */
static int same_word(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return !*a && !*b;
}
static int same_path(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static const char *after(const char *s, const char *prefix) { while (*prefix) if (*s++ != *prefix++) return 0; return s; }

/* Decimal value of a whole string; -1 when it is not one or exceeds max. */
static int64_t number(const char *s, uint64_t max)
{
    if (!*s) return -1;
    uint64_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10u + (uint64_t)(*s - '0');
        if (v > max) return -1;
    }
    return (int64_t)v;
}

/* A relative path without "." or ".." components, empty components or control bytes. */
static int clean_path(const char *p)
{
    if (!*p || *p == '/') return 0;
    for (const char *c = p; *c; ++c) {
        if ((uint8_t)*c < 0x20 || (uint8_t)*c == 0x7f) return 0;
        if (*c == '/' && (c[1] == '/' || !c[1])) return 0;
        if ((c == p || c[-1] == '/') && c[0] == '.' && (c[1] == '/' || !c[1] || (c[1] == '.' && (c[2] == '/' || !c[2]))))
            return 0;
    }
    return 1;
}

static int send_from(NbTftp *t, uint32_t from, uint32_t node, uint32_t port, uint32_t length)
{
    const int r = nb_qrtr_send_data(t->qrtr, from, node, port, t->packet, length);
    if (r) t->refused++;
    return r;
}

static void send_error_from(NbTftp *t, uint32_t from, uint32_t node, uint32_t port, uint32_t code, const char *text)
{
    uint8_t *p = t->packet;
    const uint32_t n = text_length(text, 64);
    put16be(p, NB_TFTP_ERROR);
    put16be(p + 2, code);
    for (uint32_t i = 0; i < n; ++i) p[4 + i] = (uint8_t)text[i];
    p[4 + n] = 0;
    if (!send_from(t, from, node, port, 5 + n)) t->errors_sent++;
}

static void end_session(NbTftpSession *s) { *s = (NbTftpSession){0}; }

static void fail(NbTftp *t, NbTftpSession *s, uint32_t code, const char *text)
{
    send_error_from(t, s->port, s->node, s->peer, code, text);
    end_session(s);
}

/* ---- OACK / ACK ---- */

static uint32_t put_option(uint8_t *p, uint32_t at, uint32_t cap, const char *name, uint64_t value)
{
    char digits[24];
    uint32_t n = 0;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value && n < sizeof digits);
    const uint32_t name_n = text_length(name, 16);
    if (!at || at > cap || cap - at < name_n + n + 2) return 0;
    for (uint32_t i = 0; i < name_n; ++i) p[at++] = (uint8_t)name[i];
    p[at++] = 0;
    while (n) p[at++] = (uint8_t)digits[--n];
    p[at++] = 0;
    return at;
}

/* tqftpserv tftp_send_oack order: blksize, timeoutms, tsize (when asked), wsize, rsize, seek (when set). */
static int send_oack(NbTftp *t, NbTftpSession *s)
{
    uint8_t *p = t->packet;
    const uint32_t cap = 4u + t->max_blksize;
    put16be(p, NB_TFTP_OACK);
    uint32_t at = put_option(p, 2, cap, "blksize", s->blksize);
    at = put_option(p, at, cap, "timeoutms", s->timeout_ms);
    if (s->tsize >= 0) at = put_option(p, at, cap, "tsize", (uint64_t)s->tsize);
    at = put_option(p, at, cap, "wsize", s->wsize);
    if (s->has_rsize) at = put_option(p, at, cap, "rsize", s->limit);
    if (s->has_seek) at = put_option(p, at, cap, "seek", s->seek);
    if (!at) return -1;
    return send_from(t, s->port, s->node, s->peer, at);
}

static int send_ack(NbTftp *t, NbTftpSession *s)
{
    put16be(t->packet, NB_TFTP_ACK);
    put16be(t->packet + 2, (uint32_t)(s->acked & 0xffffu));
    return send_from(t, s->port, s->node, s->peer, 4);
}

/* ---- the RAM store behind /readwrite ---- */

static int rw_find(NbTftp *t, const char *path)
{
    for (uint32_t i = 0; i < NB_TFTP_RW_FILES; ++i) if (t->rw[i].used && same_path(t->rw[i].path, path)) return (int)i;
    return -1;
}

static int rw_create(NbTftp *t, const char *path)
{
    int i = rw_find(t, path);
    if (i >= 0) { t->rw[i].bytes = 0; return i; }  /* tqftpserv opens with O_WRONLY|O_CREAT and rewrites from the start */
    for (uint32_t j = 0; j < NB_TFTP_RW_FILES; ++j) {
        if (t->rw[j].used) continue;
        const uint32_t n = text_length(path, NB_TFTP_PATH);
        if (n >= NB_TFTP_PATH) return -1;
        for (uint32_t c = 0; c <= n; ++c) t->rw[j].path[c] = c < n ? path[c] : 0;
        t->rw[j].used = 1;
        t->rw[j].bytes = 0;
        return (int)j;
    }
    return -1;
}

/* ---- reading: DATA windows ---- */

static int64_t read_block(NbTftp *t, NbTftpSession *s, uint64_t block, uint8_t *to)
{
    const uint64_t start = (block - 1) * s->blksize;
    if (start > s->limit) return 0;
    uint64_t want = s->limit - start;
    if (want > s->blksize) want = s->blksize;
    if (!want) return 0;
    const uint64_t at = s->seek + start;
    if (s->ram) {
        const uint32_t bytes = t->rw[s->handle].bytes;
        if (at > bytes || want > bytes - at) return -1;
        const uint8_t *from = t->rw_pool + (uint64_t)s->handle * NB_TFTP_RW_FILE_BYTES + at;
        for (uint64_t i = 0; i < want; ++i) to[i] = from[i];
        return (int64_t)want;
    }
    const int64_t got = t->store.read(t->store.context, s->handle, at, to, (uint32_t)want);
    return got == (int64_t)want ? got : -1;
}

/* Sends blocks next .. window_end (never past the last one). A refused send stays due for the next poll. */
static void send_window(NbTftp *t, NbTftpSession *s)
{
    while (s->used && !s->writing && s->next <= s->window_end && s->next <= s->last) {
        uint8_t *p = t->packet;
        const int64_t n = read_block(t, s, s->next, p + 4);
        if (n < 0) { fail(t, s, NB_TFTP_EUNDEF, "Read error"); return; }
        put16be(p, NB_TFTP_DATA);
        put16be(p + 2, (uint32_t)(s->next & 0xffffu));
        if (send_from(t, s->port, s->node, s->peer, 4u + (uint32_t)n)) return;
        t->bytes_out += (uint64_t)n;
        s->next++;
    }
}

/* The 16-bit block number from the client, nearest to base. */
static uint64_t unwrap(uint64_t base, uint32_t v)
{
    uint64_t c = (base & ~0xffffull) | v;
    if (c + 0x8000u < base) c += 0x10000u;
    else if (c > base + 0x8000u && c >= 0x10000u) c -= 0x10000u;
    return c;
}

static void reader_ack(NbTftp *t, NbTftpSession *s, uint32_t value)
{
    const uint64_t block = unwrap(s->acked, value);
    if (block < s->acked || block >= s->next) return;  /* stale, or a block we never sent */
    if (block == s->last) { end_session(s); t->reads++; return; }
    /* ACK n: blocks n+1 .. n+wsize. A repeated ACK resends that window. */
    s->acked = block;
    s->next = block + 1;
    s->window_end = block + s->wsize;
    send_window(t, s);
}

/* ---- requests ---- */

static NbTftpSession *new_session(NbTftp *t)
{
    for (uint32_t i = 0; i < NB_TFTP_SESSIONS; ++i) {
        if (t->session[i].used) continue;
        NbTftpSession *s = &t->session[i];
        *s = (NbTftpSession){0};
        s->used = 1;
        s->port = t->first_port + i;
        return s;
    }
    return 0;
}

static void request(NbTftp *t, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length, int write)
{
    /* filename, mode, then name/value option pairs, each NUL terminated inside the packet (tqftpserv parse_options). */
    const char *field[2 + 2 * 8];
    uint32_t count = 0, at = 2;
    while (at < length && count < sizeof field / sizeof field[0]) {
        uint32_t end = at;
        while (end < length && data[end]) ++end;
        if (end == length) { send_error_from(t, t->service_port, node, from, NB_TFTP_EBADOP, "Malformed packet"); return; }
        field[count++] = (const char *)data + at;
        at = end + 1;
    }
    if (count < 2 || (count & 1)) { send_error_from(t, t->service_port, node, from, NB_TFTP_EBADOP, "Malformed packet"); return; }
    if (!same_word(field[1], "octet")) {
        send_error_from(t, t->service_port, node, from, NB_TFTP_EBADOP, "Only octet mode supported");
        return;
    }
    const char *ro = after(field[0], RO_PREFIX), *rw = after(field[0], RW_PREFIX);
    const char *rel = ro ? ro : rw;
    if (!rel || !clean_path(rel) || text_length(field[0], NB_TFTP_PATH) >= NB_TFTP_PATH || (write && !rw)) {
        send_error_from(t, t->service_port, node, from, NB_TFTP_EACCESS, "Access violation");
        return;
    }
    NbTftpSession *s = new_session(t);
    if (!s) { send_error_from(t, t->service_port, node, from, NB_TFTP_EUNDEF, "Server busy"); return; }
    s->node = node;
    s->peer = from;
    s->writing = (uint8_t)write;
    s->blksize = 512;
    s->wsize = 1;
    s->timeout_ms = 1000;
    s->tsize = -1;
    s->idle_at = t->now + NB_TFTP_IDLE_MS;
    uint64_t rsize = 0, seek = 0, want_tsize = 0;
    int asked_tsize = 0;
    for (uint32_t i = 2; i < count; i += 2) {
        const char *name = field[i], *value = field[i + 1];
        int64_t v;
        if (same_word(name, "blksize")) { if ((v = number(value, 65464)) < 8) goto badopt; s->blksize = (uint32_t)v; }
        else if (same_word(name, "timeoutms")) { if ((v = number(value, 255000)) < 1) goto badopt; s->timeout_ms = (uint32_t)v; }
        else if (same_word(name, "tsize")) { if ((v = number(value, UINT64_MAX >> 1)) < 0) goto badopt; asked_tsize = 1; want_tsize = (uint64_t)v; }
        else if (same_word(name, "rsize")) { if ((v = number(value, MAX_RSIZE)) < 1) goto badopt; s->has_rsize = 1; rsize = (uint64_t)v; }
        else if (same_word(name, "wsize")) { if ((v = number(value, 65535)) < 1) goto badopt; s->wsize = (uint32_t)v; }
        else if (same_word(name, "seek")) { if ((v = number(value, UINT64_MAX >> 1)) < 0) goto badopt; s->has_seek = 1; seek = (uint64_t)v; }
        else continue;  /* unknown options are ignored */
        s->has_options = 1;
    }
    if (s->blksize > t->max_blksize) s->blksize = t->max_blksize;  /* the OACK carries the accepted size */
    if (write) {
        if (asked_tsize && want_tsize > NB_TFTP_RW_FILE_BYTES) { fail(t, s, NB_TFTP_ENOSPACE, "Disk full or write error"); return; }
        const int i = rw_create(t, rel);
        if (i < 0) { fail(t, s, NB_TFTP_ENOSPACE, "Disk full or write error"); return; }
        if (asked_tsize) s->tsize = (int64_t)want_tsize;
        s->ram = 1;
        s->handle = (uint32_t)i;
        s->acked = 0;
        s->oack_due = s->has_options;
        if (s->oack_due ? send_oack(t, s) : send_ack(t, s)) { if (!s->oack_due) s->next = 1; return; }  /* due on poll */
        s->oack_due = 0;
        return;
    }
    uint64_t size = 0;
    if (ro) {
        if (t->store.lookup(t->store.context, rel, &size, &s->handle)) {
            const uint32_t n = text_length(field[0], NB_TFTP_PATH - 1);
            for (uint32_t c = 0; c <= n; ++c) t->last_missing[c] = c < n ? field[0][c] : 0;
            t->not_found++;
            fail(t, s, NB_TFTP_ENOENT, "file not found");
            return;
        }
    } else {
        const int i = rw_find(t, rel);
        if (i < 0) { t->not_found++; fail(t, s, NB_TFTP_ENOENT, "file not found"); return; }
        s->ram = 1;
        s->handle = (uint32_t)i;
        size = t->rw[i].bytes;
    }
    s->size = size;
    s->seek = seek;
    if (asked_tsize) s->tsize = (int64_t)size;
    const uint64_t available = seek <= size ? size - seek : 0;
    if (s->has_rsize) {
        if (rsize > available) { fail(t, s, NB_TFTP_EUNDEF, "Read error"); return; }  /* tqftpserv refuses a short read */
        s->limit = rsize;
        s->last = (rsize + s->blksize - 1) / s->blksize;
    } else {
        s->limit = available;
        s->last = available / s->blksize + 1;  /* the final block is shorter than blksize, possibly empty */
    }
    if (s->has_options) {
        s->oack_due = 1;  /* the client answers with ACK 0, which opens the first window */
        s->next = 1;
        s->window_end = 0;
        if (!send_oack(t, s)) s->oack_due = 0;
        return;
    }
    s->next = 1;
    s->window_end = 1;
    send_window(t, s);
    return;
badopt:
    fail(t, s, NB_TFTP_EOPTNEG, "Option negotiation failed");
}

static void writer_data(NbTftp *t, NbTftpSession *s, const uint8_t *data, uint32_t length)
{
    const uint32_t payload = length - 4;
    const uint64_t block = unwrap(s->acked, be16(data + 2));
    if (block != s->acked + 1 || payload > s->blksize) {
        send_ack(t, s);  /* out of sequence: the client resends from our last ACK */
        return;
    }
    uint32_t *bytes = &t->rw[s->handle].bytes;
    if (payload > NB_TFTP_RW_FILE_BYTES - *bytes) { fail(t, s, NB_TFTP_ENOSPACE, "Disk full or write error"); return; }
    uint8_t *to = t->rw_pool + (uint64_t)s->handle * NB_TFTP_RW_FILE_BYTES + *bytes;
    for (uint32_t i = 0; i < payload; ++i) to[i] = data[4 + i];
    *bytes += payload;
    t->bytes_in += payload;
    s->acked = block;
    const int final = payload < s->blksize;
    if (final || block % s->wsize == 0) {
        if (send_ack(t, s)) { s->oack_due = 0; s->finished = (uint8_t)final; s->next = 1; return; }  /* ACK due on poll */
        if (final) { end_session(s); t->writes++; }
    }
}

int nb_tftp_session_packet(NbTftp *t, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    if (!t || port < t->first_port || port - t->first_port >= NB_TFTP_SESSIONS) return 0;
    NbTftpSession *s = &t->session[port - t->first_port];
    if (!s->used) return 1;
    if (node != s->node || from != s->peer) { t->spoofed++; return 1; }
    if (!data || length < 4) { fail(t, s, NB_TFTP_EBADOP, "Malformed packet"); return 1; }
    s->idle_at = t->now + NB_TFTP_IDLE_MS;
    const uint32_t op = be16(data);
    s->oack_due = 0;  /* anything from the client means our OACK arrived */
    if (op == NB_TFTP_ERROR) { end_session(s); return 1; }  /* code 9 is the normal end; any other ends it too */
    if (s->writing) {
        if (op != NB_TFTP_DATA) { fail(t, s, NB_TFTP_EBADOP, "Expected DATA opcode"); return 1; }
        writer_data(t, s, data, length);
    } else {
        if (op != NB_TFTP_ACK) { fail(t, s, NB_TFTP_EBADOP, "Expected ACK opcode"); return 1; }
        reader_ack(t, s, be16(data + 2));
    }
    return 1;
}

static void service_receive(void *context, NbQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data,
                            uint32_t length)
{
    (void)q; (void)port;
    NbTftp *t = context;
    if (!data || length < 4) return;
    const uint32_t op = be16(data);
    if (op == NB_TFTP_RRQ || op == NB_TFTP_WRQ) request(t, node, from, data, length, op == NB_TFTP_WRQ);
    else send_error_from(t, t->service_port, node, from, NB_TFTP_EBADOP, "Expected RRQ or WRQ");
}

int nb_tftp_init(NbTftp *t, NbQrtr *q, uint32_t service_port, uint32_t first_port, uint32_t max_blksize, const NbTftpStore *store,
                 uint8_t *packet, uint8_t *rw_pool)
{
    if (!t || !q || !store || !store->lookup || !store->read || !packet || !rw_pool || max_blksize < 512 || max_blksize > 65464 ||
        4u + max_blksize > NB_QRTR_MAX_PACKET - NB_QRTR_V1_HEADER || !first_port ||
        (service_port >= first_port && service_port - first_port < NB_TFTP_SESSIONS)) return -1;
    *t = (NbTftp){0};
    t->qrtr = q; t->service_port = service_port; t->first_port = first_port; t->max_blksize = max_blksize;
    t->store = *store; t->packet = packet; t->rw_pool = rw_pool;
    return nb_qrtr_add_service(q, NB_TFTP_SERVICE, NB_TFTP_INSTANCE, service_port, service_receive, t) ? -1 : 0;
}

void nb_tftp_client_gone(NbTftp *t, uint32_t node, uint32_t port)
{
    for (uint32_t i = 0; t && i < NB_TFTP_SESSIONS; ++i)
        if (t->session[i].used && t->session[i].node == node && (port == NB_QRTR_NODE_BROADCAST || t->session[i].peer == port))
            end_session(&t->session[i]);
}

void nb_tftp_poll(NbTftp *t, uint64_t now)
{
    if (!t) return;
    t->now = now;
    for (uint32_t i = 0; i < NB_TFTP_SESSIONS; ++i) {
        NbTftpSession *s = &t->session[i];
        if (!s->used) continue;
        if (now >= s->idle_at) { t->timeouts++; end_session(s); continue; }
        if (s->oack_due) { if (!send_oack(t, s)) s->oack_due = 0; continue; }
        if (s->writing) {
            if (s->next && !send_ack(t, s)) {  /* an ACK refused earlier */
                s->next = 0;
                if (s->finished) { end_session(s); t->writes++; }
            }
            continue;
        }
        send_window(t, s);
    }
}
