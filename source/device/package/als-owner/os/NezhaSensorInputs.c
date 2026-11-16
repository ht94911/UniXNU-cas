/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../platform/NezhaSensorBundle.h"
/* The same zero-in-CI/private-local-fill convention as __nezaudio. */
__attribute__((used,aligned(4096),section("__DATA_CONST,__nezsensor")))
const uint8_t nezha_sensor_inputs[NEZHA_SENSOR_BUNDLE_CAPACITY]={0};
