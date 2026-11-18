// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NEZHA_SENSOR_ORIENTATION_H
#define NEZHA_SENSOR_ORIENTATION_H
#include "QcomSscClient.h"
#ifdef __cplusplus
extern "C" {
#endif
enum nezha_orientation {
    NezhaOrientationUnknown, NezhaOrientationPortrait, NezhaOrientationPortraitUpsideDown,
    NezhaOrientationLandscapeLeft, NezhaOrientationLandscapeRight,
    NezhaOrientationFaceUp, NezhaOrientationFaceDown
};
struct nezha_orientation_filter {
    uint64_t epoch,last_ticks,candidate_since;
    uint32_t counter_hz,candidate,published;
};
int nezha_orientation_begin(struct nezha_orientation_filter *,uint64_t epoch,uint32_t counter_hz);
// Uses SEE's factory-mounted gravity in Q16.16 m/s2. This is a discrete
// direction classifier, never calibration or a replacement motion sample.
int nezha_orientation_sample(struct nezha_orientation_filter *,const QsscSample *,uint32_t *direction);
void nezha_orientation_invalidate(struct nezha_orientation_filter *);
#ifdef __cplusplus
}
#endif
#endif
