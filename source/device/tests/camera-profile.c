/* SPDX-License-Identifier: MIT */
#include "NezhaCameraProfile.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t resource[NezhaCameraProfileCapacity / 4];
static struct NezhaCameraOvxModeWrite decoded[2048];

static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(int argc, char **argv)
{
    uint8_t *bytes = (uint8_t *)resource;
    struct NezhaCameraProfile profile;
    assert(!nezha_camera_profile_open(bytes, sizeof(resource), &profile));
    if (argc == 1) {
        puts("Camera profile rejects the intentionally empty source-only CI slot");
        return 0;
    }
    assert(argc >= 2 && argc <= 4);
    FILE *in = fopen(argv[1], "rb");
    assert(in != NULL && fread(bytes, 1, sizeof(resource), in) == sizeof(resource));
    assert(fgetc(in) == EOF && fclose(in) == 0);
    assert(nezha_camera_profile_open(bytes, sizeof(resource), &profile));
    size_t count = profile.initialization_count + profile.mode_count;
    assert((profile.initialization_count == 1697 || profile.initialization_count == 1707) &&
           profile.mode_count == 202 && profile.data_rate == UINT64_C(8000000000));
    const bool paired = profile.vendor != 0;
    const bool as1 = argc == 4 && strtoul(argv[3], NULL, 0) == 0x41;
    const uint8_t revision = as1 ? 0x41 : 0x42;
    if (paired) {
        assert(!nezha_camera_profile_writes(&profile, decoded, 2048));
        assert(!nezha_camera_profile_revision_writes(&profile, 8, revision, decoded, 2048, &count));
        assert(count == 0);
        assert(!nezha_camera_profile_revision_writes(&profile, profile.vendor, 0x40, decoded, 2048, &count));
        assert(!nezha_camera_profile_revision_writes(&profile, profile.vendor, 0xff, decoded, 2048, &count));
        assert(nezha_camera_profile_revision_writes(&profile, profile.vendor, revision, decoded, 2048, &count));
        assert(count == (as1 ? 1909u : 1899u));
    } else {
        assert(argc != 4 && nezha_camera_profile_writes(&profile, decoded, 2048));
    }
    assert(decoded[0].reg == 0x103 && decoded[0].value == 1);
    assert(profile.phy.lane_offsets[0] == 0x200 && profile.phy.lane_offsets[1] == 0x600 &&
           profile.phy.lane_offsets[2] == 0xa00 && profile.phy.settle_count == 35);
    size_t bursts = 0;
    for (size_t i = 0; i < count; ++i) {
        assert(decoded[i].reg != 0x100);
        if (decoded[i].burst != NULL) {
            ++bursts;
            assert(decoded[i].reg == 0xffff && decoded[i].burst_bytes == 929);
            assert(decoded[i].burst >= bytes && decoded[i].burst + 929 <= bytes + profile.used_bytes);
        }
    }
    assert(bursts == 3);
    if (argc >= 3) {
        FILE *out = fopen(argv[2], "wb");
        assert(out != NULL);
        for (size_t i = 0; i < count; ++i) {
            /* Host-only roundtrip used to compare every operation to the
             * original module parser, including all continuous burst bytes. */
            const uint32_t words[] = {decoded[i].reg, decoded[i].value, decoded[i].delay_ms,
                (uint32_t)decoded[i].burst_bytes};
            assert(fwrite(words, sizeof(words), 1, out) == 1);
            if (decoded[i].burst_bytes != 0) {
                assert(fwrite(decoded[i].burst, 1, decoded[i].burst_bytes, out) == decoded[i].burst_bytes);
            }
        }
        assert(fclose(out) == 0);
    }
    decoded[0].reg = 0xa55a;
    if (paired) {
        size_t rejected;
        assert(!nezha_camera_profile_revision_writes(&profile, profile.vendor, revision, decoded, count - 1, &rejected));
        assert(rejected == 0);
    } else {
        assert(!nezha_camera_profile_writes(&profile, decoded, count - 1));
    }
    assert(decoded[0].reg == 0xa55a);
    const uint32_t last = word(bytes + 56) + (uint32_t)(profile.mode_count - 1) * 16u;
    const uint8_t a = bytes[last], b = bytes[last + 1];
    bytes[last] = 0;
    bytes[last + 1] = 1; /* A late stream-on record must reject the whole view. */
    if (paired) {
        assert(!nezha_camera_profile_revision_writes(&profile, profile.vendor, revision, decoded, 2048, &count));
    } else {
        assert(!nezha_camera_profile_writes(&profile, decoded, 2048));
    }
    assert(decoded[0].reg == 0xa55a);
    bytes[last] = a;
    bytes[last + 1] = b;
    bytes[12] ^= 1; /* A truncated resource must not escape its declared size. */
    assert(!nezha_camera_profile_open(bytes, NezhaCameraProfileHeader, &profile));
    puts("Camera profile decodes stock initialization, mode and three complete bursts without host pointers");
}
