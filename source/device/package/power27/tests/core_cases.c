/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Prepared only: NOT compiled or executed before the user selects a package.
 * Add to the chosen package's existing host-test runner after that gate. */
#include "NezhaPower.h"
#include "NezhaMca.h"
#include "NezhaCharge.h"
#include <assert.h>
#include <string.h>

static void word(uint8_t *p, uint16_t x) { p[0] = x; p[1] = x >> 8; }
static void gauge_cases(void) {
    uint8_t bytes[NP_GAUGE_WORDS][2] = {{0}};
    const uint16_t words[] = {3825, 65039, 3102, 45, 2653, 5941, 6800, 318, 100, 0};
    for (unsigned i = 0; i < NP_GAUGE_WORDS; ++i) word(bytes[i], words[i]);
    NpGauge g; NpSnapshot s;
    assert(np_decode_gauge(&g, bytes, 0x3ff, 100, 120, 1) == 0);
    assert(g.current_ma == -497 && g.temperature_centi_c == 3705 && g.design_mah == 6800);
    assert(g.remaining_mah == 2653 && g.full_mah == 5941 && g.cycles == 318);
    np_snapshot(&s, &g, 0, 130, 1);
    assert(!(s.valid & NP_BIT(NP_EXTERNAL))); /* SOC/current never prove cable */
    word(bytes[3], 100); word(bytes[9], 0x20); word(bytes[1], 0xffff);
    word(bytes[7], 0); word(bytes[4], 0);
    assert(np_decode_gauge(&g, bytes, 0x3ff, 200, 220, 2) == 0);
    assert(g.current_ma == -1 && g.cycles == 0 && g.remaining_mah == 0);
    np_snapshot(&s, &g, 0, 230, 2); assert(!(s.valid & NP_BIT(NP_EXTERNAL)));
    np_snapshot(&s, &g, 0, 200 + NP_GAUGE_TTL_MS + 1, 2);
    assert(!(s.valid & NP_BIT(NP_SOC)) && (s.faults & NP_FAULT_STALE));
    NpCharger c = {0}; c.epoch = 1; c.generation = 1; c.begin_ms = 200; c.end_ms = 220;
    c.valid = NP_BIT(NP_EXTERNAL) | NP_BIT(NP_CHARGING) | NP_BIT(NP_USB_VOLTAGE) | NP_BIT(NP_USB_CURRENT);
    c.external = c.charging = 1; c.usb_uv = 9000000; c.usb_ua = 2000000;
    np_snapshot(&s, &g, &c, 230, 2); assert(!(s.valid & NP_BIT(NP_EXTERNAL))); /* different generation */
    c.generation = 2;
    np_snapshot(&s, &g, &c, 230, 2); assert(s.external && s.charging && !s.usb_power_valid);
    c.input_pair_coherent = 1;
    np_snapshot(&s, &g, &c, 230, 2); assert(!s.usb_power_valid); /* unknown measurement point */
    c.input_current_scope = NP_INPUT_CURRENT_WHOLE_PORT;
    np_snapshot(&s, &g, &c, 230, 2); assert(s.usb_power_valid && s.usb_power_uw == 18000000);
    /* Original Android PD baseline: VBUS~18.4V, buck IBUS0 while the battery
     * charges. Even a coherent BUCK pair must not publish total USB power0. */
    c.usb_uv = 18387000; c.usb_ua = 0; c.input_current_scope = NP_INPUT_CURRENT_MAIN_BUCK;
    np_snapshot(&s, &g, &c, 230, 2);
    assert(s.external && s.charging && !s.usb_power_valid && !s.usb_power_uw);
    assert(s.valid & NP_BIT(NP_USB_CURRENT)); /* retain the valid branch reading */
    c.usb_ua = 500000;
    np_snapshot(&s, &g, &c, 230, 2); assert(!s.usb_power_valid);
    c.input_current_scope = NP_INPUT_CURRENT_WHOLE_PORT; c.input_pair_coherent = 0;
    np_snapshot(&s, &g, &c, 230, 2); assert(!s.usb_power_valid); /* scope cannot bypass timing */
    word(bytes[3], 101); np_decode_gauge(&g, bytes, 0x3ff, 300, 320, 3);
    assert(!(g.valid & NP_BIT(NP_SOC)));
    assert(np_decode_gauge(&g, bytes, 0x3ff, 400, 300, 4) == -1);
    assert(!g.valid && g.raw[0] == 3825 && g.raw[3] == 101);
    assert(np_decode_gauge(&g, bytes, 0x3ff, 500, 500 + NP_MAX_SKEW_MS + 1, 5) == -1);
    assert(!g.valid && g.raw[0] == 3825 && g.raw[1] == 0xffff);
}
static void mca_cases(void) {
    NpMca m; uint8_t request[NP_MCA_REQUEST], reply[NP_MCA_RESPONSE] = {0};
    uint32_t note = 0, value = 0;
    np_mca_init(&m); np_mca_link(&m, 1);
    assert(np_mca_begin(&m, 0x20001, NP_MCA_READ, 0, 0, 4, request) == NP_MCA_REQUEST);
    assert(np_le32(request) == 0x800a && np_le32(request + 16) == 1);
    memcpy(reply, request, 16); np_put32(reply + 20, m.sequence); np_put32(reply + 24, 1);
    assert(np_mca_receive(&m, m.epoch, 99, reply, sizeof(reply), &note, &value) == NP_MCA_IGNORE); /* not sent */
    np_mca_sent(&m, 100, 250);
    assert(np_mca_receive(&m, m.epoch, 110, reply, 279, &note, &value) == NP_MCA_IGNORE);
    np_put32(reply + 20, 2);
    assert(np_mca_receive(&m, m.epoch, 120, reply, sizeof(reply), &note, &value) == NP_MCA_IGNORE);
    np_put32(reply + 20, 1);
    assert(np_mca_receive(&m, m.epoch, 349, reply, sizeof(reply), &note, &value) == NP_MCA_REPLY);
    assert(np_le32(m.reply) == 1);
    assert(np_mca_receive(&m, m.epoch, 349, reply, sizeof(reply), &note, &value) == NP_MCA_IGNORE);
    assert(np_mca_begin(&m, 0x20001, NP_MCA_READ, 0, 0, 4, request) > 0);
    np_mca_sent(&m, 500, 250); np_put32(reply + 20, m.sequence);
    assert(np_mca_receive(&m, m.epoch, 750, reply, sizeof(reply), &note, &value) == NP_MCA_IGNORE);
    assert(m.timeouts == 1 && !m.pending);
    uint64_t old_epoch = m.epoch; uint32_t old_sequence = m.sequence;
    np_mca_link(&m, 0); np_mca_link(&m, 1);
    assert(np_mca_begin(&m, 0x20001, NP_MCA_READ, 0, 0, 4, request) > 0);
    assert(m.sequence == old_sequence + 1); np_mca_sent(&m, 800, 250);
    np_put32(reply + 20, m.sequence);
    assert(np_mca_receive(&m, old_epoch, 810, reply, sizeof(reply), &note, &value) == NP_MCA_IGNORE);
    np_put32(reply + 16, 7);
    assert(np_mca_receive(&m, m.epoch, 820, reply, sizeof(reply), &note, &value) == NP_MCA_ERROR);
    uint8_t notification[NP_MCA_NOTIFICATION] = {0};
    np_put32(notification, NP_MCA_OWNER); np_put32(notification + 4, 2);
    np_put32(notification + 12, 0); np_put32(notification + 16, 7);
    assert(np_mca_receive(&m, m.epoch, 830, notification, sizeof(notification), &note, &value) == NP_MCA_NOTE);
    assert(note == 0 && value == 7); /* advisory value is NOT USB online=true */
    assert(np_mca_receive(&m, m.epoch, 831, notification, 16, &note, &value) == NP_MCA_IGNORE);
    m.sequence = UINT32_MAX;
    assert(np_mca_begin(&m, 1, NP_MCA_READ, 0, 0, 4, request) < 0 && m.exhausted);
}
static NpChargeFacts qualified_facts(void) {
    NpChargeFacts f = {0};
    f.epoch = f.profile = 1; f.now_ms = f.sample_ms = 100;
    f.wired = f.sink = f.external = f.sample_valid = f.protocol_verified = 1;
    f.battery_authenticated = f.adapter_authenticated = f.cable_authenticated = 1;
    f.protections_verified = f.autonomous_fault_shutdown = f.limits_valid = f.remote_ready = 1;
    f.pps_min_mv = 5000; f.pps_max_mv = 11000;
    f.pps_max_ma = f.cable_max_ma = f.input_limit_ma = f.thermal_input_ma = 3000;
    f.battery_limit_ma = 4000; f.board_power_mw = 30000;
    f.requested_mv = 9000; f.requested_ma = 2000; f.requested_battery_ma = 3900;
    f.pump_off_confirmed = 1; return f;
}
static void charge_cases(void) {
    NpChargePolicy p; NpChargeCommand c; NpChargeFacts f = qualified_facts(); uint32_t wire;
    assert(!np_pps_encode(9000, 2000, &wire) && wire == (450u << 16 | 40));
    assert(np_pps_encode(9001, 2000, &wire) < 0);
    np_charge_init(&p); f.adapter_authenticated = 0;
    assert(np_charge_step(&p, &f, &c) == 0);
    f.adapter_authenticated = 1;
    assert(np_charge_step(&p, &f, &c) == 1 && c.operation == NP_ACTION_CONFIGURE_DISABLED_PUMP);
    assert(np_charge_ack(&p, c.epoch, c.request_id, 0, 101) == 1);
    f.now_ms = 102; assert(np_charge_step(&p, &f, &c) == 1 && c.operation == NP_ACTION_REQUEST_PPS);
    uint64_t stale_id = c.request_id;
    f.thermal_input_ma = 500; f.now_ms = 103;
    assert(np_charge_step(&p, &f, &c) == 1 && c.operation == NP_ACTION_DISABLE_PUMP);
    assert(np_charge_ack(&p, c.epoch, stale_id, 0, 104) == 0);
    assert(np_charge_ack(&p, c.epoch, c.request_id, 0, 104) == 1);
    f.now_ms = 105;
    assert(np_charge_step(&p, &f, &c) == 1 && c.operation == NP_ACTION_RELEASE_OWN_VOTES);
    assert(np_charge_ack(&p, c.epoch, c.request_id, 0, 106) == 1 && !p.owned);
    f = qualified_facts(); np_charge_init(&p);
    assert(np_charge_step(&p, &f, &c) == 1);
    f.epoch = 2;
    assert(np_charge_step(&p, &f, &c) == -1 && p.state == NP_POLICY_QUARANTINE);
}
int main(void) { gauge_cases(); mca_cases(); charge_cases(); return 0; }
