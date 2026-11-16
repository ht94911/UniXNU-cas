/* Host model of the IPA v5.5 data path the nezha bridge uses, after the upstream coachz_ipa_modem_model_test.c and
 * coachz_ipa_rx_model_test.c (QcomBaseband dd0199c), with the v5.5 expectations: plain QMAP (no MAPv4 checksum header or
 * trailer), the v5.0+ status layout (8-bit source in word 1, 8-bit destination in word 7) and the phone's endpoint numbers.
 * Not part of the driver. */
#include <stdio.h>
#include <string.h>
#include "nb_ipa55_modem_proto.h"
#include "nb_ipa55_rx_proto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define GROUP(name, body) do { const int before = failures; body; if (failures == before) printf("PASS %s\n", name); } while (0)

static void format(void)
{
    CHECK(CIM_DATA_FORMAT == CIM_INGRESS_DEAGGREGATION && cim_format_matches_endpoints(CIM_DATA_FORMAT));
    CHECK(!cim_format_matches_endpoints(CIM_DATA_FORMAT | CIM_INGRESS_MAP_CKSUMV4));   /* the endpoints run without offload */
    CHECK(!cim_format_matches_endpoints(0));                                          /* deaggregation is required */
    CHECK(cim_headroom(CIM_DATA_FORMAT) == 4 && cim_data_mtu(CIM_DATA_FORMAT) == CIM_IPA_MTU - 4);
    CHECK(cipa_endpoints[CIPA_EP_AP_MODEM_TX].endpoint == 2 && cipa_endpoints[CIPA_EP_AP_MODEM_RX].endpoint == 24);
}

static void egress(void)
{
    uint8_t ip[60], out[2048];
    for (unsigned i = 0; i < sizeof ip; i++) ip[i] = (uint8_t)(0x45 + i);
    const uint32_t n = cim_egress(out, sizeof out, ip, sizeof ip, 1, CIM_DATA_FORMAT);
    CHECK(n == 64 && out[0] == 0 && out[1] == 1 && out[2] == 0 && out[3] == 60 && !memcmp(out + 4, ip, sizeof ip));
    CHECK(!cim_egress(out, 63, ip, sizeof ip, 1, CIM_DATA_FORMAT) && !cim_egress(out, sizeof out, ip, 0, 1, CIM_DATA_FORMAT));
    CHECK(!cim_egress(out, sizeof out, ip, sizeof ip, 255, CIM_DATA_FORMAT));
    CHECK(cim_egress(out, sizeof out, ip, sizeof ip, 4, CIM_DATA_FORMAT) == 64 && out[1] == 4);   /* pdp_ip3 */
    CHECK(cim_tx_acceptable(64, 2048) && !cim_tx_acceptable(0, 2048) && !cim_tx_acceptable(2049, 2048));
}

static void ingress(void)
{
    /* an aggregate as AP_MODEM_RX delivers it without checksum offload: [hdr pad 0 mux 1 len 20][IPv4 20]
       [hdr pad 2 mux 1 len 42][IPv6 40 + 2 pad][hdr cmd mux 1 len 4][4][hdr len 100 (truncated)] */
    uint8_t buf[256];
    memset(buf, 0, sizeof buf);
    uint32_t o = 0;
    buf[o] = 0; buf[o + 1] = 1; buf[o + 3] = 20; buf[o + 4] = 0x45; o += 4 + 20;
    buf[o] = 2; buf[o + 1] = 1; buf[o + 3] = 42; buf[o + 4] = 0x60; o += 4 + 42;
    buf[o] = 0x80; buf[o + 1] = 1; buf[o + 3] = 4; o += 4 + 4;
    buf[o + 1] = 1; buf[o + 3] = 100; o += 4 + 10;
    uint8_t mux[CIM_MAX_LOGICAL_EP] = { 0 };
    mux[1] = 1;
    const uint8_t *p = buf;
    uint32_t resid = o;
    struct cim_packet k = cim_ingress_next(p, resid, CIM_DATA_FORMAT);
    CHECK(k.consumed == 24 && k.mux_id == 1 && k.ip_offset == 4 && k.ip_len == 20 && k.ip_version == 4 && cim_ingress_deliverable(&k, CIM_DATA_FORMAT, mux));
    p += k.consumed; resid -= k.consumed;
    k = cim_ingress_next(p, resid, CIM_DATA_FORMAT);
    CHECK(k.consumed == 46 && k.ip_len == 40 && k.ip_version == 6 && cim_ingress_deliverable(&k, CIM_DATA_FORMAT, mux));
    p += k.consumed; resid -= k.consumed;
    k = cim_ingress_next(p, resid, CIM_DATA_FORMAT);
    CHECK(k.consumed == 8 && k.command && !cim_ingress_deliverable(&k, CIM_DATA_FORMAT, mux));
    p += k.consumed; resid -= k.consumed;
    k = cim_ingress_next(p, resid, CIM_DATA_FORMAT);
    CHECK(k.consumed == 0 && resid == 14);                                     /* longer than what is left: deaggregation ends */
    uint8_t one[24] = { 0, 7, 0, 20, 0x45 };
    k = cim_ingress_next(one, 24, CIM_DATA_FORMAT);
    CHECK(k.consumed == 24 && !cim_ingress_deliverable(&k, CIM_DATA_FORMAT, mux));   /* mux 7 has no device */
    const uint8_t pad_only[8] = { 4, 1, 0, 4, 0x45 };                              /* padding covers the whole packet */
    k = cim_ingress_next(pad_only, 8, CIM_DATA_FORMAT);
    CHECK(k.consumed == 8 && !k.ip_len && !cim_ingress_deliverable(&k, CIM_DATA_FORMAT, mux));
    const uint8_t zero[8] = { 0 };
    CHECK(!cim_ingress_next(zero, 8, CIM_DATA_FORMAT).consumed && !cim_ingress_next(one, 3, CIM_DATA_FORMAT).consumed);
}

