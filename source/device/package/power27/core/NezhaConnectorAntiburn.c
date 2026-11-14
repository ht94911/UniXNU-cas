/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Independent evaluator of N0 facts verified in OS4 module56a04d0f. The MCA
 * GPL source is a research reference, not copied code. This core performs no
 * hardware action; only the existing owner may compose its charge vote. */
#include "NezhaConnectorAntiburn.h"
#include <limits.h>
#include <string.h>

typedef struct { int value; uint32_t reason; } Predicate;

static Predicate either(Predicate a, Predicate b)
{
    if (a.value == 1 || b.value == 1)
        return (Predicate){1, (a.value == 1 ? a.reason : 0) |
                              (b.value == 1 ? b.reason : 0)};
    if (a.value < 0 || b.value < 0)
        return (Predicate){-1, (a.value < 0 ? a.reason : 0) |
                               (b.value < 0 ? b.reason : 0)};
    return (Predicate){0, 0};
}

static Predicate both(Predicate a, Predicate b)
{
    if (!a.value || !b.value)
        return (Predicate){0, 0};
    if (a.value < 0 || b.value < 0)
        return (Predicate){-1, (a.value < 0 ? a.reason : 0) |
                               (b.value < 0 ? b.reason : 0)};
    return (Predicate){1, a.reason | b.reason};
}

static Predicate source_gate(const NpConnectorAntiburnSignals *s)
{
    Predicate usb = s->usb_online.valid ?
        (Predicate){s->usb_online.value != 0, 0} :
        (Predicate){-1, NP_AB_SOURCE_UNAVAILABLE};
    Predicate cid = s->cid.valid ? (Predicate){s->cid.value != 0, 0} :
        (Predicate){-1, NP_AB_SOURCE_UNAVAILABLE | NP_AB_CID_UNAVAILABLE};
    return either(usb, cid);
}

static Predicate clear_signal(NpConnectorAntiburnSignal signal, uint32_t reason)
{
    return signal.valid ? (Predicate){signal.value == 0, 0} :
                          (Predicate){-1, reason};
}

static Predicate recover(const NpConnectorAntiburnVote *v,
                         const NpConnectorAntiburnSignals *s)
{
    Predicate cold = {v->effective_temperature_c < NP_AB_RESTORE_C, 0};
    Predicate rate = v->selected_rate_valid || v->selected_rate_initialized ?
        (Predicate){v->selected_rate_c_per_s < NP_AB_MAX_RATE_C_PER_S, 0} :
        (Predicate){-1, NP_AB_RATE_UNAVAILABLE};
    return both(both(cold, rate), both(clear_signal(s->cid, NP_AB_CID_UNAVAILABLE),
        clear_signal(s->otg_plugin, NP_AB_OTG_UNAVAILABLE)));
}

static void withdraw_values(NpConnectorAntiburnVote *v)
{
    v->charge_allowed = v->decision_complete = 0;
    v->temperature_valid_mask = v->rate_valid_mask = v->rate_initialized_mask = 0;
    v->temperature_c[0] = v->temperature_c[1] = INT32_MIN;
    v->effective_temperature_c = INT32_MIN;
    v->selected_index = UINT32_MAX;
    v->selected_rate_valid = v->selected_rate_initialized = 0;
    v->selected_rate_c_per_s = v->rate_c_per_s[0] = v->rate_c_per_s[1] = 0;
    v->rate_interval_ms = 0;
}

static void break_history(NpConnectorAntiburn *s)
{
    s->history_valid = 0;
    s->history_rate_valid_mask = s->history_rate_initialized_mask = 0;
}

void np_connector_antiburn_init(NpConnectorAntiburn *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    withdraw_values(&s->vote);
    s->vote.reason = NP_AB_TEMP_UNAVAILABLE;
}

void np_connector_antiburn_invalidate(NpConnectorAntiburn *s, int32_t error)
{
    if (!s) return;
    break_history(s);
    withdraw_values(&s->vote);
    s->vote.latch = s->latch;
    s->vote.cooldown_wait = s->cooldown_wait;
    s->vote.cooldown_reason = s->cooldown_reason;
    s->vote.reason = NP_AB_WITHDRAWN | (s->latch ? NP_AB_LATCHED : 0) |
                    (s->cooldown_wait ? NP_AB_COOLDOWN_WAIT : 0);
    s->vote.error = error ? error : NP_AB_BAD_TEMPERATURE;
    s->last_error = s->vote.last_error = s->vote.error;
}

