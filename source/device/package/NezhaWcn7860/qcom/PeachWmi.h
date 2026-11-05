/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_WMI_H
#define PEACH_WMI_H
#include "PeachHtc.h"
#include "PeachHtt.h"
#define PEACH_WMI_MEMORY_REQUESTS 16u
#define PEACH_WMI_REG_RULES 64u
#define PEACH_WMI_SCAN_CHANNELS 64u
#define PEACH_WMI_BSS_ENTRIES 32u
#define PEACH_WMI_PEERS 8u
#define PEACH_WMI_MGMT_BYTES 1024u
#define PEACH_WMI_MGMT_FRAMES 4u
enum { PeachWmiNoFrame = 1, PeachWmiScanFailed = -22 };
typedef struct {
    void *context;
    int (*allocate)(void *, size_t, PeachMhiDma *);
    void (*barrier)(void *);
    void (*log)(void *, const char *, uint32_t, uint32_t);
    uint64_t iova_base, iova_bytes;
} PeachWmiOps;
typedef struct {
    uint32_t id, unit_size, flags, units;
    PeachMhiDma dma;
} PeachWmiMemory;
typedef struct {
    unsigned valid, mode, pdev, phy, lmac, bands;
    uint32_t tx_chains, rx_chains, low_2g, high_2g, low_5g, high_5g;
} PeachWmiRadio;
typedef struct { uint32_t frequencies, bandwidth_power, flags; } PeachWmiRegRule;
typedef struct {
    unsigned valid, status, generation, count_2g, count_5g;
    uint32_t alpha2, country, domain, dfs;
    PeachWmiRegRule rules[PEACH_WMI_REG_RULES];
} PeachWmiRegDomain;
typedef struct { unsigned frequency, flags, power, antenna; } PeachWmiScanChannel;
typedef struct {
    unsigned frequency, ssid_bytes, ie_bytes, ies_truncated, dtim_period;
    uint64_t seen_us;
    int rssi;
    unsigned char bssid[6], ssid[32], ies[1024]; /* Apple80211 scan IE capacity */
    uint16_t capability, beacon_interval;
} PeachWmiBss;

typedef struct {
    unsigned used, type, create_sent, create_confirmed, delete_sent, delete_response;
    unsigned assoc_sent, assoc_confirmed, assoc_id, protected_network;
    uint32_t queues_sent;
    unsigned char mac[6];
} PeachWmiPeer;

typedef struct {
    unsigned bytes, frequency;
    int rssi;
    unsigned char frame[PEACH_WMI_MGMT_BYTES];
} PeachWmiMgmt;

typedef struct {
    unsigned aid, capabilities, protected_network, rate_count, qos;
    unsigned char rates[12]; /* 500kbps, including the basic-rate bit */
} PeachWmiAssociation;

typedef struct {
    unsigned installed;
    uint32_t generation;
    uint64_t rsc;
    unsigned char bytes[16];
} PeachWmiKey;
typedef struct {
    PeachHtc *htc;
    PeachWmiOps ops;
    unsigned phase, service_ready, ext_ready, ext2_ready, ready;
    unsigned mode_required, mode_sent, mode_accepted, unknown_events, peer_metadata_version;
    uint32_t firmware_abi[6], services[32], requests, chains, max_fragments;
    uint32_t last_event, last_tag;
    unsigned char mac[6];
    PeachWmiRadio radio;
    PeachWmiRegDomain regulatory[3];
    unsigned vdev_sent, scan_active, scan_sent, scan_started, scan_done, scan_finished;
    unsigned scan_id, scan_requestor, scan_reason, scan_event, channel_count, bss_count;
    unsigned bss_dropped;
    unsigned scan_reg_generation;
    PeachWmiScanChannel channels[PEACH_WMI_SCAN_CHANNELS];
    PeachWmiBss bss[PEACH_WMI_BSS_ENTRIES];
    PeachWmiPeer peers[PEACH_WMI_PEERS];
    unsigned vdev_start_sent, vdev_started, vdev_request, vdev_frequency, vdev_reg_generation;
    unsigned vdev_stop_sent, vdev_stopped, vdev_up_sent;
    unsigned char vdev_bssid[6];
    PeachMhiDma mgmt_rx_dma;
    unsigned mgmt_rx_started, mgmt_rx_read, mgmt_rx_write;
    uint64_t mgmt_rx_received, mgmt_rx_discarded;
    PeachMhiDma mgmt_dma;
    unsigned mgmt_in_flight, mgmt_done, mgmt_id, mgmt_status;
    unsigned mgmt_head, mgmt_count, mgmt_dropped;
    PeachWmiMgmt mgmt[PEACH_WMI_MGMT_FRAMES];
    PeachWmiKey keys[5]; /* pairwise slot0, GTK indices 0..3 in slots1..4 */
    unsigned key_pending, key_done, key_index, key_group;
    int error;
    PeachWmiMemory memory[PEACH_WMI_MEMORY_REQUESTS];
    unsigned char command[2048];
    unsigned scan_target_bytes, scan_target_bssid_valid;
    unsigned char scan_target_ssid[32], scan_target_bssid[6];
} PeachWmi;
int peach_wmi_init(PeachWmi *, PeachHtc *, const PeachWmiOps *);
/* HTC service callback: validates/copies capabilities, never allocates/waits. */
void peach_wmi_receive(PeachWmi *, const void *, size_t);
/* Actual SERVICE_READY -> host memory/INIT -> successful READY. Not scan,
 * association, data transport or an iOS network-interface implementation. */
