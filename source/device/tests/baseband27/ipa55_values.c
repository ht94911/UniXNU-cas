/* Host value check of the derived IPA v5.5 tables. Not part of the driver. */
#include <stdio.h>
#include <assert.h>
#include "nb_ipa55_modem_proto.h"

static void put(const char *name, const struct cipa_endpoint *e)
{
    struct cic_write w[CIC_ENDPOINT_WRITES_MAX];
    const unsigned n = cic_endpoint_program(e, w);
    printf("%s ep%u writes %u:", name, e->endpoint, n);
    for (unsigned i = 0; i < n; i++) printf(" %05x=%08x", w[i].offset, w[i].value);
    printf("\n");
}

int main(void)
{
    put("CMD_TX", &cipa_endpoints[CIPA_EP_AP_COMMAND_TX]);
    put("LAN_RX", &cipa_endpoints[CIPA_EP_AP_LAN_RX]);
    put("MODEM_TX", &cipa_endpoints[CIPA_EP_AP_MODEM_TX]);
    put("MODEM_RX", &cipa_endpoints[CIPA_EP_AP_MODEM_RX]);
    printf("cmd_valid %d table_valid %d filter_count %u route_count %u table_bytes %u\n", cic_cmd_init_valid(0), cic_table_valid(),
           cic_filter_count(), cic_route_count(), cic_table_bytes());
    struct cic_cmd c[CGS_COMMAND_TRE_MAX];
    unsigned n = cic_mem_setup(0, 0x20010000u, c);
    printf("mem_setup %u cmds; first flags %08x\n", n, cic_rd32(c[0].payload + 8));
    assert(n == 4);
    n = cic_table_setup(0, 0x20020000u, 0x20010000u, c);
    printf("table_setup %u cmds\n", n);
    assert(n == 8);
    for (unsigned i = 0; i < n; i++) {
        uint64_t flags = cic_rd64(c[i].payload + 8);
        if (c[i].opcode == CIC_CMD_DMA_SHARED_MEM)
            printf("  zero local %04x size %u\n", c[i].payload[4] | c[i].payload[5] << 8, c[i].payload[2] | c[i].payload[3] << 8);
        else
            printf("  op %x hash %#llx/%llu nhash %#llx/%llu\n", c[i].opcode, (unsigned long long)(flags >> 12 & 0xffff),
                   (unsigned long long)(flags & 0xfff), (unsigned long long)(flags >> 40 & 0xffff), (unsigned long long)(flags >> 28 & 0xfff));
    }
    uint8_t table[256];
    cic_table_fill(table, 0x20020000u);
    printf("bitmap %#llx first %#llx\n", (unsigned long long)cic_rd64(table + 8), (unsigned long long)cic_rd64(table + 16));
    assert(cic_rd64(table + 8) == 0x94e6);

    uint8_t st[64] = { 0 };
    st[0] = 1;                    /* PACKET */
    st[4] = 5;                    /* length 5 */
    st[6] = 14;                   /* source: command endpoint */
    st[2] = 1;                    /* a tag mask bit */
    st[28 + 2] = 17;              /* destination (word 7, bits 23:16) */
    struct cir_packet p = cir_status_step(st, sizeof st, &cipa_endpoints[CIPA_EP_AP_LAN_RX]);
    printf("status kind %d consumed %u src %u dst %u\n", p.kind, p.consumed, p.status.src, p.status.dst);
    assert(p.kind == CIR_TAG && p.consumed == 32 + 8 && p.status.src == 14 && p.status.dst == 17);

    uint8_t q[2048], ip[20] = { 0x45 };
    uint32_t len = cim_egress(q, sizeof q, ip, sizeof ip, 3, CIM_DATA_FORMAT);
    printf("egress %u header %02x %02x %02x %02x format %#x match %d\n", len, q[0], q[1], q[2], q[3], CIM_DATA_FORMAT,
           cim_format_matches_endpoints(CIM_DATA_FORMAT));
    assert(len == 24 && q[1] == 3 && q[3] == 20 && cim_format_matches_endpoints(CIM_DATA_FORMAT));
    struct cim_packet m = cim_ingress_next(q, len, CIM_DATA_FORMAT);
    assert(m.consumed == 24 && m.ip_len == 20 && m.ip_version == 4 && m.mux_id == 3);

    uint8_t req[CI_INIT_DRIVER_REQ_MAX];
    size_t r = ci_init_driver_req(req, sizeof req, 1, 0, 0, CIPA_SMEM_SIZE, 0x14683000u);
    printf("init_driver %zu bytes (max %u)\n", r, CI_INIT_DRIVER_REQ_MAX);
    assert(r && r <= CI_INIT_DRIVER_REQ_MAX);
    printf("resource pairs src %u dst %u\n", cip_resource_pairs(CIPA_RSRC_GROUP_SRC_COUNT), cip_resource_pairs(CIPA_RSRC_GROUP_DST_COUNT));
    return 0;
}
