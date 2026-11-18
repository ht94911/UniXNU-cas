/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_TSENS_NATIVE_H
#define NEZHA_TSENS_NATIVE_H
#include "NezhaTsensReadOnly.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Bootstrap maps only seven N0 TM pages; never accesses SROT. Missing pages
 * stay unavailable and are reported by the sampler, without remapping. */
int np_tsens_native_prepare(void);
int np_tsens_native_sample(void *,NpTsensSample *);
#ifdef __cplusplus
}
#endif
#endif
