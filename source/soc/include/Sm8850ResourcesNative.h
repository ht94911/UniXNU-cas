/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM8850_RESOURCES_NATIVE_H
#define SM8850_RESOURCES_NATIVE_H
#include "Sm8850Resources.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Normal thread context with IRQ enabled, not an IRQ/panic spin guard. */
int sm8850_apps_config_lock(void);
void sm8850_apps_config_unlock(void);
/* USB calls while holding the common lock after install/before restore. */
void sm8850_usb_domain_publish(unsigned ready);
unsigned sm8850_usb_domain_ready(void);
/* Mapping/initialization only; no register write. Call after inherited power
 * and the selected board's existing initialization have completed. */
Sm8850Resources *sm8850_native_resources(void);
#ifdef __cplusplus
}
#endif
#endif
