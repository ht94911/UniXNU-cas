/* SPDX-License-Identifier: MIT */
#include "NezhaCameraIfeBandwidth.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const unsigned count = NezhaCameraIfeBcmCount;
    const char *names[] = {"MM0", "MM1", "SH0", "SH1", "MC0", "ACV"};
    const uint8_t vcd[] = {2, 2, 1, 1, 0, 0};
    uint8_t db[144 + NezhaCameraIfeBcmCount * 32] = {
        [4] = 0xdb, [5] = 0x30, [6] = 0x03, [7] = 0x0c,
        [8] = 5, [12] = NezhaCameraIfeBcmCount * 24, [14] = NezhaCameraIfeBcmCount,
    };
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *entry = db + 144 + 24 * i;
        memcpy(entry, names[i], 3);
        entry[16] = (uint8_t)(i * 4);
        entry[18] = 5;
        entry[20] = 8;
        entry[22] = (uint8_t)(i * 8);
        uint8_t *aux = db + 144 + 24 * count + i * 8;
        aux[1] = 0x10; /* 4096 bytes/s per vote unit */
        aux[4] = 4;
        aux[6] = vcd[i];
    }
    /* Camera011's own GPU log read ACV as unit=1 and width=0. It
     * needs the address and group, so zero width must not reject it. */
    uint8_t *acv = db + 144 + 24 * count + 5 * 8;
    acv[0] = 1;
    acv[1] = acv[4] = 0;
    struct NezhaCameraIfeBandwidth plan;
    assert(nezha_camera_ife_bandwidth(db, sizeof(db), 4000000, 8000000, 4000000, 4000000, &plan));
    assert(plan.count == 6);
    /* MC0 uses four channels for average and one bus width for peak. */
    assert(plan.wake[0].address == 0x50010 && plan.wake[0].data == (1u << 29 | 244u << 14 | 1953u));
    assert(plan.sleep[0].address == 0x50010 && plan.sleep[0].data == (1u << 29 | 244u << 14 | 976u));
    assert(plan.wake[1].address == 0x50014 && plan.wake[1].data == (1u << 30));
    assert(plan.wake[2].address == 0x50008 && plan.wake[2].data == (1u << 29 | 61u << 14 | 488u));
    assert(plan.wake[3].data == (1u << 30 | 1u << 29 | 1));
    assert(plan.wake[4].address == 0x50000 && plan.wake[4].data == (1u << 29 | 61u << 14 | 244u));
    assert(plan.wake[5].data == (1u << 30 | 1u << 29 | 1));
    for (unsigned i = 0; i < count; ++i) {
        assert(plan.wake[i].wait == (i % 2 == 1));
        assert(!plan.sleep[i].wait);
    }
    assert(nezha_camera_ife_bandwidth(db, sizeof(db), 0, 0, 0, 0, &plan));
    for (unsigned i = 0; i < count; ++i) {
        assert(plan.wake[i].data == (i % 2 ? 1u << 30 : 0));
        assert(plan.sleep[i].data == plan.wake[i].data);
    }
    assert(!nezha_camera_ife_bandwidth(db, sizeof(db), 0, 4000000, 0, 8000000, &plan));
    assert(!nezha_camera_ife_bandwidth(db, sizeof(db) - 1, 0, 0, 0, 0, &plan));
    db[144 + 24 * count + 4] = 0;
    struct NezhaCameraIfeBcmFailure error;
    assert(!nezha_camera_ife_bandwidth_checked(db, sizeof(db), 4000000, 8000000, 4000000, 4000000, &plan, &error));
    assert(error.issue == NezhaIfeBcmScale && error.index == 0 && error.address == 0x50000);
    puts("IFE0 BCM votes follow the live virtual clock groups and restore a zero vote without losing commits");
}
