/* SPDX-License-Identifier: GPL-2.0-only */
/* Two-line input follows cas@15e5361 CasWifiNet.cpp/build-cas-wifi.py.
 * Only configuration semantics are reused, never cas hardware or old ABI. */
#ifndef NEZHA_WIFI_AUTO_CONFIG_H
#define NEZHA_WIFI_AUTO_CONFIG_H
#include "NezhaWifiAuto.h"
#include "qcom/PeachNet.h"
#include <string.h>
#include <sys/errno.h>

static inline void nezha_wifi_secret_clear(void *data, size_t bytes)
{
    volatile unsigned char *p = data;
    while (bytes--) *p++ = 0;
}

static inline int nezha_wifi_hex(unsigned char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Preserve SSID/password spaces. Accept LF/CRLF and an optional UTF-8 BOM,
 * reject truncation, embedded NUL/CR and extra lines. Output is zero on error. */
static inline int nezha_wifi_auto_parse(const unsigned char *data, size_t bytes,
                                      PeachNetRequest *out)
{
    if (!out) return EINVAL;
    memset(out, 0, sizeof(*out));
    if (!data || !bytes || bytes > NEZHA_WIFI_AUTO_MAX_BYTES) return EINVAL;
    size_t start = bytes >= 3 && !memcmp(data, "\xef\xbb\xbf", 3) ? 3 : 0;
    size_t split = start;
    while (split < bytes && data[split] != '\n') split++;
    if (split == bytes) return EINVAL;
    size_t ssid_end = split;
    if (ssid_end > start && data[ssid_end - 1] == '\r') ssid_end--;
    size_t ssid_bytes = ssid_end - start;
    if (!ssid_bytes || ssid_bytes > 32) return EINVAL;
    size_t pass_start = split + 1, end = bytes;
    if (end > pass_start && data[end - 1] == '\n') {
        end--;
        if (end > pass_start && data[end - 1] == '\r') end--;
    }
    size_t pass_bytes = end - pass_start;
    if (pass_bytes && (pass_bytes < 8 || pass_bytes > 64)) return EINVAL;
    for (size_t i = start; i < ssid_end; i++)
        if (data[i] < 32 || data[i] == 127) return EINVAL;
    for (size_t i = pass_start; i < end; i++)
        if (data[i] < 32 || data[i] > 126) return EINVAL;
    if (pass_bytes == 64) {
        for (size_t i = 0; i < 32; i++) {
            int hi = nezha_wifi_hex(data[pass_start + i * 2]);
            int lo = nezha_wifi_hex(data[pass_start + i * 2 + 1]);
            if (hi < 0 || lo < 0) { nezha_wifi_secret_clear(out, sizeof(*out)); return EINVAL; }
            out->join.pmk[i] = (unsigned char)((hi << 4) | lo);
        }
    } else if (pass_bytes) {
        out->passphrase_bytes = (unsigned)pass_bytes;
        memcpy(out->passphrase, data + pass_start, pass_bytes);
    }
    out->action = PeachControlJoin;
    out->join.version = PEACH_CONTROL_VERSION;
    out->join.security = pass_bytes ? PeachStationWpa2Psk : PeachStationOpen;
    out->join.ssid_bytes = (unsigned)ssid_bytes;
    memcpy(out->join.ssid, data + start, ssid_bytes);
    return 0;
}
#endif
