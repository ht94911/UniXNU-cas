/* SPDX-License-Identifier: GPL-2.0-only */
/* QRTR and WLFW wire ABI from net/qrtr and Qualcomm's matching
 * platform/cnss_utils/wlan_firmware_service_v01.{c,h}, cnss2/{qmi,main}.c.
 * No Linux sockets, module ABI, fake replies or unowned DMA addresses. */
#include "PeachWlfw.h"
#include "PeachCe.h"
#include <string.h>

#define CTRL 0xfffffffeu
#define QRTR_TX_FLOW_LOW 5u
#define QRTR_TX_FLOW_HIGH 10u
#define WTRY(expr) do { int e_ = (expr); if (e_) return e_; } while (0)
static uint16_t u16(const unsigned char *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const unsigned char *p)
{ return u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const unsigned char *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static void p16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void p32(unsigned char *p, uint32_t v) { p16(p, v); p16(p + 2, v >> 16); }
static void p64(unsigned char *p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t now(PeachWlfw *w) { return w->host->io->now_us(w->host->io->context); }
static void pause_poll(PeachWlfw *w) { w->host->io->sleep_us(w->host->io->context, 1000); }
static void report(PeachWlfw *w, const char *s, uint32_t a, uint32_t b)
{ if (w->ops.log) w->ops.log(w->ops.context, s, a, b); }

static void header(unsigned char *p, unsigned type, uint32_t sn, uint32_t sp,
                   uint32_t dn, uint32_t dp, unsigned size, unsigned confirm)
{
    p32(p, 1); p32(p + 4, type); p32(p + 8, sn); p32(p + 12, sp);
    p32(p + 16, confirm); p32(p + 20, size); p32(p + 24, dn); p32(p + 28, dp);
}

static void control(PeachWlfw *w, unsigned type, uint32_t sn, uint32_t sp,
                    uint32_t dn, uint32_t dp)
{
    if (w->error) return;
    if (type == 2 && w->hello_sent) return;
    if (w->control_count == 64) { w->error = PeachWlfwStorage; return; }
    unsigned char *p = w->controls[w->control_wp];
    memset(p, 0, 52);
    header(p, type, sn, sp, dn, dp, 20, 0);
    p32(p + 32, type);
    if (type == 7) { p32(p + 36, sn); p32(p + 40, sp); }
    w->control_wp = (w->control_wp + 1) & 63;
    w->control_count++;
    if (type == 2) w->hello_sent = 1;
}

/* Validate the complete TLV stream, including unknown extensions, before
 * interpreting it. Duplicate requested fields cannot override earlier data. */
static int tlvs_valid(const unsigned char *p, size_t n)
{
    while (n) {
        if (n < 3 || u16(p + 1) > n - 3) return 0;
        size_t step = 3 + u16(p + 1);
        p += step; n -= step;
    }
    return 1;
}
static const unsigned char *field(PeachWlfw *w, const unsigned char *p, size_t n,
                                  unsigned type, size_t *size)
{
    const unsigned char *found = NULL;
    *size = 0;
    while (n) {
        size_t len = u16(p + 1), step = len + 3;
        if (p[0] == type) {
            if (found) { w->error = PeachWlfwProtocol; return NULL; }
            found = p + 3; *size = len;
        }
        p += step; n -= step;
    }
    return found;
}

static void memory_request(PeachWlfw *w, const unsigned char *p, size_t n)
{
    size_t len;
    const unsigned char *v = field(w, p, n, 1, &len);
    if (w->error) return;
    if (!v || !len || !v[0] || v[0] > PEACH_WLFW_SEGMENTS || w->memory_requested) {
        w->error = PeachWlfwProtocol; return;
    }
    unsigned count = *v++; len--;
    for (unsigned i = 0; i < count; i++) {
        if (len < 9) { w->error = PeachWlfwProtocol; return; }
        uint32_t bytes = u32(v), type = u32(v + 4);
        unsigned cfg = v[8];
        v += 9; len -= 9;
        report(w, "requested memory type/bytes", type, bytes);
        if (cfg) report(w, "requested memory type/configs", type, cfg);
        /* LPASS_SHARED (11) uses the same owned DMA allocation as CNSS2;
         * its optional WFDS/ADSP consumer is not enabled here. MSA and
         * pageable/MLO memory still need additional ownership or protocols. */
        if (!bytes || type < 1 || type > 12 || type == 6 || type == 8 ||
            type == 9 || cfg > 2 || cfg * 13 > len) {
            w->error = PeachWlfwUnsupported; return;
        }
        for (unsigned j = 0; j < cfg; j++, v += 13, len -= 13) {
            uint64_t offset = u64(v);
            uint32_t size = u32(v + 8);
            if (v[12] || offset > bytes || size > bytes - offset) {
                report(w, "unsupported memory config offset/size", (uint32_t)offset, size);
                report(w, "unsupported memory config type/secure", type, v[12]);
                w->error = PeachWlfwUnsupported; return;
            }
        }
        w->memory[i].size = bytes; w->memory[i].type = type;
    }
    if (len) { w->error = PeachWlfwProtocol; return; }
    w->memory_count = count; w->memory_requested = 1;
}

static void qmi_receive(PeachWlfw *w, const unsigned char *p, size_t n)
{
    if (n < 7 || u16(p + 5) != n - 7 || !tlvs_valid(p + 7, n - 7)) {
        w->error = PeachWlfwProtocol; return;
    }
    unsigned flags = p[0], txn = u16(p + 1), message = u16(p + 3);
    const unsigned char *data = p + 7;
    size_t bytes = n - 7;
    if (flags == 2) { /* QMI_RESPONSE */
        if (!w->awaiting || txn != w->awaiting || message != w->message) return;
        size_t len;
        const unsigned char *result = field(w, data, bytes, 2, &len);
        if (w->error) return;
        if (!result || len != 4 || bytes > sizeof(w->response) || w->response_ready) {
            w->error = PeachWlfwProtocol; return;
        }
        w->result = u16(result); w->remote_error = u16(result + 2);
        memcpy(w->response, data, bytes);
        w->response_bytes = (unsigned)bytes; w->response_ready = 1;
    } else if (flags == 4) { /* QMI_INDICATION */
        switch (message) {
        case 0x35: memory_request(w, data, bytes); break;
        case 0x37: w->memory_ready = 1; break;
        case 0x38: w->firmware_ready = 1; break;
        case 0x3e: { /* CAL_DONE is an indication, never inferred from mode ACK. */
            size_t len;
            const unsigned char *size = field(w, data, bytes, 0x10, &len);
            if (w->error || !w->calibration_active || w->calibration_seen ||
                (size && (len != 8 || u64(size) > w->calibration.capacity))) {
                w->error = PeachWlfwProtocol; return;
            }
            w->calibration_seen = 1;
            report(w, "WLFW CAL_DONE bytes", size ? (uint32_t)u64(size) : 0, 0);
            break;
        }
        case 0x5c: w->error = PeachMhiWrongEnvironment; break;
        default: break;
        }
    } else { w->error = PeachWlfwProtocol; }
}

static void receive(void *context, const void *packet, size_t bytes)
{
    PeachWlfw *w = context;
    const unsigned char *p = packet;
    unsigned type, confirm, hdr, size;
    uint32_t sn, sp, dn, dp;
    if (w->error) return;
    if (bytes < 16 || (bytes & 3)) { w->error = PeachWlfwProtocol; return; }
    if (p[0] == 1 && bytes >= 32 && u32(p) == 1) {
        hdr = 32; type = u32(p + 4); sn = u32(p + 8); sp = u32(p + 12);
        confirm = !!u32(p + 16); size = u32(p + 20); dn = u32(p + 24); dp = u32(p + 28);
    } else if (p[0] == 3) { /* Linux's QRTR_PROTO_VER_2 has wire version 3. */
        hdr = 16u + p[3]; type = p[1]; confirm = p[2] & 1; size = u32(p + 4);
        sn = u16(p + 8); sp = u16(p + 10); dn = u16(p + 12); dp = u16(p + 14);
        if (sp == 0xfffe) sp = CTRL;
        if (dp == 0xfffe) dp = CTRL;
    } else { w->error = PeachWlfwProtocol; return; }
    if (dp == 0xffff) dp = CTRL;
    if (hdr > bytes || !size || size > bytes - hdr || ((size + 3u) & ~3u) != bytes - hdr ||
        sn == w->local_node || sn == 0xffffffffu) { w->error = PeachWlfwProtocol; return; }
    p += hdr;
    if (type == 1) { /* QRTR_TYPE_DATA */
        if (dn != w->local_node || dp != w->local_port) return;
        /* The firmware's watermark must be acknowledged after actual delivery.
         * Queue it; the RX callback must not wait for its own MHI completion. */
        if (w->server && sn == w->server_node && sp == w->server_port) qmi_receive(w, p, size);
        if (!w->error && confirm) control(w, 7, dn, dp, sn, sp);
        return;
    }
    if (size < 20 || u32(p) != type) { w->error = PeachWlfwProtocol; return; }
    if (type == 7) {
        /* RESUME_TX names the receiver of the original DATA, i.e. this
         * flow's remote WLFW endpoint. qrtr_tx_resume keys by that payload,
         * not by the sender's local endpoint or the outer source port. */
        if (w->server && dn == w->local_node && sn == w->server_node &&
            u32(p + 4) == w->server_node && u32(p + 8) == w->server_port)
            w->flow_pending = 0;
        return;
    }
    if (dp != CTRL) { w->error = PeachWlfwProtocol; return; }
    if (type == 2) { /* HELLO */
        if (!w->hello) {
            w->peer = sn; w->hello = 1;
            control(w, 2, w->local_node, CTRL, sn, CTRL);
            report(w, "QRTR HELLO", sn, 0);
        } else if (sn != w->peer) w->error = PeachWlfwProtocol;
    } else if (type == 4) { /* NEW_SERVER: version in instance's low byte. */
        uint32_t service = u32(p + 4), instance = u32(p + 8), node = u32(p + 12), port = u32(p + 16);
        if (service == 0x45 && instance == 0x101) {
            if (!port || port >= CTRL || node == w->local_node || node == 0xffffffffu ||
                (w->server && (node != w->server_node || port != w->server_port))) {
                w->error = PeachWlfwProtocol; return;
            }
            w->server_node = node; w->server_port = port; w->server = 1;
            report(w, "WLFW service", node, port);
        }
    } else if ((type == 3 && sn == w->peer) ||
               (type == 5 && u32(p + 12) == w->server_node && u32(p + 16) == w->server_port)) {
        w->error = PeachMhiWrongEnvironment;
    }
}

int peach_wlfw_init(PeachWlfw *w, PeachMhiHost *host, const PeachWlfwOps *ops)
{
    if (!w || !host || !host->io || !ops || !ops->allocate || !ops->file_read ||
        !ops->iova_bytes || ops->iova_base >= 0x100000000ull ||
        ops->iova_bytes > 0x100000000ull - ops->iova_base) return PeachMhiInvalid;
    memset(w, 0, sizeof(*w));
    w->host = host; w->ops = *ops; w->local_node = 1; w->local_port = 0x4000;
    host->receive = receive; host->receive_context = w;
    return 0;
}

static int flush_controls(PeachWlfw *w)
{
    while (w->control_count) {
        int err = peach_mhi_host_send(w->host, w->controls[w->control_rp], 52);
        if (err == -6) return 0;
        if (err) return err;
        w->control_rp = (w->control_rp + 1) & 63; w->control_count--;
    }
    return 0;
}
int peach_wlfw_poll(PeachWlfw *w)
{
    if (!w || !w->host) return PeachMhiInvalid;
    if (w->error) return w->error;
    WTRY(flush_controls(w));
    WTRY(peach_mhi_host_poll(w->host, 32));
    if (w->error) return w->error;
    return flush_controls(w);
}

/* Payload is built in packet+39. All builders below have fixed schema bounds
 * less than the 8192-byte IPCR MTU; no kernel stack packet arrays are used. */
static size_t scalar(PeachWlfw *w, size_t n, unsigned type, uint64_t value, unsigned width)
{
    unsigned char *p = w->packet + 39 + n;
    p[0] = type; p16(p + 1, width);
    for (unsigned i = 0; i < width; i++) p[3 + i] = (unsigned char)(value >> (8 * i));
    return n + 3 + width;
}
static int request(PeachWlfw *w, unsigned message, size_t bytes)
{
    if (w->awaiting || !w->server || bytes > sizeof(w->packet) - 42) return PeachMhiInvalid;
    uint64_t end = now(w) + 10000000;
    while (w->flow_pending >= QRTR_TX_FLOW_HIGH || w->control_count) {
        WTRY(peach_wlfw_poll(w));
        if (now(w) >= end) return PeachMhiTimeout;
        if (w->flow_pending >= QRTR_TX_FLOW_HIGH || w->control_count) pause_poll(w);
    }
    if (!++w->transaction) ++w->transaction;
    w->awaiting = w->transaction; w->message = (uint16_t)message; w->response_ready = 0;
    header(w->packet, 1, w->local_node, w->local_port, w->server_node, w->server_port,
        (unsigned)bytes + 7, w->flow_pending + 1 == QRTR_TX_FLOW_LOW);
    w->packet[32] = 0; /* QMI_REQUEST */
    p16(w->packet + 33, w->awaiting); p16(w->packet + 35, message); p16(w->packet + 37, (unsigned)bytes);
    size_t length = (39 + bytes + 3) & ~(size_t)3;
    memset(w->packet + 39 + bytes, 0, length - 39 - bytes);
    int err;
    while ((err = peach_mhi_host_send(w->host, w->packet, length)) == -6) {
        WTRY(peach_wlfw_poll(w));
        if (now(w) >= end) return PeachMhiTimeout;
        pause_poll(w);
    }
    if (err) return err;
    w->flow_pending++;
    while (!w->response_ready) {
        WTRY(peach_wlfw_poll(w));
        if (now(w) >= end) { report(w, "QMI timeout", message, w->awaiting); return PeachMhiTimeout; }
        if (!w->response_ready) pause_poll(w);
    }
    w->awaiting = 0;
    if (w->result) { report(w, "QMI rejected", message, w->remote_error); return PeachWlfwRejected; }
    return 0;
}
int peach_wlfw_read_word(PeachWlfw *w, uint32_t offset, unsigned memory_type, uint32_t *value)
{
    if (!w || !value || (offset & 3) || w->phase != 9 || !w->firmware_ready || w->awaiting)
        return PeachMhiInvalid;
    if (w->error) return w->error;
    /* Matching cnss_utils IDL: three mandatory u32 TLVs, exactly 21 bytes.
     * memory_type belongs to ATHDIAG, not the WLFW DMA allocation enum. */
    size_t n = scalar(w, 0, 1, offset, 4);
    n = scalar(w, n, 2, memory_type, 4);
    n = scalar(w, n, 3, 4, 4);
    int err = request(w, 0x30, n);
    if (err == PeachMhiTimeout) {
        /* This read has no state or DMA ownership to roll back. Retire its
         * transaction so a late reply cannot occupy the next request. */
        w->awaiting = 0; w->response_ready = 0;
    }
    if (err) return err;
    size_t bytes;
    const unsigned char *data = field(w, w->response, w->response_bytes, 0x10, &bytes);
    if (w->error) return w->error;
    /* QMI_DATA_LEN is u16 on the wire; data_valid is TLV presence only. */
    if (!data || bytes != 6 || u16(data) != 4) return PeachWlfwProtocol;
    *value = u32(data + 2);
    return 0;
}
static int wait_flag(PeachWlfw *w, const unsigned *flag)
{
    uint64_t end = now(w) + 30000000;
    while (!*flag) {
        WTRY(peach_wlfw_poll(w));
        if (now(w) >= end) return PeachMhiTimeout;
        if (!*flag) pause_poll(w);
    }
    return 0;
}

static int respond_memory(PeachWlfw *w)
{
    unsigned char *p = w->packet + 39;
    p[0] = 1; p16(p + 1, 1 + w->memory_count * 17); p[3] = w->memory_count;
    for (unsigned i = 0; i < w->memory_count; i++) {
        PeachWlfwMemory *m = &w->memory[i];
        if (m->type == 4 && w->calibration.bytes) {
            m->dma = w->calibration; /* Mission firmware consumes real calibration output. */
        } else if (w->ops.allocate(w->ops.context, m->size, &m->dma)) return PeachWlfwStorage;
        if (!m->dma.bytes || m->dma.capacity < m->size || m->dma.iova < w->ops.iova_base ||
            m->dma.iova - w->ops.iova_base >= w->ops.iova_bytes ||
            m->size > w->ops.iova_bytes - (m->dma.iova - w->ops.iova_base)) return PeachWlfwStorage;
        if (m->type == 4) w->calibration = m->dma;
        unsigned char *v = p + 4 + i * 17;
        p64(v, m->dma.iova); p32(v + 8, m->size); p32(v + 12, m->type); v[16] = 0;
        report(w, "firmware memory", m->type, m->size);
        report(w, "firmware memory IOVA", m->type, (uint32_t)m->dma.iova);
    }
    WTRY(request(w, 0x36, 4 + w->memory_count * 17));
    w->memory_responded = 1;
    return 0;
}

static int download_bdf(PeachWlfw *w, const char *name, unsigned type)
{
    uint64_t total;
    if (w->ops.file_read(w->ops.context, name, 0, NULL, 0, &total) || !total || total > 0x200000)
        return PeachWlfwStorage;
    for (uint64_t off = 0, segment = 0; off < total; segment++) {
        unsigned bytes = (unsigned)(total - off > 6144 ? 6144 : total - off);
        size_t n = scalar(w, 0, 1, 1, 1);
        n = scalar(w, n, 0x10, w->board_id, 4);
        n = scalar(w, n, 0x11, total - off, 4); /* CNSS sends remaining bytes. */
        n = scalar(w, n, 0x12, segment, 4);
        unsigned char *p = w->packet + 39 + n;
        p[0] = 0x13; p16(p + 1, bytes + 2); p16(p + 3, bytes);
        if (w->ops.file_read(w->ops.context, name, off, p + 5, bytes, NULL)) return PeachWlfwStorage;
        n += 5 + bytes;
        n = scalar(w, n, 0x14, off + bytes == total, 1);
        n = scalar(w, n, 0x15, type, 1);
        WTRY(request(w, 0x25, n));
        size_t len;
        const unsigned char *flags = field(w, w->response, w->response_bytes, 0x10, &len);
        if (w->error) return w->error;
        if (flags) {
            if (len != 8) return PeachWlfwProtocol;
            if (u64(flags) & 6) return PeachWlfwUnsupported; /* radio off or extra CBC file required. */
        }
        off += bytes;
    }
    report(w, "board file downloaded", type, (uint32_t)total);
    return 0;
}

static int download_dma_file(PeachWlfw *w, const char *name, unsigned message)
{
    uint64_t total;
    PeachMhiDma dma = {0};
    if (w->ops.file_read(w->ops.context, name, 0, NULL, 0, &total) || !total || total > 0x200000 ||
        w->ops.allocate(w->ops.context, (size_t)total, &dma) || !dma.bytes || dma.capacity < total ||
        dma.iova < w->ops.iova_base || dma.iova - w->ops.iova_base >= w->ops.iova_bytes ||
        total > w->ops.iova_bytes - (dma.iova - w->ops.iova_base)) return PeachWlfwStorage;
    if (w->ops.file_read(w->ops.context, name, 0, dma.bytes, (size_t)total, NULL)) return PeachWlfwStorage;
    w->host->barrier(w->host->io->context);
    size_t n = scalar(w, 0, 1, dma.iova, 8);
    n = scalar(w, n, 2, total, 4);
    WTRY(request(w, message, n));
    report(w, "microcode downloaded", message, (uint32_t)total);
    return 0;
}

/* cnss2/qmi.c's QDSS config download, also required by TB323FU's WCN7860
 * ath12k port (0024). This phone's captured Android log selects the v2
 * fallback below. These are this phone's files, read from its active slot. */
static int download_qdss(PeachWlfw *w)
{
    static const char *const names[] = {
        "peach/qdss_trace_config_perf_v2.cfg", "peach/qdss_trace_config_v2.cfg"
    };
    uint64_t total = 0; const char *name = NULL;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!w->ops.file_read(w->ops.context, names[i], 0, NULL, 0, &total)) {
            name = names[i]; break;
        }
    }
    if (!name || !total || total > 0x200000) return PeachWlfwStorage;
    for (uint64_t off = 0, segment = 0; off < total; segment++) {
        unsigned bytes = total - off > 6144 ? 6144 : (unsigned)(total - off);
        size_t n = scalar(w, 0, 0x10, total - off, 4);
        n = scalar(w, n, 0x11, segment, 4);
        unsigned char *p = w->packet + 39 + n;
        p[0] = 0x12; p16(p + 1, bytes + 2); p16(p + 3, bytes);
        if (w->ops.file_read(w->ops.context, name, off, p + 5, bytes, NULL)) return PeachWlfwStorage;
        n += 5 + bytes;
        n = scalar(w, n, 0x13, off + bytes == total, 1);
        WTRY(request(w, 0x44, n));
        off += bytes;
    }
    report(w, "QDSS configuration downloaded", 0x44, (uint32_t)total);
    return 0;
}

