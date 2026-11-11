/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * nezha (SM8850, IPA v5.5 / GSI v5.0) version of QcomBaseband ipa/coachz_gsi_proto.h (dd0199c, BSD-2-Clause,
 * HoshinoYumeka), derived by tools/baseband27/derive-ipa55.py: same functions and names so coachz_gsi.c reads either.
 * Changes follow the mainline gsi.c version branches between v4.2 and v5.5:
 *   - the AP no longer allocates modem channels (gsi_channel_init modem_alloc is v4.2 only);
 *   - the event ring index moved from CH_C_CNTXT_0.ERINDEX to CH_C_CNTXT_1.CH_ERINDEX (v5.0+);
 *   - CH_C_QOS: PREFETCH_MODE = escape buffer only replaces USE_ESCAPE_BUF_ONLY (v4.5+), DB_IN_BYTES on every
 *     channel (v4.9+);
 *   - the event ring count comes from HW_PARAM_4.EV_PER_EE (v5.0+);
 *   - GENERIC_CMD has a params field (v4.11+, 0 here); disabling modem flow control may answer RETRY (v4.11+).
 * CHTYPE_PROTOCOL has no MSB part on v5.0+ (ch_c_cntxt_0_type_encode), as on v4.2. Registers from nb_ipa_v55.h.
 */
#ifndef COACHZ_GSI_PROTO_H
#define COACHZ_GSI_PROTO_H

#include <stdint.h>
#include "nb_ipa55_proto.h"

#define CGS_CHANNEL_COUNT_MAX  28u   /* GSI_CHANNEL_COUNT_MAX */
#define CGS_EVT_RING_COUNT_MAX 28u   /* GSI_EVT_RING_COUNT_MAX */
#define CGS_TLV_MAX            64u
#define CGS_ELEMENT            16u   /* GSI_RING_ELEMENT_SIZE */
#define CGS_COMMAND_TRE_MAX    8u    /* IPA_COMMAND_TRANS_TRE_MAX */
#define CGS_INT_MODT           32u   /* GSI_EVT_RING_INT_MODT: 1 ms of the 32 kHz timer */
#define CGS_CMD_TIMEOUT_MS     50u
#define CGS_PREFETCH_ESCAPE_BUF_ONLY 1u   /* enum gsi_prefetch_mode */
#define CGS_FLOW_RETRIES       5u    /* GSI_CHANNEL_MODEM_FLOW_RETRIES */
#define CGS_EE_AP    0u
#define CGS_EE_MODEM 1u
#define CGS_TYPE_GPI 2u

/* enum gsi_channel_state / gsi_evt_ring_state */
#define CGS_STATE_NOT_ALLOCATED 0u
#define CGS_STATE_ALLOCATED     1u
#define CGS_STATE_STARTED       2u
#define CGS_STATE_STOPPED       3u
#define CGS_STATE_STOP_IN_PROC  4u
#define CGS_STATE_ERROR         0xfu
/* opcodes */
#define CGS_CH_ALLOCATE 0x0u
#define CGS_CH_START    0x1u
#define CGS_CH_STOP     0x2u
#define CGS_CH_RESET    0x9u
#define CGS_CH_DE_ALLOC 0xau
#define CGS_EVT_ALLOCATE 0x0u
#define CGS_EVT_RESET    0x9u
#define CGS_EVT_DE_ALLOC 0xau
#define CGS_GENERIC_HALT_CHANNEL     0x1u
#define CGS_GENERIC_ALLOCATE_CHANNEL 0x2u
#define CGS_GENERIC_ENABLE_FLOW_CONTROL  0x3u
#define CGS_GENERIC_DISABLE_FLOW_CONTROL 0x4u
/* enum gsi_irq_type_id, gsi_global_irq_id, gsi_general_irq_id */
#define CGS_IRQ_CH_CTRL  0x01u
#define CGS_IRQ_EV_CTRL  0x02u
#define CGS_IRQ_GLOB_EE  0x04u
#define CGS_IRQ_IEOB     0x08u
#define CGS_IRQ_GENERAL  0x40u
#define CGS_ERROR_INT 0x1u
#define CGS_GP_INT1   0x2u
#define CGS_BUS_ERROR 0x2u
#define CGS_CMD_FIFO_OVRFLOW  0x4u
#define CGS_MCS_STACK_OVRFLOW 0x8u
#define CGS_ISR_MAX_ITER 50u
/* gsi_err_type / code, generic EE result */
#define CGS_ERR_TYPE_CHAN 2u
#define CGS_ERR_TYPE_EVT  3u
#define CGS_OUT_OF_RESOURCES 3u
#define CGS_GENERIC_SUCCESS 1u
#define CGS_GENERIC_INCORRECT_CHANNEL_STATE 2u
#define CGS_GENERIC_RETRY 6u
#define CGS_EAGAIN 35   /* XNU EAGAIN */
#define CGS_EIO    5

