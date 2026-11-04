/* SPDX-License-Identifier: GPL-2.0-only */
/* Wire ABI: fw-api/fw/htc.h, htc_services.h. Credit allocation and PCI HTT
 * policy follow htc/htc.c and dp/wifi3.0/dp_htt.c for a single WMI endpoint. */
#include "PeachHtc.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif
#define HTRY(h, expr) do { int err_ = (expr); if (err_) { (h)->error = err_; return err_; } } while (0)
static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static void p16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void report(PeachHtc *h, const char *s, unsigned a, unsigned b)
{ if (h->ops.log) h->ops.log(h->ops.context, s, a, b); }
static int bad(PeachHtc *h) { return h->error = PeachCeProtocol; }

int peach_htc_init(PeachHtc *h, PeachCe *ce, const PeachHtcOps *ops)
{
    if (!h || !ce || !ce->prepared || !ops || !ops->progress || !ops->now_us ||
        !ops->sleep_us || !ops->receive) return PeachMhiInvalid;
    memset(h, 0, sizeof(*h)); h->ce = ce; h->ops = *ops;
    /* HTC control is a pseudo service on endpoint zero, not a CONNECT. */
    h->endpoint[0] = (PeachHtcEndpoint){1, 0, 2, 256, 0, 0, 0, 0};
    return 0;
}
static int trailer(PeachHtc *h, const unsigned char *p, size_t n)
{
    while (n) {
        /* HTC_RECORD_HDR and HTC_CREDIT_REPORT are FOUR bytes in this ABI. */
        if (n < 4 || p[1] > n - 4) return bad(h);
        unsigned id = p[0], len = p[1];
        p += 4; n -= 4;
        if (id == 1) {
            if (!len || (len & 3)) return bad(h);
            for (unsigned i = 0; i < len; i += 4) {
                unsigned ep = p[i], count = p[i + 1];
                if (ep >= PEACH_HTC_ENDPOINTS || !h->endpoint[ep].service) return bad(h);
                PeachHtcEndpoint *e = &h->endpoint[ep];
                if (e->flow_control) {
                    if (count > e->allocation - e->credits) return bad(h);
                    e->credits += count;
                }
            }
        }
        p += len; n -= len;
    }
    return 0;
}
static void control_receive(PeachHtc *h, const unsigned char *p, size_t n)
{
    if (n < 2) { bad(h); return; }
    unsigned id = u16(p);
    if (id == 1) {
        if (n < 8 || h->ready || !u16(p + 2) || !u16(p + 4) || p[6] < 3 ||
            p[6] > PEACH_HTC_ENDPOINTS) { bad(h); return; }
        h->credit_count = u16(p + 2); h->credit_size = u16(p + 4);
        h->max_endpoints = p[6]; h->ready = 1;
        report(h, "HTC READY credits/size", h->credit_count, h->credit_size);
    } else if (id == 3) {
        if (n < 12 || !h->pending_service || h->response_ready || u16(p + 2) != h->pending_service ||
            p[4] || !p[5] || p[5] >= h->max_endpoints || !u16(p + 6) ||
            p[8] > n - 12 || h->endpoint[p[5]].service) { bad(h); return; }
        unsigned service = h->pending_service, ep = p[5], wmi = service == 0x100;
        if (!wmi && service != 0x300) { bad(h); return; }
        h->endpoint[ep] = (PeachHtcEndpoint){service, wmi ? 3u : 4u, wmi ? 2u : 1u,
            u16(p + 6), h->pending_credits, h->pending_credits, wmi, 0};
        if (wmi) h->wmi_endpoint = ep; else h->htt_endpoint = ep;
        h->response_ready = 1;
        report(h, "HTC connected service/endpoint", service, ep);
    } else {
        report(h, "unexpected HTC control", id, (unsigned)n); bad(h);
    }
}
void peach_htc_receive(void *context, unsigned pipe, const void *packet, size_t n)
{
    PeachHtc *h = context;
    const unsigned char *p = packet;
    if (!h || h->error) return;
    if (h->receiving || !p || n < 8) { bad(h); return; }
    unsigned ep = p[0], flags = p[1], length = u16(p + 2);
    if (ep >= PEACH_HTC_ENDPOINTS || !h->endpoint[ep].service ||
        pipe != h->endpoint[ep].rx_pipe || length > n - 8 || (flags & 0xfc) ||
        n - 8 - length > 3) { bad(h); return; }
    unsigned tail = (flags & 2) ? p[4] : 0;
    if ((flags & 2) && (!tail || tail > length)) { bad(h); return; }
    h->receiving = 1;
    if (tail && trailer(h, p + 8 + length - tail, tail)) { h->receiving = 0; return; }
    if (length > tail) {
        if (!ep) control_receive(h, p + 8, length - tail);
        else {
            int err = h->ops.receive(h->ops.context, h->endpoint[ep].service, p + 8, length - tail);
            if (err) h->error = err;
        }
    }
    h->receiving = 0;
}
int peach_htc_poll(PeachHtc *h)
{
    if (!h || !h->ce || h->receiving) return PeachMhiInvalid;
    if (h->error) return h->error;
    HTRY(h, h->ops.progress(h->ops.context));
    HTRY(h, peach_ce_poll(h->ce, 32));
    return h->error;
}
int peach_htc_send(PeachHtc *h, unsigned service, const void *payload, size_t n)
{
    if (!h || !h->ready || h->receiving || !payload || !n || n > sizeof(h->packet) - 8)
        return PeachMhiInvalid;
    if (h->error) return h->error;
    PeachHtcEndpoint *e = NULL;
    unsigned ep;
    for (ep = 0; ep < PEACH_HTC_ENDPOINTS; ep++)
        if (h->endpoint[ep].service == service) { e = &h->endpoint[ep]; break; }
    if (!e || n > e->max_bytes || n + 8 > h->ce->channel[e->tx_pipe].limit) return PeachMhiInvalid;
    unsigned credits = e->flow_control ? (unsigned)(n + 8 + h->credit_size - 1) / h->credit_size : 0;
    if (credits > e->credits) return PeachCeFull;
    memset(h->packet, 0, 8);
    h->packet[0] = ep; h->packet[1] = e->flow_control ? 1 : 0;
    p16(h->packet + 2, (unsigned)n); h->packet[5] = e->sequence;
    memcpy(h->packet + 8, payload, n);
    int err = peach_ce_send(h->ce, e->tx_pipe, ep, h->packet, n + 8);
    if (err == PeachCeFull) return err;
    if (err) { h->error = err; return err; }
    e->credits -= credits; e->sequence = (e->sequence + 1) & 255;
    return 0;
}
static int wait_flag(PeachHtc *h, const unsigned *flag)
{
    uint64_t end = h->ops.now_us(h->ops.context) + 10000000;
    while (!*flag) {
        HTRY(h, peach_htc_poll(h));
        if (h->ops.now_us(h->ops.context) >= end) return h->error = PeachMhiTimeout;
        if (!*flag) h->ops.sleep_us(h->ops.context, 1000);
    }
    return 0;
}
static int send_control(PeachHtc *h, const void *p, size_t n)
{
    uint64_t end = h->ops.now_us(h->ops.context) + 10000000;
    int err;
    while ((err = peach_htc_send(h, 1, p, n)) == PeachCeFull) {
        HTRY(h, peach_htc_poll(h));
        if (h->ops.now_us(h->ops.context) >= end) return h->error = PeachMhiTimeout;
        h->ops.sleep_us(h->ops.context, 1000);
    }
    return err;
}
static int connect_service(PeachHtc *h, unsigned service, unsigned credits)
{
    unsigned char connect[8] = {0};
    p16(connect, 2); p16(connect + 2, service);
    /* PCI HTT does not use target HTC buffers; vendor explicitly disables
     * its credit flow. WMI receives actual READY credits and keeps flow on. */
    p16(connect + 4, credits << 8 | (service == 0x300 ? 8 : 0));
    h->pending_service = service; h->pending_credits = credits; h->response_ready = 0;
    HTRY(h, send_control(h, connect, sizeof(connect)));
    HTRY(h, wait_flag(h, &h->response_ready));
    h->pending_service = 0;
    return 0;
}
int peach_htc_bootstrap(PeachHtc *h)
{
    if (!h || !h->ce || !h->ce->started || h->started) return PeachMhiInvalid;
    HTRY(h, wait_flag(h, &h->ready));
    unsigned credits = h->credit_count > 255 ? 255 : h->credit_count;
    HTRY(h, connect_service(h, 0x100, credits));
    HTRY(h, connect_service(h, 0x300, 0));
    unsigned char setup[12] = {5, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    HTRY(h, send_control(h, setup, sizeof(setup)));
    h->started = 1;
    report(h, "HTC setup complete WMI/HTT", h->wmi_endpoint, h->htt_endpoint);
    return 0;
}
