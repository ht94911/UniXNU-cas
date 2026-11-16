// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef QCOM_SSC_MATH_H
#define QCOM_SSC_MATH_H
#include "QcomSscWire.h"
#ifdef __cplusplus
extern "C" {
#endif
// Signed Q32.32 SI intermediates retain sub-LSB biases; no kernel FP or SIMD.
int qssc_float_q32(uint32_t bits,int64_t *out);
int qssc_scaled_q16(int64_t q32,uint32_t numerator,uint32_t denominator,int32_t *out);
int qssc_integer_rate(uint32_t hz,uint32_t *bits);
typedef struct {
    uint64_t epoch,counter_anchor,host_anchor;
    uint32_t counter_hz,host_hz;
    bool valid;
} QsscClock;
// Owner obtains one tightly bracketed counter/host pair, including on wake.
// Counter is the same physical 19.2MHz QTimer used by SSC, not receipt time.
int qssc_clock_anchor(QsscClock *,uint64_t epoch,uint64_t counter_before,
                      uint64_t host_ticks,uint64_t counter_after,
                      uint32_t counter_hz,uint32_t host_hz);
int qssc_clock_convert(const QsscClock *,uint64_t epoch,uint64_t sample_counter,
                       uint64_t receipt_counter,uint64_t *host_ticks);
#ifdef __cplusplus
}
#endif
#endif
