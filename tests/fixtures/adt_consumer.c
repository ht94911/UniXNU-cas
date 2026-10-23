/* Host contract probe. SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "host_shim.h"
#include <pexpert/device_tree.h>
#include "darwin_adt.h"
#include "nvram_helpers.h"

static const void *property(const char *path, const char *name, unsigned *size) {
  DTEntry node;
  const void *value = NULL;
  assert(SecureDTLookupEntry(NULL, path, &node) == kSuccess);
  assert(SecureDTGetProperty(node, name, &value, size) == kSuccess);
  return value;
}
static uint32_t word(const char *path, const char *name) {
  unsigned size;
  const void *p = property(path, name, &size);
  uint32_t value;
  assert(size == sizeof value);
  memcpy(&value, p, sizeof value);
  return value;
}
int main(int argc, char **argv) {
  assert(argc == 7);
  unsigned expected_cpus = (unsigned)strtoul(argv[2], NULL, 0);
  uint64_t expected_dist = strtoull(argv[3], NULL, 0);
  uint64_t expected_redist = strtoull(argv[4], NULL, 0);
  uint32_t expected_frequency = (uint32_t)strtoul(argv[5], NULL, 0);
  uint64_t expected_stride = strtoull(argv[6], NULL, 0);
  FILE *stream = fopen(argv[1], "rb");
  assert(stream);
  assert(fseek(stream, 0, SEEK_END) == 0);
  long length = ftell(stream);
  assert(length >= 8 && length <= (1 << 20));
  rewind(stream);
  uint8_t *blob = malloc((size_t)length);
  assert(blob && fread(blob, 1, (size_t)length, stream) == (size_t)length);
  fclose(stream);
  UINTN offset = 0, ramdisk = MAX_UINTN, trust = MAX_UINTN, addon = MAX_UINTN;
  UINT64 window = 0;
  BOOLEAN keep = FALSE;
  assert(AdtNode(blob, (UINTN)length, &offset, 0, 3, &ramdisk, &trust, &addon, &window, &keep));
  assert(offset == (UINTN)length && ramdisk == MAX_UINTN && trust == MAX_UINTN && addon == MAX_UINTN && !keep);
  assert(window == 0x10000000ULL);
  SecureDTInit(blob, (size_t)length);
  const char *required[] = {"/", "/chosen", "/chosen/memory-map", "/cpus", "/arm-io", "/arm-io/intc",
                            "/defaults", "/options", "/memory", "/reserved-memory", "/psci", "/timer"};
  for (unsigned i = 0; i < sizeof required / sizeof required[0]; ++i) {
    DTEntry entry;
    assert(SecureDTLookupEntry(NULL, required[i], &entry) == kSuccess);
  }
  DTEntry cpus, cpu;
  OpaqueDTEntryIterator iter;
  assert(SecureDTLookupEntry(NULL, "/cpus", &cpus) == kSuccess);
  assert(SecureDTInitEntryIterator(cpus, &iter) == kSuccess);
  unsigned count = 0, size;
  while (SecureDTIterateEntries(&iter, &cpu) == kSuccess) {
    const void *p;
    uint32_t reg, frequency;
    assert(SecureDTGetProperty(cpu, "reg", &p, &size) == kSuccess && size == 4);
    memcpy(&reg, p, 4);
    assert(reg == count * 0x100U);
    assert(SecureDTGetProperty(cpu, "device_type", &p, &size) == kSuccess && size == 4);
    assert(memcmp(p, "cpu", 4) == 0);
    assert(SecureDTGetProperty(cpu, "state", &p, &size) == kSuccess);
    assert(strcmp(p, count == 0 ? "running" : "waiting") == 0);
    assert(SecureDTGetProperty(cpu, "timebase-frequency", &p, &size) == kSuccess && size == 4);
    memcpy(&frequency, p, 4);
    assert(frequency == expected_frequency);
    ++count;
  }
  assert(count == expected_cpus);
  uint64_t bus[3], reg[4], stride;
  const void *p = property("/arm-io", "ranges", &size);
  assert(size == sizeof bus); memcpy(bus, p, sizeof bus);
  p = property("/arm-io/intc", "reg", &size);
  assert(size == sizeof reg); memcpy(reg, p, sizeof reg);
  p = property("/arm-io/intc", "compatible", &size);
  assert(size == 11 && memcmp(p, "arm,gic-v3", 11) == 0);
  p = property("/arm-io/intc", "redistributor-stride", &size);
  assert(size == sizeof stride); memcpy(&stride, p, sizeof stride);
  assert(stride == expected_stride);
  for (unsigned i = 0; i < 4; i += 2) {
    assert(reg[i] >= bus[0] && reg[i] - bus[0] <= bus[2]);
    assert(reg[i + 1] <= bus[2] - (reg[i] - bus[0]));
  }
  assert(bus[1] + reg[0] - bus[0] == expected_dist);
  assert(bus[1] + reg[2] - bus[0] == expected_redist);
  p = property("/psci", "method", &size);
  assert(size == 4 && memcmp(p, "smc", 4) == 0);
  assert(word("/chosen", "nvram-bank-size") == 8192);
  assert(word("/chosen", "nvram-bank-count") == 1);
  assert(word("/chosen", "nvram-current-bank") == 0);
  const uint8_t *nvram = property("/chosen", "nvram-proxy-data", &size);
  assert(size == 8192);
  uint32_t generation;
  assert(nvram_validate_header_v1v2(nvram, &generation, kNVRAMVersion1) == kIOReturnSuccess);
  assert(generation == 0);
  assert(nvram_get_adler((uint8_t *)nvram, kNVRAMVersion1) == adler32_with_version(nvram, size, kNVRAMVersion1));
  for (unsigned i = 0; i < 3; ++i) {
    unsigned pos = i == 0 ? 0 : i == 1 ? 32 : 2096;
    const struct chrp_nvram_header *header = (const void *)(nvram + pos);
    assert(header->cksum == chrp_checksum(header));
  }
  printf("{\"result\":\"pass\",\"cpus\":%u,\"frequency_hz\":%u,\"darwin_window\":%llu,\"nvram_bytes\":%u}\n",
         count, expected_frequency, (unsigned long long)window, size);
  free(blob);
  return 0;
}
