/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_GEN4_NTC_H
#define NEZHA_GEN4_NTC_H
#include <stdint.h>
/* Board full-scale65535/clip65527 and actual OS4 100k NTC table.
 * These functions convert a code, not its ownership or freshness validity.
 * Keep c1024 for stock numeric policies; centi-C is for native consumers. */
int32_t np_gen4_ntc_c1024(uint16_t code);
int32_t np_gen4_ntc_centi(uint16_t code);
#endif
