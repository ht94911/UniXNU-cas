/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal single-station WMI initialization. Wire offsets/tags are from
 * fw-api/fw/{wmi.h,wmi_unified.h,wmi_tlv_defs.h,wmi_services.h,wlan_defs.h}
 * in sm8850-modules fdd80ae0d6665f8a018c3e06a6152bafdaf1269d.
 * Resources follow wlan_tgt_def_config.h with one vdev and eight peers.
 * No capability is inferred from a command send or a successful HTC setup. */
#include "PeachWmi.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif
enum { TAG_U32 = 16, TAG_ARRAY = 18, TAG_SERVICE = 32, TAG_MEMORY = 34,
       TAG_READY = 35, TAG_INIT = 74, TAG_RESOURCES = 75, TAG_CHUNK = 76,
       TAG_EXT = 428, TAG_AVAILABLE = 559, TAG_EXT2 = 820,
       TAG_MAC_PHY = 528, TAG_HW_CAP = 529, TAG_SOC_CAP = 530,
       TAG_HAL_REG = 531,
       TAG_SCAN_EVENT = 36, TAG_MGMT = 44, TAG_SCAN = 77, TAG_CHANNELS = 79, TAG_CHANNEL = 80,
       TAG_VDEV = 86, TAG_STREAMS = 411, TAG_RULE = 608, TAG_REG = 609,
       TAG_RULE_EXT = 937, TAG_REG_EXT = 938,
       TAG_PEER_CREATE = 97, TAG_PEER_DELETE = 98, TAG_PEER_QUEUE = 549,
       TAG_PEER_CONFIRM = 868, TAG_PEER_DELETED = 451,
       TAG_START = 88, TAG_STARTED = 40, TAG_STOP = 93, TAG_STOPPED = 41,
       TAG_MGMT_TX = 422, TAG_MGMT_DONE = 423, TAG_TX_PARAMS = 644,
       TAG_ASSOC = 101, TAG_ASSOC_CONF = 434, TAG_VHT_RATES = 102, TAG_UP = 92,
       TAG_KEY = 96, TAG_KEY_DONE = 42,
       TAG_FW_READY = 1321, TAG_MODE = 1322, TAG_MODE_RESPONSE = 1323,
       RESOURCE_WORDS = 88, VDEVS = 1, PEERS = 8 };
enum { MGMT_RX_ENTRIES = 128, MGMT_RX_BYTES = 3520, MGMT_RX_BUFFERS = 1152,
       MGMT_RX_STORAGE = MGMT_RX_BUFFERS + MGMT_RX_ENTRIES * MGMT_RX_BYTES };
