/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_AF_PARKING_H
#define NEZHA_CAMERA_AF_PARKING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NezhaCameraAfParkingMoves = 50 };
struct NezhaCameraAfParking {
    unsigned count;
    uint16_t dac[NezhaCameraAfParkingMoves];
    uint16_t delay_ms[NezhaCameraAfParkingMoves];
};

/* Original nezha OFILM/SEMCO AK7316 GenerateParkLens receives current
 * DAC and both OEM parking endpoints. Preserve its double0.6 rounding,
 * minimum10-DAC moves, final endpoint and10000us delay. User-space only;
 * caller supplies the validated current DAC and module parking interval.
 * No movement, GPIO, power change or generic interval is selected here. */
bool nezha_camera_af_parking(uint16_t current, uint16_t first, uint16_t last,
    struct NezhaCameraAfParking *out);

#ifdef __cplusplus
}
#endif
#endif