int peach_wlfw_bootstrap(PeachWlfw *w)
{
    if (!w || !w->host || !w->host->ipcr_up || w->phase) return PeachMhiInvalid;
    w->phase = 1;
    control(w, 2, w->local_node, CTRL, 0xffffffffu, CTRL);
    WTRY(wait_flag(w, &w->hello));
    WTRY(wait_flag(w, &w->server));
    size_t n = scalar(w, 0, 0x15, 0x4b4e454c, 4); /* WLFW_CLIENT_ID ABI. */
    n = scalar(w, n, 0x16, 1, 1); n = scalar(w, n, 0x17, 1, 1);
    n = scalar(w, n, 0x18, 1, 1); n = scalar(w, n, 0x24, 1, 1);
    n = scalar(w, n, 0x1b, 1, 1); /* CAL_DONE */
    WTRY(request(w, 0x20, n));
    w->phase = 2;
    n = scalar(w, 0, 0x10, 1, 4);
    n = scalar(w, n, 0x14, 1, 1); /* BDF */
    n = scalar(w, n, 0x16, 1, 1); /* actual PHY microcode via M3_INFO */
    n = scalar(w, n, 0x17, 0, 1); /* no cached M3 */
    n = scalar(w, n, 0x1a, w->calibration_done, 1);
    n = scalar(w, n, 0x21, 0, 1); n = scalar(w, n, 0x33, 0, 1); /* no MLO/dynamic reallocation */
    /* CNSS and ath12k announce AUX_UC_SUPPORT before loading AUX microcode. */
    n = scalar(w, n, 0x27, 1u << 6, 8);
    unsigned char *p = w->packet + 39 + n;
    p[0] = 0x1f; p16(p + 1, 48); memset(p + 3, 0, 48);
    p64(p + 3, w->ops.iova_base); p64(p + 11, w->ops.iova_bytes); n += 51;
    WTRY(request(w, 0x34, n));
    WTRY(wait_flag(w, &w->memory_requested));
    w->phase = 3;
    WTRY(respond_memory(w));
    WTRY(wait_flag(w, &w->memory_ready));
    report(w, "WLFW memory ready", w->memory_count, 0);
    w->phase = 4;
    WTRY(request(w, 0x24, 0));
    size_t len;
    p = (unsigned char *)field(w, w->response, w->response_bytes, 0x10, &len);
    report(w, "WLFW capability field/bytes", 0x10, (uint32_t)len);
    if (!p || len != 8 || w->error) return PeachWlfwProtocol;
    w->chip_id = u32(p);
    p = (unsigned char *)field(w, w->response, w->response_bytes, 0x11, &len);
    report(w, "WLFW capability field/bytes", 0x11, (uint32_t)len);
    if (w->error || (p && len != 4)) return PeachWlfwProtocol;
    /* CNSS uses 0xff when the optional board_info is absent. */
    w->board_id = p ? u32(p) : 0xff;
    p = (unsigned char *)field(w, w->response, w->response_bytes, 0x12, &len);
    report(w, "WLFW capability field/bytes", 0x12, (uint32_t)len);
    if (!p || len != 4 || w->error) return PeachWlfwProtocol;
    w->soc_id = u32(p);
    report(w, "WLFW chip/board", w->chip_id, w->board_id);
    /* These are this phone's captured Android-selected files. Do not select
     * an unrelated board file from an older Snapdragon port. */
    WTRY(download_bdf(w, "peach/regdb_xiaomi.bin", 4));
    WTRY(download_bdf(w, "peach/bd_p1.elf", 1));
    w->phase = 5;
    WTRY(download_dma_file(w, "peach/phy_ucode20.elf", 0x3c));
    WTRY(download_dma_file(w, "peach/aux_ucode20.elf", 0x5a));
    WTRY(download_qdss(w));
    WTRY(wait_flag(w, &w->firmware_ready));
    w->phase = 6;
    report(w, "WLFW FW_INIT_DONE", w->soc_id, w->memory_count);
    return 0;
}

