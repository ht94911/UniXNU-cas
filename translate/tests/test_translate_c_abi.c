/* SPDX-License-Identifier: MIT */
#include "NativeMetalTranslate.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum { HEADER_BYTES = 8, ARCH_BYTES = 20 };
static const uint32_t air64 = UINT32_C(0x01000017);
static const uint32_t arm64 = UINT32_C(0x0100000c);
static const uint8_t thin[] = {'M', 'T', 'L', 'B', 0, 1, 2};
static unsigned int checks;

static void put_be32(uint8_t *bytes, uint32_t word)
{
    bytes[0] = (uint8_t)(word >> 24);
    bytes[1] = (uint8_t)(word >> 16);
    bytes[2] = (uint8_t)(word >> 8);
    bytes[3] = (uint8_t)word;
}

static void put_arch(uint8_t *bytes, unsigned int index, uint32_t cpu,
                     uint32_t offset, uint32_t size)
{
    uint8_t *record = bytes + HEADER_BYTES + index * ARCH_BYTES;
    put_be32(record, cpu);
    put_be32(record + 8, offset);
    put_be32(record + 12, size);
}

static void expect_slice(const uint8_t *bytes, size_t len,
                         size_t expected_offset, size_t expected_len)
{
    size_t offset = SIZE_MAX;
    size_t size = SIZE_MAX;
    assert(nmb_extract_air_slice(bytes, len, &offset, &size));
    assert(offset == expected_offset && size == expected_len);
    assert(nmb_last_error()[0] == '\0');
    ++checks;
}

static void expect_error(const uint8_t *bytes, size_t len, const char *message)
{
    size_t offset = 7;
    size_t size = 9;
    assert(!nmb_extract_air_slice(bytes, len, &offset, &size));
    assert(offset == 7 && size == 9);
    assert(strstr(nmb_last_error(), message) != NULL);
    ++checks;
}

static void *check_thread_error(void *unused)
{
    (void)unused;
    assert(nmb_last_error()[0] == '\0');
    expect_error(NULL, 3, "non-null");
    expect_slice(thin, sizeof(thin), 0, sizeof(thin));
    return NULL;
}

int main(void)
{
    assert(nmb_translate_abi_version() == 2);
    assert(nmb_last_error()[0] == '\0');

    expect_slice(thin, sizeof(thin), 0, sizeof(thin));
    expect_slice(thin, 4, 0, 4);

    enum { FAT_SIZE = HEADER_BYTES + 2 * ARCH_BYTES + 4 + sizeof(thin) };
    uint8_t fat[FAT_SIZE] = {0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 2};
    put_arch(fat, 0, arm64, HEADER_BYTES + 2 * ARCH_BYTES, 4);
    put_arch(fat, 1, air64, FAT_SIZE - sizeof(thin), sizeof(thin));
    memcpy(fat + FAT_SIZE - sizeof(thin), thin, sizeof(thin));
    expect_slice(fat, sizeof(fat), FAT_SIZE - sizeof(thin), sizeof(thin));

    put_arch(fat, 1, arm64, FAT_SIZE - sizeof(thin), sizeof(thin));
    expect_error(fat, sizeof(fat), "no AIR64");
    put_arch(fat, 1, air64, UINT32_MAX, UINT32_MAX);
    expect_error(fat, sizeof(fat), "out of bounds");
    put_arch(fat, 1, air64, FAT_SIZE - sizeof(thin), sizeof(thin));
    expect_error(fat, sizeof(fat) - 1, "out of bounds");
    expect_error(fat, HEADER_BYTES + ARCH_BYTES, "architecture table");
    expect_error(fat, 4, "fat header");
    put_arch(fat, 0, arm64, 8, 4);
    expect_error(fat, sizeof(fat), "out of bounds");

    enum { MAX_ARCHES = 16, MAX_TABLE_END = HEADER_BYTES + MAX_ARCHES * ARCH_BYTES };
    uint8_t max_fat[MAX_TABLE_END + MAX_ARCHES] =
        {0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, MAX_ARCHES};
    for (unsigned int index = 0; index < MAX_ARCHES; ++index) {
        put_arch(max_fat, index, index == MAX_ARCHES - 1 ? air64 : arm64,
                 MAX_TABLE_END + index, 1);
    }
    expect_slice(max_fat, sizeof(max_fat), sizeof(max_fat) - 1, 1);
    put_be32(max_fat + 4, MAX_ARCHES + 1);
    expect_error(max_fat, sizeof(max_fat), "architecture count");

    expect_error(NULL, sizeof(thin), "non-null");
    expect_error(thin, 0, "nonzero");
    expect_error(thin, SIZE_MAX, "addressable slice");
    expect_error(thin, 3, "magic");
    size_t untouched = 5;
    assert(!nmb_extract_air_slice(thin, sizeof(thin), NULL, &untouched));
    assert(!nmb_extract_air_slice(thin, sizeof(thin), &untouched, NULL));
    assert(!nmb_extract_air_slice(thin, sizeof(thin), &untouched, &untouched));
    assert(untouched == 5);
    assert(strstr(nmb_last_error(), "output pointers") != NULL);
    checks += 3;

    pthread_t thread;
    assert(pthread_create(&thread, NULL, check_thread_error, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(strstr(nmb_last_error(), "output pointers") != NULL);
    ++checks;
    expect_slice(thin, sizeof(thin), 0, sizeof(thin));

    assert(!nmb_device_create_a840(UINT32_MAX));
    assert(strstr(nmb_last_error(), "unknown A840 device flags") != NULL);
    NmbDevice *device = nmb_device_create_a840(0);
    if (!device)
        fprintf(stderr, "A840 device creation: %s\n", nmb_last_error());
    assert(device != NULL);
    assert(nmb_device_flags(device) == 0 && nmb_last_error()[0] == '\0');
    NmbDevice *robust_off = nmb_device_create_a840(NMB_DEVICE_FLAG_ROBUST_OFF);
    assert(robust_off != NULL);
    assert(nmb_device_flags(robust_off) == NMB_DEVICE_FLAG_ROBUST_OFF);
    assert(!nmb_library_parse(device, thin, sizeof(thin)));
    assert(nmb_last_error()[0] != '\0');
    assert(!nmb_library_parse(NULL, thin, sizeof(thin)));
    assert(strstr(nmb_last_error(), "device") != NULL);
    assert(nmb_library_function_count(NULL) == 0);
    assert(strstr(nmb_last_error(), "library") != NULL);
    char name = 'x';
    assert(!nmb_library_function_name(NULL, 0, &name, sizeof(name)));
    assert(name == 'x');
    assert(!nmb_compile_compute_spirv(device, NULL, "main"));
    assert(!nmb_compile_render_spirv(device, NULL, "vs", "fs", 70));
    size_t out_bytes = 42;
    assert(!nmb_spirv_words(NULL, 0, &out_bytes) && out_bytes == 42);
    assert(!nmb_spirv_entry_point(NULL, 0));
    assert(nmb_spirv_stage_count(NULL) == 0);
    assert(nmb_spirv_binding_count(NULL) == 0);
    assert(!nmb_spirv_binding(NULL, 0, NULL));

    nmb_spirv_result_free(NULL);
    nmb_library_destroy(NULL);
    nmb_device_destroy(NULL);
    nmb_device_destroy(robust_off);
    nmb_device_destroy(device);
    assert(nmb_last_error()[0] == '\0');
    puts("translate C ABI v2: handle, flag, error, and NULL lifetime checks passed");
    printf("translate C ABI: %u slice/error checks passed (including thread-local errors)\n",
           checks);
    return 0;
}