/* ------------------------------------------------------------------ gsi_init: the channel table */
struct cgs_channel {
    uint8_t defined, command, toward_ipa, evt_ring;
    uint16_t tre_count, event_count;   /* ring entries; tre_count is the ring size, tre_limit what may be used */
    uint16_t tre_limit;
    uint8_t trans_tre_max;             /* tlv_count */
};
struct cgs_table {
    struct cgs_channel ch[CGS_CHANNEL_COUNT_MAX];
    uint32_t modem_channels;           /* always 0 on v5.5: the modem allocates its own channels */
    uint32_t event_bitmap;
    unsigned bad;                      /* 0, or endpoint index + 1 of the first entry gsi_channel_data_valid rejects */
};

/* gsi_event_bitmap_init: ids at and above the hardware maximum, and the MHI ids 10..16, are never handed out */
static inline uint32_t cgs_event_bitmap_init(uint32_t evt_ring_max)
{
    uint32_t bitmap = evt_ring_max >= 32u ? 0u : ~0u << evt_ring_max;
    for (unsigned i = 10; i <= 16; i++) bitmap |= 1u << i;
    return bitmap;
}
static inline int cgs_pow2(uint32_t v) { return v && !(v & (v - 1u)); }

/* gsi_channel_data_valid */
static inline int cgs_channel_valid(const struct cipa_endpoint *e, int command)
{
    if (e->channel >= CGS_CHANNEL_COUNT_MAX) return 0;
    if (e->ee != CGS_EE_AP && e->ee != CGS_EE_MODEM) return 0;
    if (command && !e->toward_ipa) return 0;
    if (!e->tlv_count || e->tlv_count > CGS_TLV_MAX) return 0;
    if (command && CGS_COMMAND_TRE_MAX > e->tlv_count) return 0;
    if (e->tre_count < 2u * e->tlv_count - 1u) return 0;
    return cgs_pow2(e->tre_count) && cgs_pow2(e->event_count);
}

/* gsi_channel_init: endpoint-name order; modem entries only mark channels, AP entries take the lowest free event ring */
static inline struct cgs_table cgs_channel_table(void)
{
    struct cgs_table t;
    __builtin_memset(&t, 0, sizeof t);
    t.event_bitmap = cgs_event_bitmap_init(CGS_EVT_RING_COUNT_MAX);
    for (unsigned i = 0; i < sizeof cipa_endpoints / sizeof cipa_endpoints[0]; i++) {
        const struct cipa_endpoint *e = &cipa_endpoints[i];
        const int command = i == CIPA_EP_AP_COMMAND_TX;
        if (!cip_endpoint_defined(e)) continue;
        if (e->ee == CGS_EE_MODEM) continue;   /* no ring data, not validated, not allocated by the AP */
        if (!cgs_channel_valid(e, command)) { t.bad = i + 1u; return t; }
        if (t.event_bitmap == ~0u) { t.bad = i + 1u; return t; }
        const unsigned id = (unsigned)__builtin_ctz(~t.event_bitmap);
        t.event_bitmap |= 1u << id;
        struct cgs_channel *c = &t.ch[e->channel];
        c->defined = 1;
        c->command = (uint8_t)command;
        c->toward_ipa = e->toward_ipa;
        c->evt_ring = (uint8_t)id;
        c->tre_count = e->tre_count;
        c->event_count = e->event_count;
        c->tre_limit = e->tre_count > e->event_count ? e->event_count : e->tre_count;   /* an event for every TRE */
        c->trans_tre_max = e->tlv_count;
    }
    return t;
}
/* gsi_channel_tre_max: the hardware limit is the ring size - 1 */
static inline uint32_t cgs_tre_max(const struct cgs_channel *c) { return c->tre_limit - (c->trans_tre_max - 1u); }

