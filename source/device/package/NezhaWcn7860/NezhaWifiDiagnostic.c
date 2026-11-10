/* SPDX-License-Identifier: GPL-2.0-only */
/* One existing r008 provider. No COM ownership, filesystem, packet payloads,
 * MMIO commands or restart verb. Frozen RDDM is retained until XNU exits. */
#include "NezhaWifiPlatform.h"
#include "NezhaWifiRegistry.h"
#include "qcom/PeachApple80211.h"
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <pexpert/pexpert.h>
#include <QcomXnuUsbDebug.h>
#include <QcomUsbDiagnostic.h>
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccsha2.h>
static LCK_GRP_DECLARE(wifi_group, "nezha-wifi-domain");
static LCK_MTX_DECLARE(wifi_domain, &wifi_group);
void nezha_wifi_domain_lock(void) { lck_mtx_lock(&wifi_domain); }
void nezha_wifi_domain_unlock(void) { lck_mtx_unlock(&wifi_domain); }
extern volatile int nezha_touch_stage;
extern volatile uint32_t native_data_persist_state;
extern volatile uint64_t native_data_persist_writes;
extern volatile uint64_t native_data_persist_errors;
extern int native_ufs_store_ready(void);
int nezha_wifi_platform_ready(void)
{
    struct qcom_usb_debug_status s; qcom_usb_debug_get_status(&s);
    unsigned persist = ~0u, reset = 0;
    if (!PE_parse_boot_argn("sheng-data-persist", &persist, sizeof(persist))) return 0;
    PE_parse_boot_argn("sheng-data-reset", &reset, sizeof(reset));
    if (reset != 0) return 0;
    const uint32_t state = __atomic_load_n(&native_data_persist_state, __ATOMIC_ACQUIRE);
    const int data_ready = persist == 0 ?
        state == 0 && __atomic_load_n(&native_data_persist_writes, __ATOMIC_RELAXED) == 0 :
        persist == 2 && state == 2 &&
        __atomic_load_n(&native_data_persist_errors, __ATOMIC_RELAXED) == 0 &&
        native_ufs_store_ready();
    // AA27 resumes the existing Data. Fresh/kept/refused stores remain excluded;
    // ordinary writes after a successful resume do not revoke Wi-Fi readiness.
    /* Touch only programs its inherited fixed streams during initial start;
     * USB owns a permanent translated lease before ACTIVE. UFS retains SID60.
     * GPU uses the separate 0x03da0000 SMMU. Never race initial apps claims. */
    return s.phase == QCOM_USB_DEBUG_ACTIVE && s.dma_lease_active &&
        s.domain_result == 0 && __atomic_load_n(&nezha_touch_stage, __ATOMIC_ACQUIRE) == 7 &&
        data_ready;
}
struct registry_event {
    uint32_t attempts, successes, request;
    int32_t pid, result;
    uint64_t ticks;
};
struct wifi_snapshot {
    unsigned phase, state, in_flight, bank, slot;
    int error;
    uint64_t rx, tx, dropped, ticks;
    char step[80];
    struct registry_event registry_open, registry_name;
};
static struct wifi_snapshot latest;
static unsigned snapshot_lock, registered;
static unsigned registry_reports, registry_dropped;
static uint8_t *dump;
static uint32_t dump_length, dump_crc;
static uint32_t dump_declared_bytes, dump_header_valid, dump_version;
static char dump_sha[65];
static const char hex[] = "0123456789abcdef";
static int take(void)
{ unsigned zero = 0; return __atomic_compare_exchange_n(&snapshot_lock, &zero, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); }
static void release(void) { __atomic_store_n(&snapshot_lock, 0, __ATOMIC_RELEASE); }
void nezha_wifi_diagnostic_registry_event(unsigned event, int pid,
                                         unsigned request, int32_t result)
{
    if (event != NezhaWifiRegistryOpen && event != NezhaWifiRegistryName) return;
    if (!take()) { __atomic_fetch_add(&registry_dropped, 1, __ATOMIC_RELAXED); return; }
    struct registry_event *e = event == NezhaWifiRegistryOpen ? &latest.registry_open : &latest.registry_name;
    e->attempts++; e->successes += result == 0;
    e->pid = pid; e->request = request; e->result = result; e->ticks = qcom_usb_debug_ticks();
    unsigned report = registry_reports < 32;
    if (report) registry_reports++;
    release();
    if (report) printf("NEZHA WIFI: registry %s pid %d request %u result %d\n",
        event == NezhaWifiRegistryOpen ? "open" : "name", pid, request, result);
}
void nezha_wifi_diagnostic_publish(unsigned phase, unsigned state, int error,
    uint64_t rx, uint64_t tx, uint64_t dropped, unsigned in_flight,
    unsigned bank, unsigned slot, const char *step)
{
    if (!take()) return;
    latest.phase = phase; latest.state = state; latest.error = error;
    latest.rx = rx; latest.tx = tx; latest.dropped = dropped; latest.in_flight = in_flight;
    latest.bank = bank; latest.slot = slot; latest.ticks = qcom_usb_debug_ticks();
    strlcpy(latest.step, step ? step : "", sizeof(latest.step));
    release();
}
static int snapshot(void *cookie, char *out, size_t capacity, size_t *bytes, unsigned *truncated)
{
    (void)cookie;
    if (!take()) return QUD_BUSY;
    struct wifi_snapshot s = latest;
    release();
    NezhaWifiRegistryState registry;
    peach_wlan_registry_state(&registry);
    unsigned length = __atomic_load_n(&dump_length, __ATOMIC_ACQUIRE);
    int n = snprintf(out, capacity,
        "{\"version\":1,\"phase\":%u,\"state\":%u,\"error\":%d,\"rx\":%llu,\"tx\":%llu,"
        "\"dropped\":%llu,\"in_flight\":%u,\"bank\":%u,\"slot\":%u,\"ticks\":%llu,"
        "\"dump_bytes\":%u,\"dump_crc32\":%u,\"dump_sha256\":\"%s\","
        "\"dump_header_valid\":%u,\"dump_declared_bytes\":%u,\"dump_version\":%u,\"step\":\"%s\","
        "\"registry\":{\"valid\":%u,\"root_busy\":%u,\"stack_busy\":%u,\"controller_busy\":%u,\"interface_busy\":%u,"
        "\"open_calls\":%u,\"open_ok\":%u,\"open_pid\":%d,\"open_type\":%u,\"open_result\":%d,\"open_ticks\":%llu,"
        "\"name_calls\":%u,\"name_ok\":%u,\"name_pid\":%d,\"name_result\":%d,\"name_ticks\":%llu,\"events_dropped\":%u}}",
        s.phase, s.state, s.error, (unsigned long long)s.rx, (unsigned long long)s.tx,
        (unsigned long long)s.dropped, s.in_flight, s.bank, s.slot, (unsigned long long)s.ticks,
        length, length ? dump_crc : 0, length ? dump_sha : "", length ? dump_header_valid : 0,
        length ? dump_declared_bytes : 0, length ? dump_version : 0, s.step,
        registry.valid, registry.root_busy, registry.stack_busy, registry.controller_busy, registry.interface_busy,
        s.registry_open.attempts, s.registry_open.successes, s.registry_open.pid, s.registry_open.request,
        s.registry_open.result, (unsigned long long)s.registry_open.ticks,
        s.registry_name.attempts, s.registry_name.successes, s.registry_name.pid, s.registry_name.result,
        (unsigned long long)s.registry_name.ticks, __atomic_load_n(&registry_dropped, __ATOMIC_RELAXED));
    if (n < 0 || (size_t)n >= capacity) { *bytes = 0; *truncated = 1; return QUD_FAILED; }
    /* Keep the original provider and operations. Append a bounded software
     * view of actual ioctl replies; no phone-side control or memory reader. */
    PeachAppleObservation a;
    peach_apple80211_observation(&a);
    size_t used = (size_t)n - 1; /* replace only the root closing brace */
#define APPEND(...) do { \
    int wrote = snprintf(out + used, capacity - used, __VA_ARGS__); \
    if (wrote < 0 || (size_t)wrote >= capacity - used) { *bytes = 0; *truncated = 1; return QUD_FAILED; } \
    used += (size_t)wrote; \
} while (0)
    APPEND(",\"apple_io\":{\"v\":1,\"qseq\":%llu,\"useq\":%llu,\"evicted\":%u,\"queries\":[",
        (unsigned long long)a.query_sequence, (unsigned long long)a.set_sequence, a.unsupported_evictions);
    /* [type,seq,ticks,pid,result,state,bytes,SSID-length,SSID-tag,BSSID-tag] */
    for (unsigned i = 0; i < 3; i++) {
        const PeachAppleQueryObservation *q = &a.query[i];
        APPEND("%s[%d,%llu,%llu,%d,%d,%u,%u,%u,\"%016llx\",\"%016llx\"]", i ? "," : "",
            q->type, (unsigned long long)q->sequence, (unsigned long long)q->ticks,
            q->pid, q->result, q->state, q->bytes, q->ssid_bytes,
            (unsigned long long)q->ssid_tag, (unsigned long long)q->bssid_tag);
    }
    APPEND("],\"unsupported\":[");
    /* Four most-recent distinct types: [type,seq,ticks,pid,bytes,calls].
     * Evictions are explicit; an absent row never excludes an earlier call. */
    for (unsigned i = 0; i < 4; i++) {
        const PeachAppleUnsupportedObservation *u = &a.unsupported[i];
        APPEND("%s[%d,%llu,%llu,%d,%u,%llu]", i ? "," : "", u->type,
            (unsigned long long)u->sequence, (unsigned long long)u->ticks,
            u->pid, u->bytes, (unsigned long long)u->calls);
    }
    APPEND("]}}");
