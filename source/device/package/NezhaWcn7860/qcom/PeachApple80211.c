/* SPDX-License-Identifier: GPL-2.0-only */
/* iOS27 Apple80211 ioctl/event bridge starts from UniXNU-sheng WifiNet,
 * with all selected wire sizes checked against original iPhone24A446 IO80211.
 * Retain Peach hardware/protocol APIs and expose only implemented capabilities.
 * All mutable protocol state stays on pandora_wifi's worker. */
#include "PeachApple80211.h"
#include "PeachNet.h"
#include "PeachAppleWire.h"
#include <kern/kalloc.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/kern_event.h>
#include <sys/proc.h>
#include <sys/systm.h>

#define APPLE_SET 0x802869c8u
#define APPLE_GET 0xc02869c9u
typedef struct {
    char interface[16];
    int32_t type, value;
    uint32_t length;
    user_addr_t data __attribute__((aligned(8)));
} AppleRequest;
typedef struct { uint32_t version, channel, flags; } AppleChannel;
typedef struct {
    uint32_t version, count;
    AppleChannel channels[401];
    uint32_t unused;
} AppleChannels;
typedef struct {
    uint8_t unknown0[12];
    uint16_t count;
    uint8_t unknown1[800];
    uint8_t number[400], indoor[400], dfs[400], passive[400], wide40[400], wide80[400];
    uint8_t unknown2[1602];
    uint32_t flags[400];
    uint8_t unknown3[3600];
} __attribute__((packed)) AppleChannelsInfo;
typedef struct {
    uint32_t version;
    AppleChannel channel;
    uint16_t noise_known;
    int16_t noise, snr, rssi;
    uint16_t beacon_interval, capability;
    uint8_t bssid[6], rate_count, unused0;
    uint32_t rates[15];
    uint8_t ssid_bytes, ssid[32], unused1[3];
    uint32_t age_ms;
    uint8_t flags, unused2;
    uint16_t ie_bytes;
    uint8_t ies[2048];
    uint8_t extension[92]; /* opaque scan selector + unsupported newer metadata */
} AppleScanResult;
enum { AppleScanLegacyBytes = 5456, AppleResultLegacyBytes = 1164 };
typedef struct {
    uint32_t version, bss_type;
    uint8_t bssid[6], unused0[2];
    uint32_t ssid_bytes;
    uint8_t ssid[32];
    uint32_t scan_type, phy_mode;
    uint16_t dwell_ms, unused1;
    uint32_t rest_ms, channel_count;
    AppleChannel channels[400];
    uint64_t flags;
    uint32_t unused2, name_count;
    uint8_t names[10][56];
    uint32_t short_ssid;
    char scan_identifier[64];
    uint8_t extension[12];
} AppleScanRequest;
typedef struct {
    uint8_t down, unused0[3];
    int32_t rssi;
    uint16_t snr, noise;
    uint8_t unused1[4], involuntary, unused2[3];
    uint32_t reason, subreason;
    uint8_t unused3[4];
} AppleLink;
_Static_assert(sizeof(AppleRequest) == 40 && offsetof(AppleRequest, data) == 32,
               "Apple80211 ioctl envelope");
_Static_assert(sizeof(AppleChannels) == 4824 && sizeof(AppleChannelsInfo) == 10016 &&
               offsetof(AppleChannelsInfo, flags) == 0x12d0, "Apple80211 channels");
_Static_assert(sizeof(AppleScanRequest) == 5528 && sizeof(AppleScanResult) == 2280 &&
               offsetof(AppleScanRequest, scan_identifier) == 0x154c &&
               offsetof(AppleScanResult, ies) == 0x8c &&
               offsetof(AppleScanResult, extension) == 0x88c, "24A446 scan ABI");
