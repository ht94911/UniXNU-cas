/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_REGISTER_IO_H
#define NEZHA_CAMERA_REGISTER_IO_H

#include <stdint.h>

/* Ordered 32-bit device accesses relative to one owned register block. */
struct NezhaCameraRegisterIo {
    void *context;
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);
    void (*wait_ms)(void *context, unsigned milliseconds);
};

#endif