/* v5.0+ status: word 0 opcode/exception/mask, word 1 length and source, word 2 metadata, word 3 router rule, word 7 destination */
static void status_put(uint8_t *p, uint32_t opcode, uint32_t exception, uint32_t mask, uint32_t length, uint32_t src, uint32_t dst, uint32_t rule)
{
    memset(p, 0, CIR_STATUS_SIZE);
    cic_le32(p, opcode | exception << 8 | mask << 16);
    cic_le32(p + 4, length | src << 16);
    cic_le32(p + 8, 0x12345678u);
    cic_le32(p + 12, rule << 22);
    cic_le32(p + 28, dst << 16);
}

static void parsing(void)
{
    const struct cipa_endpoint *lan = &cipa_endpoints[CIPA_EP_AP_LAN_RX];
    const uint32_t cmd = cic_endpoint_id(CIPA_EP_AP_COMMAND_TX), dst = lan->endpoint, align = 1u << lan->rx_pad_align;
    CHECK(dst == 17 && cmd == 14 && !lan->checksum && lan->status_enable);
    uint8_t buf[256];
    status_put(buf, 1, 0, 0x10, 24, cmd, dst, 0);               /* pipeline clear: tagged, from the command endpoint */
    struct cir_packet p = cir_status_step(buf, 256, lan);
    CHECK(p.kind == CIR_TAG && p.consumed == 32 + (24 + align - 1) / align * align && p.status.metadata == 0x12345678u);
    status_put(buf, 1, 0, 0x10, 24, 2, dst, 0);
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_TAG_UNEXPECTED);
    status_put(buf, 1, 0, 0, 0, cmd, dst, 0);                   /* no packet data */
    p = cir_status_step(buf, 256, lan);
    CHECK(p.kind == CIR_SKIP && p.consumed == 32);
    status_put(buf, 1, 0, 0, 40, 2, 24, 0);                     /* for another endpoint: 8-bit destination above 15 */
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_SKIP);
    status_put(buf, 1, 0, 0, 40, 2, dst | 0x100, 0);            /* bits above the 8-bit field are not part of the endpoint */
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_DELIVER);
    status_put(buf, 16, 0, 0, 40, 2, dst, 0);                   /* LOG: not a packet format */
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_SKIP);
    status_put(buf, 1, 0, 0, 41, 2, dst, CIR_RULE_MISS);        /* routing miss; length aligned to 1 << rx_pad_align */
    p = cir_status_step(buf, 256, lan);
    CHECK(p.kind == CIR_DROP && p.consumed == 32 + (41 + align - 1) / align * align);
    status_put(buf, 4, 1, 0, 40, 2, dst, 0);                    /* deaggregation exception */
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_DROP);
    status_put(buf, 1, 4, 0, 40, 2, dst, 0);                    /* any other exception is consumed */
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_DELIVER);
    status_put(buf, 1, 0, 0, 40, 2, dst, 5);
    p = cir_status_step(buf, 256, lan);
    CHECK(p.kind == CIR_DELIVER && p.data_offset == 32 && p.length == 40);
    CHECK(cir_status_step(buf, 31, lan).kind == CIR_SHORT);
    status_put(buf, 1, 0, 0, 200, 2, dst, 5);
    CHECK(cir_status_step(buf, 100, lan).consumed == 100);      /* never past the received bytes */
}

static void buffers(void)
{
    const struct cipa_endpoint *lan = &cipa_endpoints[CIPA_EP_AP_LAN_RX];
    const struct cgs_table t = cgs_channel_table();
    const struct cgs_channel *c = &t.ch[lan->channel];
    CHECK(cir_dma_len(lan) == cir_buffer_size(lan) - CIC_NET_SKB_PAD);
    const uint32_t max = cgs_tre_max(c);
    CHECK(max && cir_capacity(c, max + 5) == max && cir_capacity(c, 16) == (max < 16 ? max : 16));
    unsigned rings = 0;
    for (uint32_t n = 1; n <= max; n++) rings += cir_doorbell(n, max - n);
    CHECK(rings == max / 16 + (max % 16 != 0) && cir_doorbell(max, 0));
    printf("LAN_RX ep%u ch%u buffer %u tre_max %u pad_align %u\n", lan->endpoint, lan->channel, cir_buffer_size(lan), max, lan->rx_pad_align);
}

int main(void)
{
    GROUP("data format", format());
    GROUP("egress", egress());
    GROUP("ingress deaggregation", ingress());
    GROUP("v5 status parsing", parsing());
    GROUP("buffers and doorbells", buffers());
    return failures != 0;
}
