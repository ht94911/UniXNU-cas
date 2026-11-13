/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CANOE_PINS_H
#define NEZHA_CAMERA_CANOE_PINS_H

/* Canoe pinctrl adds its GPIO window to the DT controller resource.
 * Same aperture as the tested nezha touch/button drivers. */
enum {
    NezhaCameraTlmmResource = 0x0f000000,
    NezhaCameraTlmmGpioWindow = 0x100000,
    NezhaCameraGpioStride = 0x1000,
    NezhaCameraGpioFirst = 89,
    NezhaCameraGpioLast = 116,
    NezhaCameraGpioOffset = NezhaCameraGpioFirst * NezhaCameraGpioStride,
    NezhaCameraGpioPhysical = NezhaCameraTlmmResource + NezhaCameraTlmmGpioWindow + NezhaCameraGpioOffset,
    NezhaCameraGpioBytes = (NezhaCameraGpioLast - NezhaCameraGpioFirst + 1) * NezhaCameraGpioStride,
};

#endif
