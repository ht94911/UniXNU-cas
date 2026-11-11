// SPDX-License-Identifier: BSD-2-Clause-Patent
// nezha input: shared between NezhaInput.c (Synaptics TouchComm SPI touch + PMK8850/TLMM keys, ported from
// data-a-live pandora_touch.c / fuxi_keys.c) and NativeHidInput.cpp (umi/sheng HID device).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NEZHA_PANEL_W 1200
#define NEZHA_PANEL_H 2608
#define NEZHA_TOUCH_MAX 10

// One coherent contact set in panel pixels (portrait, 1200 x 2608).
struct nezha_touch_frame {
	uint32_t sequence;
	uint32_t active;                 // bit per contact id
	uint16_t x[NEZHA_TOUCH_MAX], y[NEZHA_TOUCH_MAX];
};

// HID report 2 bits (sheng layout plus Home in bit 0).
#define NEZHA_KEY_HOME   (1u << 0)
#define NEZHA_KEY_POWER  (1u << 1)
#define NEZHA_KEY_VOLUP  (1u << 2)
#define NEZHA_KEY_VOLDN  (1u << 3)

#ifdef __cplusplus
extern "C" {
#endif
bool nezha_touch_next(struct nezha_touch_frame *frame);
bool nezha_keys_next(uint32_t *mask);
void nezha_input_start(void);
#ifdef __cplusplus
}
#endif