/* ------------------------------------------------------------------ gsi_ring_setup (v5.0+: HW_PARAM_2, HW_PARAM_4) */
struct cgs_counts { uint32_t channels, evt_rings; };
static inline struct cgs_counts cgs_ring_counts(uint32_t hw_param_2, uint32_t hw_param_4)
{
    struct cgs_counts c;
    c.channels = cip_decode(CGSI_HW_PARAM_2_NUM_CH_PER_EE, hw_param_2);
    c.evt_rings = cip_decode(CGSI_HW_PARAM_4_EV_PER_EE, hw_param_4);
    if (c.channels > CGS_CHANNEL_COUNT_MAX) c.channels = CGS_CHANNEL_COUNT_MAX;
    if (c.evt_rings > CGS_EVT_RING_COUNT_MAX) c.evt_rings = CGS_EVT_RING_COUNT_MAX;
    return c;
}

/* ------------------------------------------------------------------ gsi_evt_ring_program */
static inline uint32_t cgs_ev_cntxt_0(void)
{
    return cip_encode(CGSI_EV_CH_E_CNTXT_0_EV_CHTYPE, CGS_TYPE_GPI) | CGSI_EV_CH_E_CNTXT_0_EV_INTYPE |
           cip_encode(CGSI_EV_CH_E_CNTXT_0_EV_ELEMENT_SIZE, CGS_ELEMENT);   /* EV_EE 0: the AP */
}
static inline uint32_t cgs_ev_cntxt_1(uint32_t count) { return cip_encode(CGSI_EV_CH_E_CNTXT_1_R_LENGTH, count * CGS_ELEMENT); }
static inline uint32_t cgs_ev_cntxt_8(void)
{
    return cip_encode(CGSI_EV_CH_E_CNTXT_8_EV_MODT, CGS_INT_MODT) | cip_encode(CGSI_EV_CH_E_CNTXT_8_EV_MODC, 1);
}

/* ------------------------------------------------------------------ gsi_channel_program (v5.0+) */
static inline uint32_t cgs_ch_cntxt_0(const struct cgs_channel *c)
{
    uint32_t v = cip_encode(CGSI_CH_C_CNTXT_0_CHTYPE_PROTOCOL, CGS_TYPE_GPI);
    if (c->toward_ipa) v |= CGSI_CH_C_CNTXT_0_CHTYPE_DIR;
    return v | cip_encode(CGSI_CH_C_CNTXT_0_ELEMENT_SIZE, CGS_ELEMENT);
}
static inline uint32_t cgs_ch_cntxt_1(const struct cgs_channel *c)
{
    return cip_encode(CGSI_CH_C_CNTXT_1_CH_R_LENGTH, c->tre_count * CGS_ELEMENT) |
           cip_encode(CGSI_CH_C_CNTXT_1_CH_ERINDEX, c->evt_ring);
}
/* QOS: the command channel gets the largest WRR weight and the default prefetch buffers, every other channel the escape
   buffer only; all count doorbells in bytes */
static inline uint32_t cgs_ch_qos(const struct cgs_channel *c)
{
    const uint32_t v = c->command ? CGSI_CH_C_QOS_WRR_WEIGHT
                                  : cip_encode(CGSI_CH_C_QOS_PREFETCH_MODE, CGS_PREFETCH_ESCAPE_BUF_ONLY);
    return v | CGSI_CH_C_QOS_DB_IN_BYTES;
}
/* union gsi_channel_scratch, GPI: word3 bits 31:16 max_outstanding_tre, word4 bits 31:16 outstanding_threshold;
   SCRATCH_3 keeps its hardware low half */
