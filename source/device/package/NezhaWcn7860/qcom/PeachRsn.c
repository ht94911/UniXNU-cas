/* SPDX-License-Identifier: GPL-2.0-only */
/* IEEE 802.11 RSN PSK four-way/group handshake. Crypto uses XNU's existing
 * SHA1 and constant-time AES provider; no separate crypto runtime or daemon. */
#include "PeachRsn.h"
#include <libkern/libkern.h>
#include <libkern/crypto/sha1.h>
#include <libkern/crypto/aes.h>
#include <sys/random.h>

enum { ReplyM2 = 1, ReplyM4, ReplyGroup, ReplyAgain };
static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint64_t be64(const unsigned char *p)
{ uint64_t v = 0; for (unsigned i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static void put16(unsigned char *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void put64(unsigned char *p, uint64_t v)
{ for (unsigned i = 0; i < 8; i++) p[7 - i] = v >> (8 * i); }
static void erase(void *p, size_t n)
{ volatile unsigned char *v = p; while (n--) *v++ = 0; }
static int equal(const void *a, const void *b, size_t n)
{ const unsigned char *x = a, *y = b; unsigned v = 0; while (n--) v |= *x++ ^ *y++; return !v; }
static int zero(const unsigned char *p, size_t n)
{ unsigned v = 0; while (n--) v |= *p++; return !v; }
static uint64_t now(PeachRsn *r)
{ PeachHtc *h = r->station->wmi->htc; return h->ops.now_us(h->ops.context); }
static int fail(PeachRsn *r, int error)
{
    (void)peach_tx_authorize(r->station->tx, 0);
    r->phase = PeachRsnFailed; r->station->phase = PeachStationFailed;
    erase(r->pmk, sizeof(r->pmk)); erase(r->ptk, sizeof(r->ptk));
    erase(r->gtk, sizeof(r->gtk)); erase(r->plain, sizeof(r->plain));
    return r->error = error;
}
#define RTRY(r, expr) do { int err_ = (expr); if (err_) return fail((r), err_); } while (0)
static void digest(const void *p, size_t n, unsigned char out[20])
{ SHA1_CTX c; SHA1Init(&c); SHA1Update(&c, p, n); SHA1Final(out, &c); erase(&c, sizeof(c)); }
static void hmac(const unsigned char *key, size_t key_bytes, const void *p, size_t n,
                 unsigned char out[20])
{
    unsigned char pad[64] = {0}, inner[20]; SHA1_CTX c;
    if (key_bytes > sizeof(pad)) digest(key, key_bytes, pad);
    else memcpy(pad, key, key_bytes);
    for (unsigned i = 0; i < sizeof(pad); i++) pad[i] ^= 0x36;
    SHA1Init(&c); SHA1Update(&c, pad, sizeof(pad)); SHA1Update(&c, p, n); SHA1Final(inner, &c);
    for (unsigned i = 0; i < sizeof(pad); i++) pad[i] ^= 0x36 ^ 0x5c;
    SHA1Init(&c); SHA1Update(&c, pad, sizeof(pad)); SHA1Update(&c, inner, sizeof(inner)); SHA1Final(out, &c);
    erase(&c, sizeof(c)); erase(pad, sizeof(pad)); erase(inner, sizeof(inner));
}
int peach_rsn_pmk(const void *passphrase, size_t bytes, const void *ssid,
                  size_t ssid_bytes, unsigned char pmk[32])
{
    if (!passphrase || bytes < 8 || bytes > 63 || !ssid || !ssid_bytes || ssid_bytes > 32 || !pmk)
        return PeachMhiInvalid;
    const unsigned char *p = passphrase;
    for (size_t i = 0; i < bytes; i++) if (p[i] < 32 || p[i] > 126) return PeachMhiInvalid;
    unsigned char salt[36], u[20], block[20];
    memcpy(salt, ssid, ssid_bytes); memset(salt + ssid_bytes, 0, 4);
    for (unsigned n = 1; n <= 2; n++) {
        salt[ssid_bytes + 3] = n;
        hmac(p, bytes, salt, ssid_bytes + 4, u); memcpy(block, u, sizeof(block));
        for (unsigned round = 1; round < 4096; round++) {
            hmac(p, bytes, u, sizeof(u), u);
            for (unsigned j = 0; j < sizeof(block); j++) block[j] ^= u[j];
        }
        memcpy(pmk + (n - 1) * 20, block, n == 1 ? 20 : 12);
    }
    erase(salt, sizeof(salt)); erase(u, sizeof(u)); erase(block, sizeof(block));
    return 0;
}
static void derive_ptk(PeachRsn *r)
{
    static const unsigned char label[] = "Pairwise key expansion";
    unsigned char data[sizeof(label) + 76 + 1], out[20];
    const unsigned char *local = r->station->wmi->mac, *ap = r->station->bssid;
    unsigned off = sizeof(label); memcpy(data, label, off);
    unsigned first = memcmp(local, ap, 6) < 0;
    memcpy(data + off, first ? local : ap, 6); memcpy(data + off + 6, first ? ap : local, 6); off += 12;
    first = memcmp(r->snonce, r->anonce, 32) < 0;
    memcpy(data + off, first ? r->snonce : r->anonce, 32);
    memcpy(data + off + 32, first ? r->anonce : r->snonce, 32); off += 64;
    for (unsigned i = 0; i < 3; i++) {
        data[off] = i; hmac(r->pmk, 32, data, sizeof(data), out);
        memcpy(r->ptk + i * 20, out, i == 2 ? 8 : 20);
    }
    erase(data, sizeof(data)); erase(out, sizeof(out));
}
static int unwrap(PeachRsn *r, const unsigned char *p, size_t bytes, size_t *plain_bytes)
{
    if (bytes < 24 || (bytes & 7) || bytes > sizeof(r->plain)) return PeachCeProtocol;
    aes_decrypt_ctx c; unsigned char a[8], block[16];
    if (aes_decrypt_key128(r->ptk + 16, &c)) return PeachMhiDeviceError;
    memcpy(a, p, 8); memcpy(r->plain, p + 8, bytes - 8);
    unsigned n = bytes / 8 - 1; int error = 0;
    /* RFC 3394, inverse of the six AES key-wrap rounds. */
    for (int j = 5; j >= 0 && !error; j--) for (unsigned i = n; i && !error; i--) {
        put64(block, be64(a) ^ (uint64_t)(n * j + i));
        memcpy(block + 8, r->plain + (i - 1) * 8, 8);
        error = aes_decrypt(block, block, &c);
        memcpy(a, block, 8); memcpy(r->plain + (i - 1) * 8, block + 8, 8);
    }
    static const unsigned char expected[8] = {0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6};
    if (!equal(a, expected, 8)) error = PeachCeProtocol;
    erase(&c, sizeof(c)); erase(a, sizeof(a)); erase(block, sizeof(block));
    if (error) { erase(r->plain, sizeof(r->plain)); return error; }
    *plain_bytes = bytes - 8; return 0;
}
static int key_data(PeachRsn *r, size_t bytes, unsigned group)
{
    unsigned gtk = 0, rsn = 0;
    for (size_t off = 0; off < bytes;) {
        const unsigned char *p = r->plain + off;
        if (p[0] == 221 && (off + 1 == bytes || !p[1]) &&
            zero(p + 1, bytes - off - 1)) break; /* RSN AES-wrap padding */
        if (bytes - off < 2 || p[1] > bytes - off - 2) return PeachCeProtocol;
        unsigned n = p[1];
        if (p[0] == 48) {
            if (group || rsn++ || n + 2 != r->ap_rsn_bytes ||
                memcmp(p, r->ap_rsn, n + 2)) return PeachCeProtocol;
        } else if (p[0] == 221 && n >= 4 && !memcmp(p + 2, "\x00\x0f\xac\x01", 4)) {
            if (gtk++ || n != 22 || (p[6] & ~7u) || p[7]) return PeachCeProtocol;
            r->group_index = p[6] & 3; memcpy(r->gtk, p + 8, 16);
        }
        off += n + 2;
    }
    return gtk == 1 && (group || rsn == 1) ? 0 : PeachCeProtocol;
}
static void make_reply(PeachRsn *r, const unsigned char *in, unsigned info,
                       unsigned nonce, unsigned data_bytes)
{
    memset(r->reply, 0, sizeof(r->reply));
    memcpy(r->reply, r->station->bssid, 6); memcpy(r->reply + 6, r->station->wmi->mac, 6);
    r->reply[12] = 0x88; r->reply[13] = 0x8e;
    unsigned char *e = r->reply + 14, *key = e + 4;
    e[0] = in[0]; e[1] = 3; put16(e + 2, 95 + data_bytes);
    key[0] = 2; put16(key + 1, info); memcpy(key + 5, in + 9, 8);
    if (nonce) memcpy(key + 13, r->snonce, 32);
    put16(key + 93, data_bytes);
    if (data_bytes) memcpy(key + 95, r->station->rsn, data_bytes);
    unsigned char mic[20]; hmac(r->ptk, 16, e, 99 + data_bytes, mic);
    memcpy(key + 77, mic, 16); erase(mic, sizeof(mic));
    r->reply_bytes = 113 + data_bytes;
}
static void schedule(PeachRsn *r, unsigned action)
{ r->tx_action = action; r->tx_sent = r->tx_tries = 0; r->tx_deadline = now(r) + 10000000; }
static int mic_valid(PeachRsn *r, unsigned char *e, size_t bytes)
{
    unsigned char saved[16], expected[20];
    memcpy(saved, e + 81, 16); memset(e + 81, 0, 16);
    hmac(r->ptk, 16, e, bytes, expected); memcpy(e + 81, saved, 16);
    int valid = equal(saved, expected, 16);
    erase(saved, sizeof(saved)); erase(expected, sizeof(expected)); return valid;
}
int peach_rsn_prepare(PeachRsn *r, PeachStation *s, unsigned bss_index, const unsigned char pmk[32])
{
    if (!r || !s || !pmk || r->phase != PeachRsnIdle ||
        s->phase != PeachStationIdle || s->error || !s->wmi ||
        !s->wmi->scan_done || s->wmi->scan_active || bss_index >= s->wmi->bss_count)
        return PeachMhiInvalid;
    const PeachWmiBss *b = &s->wmi->bss[bss_index];
    int err = peach_station_candidate(b, PeachStationWpa2Psk);
    if (err) return err;
    memset(r, 0, sizeof(*r)); r->station = s;
    for (size_t off = 0; off + 2 <= b->ie_bytes;) {
        unsigned n = b->ies[off + 1];
        if (n > b->ie_bytes - off - 2) return PeachCeProtocol;
        if (b->ies[off] == 48) {
            r->ap_rsn_bytes = n + 2; memcpy(r->ap_rsn, b->ies + off, n + 2);
        }
        off += n + 2;
    }
    if (!r->ap_rsn_bytes) return PeachMhiInvalid;
    memcpy(r->ap_bssid, b->bssid, 6); memcpy(r->pmk, pmk, 32);
    r->phase = PeachRsnPrepared;
    return 0;
}
int peach_rsn_start(PeachRsn *r)
{
    if (!r || !r->station || r->phase != PeachRsnPrepared) return PeachMhiInvalid;
    PeachStation *s = r->station;
    if (s->phase != PeachStationAwaitKeys || s->error ||
        s->security != PeachStationWpa2Psk || !s->rsn_bytes || !s->tx->bound ||
        memcmp(s->bssid, r->ap_bssid, 6)) return PeachMhiInvalid;
    r->phase = PeachRsnM1; r->deadline = now(r) + 10000000;
    return 0;
}
static int receive_security(PeachRsn *r, const PeachRxInfo *info)
{
    PeachStation *s = r->station;
    const PeachHttPeer *p = peach_htt_peer(s->htt, 0, s->bssid);
    if (memcmp(s->bssid, r->ap_bssid, 6) ||
        !p || p->generation != s->peer_generation || p->peer != info->peer ||
        memcmp(info->transmitter, s->bssid, 6)) return 0;
    if (!info->encrypted) return 1; /* only EAPOL is allowed clear below */
    if (info->multicast > 1 || info->key_index > 3) return 0;
    unsigned slot = info->multicast ? info->key_index + 1 : 0;
    if (info->tid > 16 || info->cipher != 6 || !info->decrypted ||
        (!info->multicast && info->key_index) || !info->pn || info->pn >= 0x1000000000000ull)
        return 0;
    const PeachWmiKey *key = &s->wmi->keys[slot];
    if (!key->installed) return 0;
    if (r->pn_generation[slot] != key->generation) {
        for (unsigned tid = 0; tid <= 16; tid++) r->pn[slot][tid] = key->rsc;
        r->pn_generation[slot] = key->generation;
    }
    if (info->pn <= r->pn[slot][info->tid]) return 0;
    r->pn[slot][info->tid] = info->pn; return 1;
}
int peach_rsn_receive(PeachRsn *r, const void *packet, size_t bytes, const PeachRxInfo *info)
{
    const unsigned char *p = packet;
    if (!r || !r->station || r->error || !p || !info || bytes < 14 ||
        !receive_security(r, info)) return 0;
    if ((!(p[0] & 1) && memcmp(p, r->station->wmi->mac, 6)) ||
        !!(p[0] & 1) != info->multicast) return 0;
    if (p[12] != 0x88 || p[13] != 0x8e)
        return r->phase == PeachRsnConnected && info->encrypted;
    if (bytes < 18 || memcmp(p, r->station->wmi->mac, 6) ||
        memcmp(p + 6, r->station->bssid, 6) || !p[14] || p[14] > 3 || p[15] != 3)
        return 0;
    size_t n = 18u + be16(p + 16);
    if (n < 113 || n > bytes || n > PEACH_RSN_PACKET || r->count == PEACH_RSN_QUEUE) return 0;
    unsigned i = (r->head + r->count) % PEACH_RSN_QUEUE;
    memcpy(r->packets[i], p, n); r->packet_bytes[i] = n; r->count++;
    return 0;
}
static int process(PeachRsn *r, unsigned char *e, size_t bytes)
{
    unsigned char *key = e + 4;
    unsigned info = be16(key + 1), data_bytes = be16(key + 93), pair = !!(info & 8);
    uint64_t replay = be64(key + 5);
    if (key[0] != 2 || (info & 7) != 2 || (info & 0xec30) ||
        bytes != 99u + data_bytes) return 0;
    if (info == 0x008a) {
        if (be16(key + 3) != 16 || zero(key + 13, 32) || !zero(key + 77, 16)) return 0;
        if (r->phase == PeachRsnM3 && equal(key + 13, r->anonce, 32)) {
            /* The AP may increment its counter when retrying M1. Echo that
             * counter with the existing SNonce/PTK; never roll it back. */
            if (replay < r->m1_replay) return 0;
            r->m1_replay = r->replay = replay;
            make_reply(r, e, 0x010a, 1, r->station->rsn_bytes); schedule(r, ReplyM2); return 0;
        }
        /* In-place PTK replacement is not implemented. An unsigned M1
         * after connection must not replace keys or close the data port. */
        if ((r->have_replay && replay <= r->replay) ||
            r->phase != PeachRsnM1) return 0;
        RTRY(r, peach_tx_authorize(r->station->tx, 0));
        memcpy(r->anonce, key + 13, 32); read_random(r->snonce, 32); derive_ptk(r);
        r->m1_replay = r->replay = replay; r->have_replay = 1; r->have_response = 0;
        r->phase = PeachRsnM3; r->deadline = now(r) + 10000000;
        make_reply(r, e, 0x010a, 1, r->station->rsn_bytes); schedule(r, ReplyM2); return 0;
    }
    if ((pair && info != 0x13ca) || (!pair && info != 0x1382) ||
        (pair && be16(key + 3) != 16) || !mic_valid(r, e, bytes)) return 0;
    unsigned char fingerprint[20]; digest(e, bytes, fingerprint);
    if (r->have_response && replay == r->response_replay && pair == !r->response_group &&
        equal(fingerprint, r->response_digest, 20)) {
        schedule(r, ReplyAgain); return 0; /* reuse response; never reinstall/reset PN */
    }
    unsigned repeat_m3 = pair && r->phase == PeachRsnConnected;
    if (!r->have_replay || replay <= r->replay ||
        (pair ? (r->phase != PeachRsnM3 && !repeat_m3) || !equal(key + 13, r->anonce, 32) :
                r->phase != PeachRsnConnected)) return 0;
    size_t plain_bytes;
    int err = unwrap(r, key + 95, data_bytes, &plain_bytes);
    if (!err) err = key_data(r, plain_bytes, !pair);
    if (err) return fail(r, err); /* authenticated but unsupported/inconsistent key data */
    erase(r->plain, sizeof(r->plain));
    if (repeat_m3) {
        /* A lost M4 can cause a signed M3 with a newer counter. Answer it
         * only for the installed keys; never reinstall or reset any PN. */
        const PeachWmiKey *t = &r->station->wmi->keys[0];
        const PeachWmiKey *g = &r->station->wmi->keys[r->group_index + 1];
        unsigned same = t->installed && g->installed &&
            equal(t->bytes, r->ptk + 32, 16) && equal(g->bytes, r->gtk, 16);
        erase(r->gtk, sizeof(r->gtk));
        if (!same) return 0;
    }
    uint64_t rsc = 0;
    for (unsigned i = 0; i < 8; i++) rsc |= (uint64_t)key[61 + i] << (8 * i);
    if (rsc >= 0x1000000000000ull) return fail(r, PeachCeProtocol);
    r->group_rsc = rsc; r->replay = r->response_replay = replay;
    r->have_response = 1; r->response_group = !pair;
    memcpy(r->response_digest, fingerprint, 20);
    make_reply(r, e, pair ? 0x030a : 0x0302, 0, 0);
    schedule(r, repeat_m3 ? ReplyAgain : pair ? ReplyM4 : ReplyGroup); return 0;
}
static int install(PeachRsn *r, unsigned pair)
{
    PeachStation *s = r->station;
    const PeachHttPeer *p = peach_htt_peer(s->htt, 0, s->bssid);
    if (!p || p->generation != s->peer_generation) return PeachMhiDeviceError;
    PeachWmiKey *g = &s->wmi->keys[r->group_index + 1], *t = &s->wmi->keys[0];
    unsigned group_changed = !g->installed || !equal(g->bytes, r->gtk, 16);
    r->pair_changed = pair && (!t->installed || !equal(t->bytes, r->ptk + 32, 16));
    /* RX descriptors carry the key index, not its generation. Until the
     * old RX/TX queues can be demonstrably drained, replacing an occupied
     * slot could reset the software PN window while old-key frames remain
     * queued. Fail before either install; duplicate keys still preserve PN,
     * and a GTK in a previously unused index does not replace an old slot. */
    if ((r->pair_changed && t->installed) || (group_changed && g->installed))
        return PeachMhiInvalid;
    for (unsigned i = 0; i < 2; i++) {
        unsigned changed = i ? r->pair_changed : group_changed;
        uint32_t generation = i ? t->generation : g->generation;
        if (changed && generation == 0xffffffffu) return PeachCeProtocol;
        r->expected_keys[i] = generation + changed;
    }
    if (s->wmi->htc->ops.log) s->wmi->htc->ops.log(s->wmi->htc->ops.context,
        "RSN expected WMI group/pair key generations", r->expected_keys[0], r->expected_keys[1]);
    if (pair) RTRY(r, peach_wmi_install_ccmp(s->wmi, s->htt, 0, 0, r->ptk + 32, 0));
    RTRY(r, peach_wmi_install_ccmp(s->wmi, s->htt, 1, r->group_index, r->gtk, r->group_rsc));
    erase(r->gtk, sizeof(r->gtk));
    r->phase = PeachRsnKeys; r->deadline = now(r) + 10000000; return 0;
}
int peach_rsn_poll(PeachRsn *r)
{
    if (!r || !r->station || r->phase == PeachRsnIdle || r->phase == PeachRsnPrepared) return 0;
    if (r->error) return r->error;
    PeachStation *s = r->station;
    if (s->error || s->wmi->error || s->htt->error || s->tx->error)
        return fail(r, s->error ? s->error : s->wmi->error ? s->wmi->error :
                    s->htt->error ? s->htt->error : s->tx->error);
    if (r->tx_action) {
        if (now(r) >= r->tx_deadline) return fail(r, PeachMhiTimeout);
        if (!r->tx_sent) {
            int err = peach_tx_eapol(s->tx, s->wmi->mac, r->reply, r->reply_bytes);
            if (err == PeachCeFull) return 0;
            RTRY(r, err); r->tx_sent = 1; r->tx_tries++; return 0;
        }
        if (s->tx->control_pending) return 0;
        if (!s->tx->control_done) return fail(r, PeachCeProtocol);
        unsigned action = r->tx_action;
        if ((action == ReplyM4 || action == ReplyGroup) && !s->tx->control_acked) {
            if (r->tx_tries == 3) return fail(r, PeachMhiDeviceError);
            r->tx_sent = 0; return 0;
        }
        r->tx_action = 0;
        /* M4 must actually leave the host before the new pairwise key is
         * installed. Retransmitted M3 responses take ReplyAgain above. */
        if (action == ReplyM4 || action == ReplyGroup) RTRY(r, install(r, action == ReplyM4));
    }
    /* WMI key-install waits progress RX and can observe an AP deauth. */
    if (s->error) return fail(r, s->error);
    if (r->phase == PeachRsnKeys) {
        const PeachHttPeer *p = peach_htt_peer(s->htt, 0, s->bssid);
        if (!p || p->generation != s->peer_generation) return fail(r, PeachMhiDeviceError);
        const PeachWmiKey *t = &s->wmi->keys[0], *g = &s->wmi->keys[r->group_index + 1];
        /* Native ath12k/mac and dp_set_key_sec_type use the matching WMI
         * install completion, then commit host key state. HTT SEC_IND is
         * an independent indication; this STA firmware need not send it.
         * install_ccmp advances each epoch only after its validated ACK. */
        if (s->wmi->key_pending || !t->installed || !g->installed ||
            t->generation != r->expected_keys[1] || g->generation != r->expected_keys[0] ||
            !equal(t->bytes, r->ptk + 32, 16)) return fail(r, PeachCeProtocol);
        if (s->wmi->htc->ops.log) s->wmi->htc->ops.log(s->wmi->htc->ops.context,
            "RSN WMI group/pair confirmations validated", g->generation, t->generation);
        RTRY(r, peach_wmi_authorize(s->wmi, s->htt));
        if (s->error) return fail(r, s->error);
        RTRY(r, peach_tx_authorize(s->tx, 1)); r->phase = PeachRsnConnected;
    }
    if (r->phase != PeachRsnConnected && now(r) >= r->deadline) return fail(r, PeachMhiTimeout);
    if (r->count) {
        unsigned i = r->head;
        int err = process(r, r->packets[i] + 14, r->packet_bytes[i] - 14);
        erase(r->packets[i], r->packet_bytes[i]); r->head = (i + 1) % PEACH_RSN_QUEUE; r->count--;
        if (err) return err;
    }
    return 0;
}
void peach_rsn_clear(PeachRsn *r)
{
    if (!r) return;
    if (r->station) (void)peach_tx_authorize(r->station->tx, 0);
    erase(r, sizeof(*r));
}