#undef APPEND
    *bytes = used; *truncated = 0; return QUD_OK;
}
static int read_dump(void *cookie, const struct qud_diag_request *r, char *out,
                     size_t capacity, size_t *bytes, unsigned *truncated)
{
    (void)cookie; *bytes = 0; *truncated = 0;
    uint32_t length = __atomic_load_n(&dump_length, __ATOMIC_ACQUIRE);
    if (r->op != QUD_SUBMIT || !length || !r->arg1 || r->arg1 > 512 ||
        r->arg0 > length || r->arg1 > length - r->arg0) return QUD_INVALID;
    int head = snprintf(out, capacity, "{\"offset\":%u,\"bytes\":%u,\"total\":%u,\"hex\":\"", r->arg0, r->arg1, length);
    if (head < 0 || (size_t)head + r->arg1 * 2u + 3 >= capacity) return QUD_FAILED;
    size_t pos = (size_t)head;
    for (unsigned i = 0; i < r->arg1; i++) {
        uint8_t b = dump[r->arg0 + i]; out[pos++] = hex[b >> 4]; out[pos++] = hex[b & 15];
    }
    out[pos++] = '"'; out[pos++] = '}'; out[pos] = 0; *bytes = pos;
    return QUD_OK;
}
static const struct qud_diag_provider provider = {
    .id = 6, .version = 1, .operations = (1u << QUD_SNAPSHOT) | (1u << QUD_SUBMIT),
    .max_arg0 = NEZHA_WIFI_RDDM_BYTES, .max_arg1 = 512, .name = "nezha-wcn7860", .cookie = NULL,
    .snapshot = snapshot, .submit = read_dump,
};
int nezha_wifi_diagnostic_init(void)
{
    if (registered) return EALREADY;
    dump = kalloc_data(NEZHA_WIFI_RDDM_BYTES, Z_WAITOK);
    if (!dump) return ENOMEM;
    if (!qcom_usb_diag_register(&provider)) { kfree_data(dump, NEZHA_WIFI_RDDM_BYTES); dump = NULL; return EBUSY; }
    registered = 1; return 0;
}
int nezha_wifi_diagnostic_dump(const void *source, size_t bytes)
{
    if (!dump || !source || bytes != NEZHA_WIFI_RDDM_BYTES) return EINVAL;
    if (__atomic_load_n(&dump_length, __ATOMIC_ACQUIRE)) return EALREADY;
    /* Only the single Wi-Fi worker writes this buffer, and only once. */
    memcpy(dump, source, bytes);
    uint32_t crc = ~0u;
    for (size_t i = 0; i < bytes; i++) {
        crc ^= dump[i];
        for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    dump_crc = ~crc;
    uint8_t digest[32]; ccdigest(ccsha256_di(), bytes, dump, digest);
    for (unsigned i = 0; i < 32; i++) { dump_sha[i * 2] = hex[digest[i] >> 4]; dump_sha[i * 2 + 1] = hex[digest[i] & 15]; }
    dump_sha[64] = 0;
    /* Transport completion covers the allocated DMA buffer, including padding.
     * Separately validate the firmware's declared file table before calling
     * its payload complete. Preserve the entire buffer even if it is invalid. */
    uint32_t header_bytes;
    memcpy(&dump_version, dump, 4); memcpy(&header_bytes, dump + 4, 4);
    if (header_bytes >= 8 + 64 && header_bytes <= 8 + 12 * 64 && !((header_bytes - 8) % 64)) {
        size_t end = header_bytes;
        dump_header_valid = 1;
        for (unsigned i = 0; i < (header_bytes - 8) / 64; i++) {
            uint64_t file_bytes; memcpy(&file_bytes, dump + 8 + i * 64 + 16, 8);
            if (file_bytes > bytes - end) { dump_header_valid = 0; break; }
            end += (size_t)file_bytes;
        }
        if (dump_header_valid) dump_declared_bytes = (uint32_t)end;
    }
    __atomic_store_n(&dump_length, (uint32_t)bytes, __ATOMIC_RELEASE);
    printf("NEZHA WIFI: RDDM DMA buffer frozen bytes=%lu CRC32=%08x SHA256=%s header-valid=%u declared=%u; provider6 read-only export\n",
        (unsigned long)bytes, dump_crc, dump_sha, dump_header_valid, dump_declared_bytes);
    return dump_header_valid ? 0 : EINVAL;
}
