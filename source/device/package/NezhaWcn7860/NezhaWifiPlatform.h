/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NEZHA_WIFI_PLATFORM_H
#define NEZHA_WIFI_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#define NEZHA_WIFI_FIRMWARE_PREFIX "firmware/wcn7860"
#define NEZHA_WIFI_FIRMWARE_ROOT "/private/var/ngpu/" NEZHA_WIFI_FIRMWARE_PREFIX
#define NEZHA_WIFI_FIRMWARE_FILES 5u
#define NEZHA_WIFI_RDDM_BYTES 0x780000u
/* Fixed normal-thread APIs. No physical address comes from USB or userspace. */
unsigned nezha_wifi_mmio_access(uintptr_t, uint64_t *, unsigned, unsigned);
int nezha_wifi_firmware_prepare(void);
int nezha_wifi_firmware_pread(const char *, uint64_t, void *, size_t, uint64_t *);
void nezha_wifi_domain_lock(void);
void nezha_wifi_domain_unlock(void);
int nezha_wifi_platform_ready(void);
int nezha_wifi_diagnostic_init(void);
void nezha_wifi_diagnostic_publish(unsigned phase, unsigned state, int error,
    uint64_t rx, uint64_t tx, uint64_t dropped, unsigned in_flight,
    unsigned bank, unsigned slot, const char *step);
int nezha_wifi_diagnostic_dump(const void *, size_t);
#endif
