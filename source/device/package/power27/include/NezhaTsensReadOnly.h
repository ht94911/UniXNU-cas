/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_TSENS_READ_ONLY_H
#define NEZHA_TSENS_READ_ONLY_H

#include <stdint.h>

#define NP_TSENS_CONTROLLER_COUNT 7u
#define NP_TSENS_SENSOR_COUNT 55u
#define NP_TSENS_STATUS_OFFSET 0xa0u
#define NP_TSENS_VALID_BIT (UINT32_C(1) << 21)

enum {
    NP_TSENS_ARGUMENT = -1,
    NP_TSENS_NO_VALID_READING = -2
};

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t tm_pa, srot_pa;
    uint32_t region_bytes, sensors;
} NpTsensController;

/* N0's declared layout; SROT is retained for a later adapter and never read here. */
extern const NpTsensController np_tsens_controllers[NP_TSENS_CONTROLLER_COUNT];

typedef struct {
    uint32_t raw;
    int32_t deci_c; /* 0.1 degrees Celsius; INT32_MIN unless valid == 1. */
    int io_error;   /* Exact callback error, not replaced by a temperature. */
    uint8_t controller, sensor, read_ok, valid;
} NpTsensReading;

typedef struct {
    NpTsensReading readings[NP_TSENS_SENSOR_COUNT];
    uint64_t read_mask, valid_mask;
} NpTsensSample;

typedef struct {
    void *context;
    int (*read32)(void *, uint64_t pa, uint32_t *raw);
} NpTsensReadOps;

/* Returns 1 for valid, 0 for a clear valid bit, -1 for NULL output.
 * Invalid status leaves the caller's output unchanged. */
int np_tsens_decode(uint32_t raw, int32_t *deci_c);

/* Reads each of the 55 known TM status words once, with no retry or writes.
 * Every call replaces the sample. Returns the first exact IO error, otherwise
 * 0 when at least one temperature is valid, or NP_TSENS_NO_VALID_READING.
 * A partial sample retains its individual read/valid flags and errors. */
int np_tsens_sample(const NpTsensReadOps *ops, NpTsensSample *sample);

#ifdef __cplusplus
}
#endif

#endif
