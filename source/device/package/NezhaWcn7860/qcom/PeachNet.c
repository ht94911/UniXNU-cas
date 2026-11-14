/* SPDX-License-Identifier: GPL-2.0-only */
/* BSD Ethernet and control snapshots over the single Peach protocol worker. */
#include "PeachNet.h"
#include "PeachApple80211.h"
#include "PeachDhcp.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/kern_control.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/sockio.h>
#include <kern/locks.h>
#include <net/kpi_interface.h>
#include <net/init.h>
#include <pexpert/pexpert.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <net/bpf.h>
#include <netinet/in.h>

#define TX_QUEUE 64u
static LCK_GRP_DECLARE(net_group, "nezha-wifi");
static LCK_MTX_DECLARE(net_lock, &net_group);
static kern_ctl_ref net_control;
static ifnet_t net_interface;
static PeachControlStatusData net_snapshot;
static PeachNetInfo net_info_data;
static PeachWmiBss net_bss[PEACH_WMI_BSS_ENTRIES];
static PeachWmiScanChannel net_channels[PEACH_WMI_SCAN_CHANNELS];
static unsigned net_channel_count, net_join_pending, net_scan_pending, net_leave_pending;
static PeachNetRequest net_request;
static unsigned net_join_layout, net_power_requested = 1;
static unsigned net_power_pending, net_power_active, net_scan_during_power;
static uint64_t net_next_request, net_join_request, net_active_join;
static uint64_t net_scan_request, net_active_scan;
static mbuf_t net_queue[TX_QUEUE], net_pending;
static unsigned net_head, net_count, net_online, net_data_blocked;
static unsigned net_trace[2], net_seen[2];
/* Packet observation and address sampling run on the single protocol worker. */
static PeachNetNetwork net_observed;
static unsigned char net_mac[6], net_dhcp_xid[4];
static unsigned net_dhcp_xid_valid;
static unsigned char net_frame[1518]; /* worker-owned, never exposed as DMA */
static errno_t control_connect(kern_ctl_ref c, struct sockaddr_ctl *s, void **p)
{ (void)c; (void)s; *p = NULL; return 0; }
static errno_t control_disconnect(kern_ctl_ref c, u_int32_t u, void *p)
{ (void)c; (void)u; (void)p; return 0; }
int peach_net_request(const PeachNetRequest *request)
{
    if (!request) return EINVAL;
    unsigned opt = request->action;
    if (opt != PeachControlJoin && opt != PeachControlScan && opt != PeachControlLeave && opt != PeachControlPower)
        return ENOPROTOOPT;
    if (opt == PeachControlPower && request->radio_power > 1) return EINVAL;
    if (opt == PeachControlJoin) {
        const PeachControlJoinData *j = &request->join;
        if (j->version != PEACH_CONTROL_VERSION || j->security > PeachStationWpa2Psk ||
            !j->ssid_bytes || j->ssid_bytes > 32 || j->bssid_valid > 1 ||
            j->reserved[0] || j->reserved[1] || (j->bssid_valid && ((j->bssid[0] & 1) ||
            !memcmp(j->bssid, "\0\0\0\0\0\0", 6))) ||
            (request->passphrase_bytes && (j->security != PeachStationWpa2Psk ||
             request->passphrase_bytes < 8 || request->passphrase_bytes > 63))) return EINVAL;
        for (unsigned i = 0; i < request->passphrase_bytes; i++)
            if (request->passphrase[i] < 32 || request->passphrase[i] > 126) return EINVAL;
    }
    lck_mtx_lock(&net_lock);
    /* A successful radio-on always performs a real passive scan before its
     * completion. Keep one scan request against that in-flight operation;
     * EBUSY here makes the original client abandon the request and retry much
     * later. Never overwrite the power command or admit scans while off. */
    if (opt == PeachControlScan && net_snapshot.state == PeachNetRecovering &&
        net_power_pending && net_power_requested) {
        int err = net_scan_pending ? EBUSY : 0;
        uint64_t id = 0;
        if (!err) {
            net_scan_pending = net_scan_during_power = 1;
            id = net_scan_request = ++net_next_request;
        }
        lck_mtx_unlock(&net_lock);
        if (!err) printf("NEZHA WIFI: scan request %llu retained for real radio-on scan\n",
            (unsigned long long)id);
        return err;
    }
    unsigned finishing_leave = opt == PeachControlJoin && net_leave_pending;
    unsigned cancelling_join = (opt == PeachControlLeave ||
        (opt == PeachControlPower && !request->radio_power)) && net_join_pending;
    unsigned during_scan = opt != PeachControlScan && net_scan_pending;
    unsigned replace = (finishing_leave && net_request.action == PeachControlLeave) ||
        (cancelling_join && net_request.action == PeachControlJoin);
    int err = net_snapshot.state == PeachNetFailed ? ENXIO :
        (!net_power_requested && opt != PeachControlPower && opt != PeachControlLeave) ? ENETDOWN :
        ((net_snapshot.state != PeachNetReady && net_snapshot.state != PeachNetOnline &&
          !(net_snapshot.state == PeachNetOff && opt == PeachControlPower) &&
          !finishing_leave && !cancelling_join && !during_scan) ||
         (net_request.action && !replace) || (net_scan_pending && !during_scan)) ? EBUSY : 0;
    unsigned cancelled = 0, cancelled_layout = net_join_layout;
    static unsigned manual_failures;
    unsigned report_failure = err && opt == PeachControlJoin && request->apple_layout && manual_failures < 16;
    unsigned failure_state = net_snapshot.state, failure_pending = net_request.action;
    unsigned failure_scan = net_scan_pending, failure_join = net_join_pending, failure_leave = net_leave_pending;
    if (report_failure) manual_failures++;
    uint64_t cancelled_id = net_join_request;
    unsigned char cancelled_bssid[6]; memcpy(cancelled_bssid, net_info_data.current.bssid, 6);
    if (!err) {
        net_request = *request;
        net_request.request_id = ++net_next_request;
        if (opt != PeachControlScan) net_data_blocked = 1;
        if (opt == PeachControlPower) {
            net_power_requested = request->radio_power;
            net_power_pending = 1;
            net_snapshot.count = 0;
        }
        if (cancelling_join) { net_join_pending = 0; net_info_data.result = 2; cancelled = 1; }
        if (opt == PeachControlLeave) net_leave_pending = 1;
        if (opt == PeachControlJoin) {
            net_join_pending = 1; net_info_data.result = 0;
            net_join_layout = request->apple_layout; net_join_request = net_request.request_id;
        }
        unsigned associated_scan = opt == PeachControlScan && net_snapshot.state == PeachNetOnline;
        if (opt == PeachControlScan) {
            net_scan_pending = 1; net_scan_during_power = 0;
            net_scan_request = net_request.request_id;
        }
        net_snapshot.state = opt == PeachControlJoin ? PeachNetAssociating :
            opt == PeachControlScan ? (associated_scan ? PeachNetOnline : PeachNetScanning) : PeachNetRecovering;
        net_snapshot.error = 0;
    }
    lck_mtx_unlock(&net_lock);
    if (report_failure)
        printf("NEZHA WIFI: Apple association queue rejected %d state %u pending %u scan/join/leave %u/%u/%u\n",
            err, failure_state, failure_pending, failure_scan, failure_join, failure_leave);
    if (cancelled) peach_apple80211_join_done(cancelled_layout, 2, cancelled_bssid, cancelled_id);
    return err;
}
static errno_t control_set(kern_ctl_ref c, u_int32_t u, void *p, int opt, void *data, size_t len)
{
    (void)c; (void)u; (void)p;
    if (opt == PeachControlPower) {
        if (!data || len != sizeof(uint32_t)) return EINVAL;
        uint32_t enabled; memcpy(&enabled, data, sizeof(enabled)); return peach_net_power(enabled);
    }
    if (((opt == PeachControlScan || opt == PeachControlLeave) && len) ||
        (opt == PeachControlJoin && (!data || len != sizeof(PeachControlJoinData)))) return EINVAL;
    PeachNetRequest request = {0}; request.action = opt;
    if (opt == PeachControlJoin) memcpy(&request.join, data, len);
    int err = peach_net_request(&request);
    volatile unsigned char *secret = (void *)&request;
    for (size_t i = 0; i < sizeof(request); i++) secret[i] = 0;
    return err;
}
static errno_t control_get(kern_ctl_ref c, u_int32_t u, void *p, int opt, void *data, size_t *len)
{
    (void)c; (void)u; (void)p;
    if (opt != PeachControlStatus) return ENOPROTOOPT;
    if (!data) { *len = sizeof(net_snapshot); return 0; }
    if (*len < sizeof(net_snapshot)) { *len = sizeof(net_snapshot); return EMSGSIZE; }
    lck_mtx_lock(&net_lock); memcpy(data, &net_snapshot, sizeof(net_snapshot)); lck_mtx_unlock(&net_lock);
    *len = sizeof(net_snapshot); return 0;
}
int peach_net_power(unsigned enabled)
{
    if (enabled > 1) return EINVAL;
    lck_mtx_lock(&net_lock);
    unsigned failed = net_snapshot.state == PeachNetFailed;
    unsigned same = net_power_requested == enabled &&
        (net_info_data.powered == enabled || (net_request.action == PeachControlPower && net_request.radio_power == enabled));
    lck_mtx_unlock(&net_lock);
    if (failed) return ENXIO;
    if (same) return 0;
    PeachNetRequest request = {.action = PeachControlPower, .radio_power = enabled};
    return peach_net_request(&request);
}
void peach_net_power_complete(unsigned enabled, int error)
{
    lck_mtx_lock(&net_lock);
    net_info_data.powered = !error && enabled;
    net_power_pending = net_power_active = 0;
    if (error || !enabled) net_snapshot.count = 0;
    lck_mtx_unlock(&net_lock);
    peach_net_status(NULL, error ? PeachNetFailed : enabled ? PeachNetReady : PeachNetOff, error);
    peach_apple80211_event(PeachApplePowerChanged);
}
int peach_net_init(void)
{
    if (net_control) return EALREADY;
    struct kern_ctl_reg c = {0};
    strlcpy(c.ctl_name, PEACH_CONTROL_NAME, sizeof(c.ctl_name));
    c.ctl_flags = CTL_FLAG_PRIVILEGED;
    c.ctl_connect = control_connect; c.ctl_disconnect = control_disconnect;
    c.ctl_setopt = control_set; c.ctl_getopt = control_get;
    net_snapshot.version = PEACH_CONTROL_VERSION;
    return ctl_register(&c, &net_control);
}
int peach_net_take_request(PeachNetRequest *out)
{
    lck_mtx_lock(&net_lock);
    unsigned action = net_request.action;
    if (action) {
        *out = net_request;
        if (action == PeachControlJoin) net_active_join = net_request.request_id;
        else if (action == PeachControlScan) net_active_scan = net_request.request_id;
        else if (action == PeachControlPower) net_power_active = net_request.radio_power;
        volatile unsigned char *v = (void *)&net_request;
        for (size_t i = 0; i < sizeof(net_request); i++) v[i] = 0;
    }
    lck_mtx_unlock(&net_lock); return action != 0;
}
void peach_net_status(const PeachWmi *w, unsigned state, int error)
{
    if (w && (w->channel_count > PEACH_WMI_SCAN_CHANNELS || w->bss_count > PEACH_CONTROL_BSS ||
        w->radio.phy >= sizeof(w->regulatory) / sizeof(w->regulatory[0]))) {
        w = NULL; state = PeachNetFailed; error = EPROTO;
    }
    lck_mtx_lock(&net_lock);
    /* A new manual join may queue while leave resets the endpoint. Its
     * request and completion must survive the old connection's teardown. */
    if (state == PeachNetReady || state == PeachNetOnline) {
        if (net_request.action == PeachControlJoin) state = PeachNetAssociating;
        else if (net_request.action == PeachControlLeave || net_request.action == PeachControlPower) state = PeachNetRecovering;
        else if (net_request.action == PeachControlScan && state == PeachNetReady) state = PeachNetScanning;
    }
    if (state == PeachNetReady || state == PeachNetAssociating ||
        state == PeachNetOnline || state == PeachNetFailed ||
        (state == PeachNetRecovering && error && net_request.action != PeachControlLeave))
        net_leave_pending = 0;
    /* A Ready/Online status can arrive after a request was queued but before
     * its worker executes. It is not that request's scan completion. Bind
     * normal scans to the taken request ID, and deferred scans to an active
     * radio-on with actual finished firmware results. Terminal states still
     * complete cancellation so clients cannot wait forever. */
    unsigned scan_cancelled = state == PeachNetFailed || state == PeachNetOff ||
        (state == PeachNetRecovering && !(net_scan_during_power && net_power_requested));
    unsigned scan_results_ready = w && w->scan_finished && !w->scan_active &&
        (state == PeachNetReady || state == PeachNetOnline || state == PeachNetAssociating);
    unsigned scan_done = net_scan_pending && (scan_cancelled ||
        (scan_results_ready && (net_scan_during_power ? net_power_active :
            net_active_scan == net_scan_request)));
    if (scan_done) {
        net_scan_pending = net_scan_during_power = 0;
        net_active_scan = 0;
    }
    unsigned join_done = net_join_pending && (net_active_join == net_join_request || state == PeachNetFailed) &&
        (state == PeachNetOnline || state == PeachNetReady || state == PeachNetFailed || state == PeachNetOff || state == PeachNetRecovering);
    net_snapshot.state = state; net_snapshot.error = error;
    if (state == PeachNetRecovering) net_snapshot.count = 0;
    if (join_done) {
        net_info_data.result = state == PeachNetOnline && !error ? 1 : error == PeachMhiTimeout ? 9 : 2;
        net_join_pending = 0;
    }
    if (w) {
        net_info_data.powered = w->ready != 0;
        net_channel_count = w->channel_count;
        memcpy(net_channels, w->channels, net_channel_count * sizeof(net_channels[0]));
        net_info_data.country = w->regulatory[w->radio.phy].alpha2;
    }
    if (w && w->scan_finished && !w->scan_active) {
        net_snapshot.count = w->scan_done ? w->bss_count : 0;
        for (unsigned i = 0; i < net_snapshot.count; i++) {
            PeachControlBss *d = &net_snapshot.bss[i]; const PeachWmiBss *b = &w->bss[i];
            net_bss[i] = *b;
            memset(d, 0, sizeof(*d)); d->frequency = b->frequency; d->rssi = b->rssi;
            d->privacy = !!(b->capability & 16); d->ssid_bytes = b->ssid_bytes;
            memcpy(d->bssid, b->bssid, 6); memcpy(d->ssid, b->ssid, b->ssid_bytes);
        }
    }
    if (state == PeachNetOnline && !error) net_data_blocked = 0;
    unsigned result = net_info_data.result, layout = net_join_layout;
    uint64_t request_id = net_join_request;
    unsigned char bssid[6]; memcpy(bssid, net_info_data.current.bssid, 6);
    lck_mtx_unlock(&net_lock);
    if (state == PeachNetOnline) peach_net_link(1);
    else if (state == PeachNetReady || state == PeachNetOff || state == PeachNetHandshake || state == PeachNetFailed ||
             state == PeachNetRecovering) peach_net_link(0);
    if (scan_done) peach_apple80211_event(PeachAppleScanDone);
    if (join_done) peach_apple80211_join_done(layout, result, bssid, request_id);
}
void peach_net_selection(const PeachWmiBss *b, unsigned security)
{
    lck_mtx_lock(&net_lock); net_info_data.current = *b; net_info_data.security = security;
    lck_mtx_unlock(&net_lock);
    net_trace[0] = net_trace[1] = 0;
    net_seen[0] = net_seen[1] = 0;
    memset(&net_observed, 0, sizeof(net_observed)); net_dhcp_xid_valid = 0;
}
void peach_net_info(PeachNetInfo *out)
{
    lck_mtx_lock(&net_lock); *out = net_info_data;
    out->state = net_snapshot.state; out->error = net_snapshot.error;
    lck_mtx_unlock(&net_lock);
}
int peach_net_bss(unsigned index, PeachWmiBss *out)
{
    lck_mtx_lock(&net_lock);
    int err = net_snapshot.state == PeachNetScanning || net_snapshot.state == PeachNetRecovering ? EBUSY :
        index >= net_snapshot.count ? EIO : 0;
    if (!err) *out = net_bss[index];
    lck_mtx_unlock(&net_lock); return err;
}
unsigned peach_net_channels(PeachWmiScanChannel *out, unsigned capacity)
{
    lck_mtx_lock(&net_lock);
    unsigned count = net_channel_count < capacity ? net_channel_count : capacity;
    memcpy(out, net_channels, count * sizeof(*out));
    lck_mtx_unlock(&net_lock); return count;
}
static errno_t output(ifnet_t interface, mbuf_t m)
{
    int err = 0;
    if (mbuf_pkthdr_len(m) < 14 || mbuf_pkthdr_len(m) > sizeof(net_frame)) err = EMSGSIZE;
    if (!err) bpf_tap_out(interface, DLT_EN10MB, m, NULL, 0);
    lck_mtx_lock(&net_lock);
    if (!err) err = !net_online || net_data_blocked || !(ifnet_flags(interface) & IFF_UP) ? ENETDOWN :
        net_count == TX_QUEUE ? ENOBUFS : 0;
    if (!err) { net_queue[(net_head + net_count) % TX_QUEUE] = m; net_count++; }
    lck_mtx_unlock(&net_lock);
    if (err) mbuf_freem(m);
    return err;
}
static errno_t interface_ioctl(ifnet_t p, u_long cmd, void *data)
{
    if (cmd == 0x802869c8u || cmd == 0xc02869c9u) return peach_apple80211_ioctl(cmd, data);
    struct ifreq *r = data;
    switch (cmd) {
    case SIOCSIFADDR: return ifnet_set_flags(p, IFF_UP, IFF_UP);
    case SIOCSIFFLAGS: return 0; /* connection worker owns hardware/link state */
    case SIOCADDMULTI: case SIOCDELMULTI: return 0;
    case SIOCSIFMTU:
        return r->ifr_mtu >= 576 && r->ifr_mtu <= 1500 ? ifnet_set_mtu(p, r->ifr_mtu) : EINVAL;
    case SIOCGIFDEVMTU:
        r->ifr_devmtu.ifdm_current = ifnet_mtu(p);
        r->ifr_devmtu.ifdm_min = 576; r->ifr_devmtu.ifdm_max = 1500; return 0;
    case SIOCGIFMEDIA32: case SIOCGIFMEDIA64: {
        struct ifmediareq *m = data;
        int medium = IFM_IEEE80211 | IFM_AUTO;
        user_addr_t list = cmd == SIOCGIFMEDIA64 ? ((struct ifmediareq64 *)m)->ifmu_ulist :
            CAST_USER_ADDR_T(((struct ifmediareq32 *)m)->ifmu_ulist);
        int err = list && m->ifm_count > 0 ? copyout(&medium, list, sizeof(medium)) : 0;
        m->ifm_current = m->ifm_active = medium;
        m->ifm_mask = 0; m->ifm_count = 1;
        lck_mtx_lock(&net_lock); m->ifm_status = IFM_AVALID | (net_online ? IFM_ACTIVE : 0);
        lck_mtx_unlock(&net_lock); return err;
    }
    default: return EOPNOTSUPP;
    }
}
static void link_event(unsigned up)
{
    _Alignas(struct kern_event_msg) unsigned char message[
        KEV_MSG_HEADER_SIZE + sizeof(struct net_event_data)] = {0};
    struct kern_event_msg *header = (void *)message;
    /* kern_event_msg includes a trailing event_data[1] placeholder. Its
     * sizeof is four bytes larger than the actual wire header. */
    struct net_event_data *data = (void *)(message + KEV_MSG_HEADER_SIZE);
    header->total_size = sizeof(message); header->vendor_code = KEV_VENDOR_APPLE;
    header->kev_class = KEV_NETWORK_CLASS; header->kev_subclass = KEV_DL_SUBCLASS;
    header->event_code = up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF;
    data->if_family = ifnet_family(net_interface); data->if_unit = ifnet_unit(net_interface);
    strlcpy(data->if_name, ifnet_name(net_interface), sizeof(data->if_name));
    (void)ifnet_event(net_interface, header);
}
int peach_net_attach(const unsigned char mac[6])
{
    if (!mac || net_interface) return EINVAL;
    struct ifnet_init_eparams init = {0};
    init.ver = IFNET_INIT_CURRENT_VERSION; init.len = sizeof(init);
    init.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
    init.name = "en"; init.unit = 0; init.family = IFNET_FAMILY_ETHERNET;
    init.subfamily = IFNET_SUBFAMILY_WIFI; init.type = IFT_ETHER;
    init.uniqueid = mac; init.uniqueid_len = 6; init.output = output;
    init.demux = ether_demux; init.add_proto = ether_add_proto; init.del_proto = ether_del_proto;
    init.check_multi = ether_check_multi; init.framer_extended = ether_frameout_extended;
    init.ioctl = interface_ioctl; init.broadcast_addr = etherbroadcastaddr; init.broadcast_len = 6;
    ifnet_t p;
    int err = ifnet_allocate_extended(&init, &p);
    if (err) return err;
    ifnet_set_addrlen(p, 6); ifnet_set_hdrlen(p, 14); ifnet_set_mtu(p, 1500);
    ifnet_set_baudrate(p, 54000000); /* legacy PHY's ceiling, not measured throughput */
    ifnet_set_capabilities_enabled(p, 0, -1); ifnet_set_offload(p, 0);
    ifnet_set_flags(p, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
    err = ifnet_attach(p, NULL);
    if (err) { ifnet_release(p); return err; }
    net_interface = p; /* retain until reboot, including a later link failure */
    memcpy(net_mac, mac, sizeof(net_mac));
    err = ifnet_set_lladdr(p, mac, 6);
    if (err) return err;
    /* IPConfiguration receives DHCP via BPF. BSD input alone does not
     * register an Ethernet BPF endpoint or tap its received packets.
     * See HackiOS redwood_net.c's BSD interface integration. */
    bpfattach(p, DLT_EN10MB, 14);
    ifnet_set_flags(p, IFF_UP, IFF_UP | IFF_RUNNING);
    lck_mtx_lock(&net_lock);
    snprintf(net_snapshot.interface_name, sizeof(net_snapshot.interface_name), "%s%u", ifnet_name(p), ifnet_unit(p));
    lck_mtx_unlock(&net_lock);
    extern int peach_wlan_publish(const char *, const unsigned char *);
    return peach_wlan_publish(net_snapshot.interface_name, mac);
}
void nezha_wifi_register(void)
{
    extern void nezha_wifi_init(void);
    unsigned mode = 0;
    if (!PE_parse_boot_argn("nezha_wifi", &mode, sizeof(mode)) || (mode != 1 && mode != 2)) return;
    static unsigned queued;
    if (__atomic_exchange_n(&queued, 1, __ATOMIC_ACQ_REL)) return;
    int error = net_init_add(nezha_wifi_init);
    if (error == EALREADY) nezha_wifi_init();
    else if (error) printf("NEZHA WIFI: net_init_add failed %d; worker not started\n", error);
}
int peach_net_verify_attached(void)
{
    if (!net_interface) return ENXIO;
    ifnet_t found = NULL;
    int error = ifnet_find_by_name("en0", &found);
    if (error) return error;
    unsigned same = found == net_interface && ifnet_unit(found) == 0 &&
        ifnet_type(found) == IFT_ETHER && !strcmp(ifnet_name(found), "en");
    ifnet_release(found);
    return same ? 0 : ENXIO;
}
static void set_link(unsigned up, unsigned voluntary)
{
    lck_mtx_lock(&net_lock);
    unsigned changed = net_online != !!up; net_online = !!up;
    int rssi = net_info_data.current.rssi;
    lck_mtx_unlock(&net_lock);
    if (!up) {
        /* Called by the protocol worker. Do not retain packets belonging
         * to an ended connection for a later manual join. */
        if (net_pending) { mbuf_freem(net_pending); net_pending = NULL; }
        for (;;) {
            mbuf_t m = NULL;
            lck_mtx_lock(&net_lock);
            if (net_count) {
                m = net_queue[net_head]; net_queue[net_head] = NULL;
                net_head = (net_head + 1) % TX_QUEUE; net_count--;
            }
            lck_mtx_unlock(&net_lock);
            if (!m) break;
            mbuf_freem(m);
        }
    }
    if (!net_interface || !changed) return;
    ifnet_set_flags(net_interface, up ? IFF_RUNNING : 0, IFF_RUNNING);
    link_event(up); peach_apple80211_link(up ? 1 : voluntary ? 0 : 2, rssi);
}
void peach_net_link(unsigned up) { set_link(up, 0); }
void peach_net_leave(void) { set_link(0, 1); }
void peach_net_network(PeachNetNetwork *out)
{
    *out = net_observed;
    memset(out->ipv4, 0, sizeof(out->ipv4)); out->valid_ipv4 = 0;
    lck_mtx_lock(&net_lock); unsigned online = net_online && !net_data_blocked; lck_mtx_unlock(&net_lock);
    if (!net_interface || !online) return;
    ifaddr_t *list = NULL;
    if (ifnet_get_address_list_family(net_interface, &list, AF_INET) || !list) return;
    for (unsigned i = 0; list[i]; i++) {
        struct sockaddr_in address = {0};
        if (ifaddr_address(list[i], (struct sockaddr *)&address, sizeof(address)) || address.sin_family != AF_INET) continue;
        const unsigned char *p = (const void *)&address.sin_addr;
        if (!(p[0] | p[1] | p[2] | p[3])) continue;
        memcpy(out->ipv4, p, sizeof(out->ipv4));
        if (p[0] && p[0] != 127 && p[0] < 224 && !(p[0] == 169 && p[1] == 254)) {
            out->valid_ipv4 = 1; break;
        }
    }
    ifnet_free_address_list(list);
}
static void observe_frame(unsigned incoming, const unsigned char *p, size_t bytes)
{
    if (incoming) { net_observed.rx_frames++; net_observed.rx_bytes += bytes; }
    else { net_observed.tx_frames++; net_observed.tx_bytes += bytes; }
    PeachDhcpObservation d;
    if (!peach_dhcp_inspect(p, bytes, &d) || d.outbound == incoming || memcmp(d.client, net_mac, 6)) return;
    if (!incoming) {
        net_observed.dhcp_tx++; net_observed.dhcp_tx_type = d.message;
        if (d.message == 1 || d.message == 3) {
            memcpy(net_dhcp_xid, d.xid, sizeof(net_dhcp_xid)); net_dhcp_xid_valid = 1;
            net_observed.dhcp_ack = 0;
        }
    } else {
        if (!net_dhcp_xid_valid || memcmp(d.xid, net_dhcp_xid, sizeof(net_dhcp_xid))) return;
        net_observed.dhcp_rx++; net_observed.dhcp_rx_type = d.message;
        if (d.message == 2 || d.message == 5) memcpy(net_observed.offered, d.address, 4);
        if (d.message == 5) {
            net_observed.dhcp_ack = 1; memcpy(net_observed.mask, d.mask, 4);
            memcpy(net_observed.router, d.router, 4); memcpy(net_observed.dns, d.dns, 4);
        } else if (d.message == 6) net_observed.dhcp_ack = 0;
    }
    if (net_observed.dhcp_tx + net_observed.dhcp_rx <= 32)
        printf("NEZHA WIFI DHCP: %s type=%u matching client/transaction; yiaddr=%u.%u.%u.%u (observed, not system IP)\n",
            incoming ? "RX" : "TX", d.message, d.address[0], d.address[1], d.address[2], d.address[3]);
}
/* Bounded metadata only: enough to distinguish DHCP/ARP/DNS data flow.
 * Runs on the protocol worker, without packet payloads or key material. */
static void trace_frame(unsigned incoming, const unsigned char *p, size_t bytes)
{
    observe_frame(incoming, p, bytes);
    if (bytes < 14 || (net_trace[incoming] >= 8 && net_seen[incoming] == 15)) return;
    unsigned type = p[12] * 256u + p[13], protocol = 0, source = 0, dest = 0;
    unsigned ipv4 = type == 0x800 && bytes >= 34 && (p[14] >> 4) == 4;
    if (ipv4) {
        unsigned header = (p[14] & 15) * 4u;
        protocol = p[23];
        if (header >= 20 && bytes >= 14 + header + 4 &&
            !(p[20] & 0x1f) && !p[21] && (protocol == 6 || protocol == 17)) {
            source = p[14 + header] * 256u + p[15 + header];
            dest = p[16 + header] * 256u + p[17 + header];
        }
    }
    unsigned kind = type == 0x806 ? 1 :
        source == 67 || source == 68 || dest == 67 || dest == 68 ? 2 :
        source == 53 || dest == 53 ? 4 :
        source == 443 || dest == 443 || source == 80 || dest == 80 ? 8 : 0;
    if (net_trace[incoming] >= 8 && (!kind || (net_seen[incoming] & kind))) return;
    net_trace[incoming]++; net_seen[incoming] |= kind;
    printf("NEZHA WIFI: %s Ethernet type %#x bytes %lu IP protocol %u ports %u/%u\n",
        incoming ? "RX" : "TX", type, (unsigned long)bytes, protocol, source, dest);
    if (ipv4)
        printf("NEZHA WIFI: %s IPv4 %u.%u.%u.%u -> %u.%u.%u.%u\n", incoming ? "RX" : "TX",
            p[26], p[27], p[28], p[29], p[30], p[31], p[32], p[33]);
}
int peach_net_transmit(PeachTx *tx)
{
    if (!net_interface || !tx || !tx->authorized) return 0;
    lck_mtx_lock(&net_lock); unsigned blocked = net_data_blocked; lck_mtx_unlock(&net_lock);
    if (blocked) return 0;
    for (unsigned budget = 0; budget < 16; budget++) {
        if (!net_pending) {
            lck_mtx_lock(&net_lock);
            if (net_count) {
                net_pending = net_queue[net_head]; net_queue[net_head] = NULL;
                net_head = (net_head + 1) % TX_QUEUE; net_count--;
            }
            lck_mtx_unlock(&net_lock);
        }
        if (!net_pending) return 0;
        size_t bytes = mbuf_pkthdr_len(net_pending);
        int err = mbuf_copydata(net_pending, 0, bytes, net_frame);
        if (err) {
            mbuf_freem(net_pending); net_pending = NULL;
            ifnet_stat_increment_out(net_interface, 0, 0, 1);
            continue; /* a malformed BSD packet does not stop the radio */
        }
        err = peach_tx_send(tx, net_frame, bytes, 0);
        if (err == PeachCeFull) return 0;
        if (!err) trace_frame(0, net_frame, bytes);
        mbuf_freem(net_pending); net_pending = NULL;
        ifnet_stat_increment_out(net_interface, err ? 0 : 1, err ? 0 : bytes, err != 0);
        if (err) return err;
    }
    return 0;
}
void peach_net_receive(const void *packet, size_t bytes)
{
    if (!net_interface || !packet || bytes < 14 || bytes > sizeof(net_frame)) return;
    lck_mtx_lock(&net_lock); unsigned online = net_online && !net_data_blocked; lck_mtx_unlock(&net_lock);
    if (!online) return;
    trace_frame(1, packet, bytes);
    mbuf_t m;
    if (mbuf_getpacket(MBUF_DONTWAIT, &m)) return;
    if (mbuf_copyback(m, 0, bytes, packet, MBUF_DONTWAIT)) { mbuf_freem(m); return; }
    mbuf_setlen(m, bytes); mbuf_pkthdr_setlen(m, bytes);
    mbuf_pkthdr_setrcvif(m, net_interface); mbuf_pkthdr_setheader(m, mbuf_data(m));
    void *header = mbuf_data(m);
    mbuf_adj(m, 14);
    bpf_tap_in(net_interface, DLT_EN10MB, m, header, 14);
    struct ifnet_stat_increment_param stats = {0}; stats.packets_in = 1; stats.bytes_in = bytes;
    (void)ifnet_input(net_interface, m, &stats); /* stack owns m, including on error */
}
void peach_net_stop(int error)
{
    peach_net_status(NULL, PeachNetFailed, error);
    lck_mtx_lock(&net_lock);
    volatile unsigned char *secret = (void *)&net_request;
    for (size_t i = 0; i < sizeof(net_request); i++) secret[i] = 0;
    lck_mtx_unlock(&net_lock);
}
void peach_net_recovering(int error)
{
    lck_mtx_lock(&net_lock);
    volatile unsigned char *secret = (void *)&net_request;
    for (size_t i = 0; i < sizeof(net_request); i++) secret[i] = 0;
    net_leave_pending = 0;
    net_snapshot.state = PeachNetRecovering;
    lck_mtx_unlock(&net_lock);
    peach_net_status(NULL, PeachNetRecovering, error);
}