static const uint32_t host_abi[6] = {0x01000000, 1822, 0x5f414351, 0x4c4d, 0, 0};
#define WTRY(w, expr) do { int err_ = (expr); if (err_) { (w)->error = err_; return err_; } } while (0)
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void p32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static void tlv(unsigned char *p, unsigned tag, unsigned bytes) { p32(p, tag << 16 | bytes); }
static int bad(PeachWmi *w) { return w->error = PeachCeProtocol; }
static PeachWmiPeer *peer_find(PeachWmi *w, const unsigned char mac[6])
{
    for (unsigned i = 0; i < PEACH_WMI_PEERS; i++)
        if (w->peers[i].used && !memcmp(w->peers[i].mac, mac, 6)) return &w->peers[i];
    return NULL;
}
static void peer_mac(unsigned char *p, const unsigned char mac[6])
{
    p32(p, (uint32_t)mac[0] | (uint32_t)mac[1] << 8 |
        (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24);
    p32(p + 4, (uint32_t)mac[4] | (uint32_t)mac[5] << 8);
}
static void report(PeachWmi *w, const char *s, uint32_t a, uint32_t b)
{ if (w->ops.log) w->ops.log(w->ops.context, s, a, b); }
static unsigned service(PeachWmi *w, unsigned id)
{ return id < 1024 && ((w->services[id / 32] >> (id & 31)) & 1); }
static void add_service(PeachWmi *w, unsigned id)
{ if (id < 1024) w->services[id / 32] |= 1u << (id & 31); }
static int abi_compatible(const unsigned char *p)
{
    for (unsigned i = 0; i < 6; i++)
        if (i != 1 && u32(p + 4 * i) != host_abi[i]) return 0;
    return 1; /* vendor compares namespace + major/minor, ignores revision */
}
static int tlvs_valid(const unsigned char *p, size_t n)
{
    while (n) {
        if (n < 4) return 0;
        unsigned len = u32(p) & 0xffff;
        /* ARRAY_BYTE and later headers need not be word aligned. The
         * vendor helper advances by header+length; all loads use memcpy. */
        if (len > n - 4) return 0;
        p += 4 + len; n -= 4 + len;
    }
    return 1;
}
int peach_wmi_init(PeachWmi *w, PeachHtc *h, const PeachWmiOps *ops)
{
    if (!w || !h || !ops || !ops->allocate || !ops->barrier ||
        !ops->iova_bytes || ops->iova_base >= 0x100000000ull ||
        ops->iova_bytes > 0x100000000ull - ops->iova_base) return PeachMhiInvalid;
    memset(w, 0, sizeof(*w)); w->htc = h; w->ops = *ops;
    return 0;
}
static int service_ready(PeachWmi *w, const unsigned char *p, size_t n)
{
    unsigned stage = 0, count = 0, seen_memory = 0;
    if (w->service_ready) return bad(w);
    while (n) {
        unsigned tag = u32(p) >> 16, len = u32(p) & 0xffff;
        if (!stage) {
            if (tag != TAG_SERVICE || len < 128 || !abi_compatible(p + 8)) return bad(w);
            for (unsigned i = 0; i < 6; i++) w->firmware_abi[i] = u32(p + 8 + i * 4);
            w->chains = u32(p + 40); w->max_fragments = u32(p + 36);
            count = u32(p + 76);
            if (!w->chains || w->chains > 8 || count > PEACH_WMI_MEMORY_REQUESTS) return bad(w);
            w->requests = count;
        } else if (stage == 2) {
            if (tag != TAG_U32 || len != 128) return bad(w);
            /* Legacy service bitmap uses FOUR bits per u32, as specified
             * by WMI_SERVICE_IS_ENABLED; extended bitmaps use 32. */
            for (unsigned i = 0; i < 128; i++)
                if (u32(p + 4 + (i / 4) * 4) & (1u << (i & 3))) add_service(w, i);
        } else if (stage == 3) {
            if (tag != TAG_ARRAY || len != count * 20) return bad(w);
            for (unsigned i = 0; i < count; i++) {
                const unsigned char *v = p + 4 + i * 20;
                if (u32(v) != (TAG_MEMORY << 16 | 16)) return bad(w);
                PeachWmiMemory *m = &w->memory[i];
                m->id = u32(v + 4); m->unit_size = u32(v + 8);
                m->flags = u32(v + 12); m->units = u32(v + 16);
                if (!m->unit_size || m->flags & ~15u) return bad(w);
                for (unsigned j = 0; j < i; j++) if (w->memory[j].id == m->id) return bad(w);
            }
            seen_memory = 1;
        }
        p += len + 4; n -= len + 4; stage++;
    }
    if (!seen_memory) return bad(w);
    w->service_ready = 1;
    report(w, "WMI SERVICE_READY chains/memory", w->chains, w->requests);
    return 0;
}
static int radio_capabilities(PeachWmi *w, const unsigned char *p, size_t n)
{
    struct Mode { uint32_t id, phys, type, seen; } modes[32];
    unsigned stage = 0, count = 0, used = 0, seen = 0;
    PeachWmiRadio selected = {0};
    while (n) {
        unsigned tag = u32(p) >> 16, len = u32(p) & 0xffff;
        if (stage == 1) {
            if (tag != TAG_SOC_CAP || len < 8) return bad(w);
            count = u32(p + 4);
            if (!count || count > 32) return bad(w);
        } else if (stage == 2) {
            if (tag != TAG_ARRAY || !tlvs_valid(p + 4, len)) return bad(w);
            for (size_t off = 4; off < len + 4;) {
                const unsigned char *v = p + off;
                unsigned bytes = u32(v) & 0xffff;
                if (u32(v) >> 16 != TAG_HW_CAP || bytes < 12 || used >= count) return bad(w);
                struct Mode *m = &modes[used];
                m->id = u32(v + 4); m->phys = u32(v + 8); m->type = u32(v + 12); m->seen = 0;
                if (!m->phys) return bad(w);
                for (unsigned j = 0; j < used; j++) if (modes[j].id == m->id) return bad(w);
                used++; off += 4 + bytes;
            }
            if (used != count) return bad(w);
        } else if (stage == 3) {
            if (tag != TAG_ARRAY || !tlvs_valid(p + 4, len)) return bad(w);
            for (size_t off = 4; off < len + 4;) {
                const unsigned char *v = p + off;
                unsigned bytes = u32(v) & 0xffff, i;
                /* Fixed ABI: 60 words including tag; two embedded 10-word
                 * PPE structures precede lmac_id at word 50. No compact ABI. */
                if (u32(v) >> 16 != TAG_MAC_PHY || bytes < 236) return bad(w);
                unsigned id = u32(v + 4), phy = u32(v + 12);
                for (i = 0; i < used && modes[i].id != id; i++) {}
                if (i == used || phy >= 32 || !(modes[i].phys & (1u << phy)) ||
                    (modes[i].seen & (1u << phy))) return bad(w);
                modes[i].seen |= 1u << phy;
                /* Explicit single-PHY mode, advertised by this firmware.
                 * Ignore multi-radio/MLO modes rather than guessing mapping. */
                if ((modes[i].type & 0x7ffffffu) == 0 &&
                    !(modes[i].phys & (modes[i].phys - 1))) {
                    PeachWmiRadio c = {0};
                    c.mode = id; c.pdev = u32(v + 8) & 0xffff; c.phy = phy;
                    c.lmac = u32(v + 200); c.bands = u32(v + 20);
                    c.low_2g = u32(v + 220); c.high_2g = u32(v + 224);
                    c.low_5g = u32(v + 228); c.high_5g = u32(v + 232);
                    c.tx_chains = ((c.bands & 1) ? u32(v + 52) : 0) |
                                  ((c.bands & 2) ? u32(v + 84) : 0);
                    c.rx_chains = ((c.bands & 1) ? u32(v + 56) : 0) |
                                  ((c.bands & 2) ? u32(v + 88) : 0);
                    if (!c.pdev || c.pdev > 3 || c.phy >= 3 || c.lmac > 2 ||
                        !c.bands || c.bands & ~3u || !c.tx_chains || !c.rx_chains ||
                        (c.tx_chains | c.rx_chains) & ~0xffu)
                        return bad(w);
                    c.valid = 1;
                    if (!selected.valid || (c.bands == 3 && selected.bands != 3)) selected = c;
                }
                off += bytes + 4;
            }
            for (unsigned i = 0; i < used; i++)
                if (modes[i].seen != modes[i].phys) return bad(w);
            seen = 1;
        } else if (stage == 5) {
            if (tag != TAG_ARRAY || !tlvs_valid(p + 4, len)) return bad(w);
            unsigned matched = 0;
            for (size_t off = 4; off < len + 4;) {
                const unsigned char *v = p + off;
                unsigned bytes = u32(v) & 0xffff;
                if (u32(v) >> 16 != TAG_HAL_REG || bytes < 40) return bad(w);
                if (selected.valid && u32(v + 4) == selected.phy) {
                    if (matched++) return bad(w);
                    /* Peach leaves the per-MAC ranges unset. Use its real
                     * HAL ranges for the same PHY; later scan filtering
                     * still intersects the firmware's regulatory rules. */
                    if (!selected.low_2g && !selected.high_2g) {
                        selected.low_2g = u32(v + 28); selected.high_2g = u32(v + 32);
                    }
                    if (!selected.low_5g && !selected.high_5g) {
                        selected.low_5g = u32(v + 36); selected.high_5g = u32(v + 40);
                    }
                }
                off += bytes + 4;
            }
        }
        p += len + 4; n -= len + 4; stage++;
    }
    if (!seen || !selected.valid ||
        ((selected.bands & 1) && (!selected.low_2g || selected.low_2g > selected.high_2g)) ||
        ((selected.bands & 2) && (!selected.low_5g || selected.low_5g > selected.high_5g)))
        return bad(w);
    w->radio = selected;
    report(w, "WMI single radio mode/pdev", selected.mode, selected.pdev);
    report(w, "WMI single radio phy/lmac", selected.phy, selected.lmac);
    report(w, "WMI PHY 2GHz low/high MHz", selected.low_2g, selected.high_2g);
    report(w, "WMI PHY 5GHz low/high MHz", selected.low_5g, selected.high_5g);
    return 0;
}
static int regulatory_event(PeachWmi *w, const unsigned char *p, size_t n, unsigned extended)
{
    unsigned len = u32(p) & 0xffff;
    if (len < 56 || (u32(p) >> 16) != (extended ? TAG_REG_EXT : TAG_REG)) return bad(w);
    unsigned phy = u32(p + 8);
    if (phy >= 3) { report(w, "WMI regulatory untracked PHY", phy, u32(p + 4)); return 0; }
    PeachWmiRegDomain *r = &w->regulatory[phy];
    PeachWmiRegDomain next = {0};
    next.status = u32(p + 4);
    if (next.status) {
        r->valid = 0; r->status = next.status;
        if (r->generation == UINT32_MAX) return bad(w);
        r->generation++;
        return 0;
    }
    unsigned n2 = u32(p + 52), n5 = u32(p + 56);
    if (n2 > PEACH_WMI_REG_RULES || n5 > PEACH_WMI_REG_RULES - n2) return bad(w);
    next.alpha2 = u32(p + 12); next.country = u32(p + 20);
    next.domain = u32(p + 24); next.dfs = u32(p + 28);
    size_t off = len + 4;
    if (n - off < 4 || u32(p + off) >> 16 != TAG_ARRAY) return bad(w);
    unsigned array_bytes = u32(p + off) & 0xffff;
    if (array_bytes > n - off - 4 || !tlvs_valid(p + off + 4, array_bytes)) return bad(w);
    const unsigned char *v = p + off + 4;
    unsigned count = 0;
    for (size_t left = array_bytes; left;) {
        unsigned bytes = u32(v) & 0xffff;
        if (u32(v) >> 16 != (extended ? TAG_RULE_EXT : TAG_RULE) ||
            bytes < (extended ? 16u : 12u)) return bad(w);
        if (count < n2 + n5) {
            PeachWmiRegRule *rule = &next.rules[count];
            rule->frequencies = u32(v + 4); rule->bandwidth_power = u32(v + 8);
            rule->flags = u32(v + 12);
            unsigned low = rule->frequencies & 0xffff, high = rule->frequencies >> 16;
            if (!low || low >= high || !(rule->bandwidth_power & 0xffff)) return bad(w);
        }
        count++; v += bytes + 4; left -= bytes + 4;
    }
    if (count < n2 + n5 || (!extended && count != n2 + n5)) return bad(w);
    /* Repeated identical domain notifications do not revoke an active
     * scan/association. Only a real rule change advances its generation. */
    if (r->valid && r->alpha2 == next.alpha2 && r->country == next.country &&
        r->domain == next.domain && r->dfs == next.dfs &&
        r->count_2g == n2 && r->count_5g == n5 &&
        !memcmp(r->rules, next.rules, (n2 + n5) * sizeof(next.rules[0]))) return 0;
    if (r->generation == UINT32_MAX) return bad(w);
    next.count_2g = n2; next.count_5g = n5; next.valid = 1;
    next.generation = r->generation + 1; *r = next;
    report(w, "WMI real regulatory PHY/country", phy, r->alpha2 & 0xffff);
    return 0;
}
static int scan_event(PeachWmi *w, const unsigned char *p)
{
    if (u32(p) >> 16 != TAG_SCAN_EVENT || (u32(p) & 0xffff) < 24) return bad(w);
    /* A late event belongs to its original request. Do not complete a new
     * scan with it, or turn a stale completion into a transport failure. */
    if (!w->scan_active || !w->scan_sent || u32(p + 16) != w->scan_requestor ||
        u32(p + 20) != w->scan_id || u32(p + 24) != 0 || w->scan_finished) return 0;
    unsigned event = u32(p + 4), reason = u32(p + 8);
    w->scan_event = event; w->scan_reason = reason;
    if (event == 1) {
        if (w->scan_started) return bad(w);
        w->scan_started = 1;
    } else if (event == 2) {
        if (!reason && !w->scan_started) return bad(w);
        w->scan_done = !reason; w->scan_finished = 1;
    } else if (event == 0x10 || event == 0x40) {
        /* DEQUEUED / START_FAILED terminate this request. PREEMPTED may
         * resume and still needs its real terminal event or timeout. */
        w->scan_finished = 1;
    }
    if (event == 8) report(w, "WMI scan channel MHz/reason", u32(p + 12), reason);
    else report(w, "WMI real scan event/reason", event, reason);
    return 0;
}
static void management_event(PeachWmi *w, const unsigned char *p, size_t n)
{
    unsigned len = u32(p) & 0xffff;
    if (u32(p) >> 16 != TAG_MGMT || len < 68) { bad(w); return; }
    size_t off = len + 4;
    unsigned bytes = u32(p + 20), frequency = u32(p + 68);
    if (n - off < 4 || u32(p + off) >> 16 != 17 ||
        bytes > (u32(p + off) & 0xffff)) { bad(w); return; }
    static unsigned reported;
    if (reported < 3) {
        reported++;
        report(w, "WMI RX mgmt status/pdev", u32(p + 24), u32(p + 64));
        report(w, "WMI RX mgmt frequency/bytes", frequency, bytes);
    }
    if (u32(p + 24) || u32(p + 64) != w->radio.pdev || bytes < 24) return;
    const unsigned char *frame = p + off + 4;
    unsigned fc = u16(frame);
    if (fc & 0x030f) return;
    if (w->vdev_started && !w->vdev_stop_sent && frequency == w->vdev_frequency &&
        !memcmp(frame + 16, w->vdev_bssid, 6) && !memcmp(frame + 10, w->vdev_bssid, 6) &&
        (!memcmp(frame + 4, w->mac, 6) || (frame[4] & 1))) {
        unsigned subtype = fc & 0xfc;
        if (subtype == 0xb0 || subtype == 0x10 || subtype == 0x30 ||
            subtype == 0xa0 || subtype == 0xc0) {
            /* These are on-air packets, not firmware control responses.
             * This legacy STA implements no action/PMF/fragment path. An
             * unsupported or malformed frame must not poison the WMI
             * transport, nor may ordinary RX pressure stop the radio. */
            unsigned response = subtype == 0xb0 || subtype == 0x10 || subtype == 0x30;
            if ((fc & 0x4400) || (u16(frame + 22) & 15) ||
                bytes < (response ? 30u : 26u) || bytes > PEACH_WMI_MGMT_BYTES ||
                (response && memcmp(frame + 4, w->mac, 6))) return;
            if (w->mgmt_count == PEACH_WMI_MGMT_FRAMES) { w->mgmt_dropped++; return; }
            PeachWmiMgmt *m = &w->mgmt[(w->mgmt_head + w->mgmt_count) % PEACH_WMI_MGMT_FRAMES];
            m->bytes = bytes; m->frequency = frequency; m->rssi = (int32_t)u32(p + 48);
            memcpy(m->frame, frame, bytes); w->mgmt_count++;
        }
    }
    if (!w->scan_active || !w->scan_sent || w->scan_finished || bytes < 36) return;
    unsigned i;
    for (i = 0; i < w->channel_count && w->channels[i].frequency != frequency; i++) {}
    if (i == w->channel_count) return;
    if ((fc & 3) || ((fc & 0xfc) != 0x80 && (fc & 0xfc) != 0x50) ||
        (frame[16] & 1) || !memcmp(frame + 16, "\0\0\0\0\0\0", 6)) return;
    const unsigned char *ssid = NULL;
    unsigned ssid_bytes = 0, dtim_period = 0;
    for (size_t pos = 36; pos < bytes;) {
        if (bytes - pos < 2 || frame[pos + 1] > bytes - pos - 2) return;
        unsigned size = frame[pos + 1];
        if (!frame[pos]) {
            if (ssid || size > 32) return;
            ssid = frame + pos + 2; ssid_bytes = size;
        }
        if (frame[pos] == 5 && size >= 4) dtim_period = frame[pos + 3];
        pos += 2 + size;
    }
    if (!ssid) return;
    if (w->scan_target_bytes && (ssid_bytes != w->scan_target_bytes ||
        memcmp(ssid, w->scan_target_ssid, ssid_bytes) ||
        (w->scan_target_bssid_valid && memcmp(frame + 16, w->scan_target_bssid, 6)))) return;
    int rssi = (int32_t)u32(p + 48);
    if (rssi < -127 || rssi > 0) rssi = -128;
    for (i = 0; i < w->bss_count && memcmp(w->bss[i].bssid, frame + 16, 6); i++) {}
    if (i == w->bss_count) {
        if (i == PEACH_WMI_BSS_ENTRIES) {
            /* A full cache must not retain only the first scanned
             * channels and discard a nearby AP seen later in the sweep. */
            unsigned weakest = 0;
            for (unsigned j = 1; j < w->bss_count; j++)
                if (w->bss[j].rssi < w->bss[weakest].rssi) weakest = j;
            w->bss_dropped++;
            if (rssi <= w->bss[weakest].rssi) return;
            i = weakest;
        } else w->bss_count++;
        memset(&w->bss[i], 0, sizeof(w->bss[i]));
    }
    PeachWmiBss *b = &w->bss[i];
    b->seen_us = w->htc->ops.now_us(w->htc->ops.context);
    b->frequency = frequency; b->rssi = rssi;
    memcpy(b->bssid, frame + 16, 6);
    if (ssid_bytes || !b->ssid_bytes) {
        b->ssid_bytes = ssid_bytes; memcpy(b->ssid, ssid, ssid_bytes);
    }
    b->beacon_interval = u16(frame + 32); b->capability = u16(frame + 34);
    if (dtim_period) b->dtim_period = dtim_period;
    b->ies_truncated = bytes - 36 > sizeof(b->ies);
    b->ie_bytes = b->ies_truncated ? sizeof(b->ies) : bytes - 36;
    memcpy(b->ies, frame + 36, b->ie_bytes);
}
void peach_wmi_receive(PeachWmi *w, const void *packet, size_t n)
{
    const unsigned char *p = packet;
    if (!w || w->error) return;
    if (!p || n < 8 || !tlvs_valid(p + 4, n - 4)) { bad(w); return; }
    unsigned id = u32(p) & 0xffffff;
    p += 4; n -= 4;
    unsigned tag = u32(p) >> 16, len = u32(p) & 0xffff;
    w->last_event = id; w->last_tag = tag;
    switch (id) {
    case 0x4002: /* WMI_CHAN_INFO_EVENTID: initial scan RF evidence */
        if (tag != 38 || len < 56) { bad(w); return; }
        if (w->scan_id == 0xa001) {
            report(w, "WMI channel MHz/error", u32(p + 8), u32(p + 4));
            report(w, "WMI channel RX frames/clear", u32(p + 36), u32(p + 20));
            report(w, "WMI channel cycles/noise", u32(p + 24), u32(p + 16));
        }
        break;
    case 0x1d004: { /* WMI_UPDATE_STATS_EVENTID, legacy PDEV base prefix */
        if (tag != 70 || len < 32) { bad(w); return; }
        report(w, "WMI radio stats pdev/count", u32(p + 32), u32(p + 8));
        size_t off = len + 4;
        if (!(u32(p + 4) & 4) || !u32(p + 8)) break;
        if (n - off < 32 || u32(p + off) >> 16 != 17 ||
            (u32(p + off) & 0xffff) < 28) { bad(w); return; }
        const unsigned char *base = p + off + 4;
        report(w, "WMI radio RX frames/cycles", u32(base + 8), u32(base + 16));
        report(w, "WMI radio noise/PHY errors", u32(base), u32(base + 20));
        break;
    }
    case 0x5003: /* WMI_VDEV_INSTALL_KEY_COMPLETE_EVENTID */
        if (tag != TAG_KEY_DONE || len < 24 || !w->key_pending || w->key_done ||
            u32(p + 4) || memcmp(p + 8, w->vdev_bssid, 6) ||
            u32(p + 16) != w->key_index || (u32(p + 20) & 1) != w->key_group ||
            (u32(p + 20) & ~0x31u) || u32(p + 24)) { bad(w); return; }
        report(w, "WMI key confirm index/flags", u32(p + 16), u32(p + 20));
        w->key_done = 1;
        break;
    case 0x6006: { /* WMI_PEER_ASSOC_CONF_EVENTID */
        if (tag != TAG_ASSOC_CONF || len < 16 || u32(p + 4) || u32(p + 16)) { bad(w); return; }
        PeachWmiPeer *s = peer_find(w, p + 8);
        if (!s || !s->assoc_sent || s->assoc_confirmed || s->delete_sent) { bad(w); return; }
        s->assoc_confirmed = 1;
        break;
    }
    case 0x5001: /* WMI_VDEV_START_RESP_EVENTID */
        if (tag == TAG_STARTED && len >= 36) {
            report(w, "WMI vdev start status/pdev", u32(p + 16), u32(p + 28));
            report(w, "WMI vdev start vdev/requestor", u32(p + 4), u32(p + 8));
            report(w, "WMI vdev start response/expected requestor", u32(p + 12), w->vdev_request);
            report(w, "WMI vdev start TX/RX streams", u32(p + 32), u32(p + 36));
        }
        if (tag != TAG_STARTED || len < 36 || !w->vdev_start_sent || w->vdev_started ||
            u32(p + 4) || u32(p + 8) != w->vdev_request || u32(p + 12) || u32(p + 16) ||
            u32(p + 28) != w->radio.pdev || u32(p + 32) != 1 || u32(p + 36) != 1) {
            bad(w); return;
        }
        w->vdev_started = 1;
        break;
    case 0x5002: /* WMI_VDEV_STOPPED_EVENTID */
        if (tag != TAG_STOPPED || len < 4 || u32(p + 4) || !w->vdev_stop_sent || w->vdev_stopped) {
            bad(w); return;
        }
        w->vdev_stopped = 1; w->vdev_started = 0;
        break;
    case 0x7006: /* WMI_MGMT_TX_COMPLETION_EVENTID */
        if (tag != TAG_MGMT_DONE || len < 12 || !w->mgmt_in_flight || w->mgmt_done ||
            u32(p + 4) != w->mgmt_id || u32(p + 12) != w->radio.pdev || u32(p + 8) > 3) {
            bad(w); return;
        }
        w->mgmt_status = u32(p + 8); w->mgmt_done = 1; w->mgmt_in_flight = 0;
        break;
    case 0x601a: /* WMI_PEER_CREATE_CONF_EVENTID */
    case 0x6007: { /* WMI_PEER_DELETE_RESP_EVENTID */
        if (tag != (id == 0x601a ? TAG_PEER_CONFIRM : TAG_PEER_DELETED) ||
            len < 16 || u32(p + 4)) { bad(w); return; }
        PeachWmiPeer *s = peer_find(w, p + 8);
        if (!s || !s->create_sent || u32(p + 16)) { bad(w); return; }
        if (id == 0x601a) {
            if (s->create_confirmed) { bad(w); return; }
            s->create_confirmed = 1;
        } else {
            if (!s->delete_sent || s->delete_response) { bad(w); return; }
            s->delete_response = 1;
        }
        break;
    }
    case 0x3a001: (void)regulatory_event(w, p, n, 0); break;
    case 0x3a003: (void)regulatory_event(w, p, n, 1); break;
    case 0x3001: (void)scan_event(w, p); break;
    case 0x7001: management_event(w, p, n); break;
    case 1: (void)service_ready(w, p, n); break;
    case 2: {
        if (!w->phase || tag != TAG_READY || len < 36 || !abi_compatible(p + 4) || u32(p + 36)) {
            report(w, "WMI READY rejected/tag", len >= 36 ? u32(p + 36) : 0xffffffffu, tag);
            bad(w); return;
        }
        uint32_t lo = u32(p + 28), hi = u32(p + 32);
        for (unsigned i = 0; i < 4; i++) w->mac[i] = lo >> (i * 8);
        w->mac[4] = hi; w->mac[5] = hi >> 8;
        if ((lo & 1) || (!lo && !hi) || w->ready) { bad(w); return; }
        w->ready = 1;
        report(w, "WMI READY real MAC", lo, hi & 0xffff);
        break;
    }
    case 3: {
        if (tag != TAG_AVAILABLE || len < 20) { bad(w); return; }
        uint32_t offset = u32(p + 4);
        if ((offset & 31) || offset > 896) { bad(w); return; }
        for (unsigned i = 0; i < 128; i++)
            if (u32(p + 8 + (i / 32) * 4) & (1u << (i & 31))) add_service(w, offset + i);
        /* The following UINT32 TLV continues at segment_offset + 128.
         * Native Peach advertises MGMT_SRNG (420) here, not in the fixed
         * 128..255 bitmap. Ignoring this array silently loses that path. */
        size_t next = len + 4;
        unsigned base = offset + 128;
        while (next < n) {
            unsigned header = u32(p + next), bytes = header & 0xffff;
            if (header >> 16 != TAG_U32 || (bytes & 3) ||
                bytes > n - next - 4 || bytes / 4 > (1024 - base) / 32) {
                bad(w); return;
            }
            for (unsigned i = 0; i < bytes / 4; i++) {
                uint32_t bits = u32(p + next + 4 + i * 4);
                for (unsigned bit = 0; bit < 32; bit++)
                    if (bits & (1u << bit)) add_service(w, base + i * 32 + bit);
            }
            base += bytes * 8;
            next += bytes + 4;
        }
        report(w, "WMI extended capabilities through/MGMT_SRNG", base, service(w, 420));
        break;
    }
    case 5:
        if (tag != TAG_FW_READY || w->mode_required || w->service_ready) { bad(w); return; }
        w->mode_required = 1;
        break;
    case 6:
        if (tag != TAG_MODE_RESPONSE || len < 4 || !w->mode_sent || u32(p + 4)) { bad(w); return; }
        w->mode_accepted = 1;
        break;
    case 0x4009:
        if (tag != TAG_EXT || w->ext_ready) { bad(w); return; }
        if (radio_capabilities(w, p, n)) return;
        w->ext_ready = 1;
        report(w, "WMI SERVICE_READY_EXT", len, 0);
        break;
    case 0x4022:
        if (tag != TAG_EXT2 || len < 32 || w->ext2_ready) { bad(w); return; }
        w->peer_metadata_version = u32(p + 32) & 3;
        w->ext2_ready = 1;
        report(w, "WMI SERVICE_READY_EXT2", len, 0);
        break;
    default:
        if (w->unknown_events++ < 8) report(w, "WMI pending event", id, (uint32_t)n);
        break;
    }
}
static int progress(PeachWmi *w)
{
    WTRY(w, peach_htc_poll(w->htc));
    return w->error;
}
static int send_command(PeachWmi *w, unsigned id, size_t n)
{
    if (w->error) return w->error;
    if (n < 4 || n > sizeof(w->command)) return PeachMhiInvalid;
    p32(w->command, id);
    uint64_t end = w->htc->ops.now_us(w->htc->ops.context) + 10000000;
    int err;
    while ((err = peach_htc_send(w->htc, 0x100, w->command, n)) == PeachCeFull) {
        WTRY(w, progress(w));
        if (w->htc->ops.now_us(w->htc->ops.context) >= end) return w->error = PeachMhiTimeout;
        w->htc->ops.sleep_us(w->htc->ops.context, 1000);
    }
    return err;
}
static int wait_flag(PeachWmi *w, const unsigned *flag)
{
    if (w->error) return w->error;
    uint64_t end = w->htc->ops.now_us(w->htc->ops.context) + 10000000;
    while (!*flag) {
        WTRY(w, progress(w));
        if (w->mode_required && !w->mode_sent) {
            memset(w->command, 0, 12); tlv(w->command + 4, TAG_MODE, 4);
            w->mode_sent = 1;
            WTRY(w, send_command(w, 2, 12));
        }
        if (w->htc->ops.now_us(w->htc->ops.context) >= end) return w->error = PeachMhiTimeout;
        if (!*flag) w->htc->ops.sleep_us(w->htc->ops.context, 1000);
    }
    return 0;
}
static int allocate_memory(PeachWmi *w)
{
    for (unsigned i = 0; i < w->requests; i++) {
        PeachWmiMemory *m = &w->memory[i];
        unsigned units = m->units;
        if (m->flags & 1) units = VDEVS;
        if (m->flags & 6) units = PEERS + 1; /* vendor includes target self peer */
        uint64_t unit = ((uint64_t)m->unit_size + 3) & ~3ull;
        uint64_t bytes = unit * units;
        if (!bytes || bytes > 0x2000000 || bytes > w->ops.iova_bytes) return bad(w);
        WTRY(w, w->ops.allocate(w->ops.context, (size_t)bytes, &m->dma));
        if (!m->dma.bytes || m->dma.capacity < bytes || m->dma.iova < w->ops.iova_base ||
            m->dma.iova - w->ops.iova_base >= w->ops.iova_bytes ||
            bytes > w->ops.iova_bytes - (m->dma.iova - w->ops.iova_base)) return bad(w);
        report(w, "WMI host memory id/bytes", m->id, (uint32_t)bytes);
    }
    if (service(w, 420)) {
        WTRY(w, w->ops.allocate(w->ops.context, MGMT_RX_STORAGE, &w->mgmt_rx_dma));
        PeachMhiDma d = w->mgmt_rx_dma;
        if (!d.bytes || d.capacity < MGMT_RX_STORAGE || (d.iova & 63) ||
            d.iova < w->ops.iova_base || d.iova - w->ops.iova_base >= w->ops.iova_bytes ||
            MGMT_RX_STORAGE > w->ops.iova_bytes - (d.iova - w->ops.iova_base)) return bad(w);
        memset(d.bytes, 0, MGMT_RX_STORAGE);
    }
    w->ops.barrier(w->ops.context);
    return 0;
}
static size_t build_init(PeachWmi *w)
{
    memset(w->command, 0, sizeof(w->command));
    unsigned char *p = w->command + 4;
    tlv(p, TAG_INIT, 28);
    for (unsigned i = 0; i < 6; i++) p32(p + 4 + i * 4, host_abi[i]);
    p32(p + 28, w->requests);
    p += 32;
    tlv(p, TAG_RESOURCES, RESOURCE_WORDS * 4 - 4);
    /* Indices include the TLV header. Appended optional fields stay zero;
     * no MLO, NAN, TDLS, IPA, monitor or accelerator features are enabled. */
#define R(word, value) p32(p + 4 * (word), (value))
    R(1, VDEVS); R(2, PEERS); R(3, VDEVS + 1); R(4, 1); R(5, 2);
    R(6, 2 * (PEERS + VDEVS + 2)); R(7, 16);
    R(8, w->radio.tx_chains); R(9, w->radio.rx_chains);
    R(10, 100); R(11, 100); R(12, 100); R(13, 40);
    R(14, 2); R(15, 1); R(16, 1); /* Ethernet decap, one scan, beacon miss */
    R(29, 256); R(30, w->max_fragments > 10 ? 10 : w->max_fragments);
    R(44, 1u << 30 | 1u << 10); /* IPA disabled; native 20-bit TX MSDU IDs */
    R(54, 1); R(63, 1); R(81, 1); /* pdev IDs, single STA/active vdev */
    /* Native EAPOL uses TCL TX_NOTIFY=3. Its firmware path must first be
     * enabled by NOTIFY_FRAME_CONFIG_ENABLE (flags2 bit14), as in the
     * captured native INIT (0x013040e0) and wmi_copy_resource_config.
     * WBM FW completions are handled by PeachTx. */
    /* The firmware's RX peer-metadata helpers are also used by its EAPOL
     * TX path. V0 initializes no ML-bit getter; our RDDM records PC=0 at
     * the indirect call through 0x015598b4. Native init_cmd_api.c selects
     * V1A=2 when service365 is advertised; the phone's INIT has bits5:4=2.
     * This changes the descriptor ABI, not our single-link association. */
    R(68, w->peer_metadata_version << 4 | 1u << 14 |
        (w->mgmt_rx_dma.bytes ? 1u << 21 : 0));
    report(w, "WMI INIT flags1/flags2", u32(p + 44 * 4), u32(p + 68 * 4));
    if (service(w, 143)) R(57, 0x22); /* allow V2 map/unmap; legacy still legal */
#undef R
    p += RESOURCE_WORDS * 4;
    tlv(p, TAG_ARRAY, w->requests * 20); p += 4;
    for (unsigned i = 0; i < w->requests; i++, p += 20) {
        PeachWmiMemory *m = &w->memory[i];
        tlv(p, TAG_CHUNK, 16); p32(p + 4, m->id); p32(p + 8, (uint32_t)m->dma.iova);
        p32(p + 12, (uint32_t)m->dma.capacity); p32(p + 16, 0);
    }
    /* The selected mode is SINGLE. Vendor init_deinit_send_init_cmd
     * deliberately omits the optional HW_MODE/band-to-MAC TLVs for SINGLE
     * because firmware does not support that override in this mode. */
    return (size_t)(p - w->command);
}
int peach_wmi_wait_services(PeachWmi *w)
{
    if (!w || !w->htc || !w->htc->started || w->phase) return PeachMhiInvalid;
    if (w->error) return w->error;
    WTRY(w, wait_flag(w, &w->service_ready));
    if (w->mode_required) WTRY(w, wait_flag(w, &w->mode_accepted));
    if (service(w, 77)) WTRY(w, wait_flag(w, &w->ext_ready));
    if (service(w, 220)) WTRY(w, wait_flag(w, &w->ext2_ready));
    if (!w->radio.valid) return bad(w);
    return 0;
}
int peach_wmi_bootstrap(PeachWmi *w)
{
    int err = peach_wmi_wait_services(w);
    if (err) return err;
    unsigned advertised = w->peer_metadata_version;
    if (service(w, 365)) w->peer_metadata_version = 2;
    if (!w->peer_metadata_version || w->peer_metadata_version > 2) return PeachMhiInvalid;
    report(w, "WMI RX peer metadata advertised/selected", advertised, w->peer_metadata_version);
    WTRY(w, allocate_memory(w));
    w->phase = 1;
    WTRY(w, send_command(w, 1, build_init(w)));
    WTRY(w, wait_flag(w, &w->ready));
    /* ath12k_mac_start applies the advertised antenna masks after READY.
     * INIT's resource limits alone are not the runtime PDEV configuration.
     * This command has no ACK; only subsequent received frames prove RX. */
    for (unsigned parameter = 1; parameter <= 2; parameter++) {
        memset(w->command, 0, 20);
        tlv(w->command + 4, 82, 12); /* wmi_pdev_set_param_cmd_fixed_param */
        p32(w->command + 8, w->radio.pdev);
        p32(w->command + 12, parameter); /* TX_CHAIN_MASK / RX_CHAIN_MASK */
        p32(w->command + 16, parameter == 1 ? w->radio.tx_chains : w->radio.rx_chains);
        WTRY(w, send_command(w, 0x4003, 20));
    }
    report(w, "WMI TX/RX antenna masks submitted", w->radio.tx_chains, w->radio.rx_chains);
    w->phase = 2;
    return 0;
}
/* qcacld wlan_mgmt_rx_srng: HOST4 is a SW-to-SW source ring. The
 * hardware completion is the coherent remote tail, not the reap notification.
 * Keep 127 buffers posted; recycle only after validated tail advancement. */
int peach_wmi_start_management(PeachWmi *w, PeachHtt *h)
{
    if (!w || !w->ready || !h || w->mgmt_rx_started) return PeachMhiInvalid;
    if (!w->mgmt_rx_dma.bytes) return 0;
    PeachMhiDma d = w->mgmt_rx_dma;
    unsigned char *p = d.bytes;
    uint32_t *ring = d.bytes;
    for (unsigned i = 0; i < MGMT_RX_ENTRIES - 1; i++) {
        uint64_t address = d.iova + MGMT_RX_BUFFERS + i * MGMT_RX_BYTES;
        ring[i * 2] = (uint32_t)address;
        ring[i * 2 + 1] = (uint32_t)(address >> 32) | i << 12; /* RBM=0 */
    }
    PeachHttRing request = {
        {p, d.iova, 1024}, {p + 1024, d.iova + 1024, 4},
        {p + 1088, d.iova + 1088, 4}, 0, 15, PeachHttSwToSw, MGMT_RX_ENTRIES, 2
    };
    w->ops.barrier(w->ops.context);
    WTRY(w, peach_htt_setup_ring(h, &request));
    memset(w->command, 0, 20);
    tlv(w->command + 4, 82, 12);
    p32(w->command + 12, 0xf6); /* MGMT_SRNG_REAP_EVENT_THRESHOLD, SOC pdev=0 */
    p32(w->command + 16, 1);
    WTRY(w, send_command(w, 0x4003, 20));
    w->mgmt_rx_write = MGMT_RX_ENTRIES - 1;
    w->mgmt_rx_started = 1;
    w->ops.barrier(w->ops.context);
    *(volatile uint32_t *)(p + 1024) = w->mgmt_rx_write * 2;
    w->ops.barrier(w->ops.context);
    report(w, "WMI management refill posted entries/bytes", w->mgmt_rx_write, MGMT_RX_BYTES);
    return 0;
}
int peach_wmi_poll_management(PeachWmi *w)
{
    if (!w) return PeachMhiInvalid;
    if (w->error) return w->error;
    if (!w->mgmt_rx_started) return 0;
    unsigned char *p = w->mgmt_rx_dma.bytes;
    uint32_t *ring = w->mgmt_rx_dma.bytes;
    w->ops.barrier(w->ops.context);
    uint32_t tail = *(volatile uint32_t *)(p + 1088);
    if (tail >= MGMT_RX_ENTRIES * 2 || (tail & 1)) return bad(w);
    unsigned target = tail / 2;
    unsigned consumed = (target + MGMT_RX_ENTRIES - w->mgmt_rx_read) % MGMT_RX_ENTRIES;
    unsigned owned = (w->mgmt_rx_write + MGMT_RX_ENTRIES - w->mgmt_rx_read) % MGMT_RX_ENTRIES;
    if (consumed > owned) return bad(w);
    for (unsigned i = 0; i < consumed; i++) {
        unsigned slot = w->mgmt_rx_read, cookie = ring[slot * 2 + 1] >> 12;
        uint32_t info = ring[slot * 2 + 1];
        uint64_t address = ring[slot * 2] | (uint64_t)(info & 255) << 32;
        if (cookie >= MGMT_RX_ENTRIES || ((info >> 8) & 15) ||
            address != w->mgmt_rx_dma.iova + MGMT_RX_BUFFERS + cookie * MGMT_RX_BYTES)
            return bad(w);
        unsigned char *buffer = p + MGMT_RX_BUFFERS + cookie * MGMT_RX_BYTES;
        w->ops.barrier(w->ops.context);
        unsigned bytes = u16(buffer);
        if (bytes >= 12 && bytes <= MGMT_RX_BYTES && !u16(buffer + 2)) {
            if (w->mgmt_rx_received < 3)
                report(w, "WMI management DMA event/bytes", u32(buffer + 4), bytes - 4);
            w->mgmt_rx_received++;
            peach_wmi_receive(w, buffer + 4, bytes - 4);
            if (w->error) return w->error;
        } else w->mgmt_rx_discarded++;
        memset(buffer, 0, MGMT_RX_BYTES);
        unsigned next = w->mgmt_rx_write;
        ring[next * 2] = (uint32_t)address;
        ring[next * 2 + 1] = (uint32_t)(address >> 32) | cookie << 12;
        w->mgmt_rx_read = (slot + 1) % MGMT_RX_ENTRIES;
        w->mgmt_rx_write = (next + 1) % MGMT_RX_ENTRIES;
    }
    if (consumed) {
        w->ops.barrier(w->ops.context);
        *(volatile uint32_t *)(p + 1024) = w->mgmt_rx_write * 2;
        w->ops.barrier(w->ops.context);
    }
    return 0;
}
static int scan_channel(PeachWmi *w, unsigned frequency, unsigned band)
{
    const PeachWmiRegDomain *r = &w->regulatory[w->radio.phy];
    unsigned start = band == 1 ? 0 : r->count_2g;
    unsigned end = band == 1 ? r->count_2g : r->count_2g + r->count_5g;
    unsigned found = 0, flags = 0, power = 127, antenna = 127;
    if (!(w->radio.bands & band)) return 0;
    /* MAC/PHY limits are channel centres, unlike the regulatory rules'
     * band edges checked below. Keep 2GHz/5GHz fields separate; WMI band
     * capability bits are not array indices (Armada's 1021 range fix). */
    unsigned low = band == 1 ? w->radio.low_2g : w->radio.low_5g;
    unsigned high = band == 1 ? w->radio.high_2g : w->radio.high_5g;
    if (frequency < low || frequency > high) return 0;
    for (unsigned i = start; i < end; i++) {
        const PeachWmiRegRule *rule = &r->rules[i];
        if (frequency - 10 < (rule->frequencies & 0xffff) || frequency + 10 > rule->frequencies >> 16)
            continue;
        if ((rule->bandwidth_power & 0xffff) < 20 || (rule->flags & 0xffff) & ~15u) return 0;
        unsigned p = (rule->bandwidth_power >> 16) & 255, a = rule->bandwidth_power >> 24;
        if (p > 127 || a > 127) return 0;
        if (p < power) power = p;
        if (a < antenna) antenna = a;
        flags |= rule->flags & 15; found = 1;
    }
    if (!found || (band == 2 && (flags & 4))) return 0;
    if (w->channel_count == PEACH_WMI_SCAN_CHANNELS) return bad(w);
    w->channels[w->channel_count++] = (PeachWmiScanChannel){frequency, flags, power, antenna};
    return 0;
}
static int configure_scan_channels(PeachWmi *w)
{
    const unsigned five_ghz[] = {36,40,44,48,52,56,60,64,100,104,108,112,116,
        120,124,128,132,136,140,144,149,153,157,161,165,169,173,177};
    w->channel_count = 0;
    for (unsigned i = 1; i <= 14; i++) WTRY(w, scan_channel(w, i == 14 ? 2484 : 2407 + i * 5, 1));
    for (unsigned i = 0; i < sizeof(five_ghz) / sizeof(five_ghz[0]); i++)
        WTRY(w, scan_channel(w, 5000 + five_ghz[i] * 5, 2));
    if (!w->channel_count) return bad(w);
    report(w, "WMI passive scan channels/country", w->channel_count,
        w->regulatory[w->radio.phy].alpha2);
    memset(w->command, 0, sizeof(w->command));
    unsigned char *p = w->command + 4;
    tlv(p, TAG_CHANNELS, 12); p32(p + 4, w->channel_count);
    /* pdev_id=0 is required by this command for non-DBDC (single mode). */
    p += 16; tlv(p, TAG_ARRAY, w->channel_count * 28); p += 4;
    for (unsigned i = 0; i < w->channel_count; i++, p += 28) {
        const PeachWmiScanChannel *c = &w->channels[i];
        tlv(p, TAG_CHANNEL, 24); p32(p + 4, c->frequency); p32(p + 8, c->frequency);
        unsigned mode = c->frequency < 2500 ? ((c->flags & 4) ? 2 : 1) : 0;
        p32(p + 16, mode | 1u << 7 | ((c->flags & 2) ? 1u << 10 : 0));
        p32(p + 20, c->power << 8 | c->power << 16);
        p32(p + 24, c->antenna | c->power << 8); /* 20MHz width enum=0 */
    }
    return send_command(w, 0x3003, (size_t)(p - w->command));
}
static int create_scan_vdev(PeachWmi *w)
{
    if (w->vdev_sent) return 0;
    memset(w->command, 0, sizeof(w->command));
    unsigned char *p = w->command + 4;
    /* This phone's Android wire capture: 48B payload through cfp_enable,
     * no newer global_vdev_id. SINGLE uses PDEV 0 for VDEV creation. */
    tlv(p, TAG_VDEV, 48); p32(p + 8, 2); /* vdev 0, STA */
    uint32_t lo = 0, hi = 0;
    for (unsigned i = 0; i < 4; i++) lo |= (uint32_t)w->mac[i] << (i * 8);
    hi = w->mac[4] | (uint32_t)w->mac[5] << 8;
    p32(p + 16, lo); p32(p + 20, hi); p32(p + 24, 2);
    p += 52; tlv(p, TAG_ARRAY, 32); p += 4;
    for (unsigned band = 1; band <= 2; band++, p += 16) {
        unsigned streams = !!(w->radio.bands & band);
        /* Actual vendor builder uses TPC band IDs 0/1 for this field. */
        tlv(p, TAG_STREAMS, 12); p32(p + 4, band - 1); p32(p + 8, streams); p32(p + 12, streams);
    }
    tlv(p, TAG_ARRAY, 0); p += 4; /* no MLO MAC; same ABI as native non-MLO vdev */
    WTRY(w, send_command(w, 0x5001, (size_t)(p - w->command)));
    w->vdev_sent = 1; /* submission only: this ABI has no VDEV_CREATE ACK */
    /* Keep the firmware's exception path consistent with the Ethernet-II
     * TCL bank and RX resource configuration, including pre-key EAPOL. */
    const unsigned params[] = {0x30, 0x5b}; /* TX_ENCAP_TYPE / RX_DECAP_TYPE */
    for (unsigned i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
        memset(w->command, 0, 20); tlv(w->command + 4, 95, 12);
        p32(w->command + 12, params[i]); p32(w->command + 16, 2);
        WTRY(w, send_command(w, 0x5008, 20));
    }
    report(w, "WMI vdev TX/RX Ethernet encapsulation submitted", 2, 2);
    return 0;
}
int peach_wmi_passive_scan(PeachWmi *w)
{
    if (!w || w->phase != 2 || !w->ready || !w->radio.valid || w->radio.phy >= 3 ||
        w->scan_active || w->vdev_stop_sent || (w->vdev_start_sent && !w->vdev_up_sent))
        return PeachMhiInvalid;
    if (w->error) return w->error;
    PeachWmiRegDomain *r = &w->regulatory[w->radio.phy];
    uint64_t end = w->htc->ops.now_us(w->htc->ops.context) + 10000000;
    while (!r->valid) {
        WTRY(w, progress(w));
        if (r->status) return bad(w);
        if (w->htc->ops.now_us(w->htc->ops.context) >= end) return w->error = PeachMhiTimeout;
        if (!r->valid) w->htc->ops.sleep_us(w->htc->ops.context, 1000);
    }
    w->scan_done = w->scan_finished = 0;
    w->scan_reg_generation = r->generation;
    WTRY(w, configure_scan_channels(w));
    WTRY(w, create_scan_vdev(w));
    if (!r->valid || r->generation != w->scan_reg_generation) return bad(w);
    memset(w->command, 0, sizeof(w->command));
    memset(w->bss, 0, sizeof(w->bss)); w->bss_count = w->bss_dropped = 0;
    unsigned char *p = w->command + 4;
    tlv(p, TAG_SCAN, 168); /* 43-word current vendor fixed struct, including header */
    unsigned sequence = (w->scan_id + 1) & 0xfff;
    w->scan_id = 0xa000 | (sequence ? sequence : 1); w->scan_requestor = 0xa001;
    p32(p + 4, w->scan_id); p32(p + 8, w->scan_requestor);
    p32(p + 16, 1); p32(p + 20, 0x7b); /* low priority, channel/start/completion/failures */
    p32(p + 24, 120); p32(p + 28, 120); /* passive dwell in ms */
    p32(p + 48, 25); p32(p + 52, 15000);
    p32(p + 60, 0x21 | (w->scan_id == 0xa001 ? 0x10 : 0));
    /* PASSIVE + FILTER_PROBE_REQ; request channel counters on the first scan. */
    p32(p + 68, w->channel_count);
    p32(p + 72, 1); /* broadcast BSSID, as in ath12k_wmi_start_scan_init */
    p += 172; tlv(p, TAG_U32, w->channel_count * 4); p += 4;
    for (unsigned i = 0; i < w->channel_count; i++, p += 4) p32(p, w->channels[i].frequency);
    tlv(p, 19, 0); p += 4; /* no SSID filter */
    tlv(p, 19, 8); p += 4;
    memset(p, 255, 6); p += 8; /* wmi_mac_addr broadcast; reserved bits zero */
    const unsigned empty_tags[] = {17,18,17,19,19};
    for (unsigned i = 0; i < sizeof(empty_tags) / sizeof(empty_tags[0]); i++, p += 4)
        tlv(p, empty_tags[i], 0);
    w->scan_active = 1; w->scan_sent = w->scan_started = w->scan_done = 0;
    w->scan_reason = 0xff; w->scan_event = 0;
    int err = send_command(w, 0x3001, (size_t)(p - w->command));
    if (!err) w->scan_sent = 1;
    end = w->htc->ops.now_us(w->htc->ops.context) + 20000000;
    while (!err && !w->scan_finished) {
        err = progress(w);
        if (!err && (!r->valid || r->generation != w->scan_reg_generation)) err = bad(w);
        if (!err && !w->scan_finished && w->htc->ops.now_us(w->htc->ops.context) >= end)
            err = w->error = PeachMhiTimeout;
        if (!err && !w->scan_finished) w->htc->ops.sleep_us(w->htc->ops.context, 1000);
    }
    w->scan_active = w->scan_sent = 0;
    if (err || !w->scan_done) {
        w->scan_done = 0; w->bss_count = 0;
        return err ? err : PeachWmiScanFailed;
    }
    report(w, "WMI passive scan actual BSS/dropped", w->bss_count, w->bss_dropped);
    return 0;
}
int peach_wmi_passive_scan_target(PeachWmi *w, const unsigned char *ssid, unsigned bytes,
                                   const unsigned char *bssid)
{
    if (!w || !ssid || !bytes || bytes > 32 || w->scan_active || w->scan_target_bytes ||
        (bssid && ((bssid[0] & 1) || !memcmp(bssid, "\0\0\0\0\0\0", 6))))
        return PeachMhiInvalid;
    memcpy(w->scan_target_ssid, ssid, bytes); w->scan_target_bytes = bytes;
    w->scan_target_bssid_valid = bssid != NULL;
    if (bssid) memcpy(w->scan_target_bssid, bssid, 6);
    report(w, "WMI manual passive scan target bytes/BSSID filter", bytes, bssid != NULL);
    int err = peach_wmi_passive_scan(w);
    w->scan_target_bytes = w->scan_target_bssid_valid = 0;
    return err;
}
int peach_wmi_request_radio_stats(PeachWmi *w)
{
    if (!w || !w->ready || w->scan_active) return PeachMhiInvalid;
    memset(w->command, 0, 40);
    tlv(w->command + 4, 143, 20); /* wmi_request_stats_cmd_fixed_param */
    p32(w->command + 8, 4); /* WMI_REQUEST_PDEV_STAT */
    p32(w->command + 24, w->radio.pdev);
    tlv(w->command + 28, TAG_ARRAY, 0);
    tlv(w->command + 32, TAG_U32, 0);
    tlv(w->command + 36, 19, 0);
    return send_command(w, 0x16001, 40);
}
int peach_wmi_peer_create(PeachWmi *w, const unsigned char mac[6], unsigned type)
{
    if (!w || !mac || !w->ready || !w->vdev_sent || w->scan_active || type > 1 ||
        (mac[0] & 1) || !memcmp(mac, "\0\0\0\0\0\0", 6) || peer_find(w, mac) ||
        (!type && memcmp(mac, w->mac, 6)) || (type && !memcmp(mac, w->mac, 6)))
        return PeachMhiInvalid;
    if (w->error) return w->error;
    PeachWmiPeer *s = NULL;
    for (unsigned i = 0; i < PEACH_WMI_PEERS; i++)
        if (!w->peers[i].used) { s = &w->peers[i]; break; }
    if (!s) return PeachCeFull;
    /* Current fixed struct is 32B, followed by empty non-MLO array. */
    memset(w->command, 0, 40); tlv(w->command + 4, TAG_PEER_CREATE, 28);
    /* type is this host's self/remote ownership marker. Native STA AP
     * peers use WMI_PEER_TYPE_DEFAULT=0, not that internal role value. */
    peer_mac(w->command + 12, mac); p32(w->command + 20, 0);
    tlv(w->command + 36, TAG_ARRAY, 0);
    memset(s, 0, sizeof(*s)); s->used = 1; s->type = type; memcpy(s->mac, mac, 6);
    WTRY(w, send_command(w, 0x6001, 40)); s->create_sent = 1;
    if (service(w, 234)) WTRY(w, wait_flag(w, &s->create_confirmed));
    return 0;
}
int peach_wmi_peer_delete(PeachWmi *w, const unsigned char mac[6])
{
    if (!w || !mac) return PeachMhiInvalid;
    if (w->error) return w->error;
    PeachWmiPeer *s = peer_find(w, mac);
    if (!s || !s->create_sent || s->delete_sent) return PeachMhiInvalid;
    /* Stop/flush TX and detach queues separately; this command does not
     * release any retained DMA allocation or permit queue reuse. */
    memset(w->command, 0, 24); tlv(w->command + 4, TAG_PEER_DELETE, 12);
    peer_mac(w->command + 12, mac); tlv(w->command + 20, TAG_ARRAY, 0);
    WTRY(w, send_command(w, 0x6002, 24)); s->delete_sent = 1;
    if (service(w, 92)) WTRY(w, wait_flag(w, &s->delete_response));
    return 0;
}
int peach_wmi_peer_queue(PeachWmi *w, const PeachHtt *h, const unsigned char mac[6],
                         unsigned tid, PeachMhiDma queue)
{
    if (!w || !h || !mac || tid > 16 || !queue.bytes || !queue.iova ||
        (queue.iova & 127) || ((uintptr_t)queue.bytes & 127) ||
        queue.iova >= 0x100000000ull || queue.capacity < (tid == 16 ? 128u : 512u) ||
        (tid == 16 ? 128u : 512u) > 0x100000000ull - queue.iova) return PeachMhiInvalid;
    if (w->error || h->error) return w->error = w->error ? w->error : h->error;
    PeachWmiPeer *s = peer_find(w, mac);
    const PeachHttPeer *mapped = peach_htt_peer(h, 0, mac);
    if (!s || !s->type || !s->create_sent || s->delete_sent ||
        (service(w, 234) && !s->create_confirmed) ||
        !mapped || (s->queues_sent & (1u << tid))) return PeachMhiInvalid;
    uint32_t generation = mapped->generation;
    memset(w->command, 0, 44); tlv(w->command + 4, TAG_PEER_QUEUE, 36);
    peer_mac(w->command + 12, mac); p32(w->command + 20, tid);
    p32(w->command + 24, (uint32_t)queue.iova);
    p32(w->command + 28, (uint32_t)(queue.iova >> 32));
    p32(w->command + 32, tid); p32(w->command + 36, 1); p32(w->command + 40, 1);
    w->ops.barrier(w->ops.context);
    WTRY(w, send_command(w, 0x6013, 44));
    s->queues_sent |= 1u << tid; /* submitted, not acknowledged */
    mapped = peach_htt_peer(h, 0, mac);
    if (!mapped || mapped->generation != generation) return bad(w);
    return 0;
}
int peach_wmi_vdev_start(PeachWmi *w, unsigned bss_index, unsigned pmf)
{
    if (!w || !w->ready || !w->vdev_sent || w->scan_active || !w->scan_done ||
        w->vdev_start_sent || bss_index >= w->bss_count || pmf > 1 || !service(w, 76))
        return PeachMhiInvalid;
    if (w->error) return w->error;
    const PeachWmiBss *b = &w->bss[bss_index];
    const PeachWmiRegDomain *r = &w->regulatory[w->radio.phy];
    if (!r->valid || r->generation != w->scan_reg_generation || !b->beacon_interval ||
        !b->dtim_period || (b->capability & 3) != 1 || b->ies_truncated) return PeachMhiInvalid;
    const PeachWmiScanChannel *c = NULL;
    for (unsigned i = 0; i < w->channel_count; i++)
        if (w->channels[i].frequency == b->frequency) { c = &w->channels[i]; break; }
    if (!c) return PeachMhiInvalid;
    if (!w->mgmt_dma.bytes) {
        WTRY(w, w->ops.allocate(w->ops.context, PEACH_WMI_MGMT_BYTES, &w->mgmt_dma));
        PeachMhiDma d = w->mgmt_dma;
        if (!d.bytes || d.capacity < PEACH_WMI_MGMT_BYTES || !d.iova || (d.iova & 3) ||
            ((uintptr_t)d.bytes & 3) || d.iova < w->ops.iova_base ||
            d.iova - w->ops.iova_base > w->ops.iova_bytes ||
            PEACH_WMI_MGMT_BYTES > w->ops.iova_bytes - (d.iova - w->ops.iova_base)) return bad(w);
    }
    /* Native 35-word fixed struct, channel, NOA/MLO/partner arrays.
     * No HT/VHT/HE/EHT or MLO capability is advertised by this start. */
    memset(w->command, 0, sizeof(w->command));
    unsigned char *p = w->command + 4;
    /* Peach's vendor start command leaves requestor_id zero. One start
     * per boot is allowed; match that zero with the pending vdev below. */
    tlv(p, TAG_START, 136); p32(p + 8, w->vdev_request);
    p32(p + 12, b->beacon_interval); p32(p + 16, b->dtim_period);
    p32(p + 20, pmf << 1); p32(p + 76, 1); p32(p + 80, 1);
    p32(p + 132, 1); p32(p + 136, 1);
    p += 140; tlv(p, TAG_CHANNEL, 24); p32(p + 4, c->frequency); p32(p + 8, c->frequency);
    unsigned mode = c->frequency < 2500 ? ((c->flags & 4) ? 2 : 1) : 0;
    p32(p + 16, mode | ((c->flags & 2) ? 1u << 10 : 0));
    p32(p + 20, c->power << 8 | c->power << 16);
    p32(p + 24, c->antenna | c->power << 8); p += 28;
    for (unsigned i = 0; i < 3; i++, p += 4) tlv(p, TAG_ARRAY, 0);
    w->vdev_frequency = b->frequency; w->vdev_reg_generation = r->generation;
    memcpy(w->vdev_bssid, b->bssid, 6); w->mgmt_head = w->mgmt_count = 0;
    WTRY(w, send_command(w, 0x5003, (size_t)(p - w->command))); w->vdev_start_sent = 1;
    WTRY(w, wait_flag(w, &w->vdev_started));
    if (!r->valid || r->generation != w->vdev_reg_generation) return bad(w);
    return 0;
}
int peach_wmi_vdev_stop(PeachWmi *w)
{
    if (!w || !w->vdev_started || w->vdev_stop_sent || w->mgmt_in_flight) return PeachMhiInvalid;
    if (w->error) return w->error;
    memset(w->command, 0, 12); tlv(w->command + 4, TAG_STOP, 4);
    WTRY(w, send_command(w, 0x5006, 12)); w->vdev_stop_sent = 1;
    return wait_flag(w, &w->vdev_stopped);
}
int peach_wmi_wmm(PeachWmi *w, const unsigned char *ies, size_t bytes)
{
    if (!w || !w->vdev_started || w->vdev_stop_sent || (!ies && bytes)) return PeachMhiInvalid;
    if (w->error) return w->error;
    const unsigned char *params = NULL;
    while (bytes) {
        if (bytes < 2 || ies[1] > bytes - 2) return PeachMhiInvalid;
        if (ies[0] == 221 && ies[1] == 24 &&
            !memcmp(ies + 2, "\x00\x50\xf2\x02\x01\x01", 6)) {
            if (params) return PeachMhiInvalid;
            params = ies + 10;
        }
        size_t n = ies[1] + 2u; ies += n; bytes -= n;
    }
    static const unsigned defaults[4][4] = {
        {15,1023,3,0}, {15,1023,7,0}, {7,15,2,94}, {3,7,2,47}
    };
    memset(w->command, 0, 128); unsigned char *p = w->command + 4;
    tlv(p, 199, 120); /* VDEV_SET_WMM_PARAMS fixed 124B, EDCA type 0 */
    for (unsigned ac = 0; ac < 4; ac++) {
        unsigned char *q = p + 8 + ac * 28; tlv(q, 199, 24);
        for (unsigned field = 0; field < 4; field++) p32(q + 4 + field * 4, defaults[ac][field]);
    }
    unsigned seen = 0;
    if (params) for (unsigned i = 0; i < 4; i++) {
        const unsigned char *q = params + i * 4;
        unsigned ac = (q[0] >> 5) & 3, min = q[1] & 15, max = q[1] >> 4;
        if ((seen & (1u << ac)) || !(q[0] & 15) || min > max) return PeachMhiInvalid;
        seen |= 1u << ac;
        unsigned char *d = p + 8 + ac * 28;
        p32(d + 4, (1u << min) - 1); p32(d + 8, (1u << max) - 1);
        p32(d + 12, q[0] & 15); p32(d + 16, u16(q + 2)); p32(d + 20, (q[0] >> 4) & 1);
    }
    WTRY(w, send_command(w, 0x500d, 128));
    report(w, "WMI EDCA queues/AP parameters submitted", 4, !!params);
    return 0;
}
int peach_wmi_mgmt_tx(PeachWmi *w, const void *packet, size_t bytes)
{
    const unsigned char *frame = packet;
    if (!w || !frame || !w->vdev_started || w->vdev_stop_sent || w->mgmt_in_flight ||
        !w->mgmt_dma.bytes || bytes < 24 || bytes > PEACH_WMI_MGMT_BYTES ||
        w->mgmt_id == 0xffffffffu || (u16(frame) & 0x430f) ||
        memcmp(frame + 4, w->vdev_bssid, 6) || memcmp(frame + 10, w->mac, 6) ||
        memcmp(frame + 16, w->vdev_bssid, 6)) return PeachMhiInvalid;
    if (w->error) return w->error;
    const PeachWmiRegDomain *r = &w->regulatory[w->radio.phy];
    if (!r->valid || r->generation != w->vdev_reg_generation) return bad(w);
    memcpy(w->mgmt_dma.bytes, frame, bytes); w->ops.barrier(w->ops.context);
    memset(w->command, 0, sizeof(w->command));
    unsigned char *p = w->command + 4;
    size_t prefix = bytes < 64 ? bytes : 64, padded = (prefix + 3) & ~(size_t)3;
    tlv(p, TAG_MGMT_TX, 40); p32(p + 8, ++w->mgmt_id);
    p32(p + 12, w->vdev_frequency); p32(p + 16, (uint32_t)w->mgmt_dma.iova);
    p32(p + 20, (uint32_t)(w->mgmt_dma.iova >> 32));
    p32(p + 24, (uint32_t)bytes); p32(p + 28, (uint32_t)prefix);
    p += 44; tlv(p, 17, (unsigned)padded); p += 4;
    memcpy(p, frame, prefix); p += padded;
    /* tx_params_valid=0 still requires this fixed TLV. */
    tlv(p, TAG_TX_PARAMS, 8); p += 12;
    /* Native non-MLO management TX ends at tx_send_params. */
    w->mgmt_done = 0; w->mgmt_status = 0xffffffffu;
    WTRY(w, send_command(w, 0x7008, (size_t)(p - w->command))); w->mgmt_in_flight = 1;
    WTRY(w, wait_flag(w, &w->mgmt_done));
    return w->mgmt_status ? PeachMhiDeviceError : 0;
}
int peach_wmi_mgmt_rx(PeachWmi *w, PeachWmiMgmt *out)
{
    if (!w || !out) return PeachMhiInvalid;
    if (w->error) return w->error;
    if (!w->mgmt_count) return PeachWmiNoFrame;
    *out = w->mgmt[w->mgmt_head];
    w->mgmt_head = (w->mgmt_head + 1) % PEACH_WMI_MGMT_FRAMES; w->mgmt_count--;
    return 0;
}
int peach_wmi_peer_assoc(PeachWmi *w, const PeachHtt *h, const PeachWmiAssociation *a)
{
    if (!w || !h || !a || !w->vdev_started || w->vdev_stop_sent || w->vdev_up_sent ||
        a->aid < 1 || a->aid > 2007 || a->capabilities > 65535 || a->protected_network > 1 || a->qos > 1 ||
        !a->rate_count || a->rate_count > sizeof(a->rates) ||
        !!(a->capabilities & 16) != a->protected_network) return PeachMhiInvalid;
    if (w->error || h->error) return w->error = w->error ? w->error : h->error;
    PeachWmiPeer *s = peer_find(w, w->vdev_bssid);
    const PeachHttPeer *mapped = peach_htt_peer(h, 0, w->vdev_bssid);
    if (!s || !s->type || !s->create_sent || s->delete_sent || s->assoc_sent || !mapped ||
        (service(w, 234) && !s->create_confirmed)) return PeachMhiInvalid;
    uint32_t generation = mapped->generation;
    static const unsigned char legacy[] = {2,4,11,22,12,18,24,36,48,72,96,108};
    for (unsigned i = 0; i < a->rate_count; i++) {
        unsigned rate = a->rates[i] & 127, found = 0;
        for (unsigned j = w->vdev_frequency < 2500 ? 0 : 4; j < sizeof(legacy); j++)
            if (rate == legacy[j]) found = 1;
        for (unsigned j = 0; j < i; j++) if (rate == (a->rates[j] & 127)) return PeachMhiInvalid;
        if (!found) return PeachMhiInvalid;
    }
    /* Legacy PHY, with WMM only if accepted by the AP; no aggregation. */
    memset(w->command, 0, sizeof(w->command));
    unsigned char *p = w->command + 4;
    /* Native Peach module send_peer_assoc_cmd_tlv: 280-byte fixed struct,
     * not the later UHR/V2 ABI. Follow its five optional arrays below. */
    tlv(p, TAG_ASSOC, 276); peer_mac(p + 4, s->mac);
    p32(p + 16, 1); p32(p + 20, a->aid);
    /* Native protected association requests PTK/GTK handshakes. PRIV
     * would enable encryption before a pairwise key exists for M2. */
    p32(p + 24, (a->protected_network ? 0x14 : 1) | (a->qos ? 2u : 0));
    p32(p + 28, a->capabilities); p32(p + 32, 10); p32(p + 52, 1);
    unsigned mode = w->vdev_frequency < 2500 ? 1 : 0;
    for (unsigned i = 0; i < w->channel_count; i++)
        if (w->channels[i].frequency == w->vdev_frequency && (w->channels[i].flags & 4)) mode = 2;
    p32(p + 60, mode); p32(p + 72, a->rate_count);
    p32(p + 172, a->protected_network ? 11 : 1); /* RSNA_PSK / OPEN */
    p32(p + 252, 1); /* peer_max_tx_nss */
    p += 280; unsigned padded = (a->rate_count + 3) & ~3u;
    tlv(p, 17, padded); memcpy(p + 4, a->rates, a->rate_count); p += 4 + padded;
    tlv(p, 17, 0); p += 4; tlv(p, TAG_VHT_RATES, 20); p += 24;
    for (unsigned i = 0; i < 5; i++, p += 4) tlv(p, TAG_ARRAY, 0);
    report(w, "WMI native peer assoc bytes/rates", (uint32_t)(p - w->command), a->rate_count);
    report(w, "WMI peer negotiated QoS/flags", a->qos, u32(w->command + 28));
    WTRY(w, send_command(w, 0x6005, (size_t)(p - w->command))); s->assoc_sent = 1;
    s->assoc_id = a->aid; s->protected_network = a->protected_network;
    if (service(w, 79)) WTRY(w, wait_flag(w, &s->assoc_confirmed));
    mapped = peach_htt_peer(h, 0, s->mac);
    if (!mapped || mapped->generation != generation) return bad(w);
    return 0;
}
int peach_wmi_vdev_up(PeachWmi *w)
{
    if (!w || !w->vdev_started || w->vdev_stop_sent || w->vdev_up_sent) return PeachMhiInvalid;
    if (w->error) return w->error;
    PeachWmiPeer *s = peer_find(w, w->vdev_bssid);
    if (!s || !s->assoc_sent || s->delete_sent ||
        (service(w, 79) && !s->assoc_confirmed)) return PeachMhiInvalid;
    memset(w->command, 0, 44); tlv(w->command + 4, TAG_UP, 36);
    p32(w->command + 12, s->assoc_id); peer_mac(w->command + 16, s->mac);
    WTRY(w, send_command(w, 0x5005, 44)); w->vdev_up_sent = 1;
    return 0;
}
int peach_wmi_install_ccmp(PeachWmi *w, const PeachHtt *h, unsigned group, unsigned index,
                           const unsigned char key[16], uint64_t rsc)
{
    if (!w || !h || !key || group > 1 || index > 3 || (!group && index) ||
        rsc >= 0x1000000000000ull || !w->vdev_up_sent || w->vdev_stop_sent || w->key_pending)
        return PeachMhiInvalid;
    if (w->error || h->error) return w->error = w->error ? w->error : h->error;
    PeachWmiPeer *s = peer_find(w, w->vdev_bssid);
    const PeachHttPeer *p = peach_htt_peer(h, 0, w->vdev_bssid);
    if (!s || !s->protected_network || !s->assoc_sent || s->delete_sent || !p)
        return PeachMhiInvalid;
    PeachWmiKey *slot = &w->keys[group ? index + 1 : 0];
    /* A retransmitted M3/group message must not reinstall an existing key
     * or lower PN. The handshake owner still checks replay/MIC itself. */
    if (slot->installed && !memcmp(slot->bytes, key, 16)) return 0;
    if (slot->generation == 0xffffffffu) return bad(w);
    uint32_t generation = p->generation;
    memset(w->command, 0, 128);
    unsigned char *out = w->command + 4;
    tlv(out, TAG_KEY, 100); peer_mac(out + 8, s->mac);
    p32(out + 16, index); p32(out + 20, group); p32(out + 24, 4);
    p32(out + 28, (uint32_t)rsc); p32(out + 32, (uint32_t)(rsc >> 32));
    p32(out + 84, 16); tlv(out + 104, 17, 16); memcpy(out + 108, key, 16);
    w->key_pending = 1; w->key_done = 0; w->key_group = group; w->key_index = index;
    int err = send_command(w, 0x5009, 128);
    /* HTC/CE already copied the command into their retained TX storage. */
    memset(w->command, 0, 128);
    if (err) return w->error = err; /* pending/old/new DMA state is unknown */
    WTRY(w, wait_flag(w, &w->key_done));
    p = peach_htt_peer(h, 0, s->mac);
    if (!p || p->generation != generation) return bad(w);
    memcpy(slot->bytes, key, 16); slot->rsc = rsc;
    slot->generation++; slot->installed = 1; w->key_pending = 0;
    return 0;
}
int peach_wmi_authorize(PeachWmi *w, const PeachHtt *h)
{
    if (!w || !h || !w->vdev_up_sent || w->vdev_stop_sent || w->key_pending)
        return PeachMhiInvalid;
    if (w->error || h->error) return w->error = w->error ? w->error : h->error;
    PeachWmiPeer *s = peer_find(w, w->vdev_bssid);
    const PeachHttPeer *p = peach_htt_peer(h, 0, w->vdev_bssid);
    if (!s || !p || !s->assoc_sent || s->delete_sent) return PeachMhiInvalid;
    unsigned group_installed = 0;
    for (unsigned i = 1; i < 5; i++)
        group_installed |= w->keys[i].installed && w->keys[i].generation;
    if (s->protected_network && (!w->keys[0].installed || !w->keys[0].generation ||
        !group_installed)) return PeachMhiInvalid;
    /* wmi_peer_set_param_cmd_fixed_param: vdev, MAC, param, value. There is
     * no completion for this command; success means queued, not data proof. */
    memset(w->command, 0, 28); tlv(w->command + 4, 100, 20);
    peer_mac(w->command + 12, s->mac); p32(w->command + 20, 3);
    /* WMI_PEER_AUTHORIZE_TYPE distinguishes secured and open ports. */
    p32(w->command + 24, s->protected_network ? 1 : 2);
    return send_command(w, 0x6004, 28);
}
int peach_wmi_retire_unkeyed(PeachWmi *w, PeachHtt *h, uint32_t generation)
{
    if (!w || !h || !w->ready || !generation || w->scan_active ||
        w->mgmt_in_flight || w->key_pending || !service(w, 92)) return PeachMhiInvalid;
    if (w->error || h->error) return w->error ? w->error : h->error;
    for (unsigned i = 0; i < 5; i++)
        if (w->keys[i].installed) return PeachMhiInvalid;
    PeachWmiPeer *s = peer_find(w, w->vdev_bssid);
    const PeachHttPeer *p = peach_htt_peer(h, 0, w->vdev_bssid);
    if (!s || !s->type || !s->create_sent || s->delete_sent ||
        (p && p->generation != generation)) return PeachMhiInvalid;
    if (w->vdev_started) WTRY(w, peach_wmi_vdev_stop(w));
    else if (w->vdev_start_sent && !w->vdev_stopped) return PeachMhiInvalid;
    WTRY(w, peach_wmi_peer_delete(w, s->mac));
    uint64_t end = w->htc->ops.now_us(w->htc->ops.context) + 10000000;
    while ((p = peach_htt_peer(h, 0, s->mac))) {
        if (p->generation != generation) return bad(w);
        WTRY(w, progress(w));
        if (w->htc->ops.now_us(w->htc->ops.context) >= end)
            return w->error = PeachMhiTimeout;
        w->htc->ops.sleep_us(w->htc->ops.context, 1000);
    }
    if (h->error) return h->error;
    if (w->vdev_up_sent) {
        memset(w->command, 0, 12); tlv(w->command + 4, 94, 4);
        WTRY(w, send_command(w, 0x5007, 12)); /* VDEV_DOWN, vdev0 */
    }
    memset(s, 0, sizeof(*s));
    w->vdev_start_sent = w->vdev_started = w->vdev_stop_sent = w->vdev_stopped = w->vdev_up_sent = 0;
    w->vdev_frequency = w->vdev_reg_generation = 0;
    w->mgmt_head = w->mgmt_count = w->mgmt_done = 0;
    memset(w->vdev_bssid, 0, sizeof(w->vdev_bssid));
    return 0;
}
