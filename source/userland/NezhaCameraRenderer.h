/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_RENDERER_H
#define NEZHA_CAMERA_RENDERER_H
#include <stdbool.h>
#include <stdint.h>
#include <mach-o/loader.h>
bool nezha_camera_renderer_install(const struct mach_header *header,intptr_t slide);
void nezha_camera_renderer_observe(void);
#endif
