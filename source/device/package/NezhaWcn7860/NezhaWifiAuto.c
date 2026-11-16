/* SPDX-License-Identifier: GPL-2.0-only */
/* Optional local RAM configuration and cas-style background join. This worker
 * uses the existing Peach request/RSN path; no static IP or synthetic success. */
#include "NezhaWifiAutoConfig.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <pexpert/device_tree.h>
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccsha2.h>

enum { AutoDisabled, AutoSearching, AutoQueued, AutoLink, AutoIp, AutoFailed, AutoStopped };
static const char *const auto_names[] = {"disabled", "searching", "requested", "link-up", "system-IP", "failed", "stopped"};
static PeachNetRequest auto_request;
static unsigned auto_prepared, auto_state, auto_scans, auto_rows_lock;
static unsigned auto_rearm_enabled, auto_off_completed;
static int auto_error;
static uint64_t auto_next, auto_deadline;
static char auto_rows[3][120];

int nezha_wifi_auto_prepare(void)
{
    if (auto_prepared) return 0;
    DTEntry chosen; const void *data = NULL; unsigned length = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess) return ENOENT;
    if (SecureDTGetProperty(chosen, "nezha-wifi-autojoin-sha256", &data, &length) != kSuccess) {
        auto_prepared = 1; return 0; /* Optional and disabled unless locally pinned. */
    }
    if (length != 32) return EINVAL;
    uint8_t expected[32]; memcpy(expected, data, sizeof(expected));
    unsigned rearm = 0;
    if (SecureDTGetProperty(chosen, "nezha-wifi-autojoin-rearm", &data, &length) == kSuccess) {
        if (length != sizeof(uint32_t)) return EINVAL;
        memcpy(&rearm, data, sizeof(uint32_t));
        if (rearm > 1) return EINVAL;
    }
    unsigned nonzero = 0; for (unsigned i = 0; i < 32; i++) nonzero |= expected[i];
    if (!nonzero) return EINVAL;
    vfs_context_t ctx = vfs_context_kernel(); vnode_t file = NULLVP;
    int error = vnode_open(NEZHA_WIFI_AUTO_PATH, FREAD | O_NOFOLLOW, 0, 0, &file, ctx);
    if (error) return error;
    uint8_t input[NEZHA_WIFI_AUTO_MAX_BYTES] = {0};
    struct vnode_attr va; VATTR_INIT(&va); VATTR_WANTED(&va, va_data_size);
    error = vnode_getattr(file, &va, ctx);
    if (!error && (!VATTR_IS_SUPPORTED(&va, va_data_size) || vnode_vtype(file) != VREG ||
        !va.va_data_size || va.va_data_size > sizeof(input))) error = EINVAL;
    if (!error) {
        int residual = (int)va.va_data_size;
        error = vn_rdwr(UIO_READ, file, (caddr_t)input, (int)va.va_data_size, 0, UIO_SYSSPACE,
            IO_NOAUTH, vfs_context_ucred(ctx), &residual, vfs_context_proc(ctx));
        if (!error && residual) error = EIO;
    }
    int closed = vnode_close(file, FREAD, ctx);
    if (!error) error = closed;
    if (!error) {
        uint8_t digest[32]; ccdigest(ccsha256_di(), (size_t)va.va_data_size, input, digest);
        if (memcmp(digest, expected, sizeof(digest))) error = EAUTH;
        nezha_wifi_secret_clear(digest, sizeof(digest));
    }
    if (!error) error = nezha_wifi_auto_parse(input, (size_t)va.va_data_size, &auto_request);
    nezha_wifi_secret_clear(input, sizeof(input));
    nezha_wifi_secret_clear(expected, sizeof(expected));
    if (error) { nezha_wifi_secret_clear(&auto_request, sizeof(auto_request)); return error; }
    auto_prepared = 1; auto_state = AutoSearching; auto_rearm_enabled = rearm;
    printf("NEZHA WIFI AUTO: local RAM configuration verified; SSID bytes=%u security=%u; credentials redacted\n",
        auto_request.join.ssid_bytes, auto_request.join.security);
    return 0;
}

void nezha_wifi_auto_stop(int error)
{
    nezha_wifi_secret_clear(&auto_request, sizeof(auto_request));
    auto_error = error; auto_state = error ? AutoFailed : AutoStopped;
    auto_next = 0;
}