/* Capabilities must be known before preparing host REO rings for INIT. */
int peach_wmi_wait_services(PeachWmi *);
int peach_wmi_bootstrap(PeachWmi *);
/* Native Peach management RX: retained coherent HOST4 ring, polled remote
 * tail; each buffer contains its 4B length prefix and an ordinary WMI event. */
int peach_wmi_start_management(PeachWmi *, PeachHtt *);
int peach_wmi_poll_management(PeachWmi *);
/* Optional bring-up diagnostic; the real asynchronous reply is logged. */
int peach_wmi_request_radio_stats(PeachWmi *);
/* One STA vdev, firmware-supplied regulatory rules, passive 2.4/5GHz scan.
 * Completion requires real scan events; BSS entries come only from received
 * beacon/probe-response bytes. PeachWmiScanFailed means a real terminal
 * failure event; a new scan is allowed. Timeout or transport failure still
 * requires containment because the firmware may retain the old request. */
int peach_wmi_passive_scan(PeachWmi *);
/* One manual selection only; filters received BSS records on the host.
 * It sends no directed probe and clears the filter before returning. */
int peach_wmi_passive_scan_target(PeachWmi *, const unsigned char *ssid, unsigned bytes,
                                   const unsigned char *bssid);
/* vdev0 only; type 0=self, 1=BSS. Optional firmware confirmation is waited
 * for only when advertised. The caller must ALSO wait for a real HTT map.
 * Delete retains the peer slot/queue memory; no drain or cache flush implied. */
int peach_wmi_peer_create(PeachWmi *, const unsigned char mac[6], unsigned type);
int peach_wmi_peer_delete(PeachWmi *, const unsigned char mac[6]);
/* Initial non-BA queues only, after a live HTT map. Success means command
 * submission: this ABI has no queue-setup ACK. Protected RX needs PN setup. */
int peach_wmi_peer_queue(PeachWmi *, const PeachHtt *, const unsigned char mac[6],
                         unsigned tid, PeachMhiDma);
/* Start on a scanned, regulatory-valid 20MHz BSS; wait for matching
 * successful START_RESP. No authentication/association or network link. */
int peach_wmi_vdev_start(PeachWmi *, unsigned bss_index, unsigned pmf);
int peach_wmi_vdev_stop(PeachWmi *);
/* No installed keys or outstanding management TX. Requires an observed
 * peer generation, real STOP/DELETE responses and HTT unmap before reset.
 * Queue descriptors and packet DMA remain allocated by their owners. */
int peach_wmi_retire_unkeyed(PeachWmi *, PeachHtt *, uint32_t peer_generation);
/* Program four EDCA queues from the AP's WMM parameters, or defaults
 * for a legacy AP. No U-APSD or MU-EDCA is enabled. */
int peach_wmi_wmm(PeachWmi *, const unsigned char *ies, size_t bytes);
/* One retained pass-by-reference buffer. Reuse only after matching real
 * MGMT_TX_COMPLETION. RX frames are copied out of CE before callback return.
 * These primitives do not manufacture auth/association responses. */
int peach_wmi_mgmt_tx(PeachWmi *, const void *, size_t);
int peach_wmi_mgmt_rx(PeachWmi *, PeachWmiMgmt *);
/* Caller has already validated the AP's successful on-air association
 * response. WMI ASSOC_CONF only acknowledges the firmware configuration.
 * Legacy 20MHz, one stream, optional WMM; protected peers remain unauthorized. */
int peach_wmi_peer_assoc(PeachWmi *, const PeachHtt *, const PeachWmiAssociation *);
/* No ACK in this ABI: success records vdev_up_sent, not a connected NIC. */
int peach_wmi_vdev_up(PeachWmi *);
/* Caller has authenticated the EAPOL key material. AES-CCMP128 only,
 * serialized matching INSTALL_KEY_COMPLETE required. Repeated same key is
 * not reinstalled and does not reset its replay counter. ACK/SEC_IND alone
 * do not authorize the port; RX PN setup and the 4-way exchange remain. */
int peach_wmi_install_ccmp(PeachWmi *, const PeachHtt *, unsigned group, unsigned index,
                           const unsigned char key[16], uint64_t rsc);
/* Submit firmware peer authorization only after real association and, for
 * protected peers, successful key ACKs plus matching HTT security state. */
int peach_wmi_authorize(PeachWmi *, const PeachHtt *);
#endif
