/* SPDX-License-Identifier: APSL-2.0 */
/* Board coordinates from nezha's stock FDT, operations from pandora GPI. This
 * peripheral layer never allocates/reprograms someone else's SMMU context or
 * copies the entire GCC vote register back over another consumer's votes. */
#include "NezhaGaugeBus.h"
#include <sys/errno.h>
#include <string.h>

static int open_bus(NpGaugeBus *b)
{
    if (!b || !b->acquire || !b->release || b->gpii >= 5 || !b->clock.clk_div || !b->clock.t_cycle)
        return EINVAL;
    int r = b->acquire(b->context);
    if (r) return r;
    b->gpi.quiet = true;
    r = np_gpi_open(&b->gpi, 0xa00000, 0x60000, 0x10000, b->gpii, 5, 3);
    if (r) {
        const int stop = np_gpi_close(&b->gpi);
        b->release(b->context, stop == 0 && !b->gpi.poisoned);
    }
    return r;
}
static int close_bus(NpGaugeBus *b, int result)
{
    const int stop = np_gpi_close(&b->gpi);
    b->release(b->context, stop == 0 && !b->gpi.poisoned);
    return stop ? stop : result;
}
int np_gauge_bus_read(void *context, uint8_t out[NP_GAUGE_WORDS][2], uint32_t *valid)
{
    NpGaugeBus *b = context;
    if (!out || !valid) return EINVAL;
    *valid = 0; memset(out, 0, NP_GAUGE_WORDS * 2);
    int r = open_bus(b);
    if (r) return r;
    for (unsigned i = 0; i < NP_GAUGE_WORDS; ++i) {
        const uint8_t command = np_gauge_commands[i];
        r = np_gpi_i2c(&b->gpi, &b->clock, 0x55, &command, 1, out[i], 2);
        if (r) break; /* no further transfers after a bus/ownership error */
        *valid |= 1u << i;
    }
    return close_bus(b, r);
}
int np_pump_bus_read(NpGaugeBus *b, NpPump *out)
{
    if (!out) return EINVAL;
    memset(out, 0, sizeof(*out));
    if (!b || !b->pump_registers_owned) return EACCES;
    int r = open_bus(b);
    if (r) return r;
    uint8_t reg = 0x6e, chip = 0, c[13] = {0}, a[18] = {0};
    r = np_gpi_i2c(&b->gpi, &b->clock, 0x6e, &reg, 1, &chip, 1);
    if (!r && chip != 0x85) r = ENODEV;
    /* Exclusive event ownership is required: 15h includes ADC_DONE_FLAG and
     * its read side effects are not qualified. Never run alongside the stock
     * SC8585 event handler. Do not turn on the ADC merely to measure it. */
    static const uint8_t registers[] = {0x0b, 0x0e, 0x15, 0x16};
    uint16_t control_valid = 0;
    for (unsigned i = 0; i < sizeof(registers) && !r; ++i) {
        reg = registers[i];
        r = np_gpi_i2c(&b->gpi, &b->clock, 0x6e, &reg, 1, &c[reg - 0x0a], 1);
        if (!r) control_valid |= 1u << (reg - 0x0a);
    }
    if (!r && (c[11] & 0x80)) {
        reg = 0x17;
        r = np_gpi_i2c(&b->gpi, &b->clock, 0x6e, &reg, 1, a, sizeof(a));
    }
    if (!r) r = np_sc8585_decode(out, chip, c, control_valid, a) ? EIO : 0;
    return close_bus(b, r);
}
