/* SPDX-License-Identifier: MIT */
#include "NezhaCameraProfile.h"

/* Local resource format emitted by pack-nezha27-camera-profile.py. The
 * existing native-resource finalization / strict-prelink / HMAC sequence
 * protects these bytes. This is not a second vendor module parser. */
_Static_assert(sizeof(struct NezhaCameraPhyRegister) == 16, "PHY resource ABI");

static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool range(uint32_t offset, uint32_t count, uint32_t stride, size_t used)
{
    return offset >= NezhaCameraProfileHeader && (offset & 3u) == 0 &&
        offset <= used && count != 0 && count <= (used - offset) / stride;
}

static bool valid_record(const uint8_t *p, size_t used)
{
    const unsigned reg = (unsigned)p[0] | (unsigned)p[1] << 8;
    const uint32_t delay = word(p + 4), offset = word(p + 8), bytes = word(p + 12);
    if (reg == 0x100 || delay > UINT16_MAX) {
        return false;
    }
    if (p[3] == 0) {
        return offset == 0 && bytes == 0;
    }
    return p[3] == 1 && bytes != 0 && bytes <= UINT16_MAX &&
        offset >= NezhaCameraProfileHeader && offset <= used && bytes <= used - offset;
}

bool nezha_camera_profile_open(const uint8_t *bytes, size_t capacity,
    struct NezhaCameraProfile *profile)
{
    static const uint8_t magic[8] = {'N', '2', '7', 'C', 'A', 'M', '0', '1'};
    if (bytes == NULL || profile == NULL || (uintptr_t)bytes % 4 != 0 ||
        capacity < NezhaCameraProfileHeader || capacity > NezhaCameraProfileCapacity) {
        return false;
    }
    for (unsigned i = 0; i < sizeof(magic); ++i) {
        if (bytes[i] != magic[i]) {
            return false;
        }
    }
    const uint32_t used = word(bytes + 12);
    if (word(bytes + 8) != 1 || used < NezhaCameraProfileHeader || used > capacity ||
        word(bytes + 16) != NezhaCameraOvx10500uId || word(bytes + 20) != 4096 ||
        word(bytes + 24) != 3072 || word(bytes + 28) != 10 || word(bytes + 32) != 0x210 ||
        word(bytes + 36) > UINT16_MAX || word(bytes + 104) != 1 || word(bytes + 108) != 1 ||
        word(bytes + 124) != 0x2b) {
        return false;
    }
    struct NezhaCameraProfile next = {.bytes = bytes, .used_bytes = used,
        .width = word(bytes + 20), .height = word(bytes + 24),
        .data_rate = (uint64_t)word(bytes + 40) | (uint64_t)word(bytes + 44) << 32};
    if (next.data_rate == 0) {
        return false;
    }
    for (unsigned table = 0; table < 2; ++table) {
        const uint32_t offset = word(bytes + 48 + table * 8u);
        const uint32_t count = word(bytes + 52 + table * 8u);
        if (!range(offset, count, 16, used)) {
            return false;
        }
        for (unsigned i = 0; i < count; ++i) {
            if (!valid_record(bytes + offset + i * 16u, used)) {
                return false;
            }
        }
        if (table == 0) {
            next.initialization_count = count;
        } else {
            next.mode_count = count;
        }
    }
    struct NezhaCameraPhyTable *tables[] = {&next.phy.common, &next.phy.reset_enter,
        &next.phy.reset_exit, &next.phy.lane, &next.phy.rate};
    for (unsigned table = 0; table < 5; ++table) {
        const uint32_t offset = word(bytes + 64 + table * 8u);
        const uint32_t count = word(bytes + 68 + table * 8u);
        if (!range(offset, count, 16, used)) {
            return false;
        }
        tables[table]->registers = (const struct NezhaCameraPhyRegister *)(bytes + offset);
        tables[table]->count = count;
    }
    for (unsigned lane = 0; lane < 3; ++lane) {
        next.phy.lane_offsets[lane] = word(bytes + 112 + lane * 4u);
    }
    next.phy.settle_count = (uint16_t)word(bytes + 36);
    *profile = next;
    return true;
}

bool nezha_camera_profile_writes(const struct NezhaCameraProfile *profile,
    struct NezhaCameraOvxModeWrite *out, size_t capacity)
{
    if (profile == NULL || out == NULL) {
        return false;
    }
    struct NezhaCameraProfile verified;
    if (!nezha_camera_profile_open(profile->bytes, profile->used_bytes, &verified) ||
        verified.initialization_count + verified.mode_count > capacity) {
        return false;
    }
    size_t cursor = 0;
    for (unsigned table = 0; table < 2; ++table) {
        const uint32_t offset = word(verified.bytes + 48 + table * 8u);
        const size_t count = table == 0 ? verified.initialization_count : verified.mode_count;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *p = verified.bytes + offset + i * 16u;
            out[cursor++] = (struct NezhaCameraOvxModeWrite){
                .reg = (uint16_t)((unsigned)p[0] | (unsigned)p[1] << 8),
                .value = p[2], .delay_ms = (uint16_t)word(p + 4),
                .burst = p[3] == 1 ? verified.bytes + word(p + 8) : NULL,
                .burst_bytes = word(p + 12)};
        }
    }
    return true;
}