static inline uint32_t cgs_ch_scratch_2(const struct cgs_channel *c) { return (uint32_t)c->trans_tre_max * CGS_ELEMENT << 16; }
static inline uint32_t cgs_ch_scratch_3(uint32_t reg) { return (2u * CGS_ELEMENT) << 16 | (reg & 0xffffu); }

/* ------------------------------------------------------------------ command words */
static inline uint32_t cgs_ch_cmd(uint32_t channel, uint32_t opcode)
{
    return cip_encode(CGSI_CH_CMD_CH_CHID, channel) | cip_encode(CGSI_CH_CMD_CH_OPCODE, opcode);
}
static inline uint32_t cgs_ev_cmd(uint32_t ring, uint32_t opcode)
{
    return cip_encode(CGSI_EV_CH_CMD_EV_CHID, ring) | cip_encode(CGSI_EV_CH_CMD_EV_OPCODE, opcode);
}
/* GENERIC_CMD on behalf of the modem EE (params from v4.11; 0 for every command used here) */
static inline uint32_t cgs_generic_cmd(uint32_t channel, uint32_t opcode)
{
    return cip_encode(CGSI_GENERIC_CMD_GENERIC_OPCODE, opcode) | cip_encode(CGSI_GENERIC_CMD_GENERIC_CHID, channel) |
           cip_encode(CGSI_GENERIC_CMD_GENERIC_EE, CGS_EE_MODEM) | cip_encode(CGSI_GENERIC_CMD_GENERIC_PARAMS, 0);
}
/* gsi_modem_channel_flow_control: how often a command that answered RETRY is sent again */
static inline uint32_t cgs_flow_retries(int enable) { return enable ? 0u : CGS_FLOW_RETRIES; }
/* gsi_isr_gp_int1: SUCCESS and INCORRECT_CHANNEL_STATE are fine, RETRY asks again, anything else is an error */
static inline int cgs_generic_result(uint32_t scratch_0)
{
    const uint32_t r = cip_decode(CGSI_CNTXT_SCRATCH_0_GENERIC_EE_RESULT, scratch_0);
    if (r == CGS_GENERIC_SUCCESS || r == CGS_GENERIC_INCORRECT_CHANNEL_STATE) return 0;
    return r == CGS_GENERIC_RETRY ? -CGS_EAGAIN : -CGS_EIO;
}
static inline uint32_t cgs_state(uint32_t cntxt_0) { return cip_decode(CGSI_CH_C_CNTXT_0_CHSTATE, cntxt_0); }
static inline uint32_t cgs_ev_state(uint32_t cntxt_0) { return cip_decode(CGSI_EV_CH_E_CNTXT_0_EV_CHSTATE, cntxt_0); }

/* ------------------------------------------------------------------ rings in one DMA block */
/* Each ring is 2^n bytes aligned to its size (gsi_ring_alloc relies on dma_alloc_coherent for that). Rings are placed
   largest first from an aligned base, so every one stays aligned; returns the bytes used, or 0 if they do not fit. */
struct cgs_ring_place { uint32_t offset[CGS_CHANNEL_COUNT_MAX], evt_offset[CGS_EVT_RING_COUNT_MAX]; };
static inline uint32_t cgs_place_rings(const struct cgs_table *t, uint32_t room, struct cgs_ring_place *p)
{
    uint32_t used = 0;
    for (uint32_t size = 0x10000u; size >= CGS_ELEMENT; size >>= 1)
        for (unsigned i = 0; i < CGS_CHANNEL_COUNT_MAX; i++) {
            const struct cgs_channel *c = &t->ch[i];
            if (!c->defined) continue;
            if (c->tre_count * CGS_ELEMENT == size) { p->offset[i] = used; used += size; }
            if (c->event_count * CGS_ELEMENT == size) { p->evt_offset[c->evt_ring] = used; used += size; }
        }
    return used <= room ? used : 0;
}

#endif /* COACHZ_GSI_PROTO_H */