static uint32_t input_reason(const NpConnectorAntiburn *s,
                            const NpConnectorThermal *c, uint64_t now,
                            int32_t *error)
{
    uint32_t reason = 0;
    int32_t io_error = c ? c->error : 0;
    if (!c || !c->enabled || !c->generation || c->attempted != 3 ||
        c->valid_mask != 3 || c->retained || c->error)
        reason |= NP_AB_TEMP_UNAVAILABLE;
    if (s->has_now && now < s->last_now_ms)
        reason |= NP_AB_TIME_ORDER;
    if (c) {
        if (s->generation_high_watermark &&
            c->generation < s->generation_high_watermark)
            reason |= NP_AB_GENERATION_ORDER;
        if (c->begin_ms > c->end_ms || c->end_ms > now ||
            c->sampled_ms[0] < c->begin_ms ||
            c->sampled_ms[1] < c->sampled_ms[0] ||
            c->sampled_ms[1] > c->end_ms)
            reason |= NP_AB_TIME_ORDER;
        if (c->generation && s->generation_high_watermark &&
            c->generation >= s->generation_high_watermark) {
            if (c->end_ms < s->end_high_watermark)
                reason |= NP_AB_TIME_ORDER;
            else if (c->generation == s->generation_high_watermark &&
                     c->end_ms != s->end_high_watermark)
                reason |= NP_AB_GENERATION_ORDER;
        }
        for (unsigned i = 0; i < 2; ++i) {
            if (!c->reading[i].converted || !c->reading[i].quiescent ||
                c->reading[i].error || c->reading[i].centi_c == INT32_MIN)
                reason |= NP_AB_TEMP_UNAVAILABLE;
            if (!io_error && c->reading[i].error)
                io_error = c->reading[i].error;
        }
        if (now >= c->end_ms && now - c->end_ms > NP_CONNECTOR_TTL_MS)
            reason |= NP_AB_TEMP_STALE;
    }
    if (reason & NP_AB_TIME_ORDER) *error = NP_AB_BAD_TIME;
    else if (reason & NP_AB_GENERATION_ORDER) *error = NP_AB_BAD_GENERATION;
    else if (reason & NP_AB_TEMP_STALE) *error = NP_AB_STALE_TEMPERATURE;
    else if (reason) *error = io_error ? io_error : NP_AB_BAD_TEMPERATURE;
    return reason;
}

static int64_t measured_rate(int32_t current, int32_t previous, uint64_t gap)
{
    /* Wider multiplication preserves the specified integer-C formula without
     * inheriting the reference implementation's signed32 overflow. */
    int64_t numerator = ((int64_t)current - previous) * 1000;
    uint64_t magnitude = (uint64_t)(numerator < 0 ? -numerator : numerator);
    int64_t quotient = (int64_t)(magnitude / gap);
    return numerator < 0 ? -quotient : quotient;
}

static void temperatures_and_rates(NpConnectorAntiburn *s, NpConnectorAntiburnVote *v)
{
    const NpConnectorThermal *c = &v->cohort;
    v->temperature_valid_mask = 3;
    for (unsigned i = 0; i < 2; ++i)
        v->temperature_c[i] = c->reading[i].centi_c / 100;
    if (s->history_valid && c->generation == s->history_generation) {
        v->rate_valid_mask = s->history_rate_valid_mask;
        v->rate_initialized_mask = s->history_rate_initialized_mask;
        for (unsigned i = 0; i < 2; ++i)
            v->rate_c_per_s[i] = s->history_rate_c_per_s[i];
        v->rate_interval_ms = s->vote.rate_interval_ms;
    } else {
        if (s->history_valid && c->end_ms > s->history_end_ms) {
            v->rate_interval_ms = c->end_ms - s->history_end_ms;
            for (unsigned i = 0; i < 2; ++i)
                v->rate_c_per_s[i] = measured_rate(v->temperature_c[i],
                    s->history_temperature_c[i], v->rate_interval_ms);
            v->rate_valid_mask = 3;
        } else if (!s->ever_initialized) {
            v->rate_initialized_mask = 3;
            s->ever_initialized = 1;
        }
        s->history_generation = c->generation;
        s->history_end_ms = c->end_ms;
        s->history_rate_valid_mask = v->rate_valid_mask;
        s->history_rate_initialized_mask = v->rate_initialized_mask;
        for (unsigned i = 0; i < 2; ++i) {
            s->history_temperature_c[i] = v->temperature_c[i];
            s->history_rate_c_per_s[i] = v->rate_c_per_s[i];
        }
        s->history_valid = 1;
    }
    v->selected_index = v->temperature_c[1] > v->temperature_c[0] ? 1 : 0;
    v->effective_temperature_c = v->temperature_c[v->selected_index];
    if (v->effective_temperature_c < NP_AB_MIN_EFFECTIVE_C)
        v->effective_temperature_c = NP_AB_MIN_EFFECTIVE_C;
    v->selected_rate_c_per_s = v->rate_c_per_s[v->selected_index];
    v->selected_rate_valid = (v->rate_valid_mask >> v->selected_index) & 1u;
    v->selected_rate_initialized = (v->rate_initialized_mask >> v->selected_index) & 1u;
}

