// SPDX-License-Identifier: MIT
// Actual ID1 methods; registry objects modeled, no target IOKit claim.
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

struct OSObject { virtual ~OSObject() = default; };
struct OSData : OSObject {
    uint64_t bytes = 0x0807060504030201ULL;
    size_t length = 8;
    size_t getLength() const { return length; }
    const void *getBytesNoCopy() const { return &bytes; }
};
struct IORegistryEntry {
    OSObject *property = nullptr;
    OSObject *getProperty(const char *name) {
        assert(!strcmp(name, "unique-chip-id"));
        return property;
    }
};
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
static uint8_t aks_device_id[8];
static uint32_t aks_device_id_ready;
#include "actual-id.inc"

int main()
{
    uint8_t out[16]; memset(out, 0xcc, sizeof out);
    assert(native_aks_device_id(out, sizeof out) == 0);
    native_aks_prepare_device_id(nullptr);
    IORegistryEntry chosen;
    native_aks_prepare_device_id(&chosen);
    OSObject wrong;
    chosen.property = &wrong;
    native_aks_prepare_device_id(&chosen);
    assert(native_aks_device_id(out, sizeof out) == 0);
    OSData data;
    chosen.property = &data;
    for (size_t length : {size_t(0), size_t(1), size_t(7), size_t(9), size_t(32)}) {
        data.length = length;
        native_aks_prepare_device_id(&chosen);
        assert(native_aks_device_id(out, sizeof out) == 0);
    }
    data.length = 8;
    for (uint64_t value : {uint64_t(0), UINT64_MAX}) {
        data.bytes = value;
        native_aks_prepare_device_id(&chosen);
        assert(native_aks_device_id(out, sizeof out) == 0);
    }
    const uint8_t expected[8] = {1,2,3,4,5,6,7,8};
    data.bytes = 0x0807060504030201ULL;
    std::array<std::thread, 8> readers;
    for (auto &thread : readers) {
        thread = std::thread([&] {
            uint8_t value[8];
            for (unsigned i = 0; i < 10000; ++i) {
                size_t n = native_aks_device_id(value, sizeof value);
                assert(n == 0 || (n == 8 && !memcmp(value, expected, 8)));
            }
        });
    }
    native_aks_prepare_device_id(&chosen);
    for (auto &thread : readers) thread.join();
    assert(native_aks_device_id(nullptr, 8) == 0);
    for (size_t length = 0; length < 8; ++length)
        assert(native_aks_device_id(out, length) == 0);
    assert(native_aks_device_id(out, sizeof out) == 8 && !memcmp(out, expected, 8));
    for (size_t i = 8; i < sizeof out; ++i) assert(out[i] == 0xcc);
    // Subsequent registry changes cannot change the already published ID.
    data.bytes = 123;
    native_aks_prepare_device_id(&chosen);
    assert(native_aks_device_id(out, sizeof out) == 8 && !memcmp(out, expected, 8));
    puts("PASS actual ID1: bounded input/output, one publication, concurrent readers and immutable lifetime");
}