/* CNSS cold-boot sequence, also recorded in Armada patch 1151. The
 * endpoint has not seen CE/WLAN_CFG yet. No calibration data is fabricated
 * or written to persistent storage. */
int peach_wlfw_calibrate(PeachWlfw *w)
{
    if (!w || w->phase != 6 || !w->firmware_ready || w->calibration_done ||
        !w->calibration.bytes || w->calibration_active) return PeachMhiInvalid;
    uint64_t start = now(w);
    w->calibration_active = 1;
    size_t n = scalar(w, 0, 1, 7, 4); /* CALIBRATION */
    n = scalar(w, n, 0x10, 0, 1);
    WTRY(request(w, 0x22, n));
    WTRY(wait_flag(w, &w->calibration_seen));
    w->host->barrier(w->host->io->context);
    report(w, "WLFW cold calibration completed ms/IOVA", (uint32_t)((now(w) - start) / 1000),
        (uint32_t)w->calibration.iova);
    n = scalar(w, 0, 1, 4, 4); /* OFF before endpoint reset */
    n = scalar(w, n, 0x10, 0, 1);
    WTRY(request(w, 0x22, n));
    w->calibration_done = 1;
    return 0;
}

int peach_wlfw_wlan_enable(PeachWlfw *w, const unsigned char mac[6])
{
    if (!w || w->phase != 6 || !w->firmware_ready) return PeachMhiInvalid;
    if (w->error) return w->error;
    if (!mac || (mac[0] & 1) || !memcmp(mac, "\0\0\0\0\0\0", 6)) return PeachMhiInvalid;
    /* CNSS provisions the collected host address before WLAN_CFG/MISSION;
     * otherwise this firmware reports an all-zero MAC in WMI READY. */
    unsigned char *p = w->packet + 39;
    p[0] = 0x10; p16(p + 1, 6); memcpy(p + 3, mac, 6);
    WTRY(request(w, 0x33, 9));
    report(w, "WLFW MAC provisioned", 0, 0);
    /* cnss2/qmi.c WLAN_CFG schema: variable arrays carry an 8-bit count,
     * all pipe fields are 32-bit. Use the vendor's Peach-selected table,
     * including its terminator. No shadow/MSI capabilities are asserted. */
    p = w->packet + 39;
    static const char version[] = "UniXNU-Nezha";
    p[0] = 0x10; p16(p + 1, sizeof(version) - 1);
    memcpy(p + 3, version, sizeof(version) - 1);
    size_t n = 3 + sizeof(version) - 1;
    p += n;
    p[0] = 0x11; p16(p + 1, 1 + 9 * 20); p[3] = 9;
    for (unsigned i = 0; i < 9; i++) {
        const PeachCeTarget *t = &peach_ce_target[i];
        unsigned char *v = p + 4 + i * 20;
        p32(v, t->pipe); p32(v + 4, t->direction); p32(v + 8, t->entries);
        p32(v + 12, t->max_bytes); p32(v + 16, t->flags);
    }
    n += 4 + 9 * 20; p = w->packet + 39 + n;
    p[0] = 0x12; p16(p + 1, 1 + 15 * 12); p[3] = 15;
    for (unsigned i = 0; i < 15; i++) {
        const PeachCeService *s = &peach_ce_services[i];
        unsigned char *v = p + 4 + i * 12;
        p32(v, s->service); p32(v + 4, s->direction); p32(v + 8, s->pipe);
    }
    n += 4 + 15 * 12;
    w->phase = 7;
    WTRY(request(w, 0x23, n));
    n = scalar(w, 0, 1, 0, 4); /* MISSION */
    n = scalar(w, n, 0x10, 0, 1); /* no hardware debug */
    w->phase = 8;
    WTRY(request(w, 0x22, n));
    w->phase = 9;
    report(w, "WLFW MISSION acknowledged", 9, 15);
    return 0;
}
