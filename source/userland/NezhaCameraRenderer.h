/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_RENDERER_H
#define NEZHA_CAMERA_RENDERER_H
#include <stdbool.h>
#include <stdint.h>
struct mach_header;
bool nezha_camera_renderer_install(const struct mach_header *header,intptr_t slide);
void nezha_camera_renderer_observe(void);
/* Existing provider output reports can observe the original graph even if
 * one downstream callback has not returned. No new timer or frame read. */
void nezha_camera_renderer_report_flow(uint64_t sequence,uint64_t ticks,unsigned generation)
    __attribute__((weak));
#endif