void nezha_wifi_auto_radio_complete(unsigned enabled, int error)
{
    /* Optional test mode: never power the radio on, override a queued join,
     * or retain a password. Only an actual successful off/on cycle permits
     * rereading the same digest-pinned local RAM configuration. Completion
     * callbacks run on the existing Wi-Fi worker, outside its request lock. */
    if (!auto_rearm_enabled || enabled > 1) return;
    if (error) {
        if (enabled && auto_off_completed) {
            auto_off_completed = 0;
            nezha_wifi_auto_stop(error);
        }
        return;
    }
    if (!enabled) {
        auto_off_completed = 1;
        nezha_wifi_auto_stop(0);
        return;
    }
    if (!auto_off_completed) return;
    auto_off_completed = 0; /* Consume once, including a failed RAM read. */
    nezha_wifi_auto_stop(0);
    auto_prepared = 0; auto_scans = 0; auto_deadline = 0;
    error = nezha_wifi_auto_prepare();
    if (error) {
        auto_prepared = 1; /* No background retry after a failed input check. */
        nezha_wifi_auto_stop(error);
    }
    printf("NEZHA WIFI AUTO: completed radio off/on; pinned RAM rearm result=%d; credentials redacted\n", error);
}

unsigned native_wifi27_screen_rows(char (*out)[120], unsigned capacity)
{
    if (!out || !capacity) return 0;
    unsigned zero = 0;
    if (!__atomic_compare_exchange_n(&auto_rows_lock, &zero, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    unsigned count = auto_rows[0][0] ? (capacity < 3 ? capacity : 3) : 0;
    if (count) memcpy(out, auto_rows, count * sizeof(auto_rows[0]));
    __atomic_store_n(&auto_rows_lock, 0, __ATOMIC_RELEASE);
    return count;
}

void nezha_wifi_auto_tick(uint64_t now)
{
    if (now < auto_next) return;
    auto_next = now + 5000000;
    PeachNetInfo info; peach_net_info(&info);
    PeachNetNetwork network; peach_net_network(&network);
    if (auto_state == AutoSearching) {
        if (!auto_deadline) auto_deadline = now + 180000000;
        if (info.state == PeachNetOff) nezha_wifi_auto_stop(0);
        else if (info.state == PeachNetFailed) nezha_wifi_auto_stop(info.error ? info.error : EIO);
        else if (now >= auto_deadline || auto_scans >= 24) nezha_wifi_auto_stop(ETIMEDOUT);
        else if (info.state == PeachNetReady) {
            unsigned found = 0;
            PeachWmiBss bss;
            for (unsigned i = 0; i < PEACH_CONTROL_BSS; i++) {
                if (peach_net_bss(i, &bss)) break;
                if (bss.ssid_bytes == auto_request.join.ssid_bytes &&
                    !memcmp(bss.ssid, auto_request.join.ssid, bss.ssid_bytes)) { found = 1; break; }
            }
            PeachNetRequest scan = {.action = PeachControlScan};
            int e = peach_net_request(found ? &auto_request : &scan);
            if (!e && found) {
                auto_state = AutoQueued;
                nezha_wifi_secret_clear(&auto_request, sizeof(auto_request));
                printf("NEZHA WIFI AUTO: join accepted by worker; association/security/IP still pending\n");
            } else if (!e) auto_scans++;
            else if (e != EBUSY && e != EAGAIN) nezha_wifi_auto_stop(e);
        }
    } else if (auto_state == AutoQueued || auto_state == AutoLink || auto_state == AutoIp) {
        if (info.state == PeachNetOnline) auto_state = network.valid_ipv4 ? AutoIp : AutoLink;
        else if (info.state == PeachNetFailed || info.state == PeachNetReady) nezha_wifi_auto_stop(info.error ? info.error : ENOTCONN);
        else if (info.state == PeachNetOff || info.state == PeachNetRecovering) nezha_wifi_auto_stop(0);
        else if (auto_state != AutoQueued) auto_state = AutoQueued;
    }
    char rows[3][120];
    snprintf(rows[0], sizeof(rows[0]), "WIFI auto=%s scan=%u state=%u err=%d IP=%u.%u.%u.%u valid=%u",
        auto_names[auto_state], auto_scans, info.state, auto_error ? auto_error : info.error,
        network.ipv4[0], network.ipv4[1], network.ipv4[2], network.ipv4[3], network.valid_ipv4);
    snprintf(rows[1], sizeof(rows[1]), "WIFI DHCP tx/rx=%u/%u type=%u/%u ACK=%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u",
        network.dhcp_tx, network.dhcp_rx, network.dhcp_tx_type, network.dhcp_rx_type, network.dhcp_ack,
        network.router[0], network.router[1], network.router[2], network.router[3],
        network.dns[0], network.dns[1], network.dns[2], network.dns[3]);
    snprintf(rows[2], sizeof(rows[2]), "WIFI BSD frames tx/rx=%llu/%llu bytes=%llu/%llu; return traffic verified separately",
        (unsigned long long)network.tx_frames, (unsigned long long)network.rx_frames,
        (unsigned long long)network.tx_bytes, (unsigned long long)network.rx_bytes);
    unsigned zero = 0;
    if (__atomic_compare_exchange_n(&auto_rows_lock, &zero, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        memcpy(auto_rows, rows, sizeof(rows));
        __atomic_store_n(&auto_rows_lock, 0, __ATOMIC_RELEASE);
    }
    for (unsigned i = 0; i < 3; i++) printf("NEZHA %s\n", rows[i]);
}
