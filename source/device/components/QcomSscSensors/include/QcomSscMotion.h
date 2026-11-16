// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef QCOM_SSC_MOTION_H
#define QCOM_SSC_MOTION_H
#include "QcomSscClient.h"
#ifdef __cplusplus
extern "C" {
#endif
#define QSSC_MOTION_HISTORY 64u
// Real SEE game_rv tick is the frame time. Gravity/gyro values use the latest
// not-later sample within40ms; source ticks and skew are retained for diagnostics.
typedef struct {
    uint64_t epoch,timestamp,ssc_ticks,sequence,client_id,gravity_ticks,gyro_ticks;
    int32_t quaternion[4],user_accel[3],rotation_rate[3];
    uint32_t status;
    bool valid;
} QsscMotionFrame;
typedef struct {
    QsscSample values[3][QSSC_MOTION_HISTORY];
    uint32_t head[3],count[3];
    uint64_t epoch,sequence,last_frame;
    uint32_t counter_hz,dropped,invalid;
} QsscMotion;
int qssc_motion_begin(QsscMotion *,uint64_t epoch,uint32_t counter_hz);
int qssc_motion_add(QsscMotion *,const QsscSample *);
int qssc_motion_next(QsscMotion *,QsscMotionFrame *);
void qssc_motion_invalidate(QsscMotion *);
// Original CoreMotion type4 vendor payload:60 bytes. Binary32 values are encoded
// from integer fixed point, no FP instructions. Caller supplies at least60 bytes.
int qssc_motion_payload(const QsscMotionFrame *,uint8_t *,size_t);
#ifdef __cplusplus
}
#endif
#endif
