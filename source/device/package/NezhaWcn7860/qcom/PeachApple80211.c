/* SPDX-License-Identifier: GPL-2.0-only */
/* iOS27 Apple80211 ioctl/event ABI adapted from the checked24A446
 * UniXNU-sheng WifiNet contract. Retain the original Peach hardware API. Only implemented Peach operations/capabilities are
 * exposed. All mutable protocol state stays on pandora_wifi's worker. */
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
    uint8_t ies[1024];
} AppleScanResult;
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
    uint32_t short_ssid, unused3;
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
_Static_assert(sizeof(AppleScanRequest) == 5456 && sizeof(AppleScanResult) == 1164 &&
               offsetof(AppleScanResult, ies) == 0x8c, "Apple80211 scan ABI");
_Static_assert(sizeof(AppleLink) == 32, "Apple80211 link ABI");
static LCK_GRP_DECLARE(apple_group, "nezha-apple80211");
static LCK_MTX_DECLARE(apple_lock, &apple_group);
static unsigned result_next, result_generation;
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
    AppleScanResult *out = kalloc_data(sizeof(*out), Z_WAITOK | Z_ZERO);
    if (!out) return ENOMEM;
    unsigned room = sizeof(out->ies);
    int err = r->length < offsetof(AppleScanResult, ies) ? EINVAL :
        copyin(r->data, out, offsetof(AppleScanResult, ies));
    if (err) { kfree_data(out, sizeof(*out)); return err; }
    if (out->ie_bytes < room) room = out->ie_bytes;
    memset(out, 0, sizeof(*out));
    out->version = 1; out->channel = bss_channel(b->frequency);
    out->rssi = b->rssi; out->beacon_interval = b->beacon_interval; out->capability = b->capability;
    memcpy(out->bssid, b->bssid, 6); out->ssid_bytes = b->ssid_bytes;
    memcpy(out->ssid, b->ssid, b->ssid_bytes);
    out->ie_bytes = b->ie_bytes <= room ? b->ie_bytes : 0;
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
    err = give(r, out, sizeof(*out)); kfree_data(out, sizeof(*out)); return err;
}
static int get(AppleRequest *r)
{
    PeachNetInfo info; peach_net_info(&info);
    switch (r->type) {
    case 43: case 44: {
        static const char name[256] = "UniXNU Nezha WCN7860 Peach v2";
        return give(r, name, sizeof(name));
    }
    case 12: {
        /* AES, AES-CCM, short preamble/slot, WPA2 and WME. No HT, power-save
         * or PMF capability until their actual protocol paths exist. */
        const uint8_t capabilities[12] = {0x0c, 0x46, 0x02};
        return give(r, capabilities, sizeof(capabilities));
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
        int err = peach_net_bss(index, &b);
        lck_mtx_unlock(&apple_lock);
        if (!err) err = network(r, &b);
        /* User memory faults and allocation must not block scan events
         * on the protocol worker. Advance only this successful snapshot. */
        lck_mtx_lock(&apple_lock);
        if (!err && result_next == index && result_generation == generation) result_next++;
        lck_mtx_unlock(&apple_lock);
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
static int join(const AppleRequest *r)
{
    unsigned char wire[PeachAppleJoin27Bytes];
    PeachAppleDecodedJoin decoded = {0};
    PeachNetRequest request = {0};
    if (r->length != PeachAppleJoin27Bytes && r->length != PeachAppleJoinLegacyBytes) return EINVAL;
    int err = copyin(r->data, wire, r->length);
    if (!err && peach_apple_decode_join(wire, r->length, &decoded)) err = EINVAL;
    if (!err) {
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
        if (r->length != sizeof(AppleScanRequest)) return EINVAL;
        AppleScanRequest *a = kalloc_data(sizeof(*a), Z_WAITOK);
        if (!a) return ENOMEM;
        int err = copyin(r->data, a, sizeof(*a));
        if (!err && (a->version != 1 || a->ssid_bytes > 32 || a->channel_count > 400 ||
            a->name_count > 10 || (a->scan_type != 1 && a->scan_type != 2))) err = EINVAL;
        /* Backend performs a full passive scan. It never transmits probes
         * or overrides the firmware's board-specific regulatory rules. */
        if (!err) { PeachNetRequest request = { .action = PeachControlScan }; err = peach_net_request(&request); }
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
        return err ? err : set(r);
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
    if (code == PeachAppleScanDone) {
        lck_mtx_lock(&apple_lock); result_next = 0; result_generation++; lck_mtx_unlock(&apple_lock);
    }
    int err = post(code, NULL, 0);
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
