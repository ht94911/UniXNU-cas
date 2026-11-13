/* SPDX-License-Identifier: MIT */
#include "NezhaCameraIfeBandwidth.h"

/* Topology from Canoe's ICC nodes and BCM memberships. IFE0 RDI uses
 * the dedicated HF route through MNOC, LLCC and EBI. The two path masks
 * are one-hot enable bits; ACV has no enable or perf mask in this table. */
static const struct {
    const char *name;
    unsigned width, channels, mask;
} route[NezhaCameraIfeBcmCount] = {
    {"MM0", 32, 2, 0}, {"MM1", 0, 0, 1}, {"SH0", 16, 4, 0},
    {"SH1", 0, 0, 1}, {"MC0", 4, 4, 0}, {"ACV", 0, 0, 0},
};

static uint32_t divide(uint64_t number, uint32_t divisor)
{
    const uint64_t quotient = number / divisor;
    return quotient > 0x3fff ? 0x3fffu : quotient ? (uint32_t)quotient : number != 0;
}

static uint32_t make_vote(const QcomRpmhResource *resource, unsigned index,
    uint32_t average, uint32_t peak)
{
    uint32_t x = 0, y = 0;
    if (route[index].width) {
        const uint32_t unit = (uint32_t)resource->aux[0] | (uint32_t)resource->aux[1] << 8 |
            (uint32_t)resource->aux[2] << 16 | (uint32_t)resource->aux[3] << 24;
        const uint32_t width = (uint32_t)resource->aux[4] | (uint32_t)resource->aux[5] << 8;
        /* ICC rounds down to kB/s first; its first per-node division is
         * not capped. Only the final 14-bit vote fields are saturated. */
        const uint64_t avg_numerator = (uint64_t)(average / 1000) * width;
        const uint64_t peak_numerator = (uint64_t)(peak / 1000) * width;
        const unsigned avg_width = route[index].width * route[index].channels;
        const unsigned peak_width = route[index].width;
        const uint64_t avg_node = avg_numerator && avg_numerator < avg_width ? 1 : avg_numerator / avg_width;
        const uint64_t peak_node = peak_numerator && peak_numerator < peak_width ? 1 : peak_numerator / peak_width;
        x = divide(avg_node * 1000, unit);
        y = divide(peak_node * 1000, unit);
    } else if (average || peak) {
        y = route[index].mask;
    }
    return (x || y ? 1u << 29 : 0) | (x << 14) | y;
}

static bool reject(struct NezhaCameraIfeBcmFailure *failure, enum NezhaCameraIfeBcmIssue issue,
    unsigned index, uint32_t address, uint32_t detail)
{
    if (failure) *failure = (struct NezhaCameraIfeBcmFailure){issue, index, address, detail};
    return false;
}

bool nezha_camera_ife_bandwidth_checked(const uint8_t *db, size_t db_bytes,
    uint32_t wake_average, uint32_t wake_peak, uint32_t sleep_average,
    uint32_t sleep_peak, struct NezhaCameraIfeBandwidth *out,
    struct NezhaCameraIfeBcmFailure *failure)
{
    if (!out || wake_peak < wake_average || sleep_peak < sleep_average ||
        wake_average < sleep_average || wake_peak < sleep_peak) {
        return reject(failure, NezhaIfeBcmArguments, 0, 0, 0);
    }
    QcomRpmhResource resource[NezhaCameraIfeBcmCount];
    unsigned order[NezhaCameraIfeBcmCount];
    for (unsigned i = 0; i < NezhaCameraIfeBcmCount; ++i) {
        QcomRpmhResource *bcm = &resource[i];
        const int status = qcom_cmd_db_find(db, db_bytes, route[i].name, bcm);
        if (status != QcomRpmhOk) return reject(failure, NezhaIfeBcmLookup, i, 0, (uint32_t)status);
        if ((bcm->address & 0xffff0003u) != 0x50000u)
            return reject(failure, NezhaIfeBcmAddress, i, bcm->address, 0);
        if (bcm->aux_bytes < 8) return reject(failure, NezhaIfeBcmAuxLength, i, bcm->address, bcm->aux_bytes);
        /* The board's ACV is a mask BCM, with unit 1 and width 0.
         * Only bandwidth BCMs use either field in their arithmetic. All
         * BCMs still require their own valid address and VCD group. */
        if (route[i].width && (!(bcm->aux[0] | bcm->aux[1] | bcm->aux[2] | bcm->aux[3]) ||
                              !(bcm->aux[4] | bcm->aux[5]))) {
            const uint32_t scale = (uint32_t)bcm->aux[4] | (uint32_t)bcm->aux[5] << 8;
            return reject(failure, NezhaIfeBcmScale, i, bcm->address, scale);
        }
        for (unsigned j = 0; j < i; ++j) {
            if (bcm->address == resource[j].address) {
                return reject(failure, NezhaIfeBcmDuplicate, i, bcm->address, j);
            }
        }
        unsigned j = i;
        while (j > 0 && resource[order[j - 1]].aux[6] > bcm->aux[6]) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = i;
    }
    struct NezhaCameraIfeBandwidth result = {.count = NezhaCameraIfeBcmCount};
    for (unsigned i = 0; i < NezhaCameraIfeBcmCount; ++i) {
        const unsigned index = order[i];
        const QcomRpmhResource *bcm = &resource[index];
        const bool commit = i + 1 == NezhaCameraIfeBcmCount || bcm->aux[6] != resource[order[i + 1]].aux[6];
        const uint32_t bit = commit ? 1u << 30 : 0;
        result.wake[i] = (struct NezhaCameraIfeBcmCommand){bcm->address,
            bit | make_vote(bcm, index, wake_average, wake_peak), commit};
        result.sleep[i] = (struct NezhaCameraIfeBcmCommand){bcm->address,
            bit | make_vote(bcm, index, sleep_average, sleep_peak), false};
    }
    *out = result;
    if (failure) *failure = (struct NezhaCameraIfeBcmFailure){0};
    return true;
}

bool nezha_camera_ife_bandwidth(const uint8_t *db, size_t db_bytes,
    uint32_t wake_average, uint32_t wake_peak, uint32_t sleep_average,
    uint32_t sleep_peak, struct NezhaCameraIfeBandwidth *out)
{
    return nezha_camera_ife_bandwidth_checked(db, db_bytes, wake_average, wake_peak,
        sleep_average, sleep_peak, out, NULL);
}