static void decide(NpConnectorAntiburn *s, NpConnectorAntiburnVote *v,
                   const NpConnectorAntiburnSignals *signals)
{
    Predicate temperature = {v->effective_temperature_c >= NP_AB_TRIGGER_C,
                             NP_AB_TRIGGER_TEMPERATURE};
    Predicate board = {0, 0}, rate = {0, 0};
    if (v->effective_temperature_c >= NP_AB_WITH_BOARD_C)
        board = signals->board_temperature_c.valid ?
            (Predicate){signals->board_temperature_c.value <= NP_AB_BOARD_MAX_C, NP_AB_TRIGGER_BOARD} :
            (Predicate){-1, NP_AB_BOARD_UNAVAILABLE | NP_AB_TRIGGER_BOARD};
    if (v->effective_temperature_c >= NP_AB_WITH_RATE_C)
        rate = v->selected_rate_valid || v->selected_rate_initialized ?
            (Predicate){v->selected_rate_c_per_s >= NP_AB_MAX_RATE_C_PER_S, NP_AB_TRIGGER_RATE} :
            (Predicate){-1, NP_AB_RATE_UNAVAILABLE | NP_AB_TRIGGER_RATE};
    Predicate heat = either(temperature, either(board, rate));
    Predicate source = source_gate(signals);
    Predicate trigger = both(heat, source);
    if (!s->latch && trigger.value == 1) {
        s->latch = 1;
        s->cooldown_wait = s->cooldown_reason = 0;
        v->reason = trigger.reason | NP_AB_LATCHED;
        v->decision_complete = 1;
        return;
    }
    if (!s->latch && trigger.value < 0) {
        /* A missing differencing history alone is transient. Persist cooling
         * only for the observed>=60/unknown-board risk, or a known heat
         * predicate whose USB/CID trigger gate is unavailable. */
        if ((board.value < 0 && source.value != 0) ||
            (heat.value == 1 && source.value < 0)) {
            s->cooldown_wait = 1;
            s->cooldown_reason |= trigger.reason;
        }
        v->reason = trigger.reason | (s->cooldown_wait ? NP_AB_COOLDOWN_WAIT : 0);
        return;
    }
    if (s->latch || s->cooldown_wait) {
        Predicate recovery = recover(v, signals);
        v->reason = (s->latch ? NP_AB_LATCHED : NP_AB_COOLDOWN_WAIT) | recovery.reason;
        if (recovery.value < 0) return;
        v->decision_complete = 1;
        if (recovery.value == 1) {
            s->latch = s->cooldown_wait = s->cooldown_reason = 0;
            v->reason = NP_AB_RECOVERED;
            v->charge_allowed = 1;
        }
        return;
    }
    v->decision_complete = v->charge_allowed = 1;
}

int np_connector_antiburn_step(NpConnectorAntiburn *s, const NpConnectorThermal *c,
                              const NpConnectorAntiburnSignals *signals, uint64_t now)
{
    if (!s) return NP_AB_ARGUMENT;
    NpConnectorAntiburnVote v;
    NpConnectorAntiburnSignals unavailable = {0};
    memset(&v, 0, sizeof(v));
    withdraw_values(&v);
    v.now_ms = now;
    if (c) v.cohort = *c;
    v.reason = input_reason(s, c, now, &v.error);
    if (!s->has_now || now >= s->last_now_ms) s->last_now_ms = now;
    s->has_now = 1;
    if (c && c->generation >= s->generation_high_watermark &&
        !(v.reason & (NP_AB_TIME_ORDER | NP_AB_GENERATION_ORDER))) {
        s->generation_high_watermark = c->generation;
        s->end_high_watermark = c->end_ms;
    }
    if (v.reason) {
        break_history(s);
        s->last_error = v.error;
        v.reason |= (s->latch ? NP_AB_LATCHED : 0) |
                    (s->cooldown_wait ? NP_AB_COOLDOWN_WAIT : 0);
    } else {
        temperatures_and_rates(s, &v);
        decide(s, &v, signals ? signals : &unavailable);
    }
    v.latch = s->latch;
    v.cooldown_wait = s->cooldown_wait;
    v.cooldown_reason = s->cooldown_reason;
    v.last_error = s->last_error;
    s->vote = v;
    return 0;
}

int np_connector_antiburn_copy(const NpConnectorAntiburn *s, NpConnectorAntiburnVote *out)
{
    if (!s || !out) return NP_AB_ARGUMENT;
    *out = s->vote;
    return 0;
}
