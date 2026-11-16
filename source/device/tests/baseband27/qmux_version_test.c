/* SPDX-License-Identifier: BSD-2-Clause */
/* CTL wire-contract regression for the service scale in the 2026-11-07 stock
 * modem catalogue. Only public QMI service numbers are used; ports, versions
 * and clients are synthetic. No SIM identifiers or phone state are fixtures. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "coachz_bb_qmux.h"

static unsigned failures;
#define CHECK(c) do { if (!(c)) { \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #c); failures++; \
} } while (0)

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static struct cbq_result versions(struct cbq_bridge *b, uint8_t *out, uint32_t room)
{
    uint8_t frame[12] = { 1, 11, 0, 0, 0, 0, 0, 7, 0x21, 0, 0, 0 };
    return cbq_from_host(b, 0, frame, sizeof frame, out, room);
}

/* Decode the returned wire TLVs independently of cbq_ctl's list construction. */
static unsigned catalogue(const uint8_t *out, uint32_t bytes, uint8_t seen[256])
{
    memset(seen, 0, 256);
    if (bytes < 12 || out[0] != 1 || rd16(out + 1) + 1u != bytes ||
        out[3] != 0x80 || out[4] || out[5] || out[6] != 1 ||
        out[7] != 7 || rd16(out + 8) != 0x21 || rd16(out + 10) != bytes - 12)
        return 0;
    unsigned success = 0, count = 0;
    for (uint32_t at = 12; at < bytes;) {
        if (bytes - at < 3) return 0;
        const uint32_t len = rd16(out + at + 1);
        if (len > bytes - at - 3) return 0;
        const uint8_t *value = out + at + 3;
        if (out[at] == 2) {
            if (len != 4 || rd16(value) || rd16(value + 2)) return 0;
            success = 1;
        } else if (out[at] == 1) {
            if (!len || len != 1u + 5u * value[0] || count) return 0;
            count = value[0];
            for (unsigned i = 0; i < count; i++) {
                const uint8_t *entry = value + 1 + 5 * i;
                if (!entry[0] || seen[entry[0]]++) return 0;
                if (!rd16(entry + 1) || rd16(entry + 3)) return 0;
            }
        }
        at += 3 + len;
    }
    return success ? count : 0;
}

static void stock_scale(void)
{
    static const uint8_t services[] = {
        66, 43, 15, 21, 51, 24, 22, 23, 36, 49, 74, 17, 34, 41, 87, 55,
        76, 77, 89, 88, 90, 9, 16, 33, 228, 18, 71, 50, 12, 5, 10, 47,
        11, 25, 1, 4, 26, 8, 3, 48, 46, 68, 73, 78, 7, 86, 92, 94,
        42, 2, 29, 85
    };
    struct cbq_bridge b;
    uint8_t out[512], seen[256], expected[256] = { 0 };
    cbq_init(&b);
    for (unsigned i = 0; i < sizeof services; i++) {
        CHECK(cbq_server_new(&b, services[i], 1, 0, 0x100 + i));
        expected[services[i]] = 1;
    }
    /* Another instance does not add a duplicate service-version entry. */
    CHECK(cbq_server_new(&b, 3, 1 | 2u << 8, 0, 0x200));
    CHECK(!cbq_server_new(&b, 4097, 1, 0, 0x201));
    const struct cbq_result r = versions(&b, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST);
    CHECK(catalogue(out, r.len, seen) == sizeof services);
    CHECK(!memcmp(seen, expected, sizeof seen));
    CHECK(seen[1] && seen[3] && seen[11] && seen[2]);  /* WDS, NAS, UIM, DMS */
    CHECK(seen[9] && seen[18] && seen[33]);            /* VOICE, IMSS, IMSA */
}

static void full_and_short_reply(void)
{
    struct cbq_bridge b;
    uint8_t out[23 + 5 * CBQ_MAX_SERVERS + 16], seen[256];
    cbq_init(&b);
    for (unsigned i = 0; i < CBQ_MAX_SERVERS; i++)
        CHECK(cbq_server_new(&b, i + 1, 1, 0, 0x300 + i));
    memset(out, 0xa5, sizeof out);
    const uint32_t exact = sizeof out - 16;
    struct cbq_result r = versions(&b, out, exact);
    CHECK(r.kind == CBQ_TO_HOST && r.len == exact);
    CHECK(catalogue(out, r.len, seen) == CBQ_MAX_SERVERS);
    for (unsigned i = exact; i < sizeof out; i++) CHECK(out[i] == 0xa5);
    memset(out, 0xa5, sizeof out);
    r = versions(&b, out, exact - 1);
    CHECK(r.kind == CBQ_DROP && !r.len);
    for (unsigned i = exact - 1; i < sizeof out; i++) CHECK(out[i] == 0xa5);
}

int main(void)
{
    stock_scale();
    full_and_short_reply();
    if (failures) return 1;
    puts("PASS QMUX version catalogue: all 52 stock-scale services, 64-slot boundary, short reply");
    return 0;
}