_Static_assert(sizeof(AppleLink) == 32, "Apple80211 link ABI");
static LCK_GRP_DECLARE(apple_group, "nezha-apple80211");
static LCK_MTX_DECLARE(apple_lock, &apple_group);
/* Iterator epoch changes on a new scan or a completed enumeration. */
static unsigned result_next, result_generation;
static int trace_slot(unsigned *count, unsigned limit)
{
    unsigned n = __atomic_load_n(count, __ATOMIC_RELAXED);
    while (n < limit)
        if (__atomic_compare_exchange_n(count, &n, n + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return 1;
    return 0;
}
static void erase(void *p, size_t n)
{ volatile unsigned char *v = p; while (n--) *v++ = 0; }
static int give(AppleRequest *r, const void *data, size_t bytes)
{
    if (r->length < bytes) { r->length = (uint32_t)bytes; return EMSGSIZE; }
    if (!r->data && bytes) return EFAULT;
    return copyout(data, r->data, bytes);
}
static int answer_value(AppleRequest *r, uint32_t value)
{
    if (!r->length && !r->data) { r->value = (int32_t)value; return 0; }
    const uint32_t reply[2] = {1, value};
    return give(r, reply, sizeof(reply));
}
static AppleChannel channel(unsigned frequency, unsigned flags)
{
    AppleChannel c = {1, frequency == 2484 ? 14 : frequency < 2500 ?
        (frequency - 2407) / 5 : (frequency - 5000) / 5,
        2u | (frequency < 2500 ? 8u : 16u) | (!(flags & 1) ? 0x80u : 0) |
        ((flags & 2) ? 0x100u : 0)};
    return c;
}
static AppleChannel bss_channel(unsigned frequency)
{
    PeachWmiScanChannel channels[PEACH_WMI_SCAN_CHANNELS];
    unsigned n = peach_net_channels(channels, PEACH_WMI_SCAN_CHANNELS);
    for (unsigned i = 0; i < n; i++)
        if (channels[i].frequency == frequency) return channel(frequency, channels[i].flags);
    return channel(frequency, 1); /* no active-channel claim without a rule */
}
static int network(AppleRequest *r, const PeachWmiBss *b)
{
    /* Accept only observed layouts, never a partial structure or a guessed
     * future length. The old public Peach layout remains explicitly separate. */
    if (r->length != sizeof(AppleScanResult) && r->length != AppleResultLegacyBytes) return EINVAL;
    if (b->ssid_bytes > sizeof(b->ssid) || b->ie_bytes > sizeof(b->ies)) return EINVAL;
    AppleScanResult *out = kalloc_data(sizeof(*out), Z_WAITOK | Z_ZERO);
    if (!out) return ENOMEM;
    unsigned room = r->length == AppleResultLegacyBytes ? 1024 : sizeof(out->ies);
    int err = copyin(r->data, out, offsetof(AppleScanResult, ies));
    if (err) { kfree_data(out, sizeof(*out)); return err; }
    if (out->ie_bytes < room) room = out->ie_bytes;
    /* Never silently strip RSN/security IEs and advance the scan cursor. */
    if (b->ie_bytes > room) { kfree_data(out, sizeof(*out)); return EMSGSIZE; }
    memset(out, 0, sizeof(*out));
    out->version = 1; out->channel = bss_channel(b->frequency);
    out->rssi = b->rssi; out->beacon_interval = b->beacon_interval; out->capability = b->capability;
    memcpy(out->bssid, b->bssid, 6); out->ssid_bytes = b->ssid_bytes;
    memcpy(out->ssid, b->ssid, b->ssid_bytes);
    out->ie_bytes = b->ie_bytes;
    memcpy(out->ies, b->ies, out->ie_bytes);
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    uint64_t age = ns / 1000 > b->seen_us ? (ns / 1000 - b->seen_us) / 1000 : 0;
    out->age_ms = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
    for (unsigned off = 0; off + 2 <= b->ie_bytes && b->ies[off + 1] <= b->ie_bytes - off - 2;) {
        unsigned n = b->ies[off + 1];
        if (b->ies[off] == 1 || b->ies[off] == 50)
            for (unsigned i = 0; i < n && out->rate_count < 15; i++) {
                unsigned rate = (b->ies[off + 2 + i] & 127) / 2;
                if (rate && rate <= 54) out->rates[out->rate_count++] = rate;
            }
        off += n + 2;
    }
    err = give(r, out, r->length); kfree_data(out, sizeof(*out)); return err;
}
static int get(AppleRequest *r)
{
    PeachNetInfo info; peach_net_info(&info);
    switch (r->type) {
    case 43: case 44: {
        static const char name[384] = "UniXNU Nezha WCN7860 Peach v2";
        return give(r, name, r->length == 256 ? 256 : sizeof(name));
    }
    case 12: {
        /* AES, AES-CCM, short preamble/slot, WPA2 and WME. No HT, power-save
         * or PMF capability until their actual protocol paths exist. */
        const uint8_t capabilities[23] = {0x0c, 0x46, 0x02};
        return give(r, capabilities, r->length == 12 ? 12 : sizeof(capabilities));
    }
    case 15: return answer_value(r, 1); /* station */
    case 19: {
        if (info.state == PeachNetFailed) return ENXIO;
        uint32_t power[6] = {1, 1, info.powered, 0, 0, 0};
        return give(r, power, sizeof(power));
    }
    case 13:
        return answer_value(r, info.state == PeachNetOnline ? 4 : info.state == PeachNetAssociating ? 2 :
            info.state == PeachNetHandshake ? 3 : info.state == PeachNetScanning ? 1 : 0);
    case 21: return answer_value(r, info.result);
    case 51: {
        const char country[3] = {(char)info.country, (char)(info.country >> 8), 0};
        return give(r, country, sizeof(country));
    }
    case 27: case 254: case 207: {
        PeachWmiScanChannel channels[PEACH_WMI_SCAN_CHANNELS];
        unsigned count = peach_net_channels(channels, PEACH_WMI_SCAN_CHANNELS);
        if (!count) return ENXIO;
        size_t bytes = r->type == 207 ? sizeof(AppleChannelsInfo) : sizeof(AppleChannels);
        void *out = kalloc_data(bytes, Z_WAITOK | Z_ZERO);
        if (!out) return ENOMEM;
        if (r->type == 207) {
            AppleChannelsInfo *a = out; a->count = count;
            for (unsigned i = 0; i < count; i++) {
                AppleChannel c = channel(channels[i].frequency, channels[i].flags);
                a->number[i] = c.channel; a->flags[i] = c.flags;
                a->dfs[i] = !!(channels[i].flags & 2);
                a->passive[i] = !!(channels[i].flags & 3);
                a->indoor[i] = !!(channels[i].flags & 8);
            }
        } else {
            AppleChannels *a = out; a->version = 1; a->count = count;
            for (unsigned i = 0; i < count; i++) a->channels[i] = channel(channels[i].frequency, channels[i].flags);
        }
        int err = give(r, out, bytes); kfree_data(out, bytes); return err;
    }
    case 11: {
        PeachWmiBss b;
        lck_mtx_lock(&apple_lock);
        unsigned index = result_next, generation = result_generation;
        int source_error = peach_net_bss(index, &b);
        /* sheng resets at the terminal EIO so the next caller can read the
         * cached list again. Keep that EIO for this caller's loop terminator.
         * A new epoch prevents an old copyout from advancing the next pass. */
        if (source_error == EIO) { result_next = 0; result_generation++; }
        lck_mtx_unlock(&apple_lock);
        int err = source_error ? source_error : network(r, &b);
        /* User memory faults and allocation must not block scan events
         * on the protocol worker. Advance only this successful snapshot. */
        lck_mtx_lock(&apple_lock);
        if (!err && result_next == index && result_generation == generation) result_next++;
        lck_mtx_unlock(&apple_lock);
        static unsigned reports;
        if ((!index || err) && trace_slot(&reports, 128))
            printf("NEZHA WIFI: Apple scan result pid %d index %u epoch %u bytes %u source-error %d result %d IE-bytes %u state %u\n",
                proc_pid(current_proc()), index, generation, r->length, source_error, err,
                source_error ? 0 : b.ie_bytes, info.state);
        return err;
    }
    default: break;
    }
    if (info.state != PeachNetOnline) return ENXIO;
    switch (r->type) {
    case 1: {
        int err = give(r, info.current.ssid, info.current.ssid_bytes);
        if (!err) r->length = info.current.ssid_bytes;
        return err;
    }
    case 9: return give(r, info.current.bssid, 6);
    case 4: {
        struct { uint32_t version; AppleChannel channel; } a = {1, bss_channel(info.current.frequency)};
        return give(r, &a, sizeof(a));
    }
    case 16: {
        struct { uint32_t version, radios, unit; int32_t each[4], all, each_ext[4], all_ext; }
            a = { .version = 1, .radios = 1, .each = {info.current.rssi}, .all = info.current.rssi,
                  .each_ext = {info.current.rssi}, .all_ext = info.current.rssi };
        return give(r, &a, sizeof(a));
    }
    case 2: {
        uint32_t auth[4] = {1, 1, info.security == PeachStationWpa2Psk ? 0x10u : 0, 0};
        return give(r, auth, r->length == 12 ? 12 : sizeof(auth));
    }
    case 103: return network(r, &info.current);
    default: return EOPNOTSUPP;
    }
}
static int association_trace_slot(void)
{
    static unsigned count;
    return trace_slot(&count, 32);
}
static int join(const AppleRequest *r)
{
    unsigned char wire[PeachAppleJoin27Bytes];
    PeachAppleDecodedJoin decoded = {0};
    PeachNetRequest request = {0};
    PeachAppleJoinMetadata metadata = {0};
    unsigned trace = association_trace_slot(), stage = 1;
    if (r->length != PeachAppleJoin27Bytes && r->length != PeachAppleJoinLegacyBytes) {
        if (trace) printf("NEZHA WIFI: Apple association length %u unsupported; no user contents read\n", r->length);
        return EINVAL;
    }
    int err = copyin(r->data, wire, r->length);
    if (!err) {
        stage = 2;
        (void)peach_apple_join_metadata(wire, r->length, &metadata);
        if (peach_apple_decode_join(wire, r->length, &decoded)) err = EINVAL;
    }
    if (!err) {
        stage = 3;
        PeachAppleNetwork *a = &decoded.network;
        request.action = PeachControlJoin; request.apple_layout = r->length;
        request.join.version = PEACH_CONTROL_VERSION;
        request.join.security = decoded.secured ? PeachStationWpa2Psk : PeachStationOpen;
        request.join.ssid_bytes = a->ssid_bytes; memcpy(request.join.ssid, a->ssid, a->ssid_bytes);
        if (memcmp(a->bssid, "\0\0\0\0\0\0", 6)) {
            request.join.bssid_valid = 1; memcpy(request.join.bssid, a->bssid, 6);
        }
        if (decoded.secured && a->key_cipher == 6) memcpy(request.join.pmk, a->key, 32);
        else if (decoded.secured) {
            request.passphrase_bytes = a->key_bytes;
            memcpy(request.passphrase, a->key, a->key_bytes);
        }
        err = peach_net_request(&request);
    }
    if (trace) {
        PeachNetInfo info; peach_net_info(&info);
        printf("NEZHA WIFI: Apple association bytes %u stage %u result %d state %u powered %u version %u mode %u auth %#x/%#x ssid-bytes %u key-meta %u/%u/%u/%#x/%u; contents redacted\n",
            r->length, stage, err, info.state, info.powered, metadata.version, metadata.mode,
            metadata.auth_lower, metadata.auth_upper, metadata.ssid_bytes, metadata.key_version,
            metadata.key_bytes, metadata.key_cipher, metadata.key_flags, metadata.key_index);
    }
    erase(wire, sizeof(wire)); erase(&decoded, sizeof(decoded)); erase(&request, sizeof(request));
    return err;
}
static int set(const AppleRequest *r)
{
    if (r->type == 20) return join(r);
    if (r->type == 22) {
        PeachNetInfo info; peach_net_info(&info);
        if (info.state == PeachNetReady || info.state == PeachNetScanning ||
            info.state == PeachNetRecovering) return 0;
        PeachNetRequest request = { .action = PeachControlLeave };
        return peach_net_request(&request);
    }
    if (r->type == 10) {
        if (r->length != sizeof(AppleScanRequest) && r->length != AppleScanLegacyBytes) return EINVAL;
        AppleScanRequest *a = kalloc_data(sizeof(*a), Z_WAITOK | Z_ZERO);
        if (!a) return ENOMEM;
        int err = copyin(r->data, a, r->length);
        if (!err && (a->version != 1 || a->ssid_bytes > 32 || a->channel_count > 400 ||
            a->name_count > 10 || (a->scan_type < 1 || a->scan_type > 3))) err = EINVAL;
        /* Backend performs a full passive scan. It never transmits probes
         * or overrides the firmware's board-specific regulatory rules. */
        if (!err) { PeachNetRequest request = { .action = PeachControlScan }; err = peach_net_request(&request); }
        static unsigned reports;
        if (trace_slot(&reports, 64))
            printf("NEZHA WIFI: Apple scan request pid %d bytes %u version %u type %u channels %u names %u result %d; contents redacted\n",
                proc_pid(current_proc()), r->length, a->version, a->scan_type, a->channel_count, a->name_count, err);
        kfree_data(a, sizeof(*a)); return err;
    }
    if (r->type == 19) {
        uint32_t power[6];
        if (r->length < sizeof(power)) return EINVAL;
        int err = copyin(r->data, power, sizeof(power));
        if (err) return err;
        if (power[0] != 1 || power[1] != 1 || power[2] > 1) return EINVAL;
        return peach_net_power(power[2]);
    }
    return EOPNOTSUPP;
}
int peach_apple80211_ioctl(u_long command, void *data)
{
    if (command != APPLE_SET && command != APPLE_GET) return EOPNOTSUPP;
    AppleRequest *r = data;
    if (!r || strncmp(r->interface, "en0", sizeof(r->interface))) return ENXIO;
    if (command == APPLE_SET) {
        int err = proc_suser(current_proc());
        if (err && r->type == 20 && association_trace_slot())
            printf("NEZHA WIFI: Apple association permission rejected %d\n", err);
        if (!err) err = set(r);
        if (r->type == 10 || r->type == 19 || r->type == 20 || r->type == 22) {
            static unsigned reports;
            if (trace_slot(&reports, 96)) {
                PeachNetInfo info; peach_net_info(&info);
                printf("NEZHA WIFI: Apple set pid %d type %d bytes %u result %d state %u powered %u\n",
                    proc_pid(current_proc()), r->type, r->length, err, info.state, info.powered);
            }
        }
        if (err == EOPNOTSUPP) {
            static int32_t seen[16];
            static unsigned count;
            unsigned report = 0;
            lck_mtx_lock(&apple_lock);
            unsigned i = 0;
            while (i < count && seen[i] != r->type) i++;
            if (i == count && count < 16) { seen[count++] = r->type; report = 1; }
            lck_mtx_unlock(&apple_lock);
            if (report) printf("NEZHA WIFI: unsupported Apple set pid %d type %d length %u; contents not logged\n",
                proc_pid(current_proc()), r->type, r->length);
        }
        return err;
    }
    return get(r);
}
static int post(unsigned code, const void *data, unsigned bytes)
{
    char name[16] = "en0";
    struct kev_msg message = { .vendor_code = KEV_VENDOR_APPLE, .kev_class = KEV_IEEE80211_CLASS,
        .kev_subclass = 1, .event_code = code };
    message.dv[0].data_ptr = name; message.dv[0].data_length = sizeof(name);
    message.dv[1].data_ptr = (void *)data; message.dv[1].data_length = bytes;
    return kev_post_msg(&message);
}
void peach_apple80211_event(unsigned code)
{
    unsigned generation = 0;
    if (code == PeachAppleScanDone) {
        lck_mtx_lock(&apple_lock); result_next = 0; generation = ++result_generation; lck_mtx_unlock(&apple_lock);
    }
    int err = post(code, NULL, 0);
    if (code == PeachAppleScanDone) {
        static unsigned reports;
        if (trace_slot(&reports, 64))
            printf("NEZHA WIFI: Apple scan event epoch %u result %d\n", generation, err);
    }
    if (err) printf("NEZHA WIFI: event %u delivery failed %d\n", code, err);
}
void peach_apple80211_link(unsigned status, int rssi)
{
    AppleLink link = { .down = status != 1, .rssi = status == 1 ? rssi : 0,
        .involuntary = status > 1, .reason = status > 1 ? 4 : 0 };
    post(2, NULL, 0); post(3, NULL, 0); post(4, &link, sizeof(link));
}

void peach_apple80211_join_done(unsigned layout, unsigned result, const unsigned char bssid[6], uint64_t request_id)
{
    PeachAppleAssocEvent27 event;
    peach_apple_assoc_event(&event, result, bssid);
    int err = post(PeachAppleJoinDone, layout == PeachAppleJoin27Bytes ? &event : NULL,
        layout == PeachAppleJoin27Bytes ? sizeof(event) : 0);
    printf("NEZHA WIFI: request %llu association result %u layout %u event-error %d\n",
        (unsigned long long)request_id, result, layout, err);
}
