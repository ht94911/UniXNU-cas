/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Same SM8850 TSENS-v2 status interpretation as the fixed pandora_cpu.c
 * reference. Board resources come only from the captured N0 FDT. */
#include "NezhaTsensReadOnly.h"
#include <limits.h>
#include <string.h>

const NpTsensController np_tsens_controllers[NP_TSENS_CONTROLLER_COUNT] = {
    { UINT64_C(0xc229000), UINT64_C(0xc222000), 0x1ff, 5 },
    { UINT64_C(0xc22a000), UINT64_C(0xc223000), 0x1ff, 12 },
    { UINT64_C(0xc22b000), UINT64_C(0xc224000), 0x1ff, 7 },
    { UINT64_C(0xc22c000), UINT64_C(0xc225000), 0x1ff, 4 },
    { UINT64_C(0xc22d000), UINT64_C(0xc226000), 0x1ff, 8 },
    { UINT64_C(0xc22e000), UINT64_C(0xc227000), 0x1ff, 12 },
    { UINT64_C(0xc22f000), UINT64_C(0xc228000), 0x1ff, 7 }
};

int np_tsens_decode(uint32_t raw, int32_t *deci_c)
{
    if (!deci_c)
        return NP_TSENS_ARGUMENT;
    if (!(raw & NP_TSENS_VALID_BIT))
        return 0;
    /* Equivalent to pandora's (int32_t)(raw << 20) >> 20, without relying
     * on implementation-defined signed shifts or unsigned-to-signed casts. */
    int32_t value = (int32_t)(raw & UINT32_C(0xfff));
    if (value & 0x800)
        value -= 0x1000;
    *deci_c = value;
    return 1;
}

int np_tsens_sample(const NpTsensReadOps *ops, NpTsensSample *sample)
{
    if (!ops || !ops->read32 || !sample)
        return NP_TSENS_ARGUMENT;
    memset(sample, 0, sizeof(*sample));
    unsigned flat = 0;
    int first_error = 0;
    for (unsigned group = 0; group < NP_TSENS_CONTROLLER_COUNT; ++group) {
        const NpTsensController *c = &np_tsens_controllers[group];
        for (unsigned sensor = 0; sensor < c->sensors; ++sensor, ++flat) {
            NpTsensReading *r = &sample->readings[flat];
            const uint64_t bit = UINT64_C(1) << flat;
            const uint64_t pa = c->tm_pa + NP_TSENS_STATUS_OFFSET + sensor * 4u;
            uint32_t raw = 0;
            r->controller = (uint8_t)group;
            r->sensor = (uint8_t)sensor;
            r->deci_c = INT32_MIN;
            r->io_error = ops->read32(ops->context, pa, &raw);
            if (r->io_error) {
                if (!first_error)
                    first_error = r->io_error;
                continue;
            }
            r->raw = raw;
            r->read_ok = 1;
            sample->read_mask |= bit;
            if (np_tsens_decode(raw, &r->deci_c) == 1) {
                r->valid = 1;
                sample->valid_mask |= bit;
            }
        }
    }
    return first_error ? first_error :
        (sample->valid_mask ? 0 : NP_TSENS_NO_VALID_READING);
}
