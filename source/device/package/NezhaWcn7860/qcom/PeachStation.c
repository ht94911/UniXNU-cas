/* SPDX-License-Identifier: GPL-2.0-only */
/* 802.11 legacy STA MLME over Peach WMI management TX/RX. No synthetic
 * responses or autonomous network choice. Firmware wire format is handled
 * by PeachWmi; association here requires actual AP response bytes. */
#include "PeachStation.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif
static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static void p16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static int fail(PeachStation *s, int error)
{
    (void)peach_tx_authorize(s->tx, 0);
    s->phase = PeachStationFailed;
    return s->error = error;
}
#define STRY(s, expr) do { int err_ = (expr); if (err_) return fail((s), err_); } while (0)
static uint64_t now(PeachStation *s) { return s->wmi->htc->ops.now_us(s->wmi->htc->ops.context); }
static void sleep_ms(PeachStation *s) { s->wmi->htc->ops.sleep_us(s->wmi->htc->ops.context, 1000); }
static int progress(PeachStation *s)
{
    int err = peach_htc_poll(s->wmi->htc);
    if (!err) err = s->wmi->error ? s->wmi->error : s->htt->error;
    return err;
}
static int rate_add(unsigned char rates[12], unsigned *count, unsigned value, unsigned frequency)
{
    static const unsigned char supported[] = {2,4,11,22,12,18,24,36,48,72,96,108};
    unsigned found = 0, rate = value & 127;
    for (unsigned i = frequency < 2500 ? 0 : 4; i < sizeof(supported); i++)
        if (rate == supported[i]) found = 1;
    if (!found) return value & 128 ? PeachMhiInvalid : 0;
    for (unsigned i = 0; i < *count; i++)
        if ((rates[i] & 127) == rate) return PeachCeProtocol;
    if (*count == 12) return PeachCeProtocol;
    rates[(*count)++] = value;
    return 0;
}
static int rates_from_ies(const unsigned char *p, size_t n, unsigned frequency,
                          unsigned char rates[12], unsigned *count)
{
    unsigned seen = 0; *count = 0;
    while (n) {
        if (n < 2 || p[1] > n - 2) return PeachCeProtocol;
        unsigned tag = p[0], len = p[1];
        if (tag == 1 || tag == 50) {
            unsigned bit = tag == 1 ? 1 : 2;
            if ((seen & bit) || !len || (tag == 1 && len > 8)) return PeachCeProtocol;
            seen |= bit;
            for (unsigned i = 0; i < len; i++) {
                int err = rate_add(rates, count, p[2 + i], frequency);
                if (err) return err;
            }
        }
        p += 2 + len; n -= 2 + len;
    }
    return (seen & 1) && *count ? 0 : PeachMhiInvalid;
}
static int suite(const unsigned char *p, unsigned type)
{ return !p[0] && p[1] == 15 && p[2] == 172 && p[3] == type; }
static int rsn_supported(const unsigned char *p, size_t n)
{
    if (n < 8 || u16(p) != 1 || !suite(p + 2, 4)) return 0;
    p += 6; n -= 6;
    for (unsigned list = 0; list < 2; list++) {
        if (n < 2) return 0;
        unsigned count = u16(p), found = 0;
        p += 2; n -= 2;
        if (!count || count > n / 4) return 0;
        for (unsigned i = 0; i < count; i++) if (suite(p + 4 * i, list ? 2 : 4)) found = 1;
        if (!found) return 0;
        p += count * 4; n -= count * 4;
    }
    if (!n) return 1; /* omitted RSN capabilities = zero */
    if (n < 2 || (u16(p) & 0x40)) return 0; /* no required PMF support yet */
    p += 2; n -= 2;
    if (!n) return 1;
    if (n < 2) return 0;
    unsigned count = u16(p); p += 2; n -= 2;
    if (count > n / 16) return 0;
    p += count * 16; n -= count * 16;
    return !n || (n == 4 && suite(p, 6));
}
static int bss_security(unsigned security, const PeachWmiBss *b)
{
    const unsigned char *rsn = NULL; unsigned bytes = 0;
    for (size_t off = 0; off < b->ie_bytes;) {
        if (b->ie_bytes - off < 2 || b->ies[off + 1] > b->ie_bytes - off - 2)
            return PeachCeProtocol;
        unsigned n = b->ies[off + 1];
        if (b->ies[off] == 48) {
            if (rsn) return PeachCeProtocol;
            rsn = b->ies + off + 2; bytes = n;
        }
        off += 2 + n;
    }
    if (security == PeachStationOpen)
        return (b->capability & 16) || rsn ? PeachMhiInvalid : 0;
    if (!(b->capability & 16) || !rsn || !rsn_supported(rsn, bytes)) return PeachMhiInvalid;
    return 0;
}
int peach_station_candidate(const PeachWmiBss *b, unsigned security)
{
    if (!b || security > PeachStationWpa2Psk || !b->ssid_bytes || b->ssid_bytes > 32 ||
        b->ies_truncated || !b->beacon_interval || !b->dtim_period || (b->capability & 3) != 1 ||
        (b->frequency >= 2500 && (b->frequency < 4900 || b->frequency >= 5925)))
        return PeachMhiInvalid;
    int err = bss_security(security, b);
    unsigned char rates[12]; unsigned count;
    return err ? err : rates_from_ies(b->ies, b->ie_bytes, b->frequency, rates, &count);
}
int peach_station_init(PeachStation *s, PeachWmi *w, PeachHtt *h, PeachRx *r, PeachTx *t)
{
    if (!s || !w || !h || !r || !t || !w->ready || !h->confirmed || !r->started || !t->started)
        return PeachMhiInvalid;
    memset(s, 0, sizeof(*s)); s->wmi = w; s->htt = h; s->rx = r; s->tx = t;
    return 0;
}
static int mapping_live(PeachStation *s)
{
    const PeachHttPeer *p = peach_htt_peer(s->htt, 0, s->bssid);
    return p && p->generation == s->peer_generation;
}
static int wait_peer(PeachStation *s)
{
    uint64_t end = now(s) + 10000000;
    for (;;) {
        const PeachHttPeer *p = peach_htt_peer(s->htt, 0, s->bssid);
        if (p) {
            if (!p->ast_valid || !p->hash_valid) return PeachMhiInvalid;
            s->peer_generation = p->generation; return 0;
        }
        STRY(s, progress(s));
        if (now(s) >= end) return PeachMhiTimeout;
        sleep_ms(s);
    }
}
static void header(PeachStation *s, unsigned subtype)
{
    memset(s->frame, 0, sizeof(s->frame)); s->frame[0] = subtype;
    memcpy(s->frame + 4, s->bssid, 6); memcpy(s->frame + 10, s->wmi->mac, 6);
    memcpy(s->frame + 16, s->bssid, 6);
    p16(s->frame + 22, (s->sequence++ & 4095) << 4);
}
static int response(PeachStation *s, unsigned subtype, size_t minimum)
{
    uint64_t end = now(s) + 1000000;
    for (;;) {
        int err = peach_wmi_mgmt_rx(s->wmi, &s->received);
        if (!err) {
            const unsigned char *p = s->received.frame;
            unsigned got = p[0] & 0xfc;
            if (got == 0xc0 || got == 0xa0) {
                s->on_air_status = s->received.bytes >= 26 ? u16(p + 24) : 0xffff;
                return PeachMhiDeviceError;
            }
            if (got == subtype) {
                if (s->received.bytes < minimum || memcmp(p + 4, s->wmi->mac, 6) ||
                    (u16(p + 22) & 15) || (u16(p) & 0x0400)) return PeachCeProtocol;
                return 0;
            }
        } else if (err != PeachWmiNoFrame) return err;
        STRY(s, progress(s));
        if (!mapping_live(s)) return PeachMhiDeviceError;
        if (now(s) >= end) return PeachMhiTimeout;
        sleep_ms(s);
    }
}
static int authenticate(PeachStation *s)
{
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        header(s, 0xb0); p16(s->frame + 26, 1); /* open-system alg 0, transaction 1 */
        int err = peach_wmi_mgmt_tx(s->wmi, s->frame, 30);
        if (err && err != PeachMhiDeviceError) return err;
        /* An AP response can arrive even when the host misses TX ACK. */
        err = response(s, 0xb0, 30);
        if (err == PeachMhiTimeout && attempt < 2) continue;
        if (err) return err;
        const unsigned char *p = s->received.frame;
        if (u16(p + 24) || u16(p + 26) != 2) return PeachCeProtocol;
        s->on_air_status = u16(p + 28);
        return s->on_air_status ? PeachMhiDeviceError : 0;
    }
    return PeachMhiTimeout;
}
static unsigned wmm_parameters(const unsigned char *p, size_t bytes)
{
    while (bytes >= 2 && p[1] <= bytes - 2) {
        if (p[0] == 221 && p[1] == 24 &&
            !memcmp(p + 2, "\x00\x50\xf2\x02\x01\x01", 6)) return 1;
        size_t n = p[1] + 2u; p += n; bytes -= n;
    }
    return 0;
}
static int associate(PeachStation *s, const PeachWmiBss *b)
{
    header(s, 0); p16(s->frame + 24, s->requested_caps); p16(s->frame + 26, 10);
    unsigned off = 28;
    s->frame[off++] = 0; s->frame[off++] = b->ssid_bytes;
    memcpy(s->frame + off, b->ssid, b->ssid_bytes); off += b->ssid_bytes;
    unsigned first = s->rate_count > 8 ? 8 : s->rate_count;
    s->frame[off++] = 1; s->frame[off++] = first;
    memcpy(s->frame + off, s->rates, first); off += first;
    if (s->rate_count > first) {
        s->frame[off++] = 50; s->frame[off++] = s->rate_count - first;
        memcpy(s->frame + off, s->rates + first, s->rate_count - first); off += s->rate_count - first;
    }
    memcpy(s->frame + off, s->rsn, s->rsn_bytes); off += s->rsn_bytes;
    unsigned qos = wmm_parameters(b->ies, b->ie_bytes);
    if (qos) {
        static const unsigned char info[] = {221,7,0,0x50,0xf2,2,0,1,0};
        memcpy(s->frame + off, info, sizeof(info)); off += sizeof(info);
    }
    int err = peach_wmi_mgmt_tx(s->wmi, s->frame, off);
    if (err && err != PeachMhiDeviceError) return err;
    err = response(s, 0x10, 30);
    if (err) return err;
    const unsigned char *p = s->received.frame;
    s->on_air_status = u16(p + 26);
    if (s->on_air_status) return PeachMhiDeviceError;
    PeachWmiAssociation *a = &s->association;
    a->aid = u16(p + 28) & 0x3fff; a->capabilities = u16(p + 24);
    a->protected_network = s->security == PeachStationWpa2Psk;
    a->qos = qos && wmm_parameters(p + 30, s->received.bytes - 30);
    if (!a->aid || a->aid > 2007 || (a->capabilities & 3) != 1 ||
        !!(a->capabilities & 16) != a->protected_network) return PeachCeProtocol;
    err = rates_from_ies(p + 30, s->received.bytes - 30, b->frequency, a->rates, &a->rate_count);
    if (err) return err;
    /* Every response rate must have been offered, including AP basic rates. */
    for (unsigned i = 0; i < a->rate_count; i++) {
        unsigned found = 0;
        for (unsigned j = 0; j < s->rate_count; j++)
            if ((a->rates[i] & 127) == (s->rates[j] & 127)) found = 1;
        if (!found) return PeachCeProtocol;
    }
    return 0;
}
int peach_station_associate(PeachStation *s, unsigned bss_index, unsigned security)
{
    if (!s || !s->wmi || s->phase != PeachStationIdle || security > PeachStationWpa2Psk ||
        bss_index >= s->wmi->bss_count || !s->wmi->scan_done || s->wmi->scan_active)
        return PeachMhiInvalid;
    const PeachWmiBss *b = &s->wmi->bss[bss_index];
    int err = peach_station_candidate(b, security);
    if (err) return err;
    const PeachWmiRegDomain *reg = &s->wmi->regulatory[s->wmi->radio.phy];
    if (!reg->valid || reg->generation != s->wmi->scan_reg_generation) return PeachMhiInvalid;
    s->security = security; memcpy(s->bssid, b->bssid, 6);
    if (security) {
        static const unsigned char selected[] = {48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
        memcpy(s->rsn, selected, sizeof(selected)); s->rsn_bytes = sizeof(selected);
    }
    STRY(s, rates_from_ies(b->ies, b->ie_bytes, b->frequency, s->rates, &s->rate_count));
    s->requested_caps = 1u | (b->capability & 0x420) | (security ? 16u : 0);
    s->phase = PeachStationPeer;
    STRY(s, peach_wmi_peer_create(s->wmi, s->bssid, 1));
    STRY(s, wait_peer(s));
    STRY(s, peach_wmi_vdev_start(s->wmi, bss_index, 0));
    /* Vendor initializes both default data and non-QoS queues. No BA. */
    STRY(s, peach_wmi_peer_queue(s->wmi, s->htt, s->bssid, 0, peach_rx_queue(s->rx, 0)));
    STRY(s, peach_wmi_peer_queue(s->wmi, s->htt, s->bssid, 16, peach_rx_queue(s->rx, 16)));
    s->phase = PeachStationAuth; STRY(s, authenticate(s));
    s->phase = PeachStationAssoc; STRY(s, associate(s, b));
    STRY(s, peach_wmi_wmm(s->wmi, s->association.qos ? s->received.frame + 30 : NULL,
        s->association.qos ? s->received.bytes - 30 : 0));
    /* WMM APs can use every traffic class, including video and background
     * IP traffic. TID0 was prepared above; queues already exist for 1..7. */
    if (s->association.qos) for (unsigned tid = 1; tid < 8; tid++)
        STRY(s, peach_wmi_peer_queue(s->wmi, s->htt, s->bssid, tid, peach_rx_queue(s->rx, tid)));
    STRY(s, peach_wmi_peer_assoc(s->wmi, s->htt, &s->association));
    STRY(s, peach_wmi_vdev_up(s->wmi));
    STRY(s, peach_tx_bind(s->tx, 0, s->bssid));
    s->tx->qos = s->association.qos;
    s->phase = security ? PeachStationAwaitKeys : PeachStationOpenAssociated;
    return 0; /* caller still has no authorized TX/NIC */
}
int peach_station_poll(PeachStation *s)
{
    if (!s || !s->wmi) return 0;
    if (s->error) return s->error;
    if (s->phase != PeachStationAwaitKeys && s->phase != PeachStationOpenAssociated) return 0;
    const PeachWmiRegDomain *r = &s->wmi->regulatory[s->wmi->radio.phy];
    if (!mapping_live(s) || !r->valid || r->generation != s->wmi->vdev_reg_generation)
        return fail(s, PeachMhiDeviceError);
    for (unsigned budget = 0; budget < PEACH_WMI_MGMT_FRAMES; budget++) {
        int err = peach_wmi_mgmt_rx(s->wmi, &s->received);
        if (err == PeachWmiNoFrame) return 0;
        if (err) return fail(s, err);
        unsigned subtype = s->received.frame[0] & 0xfc;
        if (subtype == 0xc0 || subtype == 0xa0) {
            s->on_air_status = s->received.bytes >= 26 ? u16(s->received.frame + 24) : 0xffff;
            return fail(s, PeachMhiDeviceError);
        }
    }
    return 0;
}
int peach_station_recover_unkeyed(PeachStation *s)
{
    if (!s || !s->wmi || !s->tx || !s->rx || !s->htt) return PeachMhiInvalid;
    PeachWmi *w = s->wmi; PeachTx *t = s->tx;
    if (w->error || s->htt->error || s->rx->error || t->error || w->key_pending)
        return PeachMhiInvalid;
    for (unsigned i = 0; i < 5; i++) if (w->keys[i].installed) return PeachMhiInvalid;
    if (s->phase == PeachStationIdle) return 0; /* selection/PMK failed before setup */
    if (!s->peer_generation) return PeachMhiInvalid;
    (void)peach_tx_authorize(t, 0);
    s->phase = PeachStationFailed; /* no new MLME while teardown polls */
    uint64_t end = now(s) + 10000000;
    while (t->in_flight || w->mgmt_in_flight) {
        int err = progress(s);
        if (err) return err;
        if (now(s) >= end) return PeachMhiTimeout;
        sleep_ms(s);
    }
    int err = peach_wmi_retire_unkeyed(w, s->htt, s->peer_generation);
    if (err) return err;
    PeachMhiDma queues;
    err = w->ops.allocate(w->ops.context, PEACH_RX_QUEUE_BYTES, &queues);
    if (!err) err = peach_rx_new_queues(s->rx, queues);
    if (!err) err = peach_tx_unbind(t);
    if (err) return err;
    return peach_station_init(s, w, s->htt, s->rx, t);
}
