/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Standard-command decoding: nezha17 NezhaBattery.c, with independent validity,
 * explicit units and no inference of cable state from SOC/current. */
#include "NezhaPower.h"
#include <string.h>

const uint8_t np_gauge_commands[NP_GAUGE_WORDS] =
    {0x08, 0x0c, 0x06, 0x2c, 0x10, 0x12, 0x3c, 0x2a, 0x2e, 0x0a};

int np_decode_gauge(NpGauge *g, const uint8_t b[NP_GAUGE_WORDS][2],
                    uint32_t mask, uint64_t begin, uint64_t end, uint64_t gen)
{
    if (!g || !b) return -1;
    memset(g, 0, sizeof(*g));
    g->begin_ms = begin; g->end_ms = end; g->generation = gen;
    if (!gen || end < begin || end - begin > NP_MAX_SKEW_MS) return -1;
    for (unsigned i = 0; i < NP_GAUGE_WORDS; ++i)
        g->raw[i] = (uint16_t)b[i][0] | (uint16_t)b[i][1] << 8;
#define HAS(i) ((mask & (1u << (i))) && g->raw[i] != UINT16_MAX)
    if (HAS(0) && g->raw[0] >= 2500 && g->raw[0] <= 5000) {
        g->voltage_mv = g->raw[0]; g->present = 1;
        g->valid |= NP_BIT(NP_VOLTAGE) | NP_BIT(NP_PRESENT);
    }
    /* 0xffff is a legitimate -1 mA. Unlike unsigned fields it is not a
     * sentinel. Decode two's complement without implementation-defined casts. */
    if (mask & (1u << 1)) {
        g->current_ma = g->raw[1] < 0x8000 ? g->raw[1] : (int32_t)g->raw[1] - 65536;
        if (g->current_ma != -32768) g->valid |= NP_BIT(NP_CURRENT);
    }
    if (HAS(2) && g->raw[2] >= 2331 && g->raw[2] <= 3631) {
        /* BQ 0.1 kelvin. Exact conversion in centi-C is raw*10 - 27315.
         * Android's historical integer 2730 approximation is a separate UI
         * convention, not a calibration correction. */
        g->temperature_centi_c = (int32_t)g->raw[2] * 10 - 27315;
        g->valid |= NP_BIT(NP_TEMPERATURE);
    }
    if (HAS(3) && g->raw[3] <= 100) {
        g->soc = g->raw[3]; g->valid |= NP_BIT(NP_SOC);
    }
    if (HAS(4)) { g->remaining_mah = g->raw[4]; g->valid |= NP_BIT(NP_REMAINING); }
    if (HAS(5) && g->raw[5]) { g->full_mah = g->raw[5]; g->valid |= NP_BIT(NP_FULL_CAPACITY); }
    if (HAS(6) && g->raw[6]) { g->design_mah = g->raw[6]; g->valid |= NP_BIT(NP_DESIGN); }
    if (HAS(7)) { g->cycles = g->raw[7]; g->valid |= NP_BIT(NP_CYCLES); }
    /* Health's low byte alone is not a system health/identity certificate. */
    if (HAS(9)) { g->full_flag = !!(g->raw[9] & 0x20); g->valid |= NP_BIT(NP_FULL_FLAG); }
#undef HAS
    return (g->valid & NP_GAUGE_REQUIRED) == NP_GAUGE_REQUIRED ? 0 : -1;
}

static int fresh(uint64_t now, uint64_t begin, uint64_t end, uint64_t ttl)
{
    return end >= begin && end - begin <= NP_MAX_SKEW_MS && now >= end && now - begin <= ttl;
}

void np_snapshot(NpSnapshot *s, const NpGauge *g, const NpCharger *c,
                 uint64_t now, uint64_t generation)
{
    memset(s, 0, sizeof(*s));
    s->at_ms = now; s->generation = generation;
    if (g) s->gauge = *g;
    if (c) s->charger = *c;
    int gf = g && g->generation && fresh(now, g->begin_ms, g->end_ms, NP_GAUGE_TTL_MS);
    int cf = c && c->generation && fresh(now, c->begin_ms, c->end_ms, NP_CHARGER_TTL_MS);
    /* The publisher commits whole refresh cohorts. Reject a caller mixing
     * otherwise fresh data from different refresh generations. */
    if (!generation || (g && g->generation != generation)) gf = 0;
    if (!generation || (c && (c->generation != generation || !c->epoch))) cf = 0;
    if (gf && cf) {
        const uint64_t first = g->begin_ms < c->begin_ms ? g->begin_ms : c->begin_ms;
        const uint64_t last = g->end_ms > c->end_ms ? g->end_ms : c->end_ms;
        if (last - first > NP_MAX_SKEW_MS) cf = 0;
    }
    if (gf) s->valid = g->valid;
    else s->faults |= NP_FAULT_STALE;
    if (!gf || (g->valid & NP_GAUGE_REQUIRED) != NP_GAUGE_REQUIRED) s->faults |= NP_FAULT_SAMPLE;
    if (!cf) s->faults |= NP_FAULT_STALE;
    if (c) s->faults |= c->fault;
    if (cf && (c->valid & NP_BIT(NP_EXTERNAL))) {
        s->external = c->external;
        s->valid |= NP_BIT(NP_EXTERNAL);
        if (!s->external) {
            s->valid |= NP_BIT(NP_CHARGING) | NP_BIT(NP_CHARGE_DONE);
        } else if (c->valid & NP_BIT(NP_CHARGING)) {
            s->charging = c->charging;
            s->valid |= NP_BIT(NP_CHARGING);
        }
        if (s->external && (c->valid & NP_BIT(NP_CHARGE_DONE))) {
            s->fully_charged = c->done; s->valid |= NP_BIT(NP_CHARGE_DONE);
        }
    }
    /* FC=1 and SOC=100 do not by themselves establish external supply or
     * charger completion. Never manufacture those states from gauge data. */
    if (cf) s->valid |= c->valid & (NP_BIT(NP_USB_VOLTAGE) | NP_BIT(NP_USB_CURRENT));
    if (cf && c->input_pair_coherent && (c->valid & (NP_BIT(NP_USB_VOLTAGE) | NP_BIT(NP_USB_CURRENT))) ==
              (NP_BIT(NP_USB_VOLTAGE) | NP_BIT(NP_USB_CURRENT)) && c->usb_uv >= 0 && c->usb_ua >= 0) {
        s->usb_power_uw = (int64_t)c->usb_uv * c->usb_ua / 1000000;
        s->usb_power_valid = 1;
        s->valid |= NP_BIT(NP_USB_VOLTAGE) | NP_BIT(NP_USB_CURRENT);
    }
}
