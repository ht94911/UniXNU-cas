/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_BOOT_ARC_H
#define NEZHA_CAMERA_BOOT_ARC_H

/* The retained nezha stock FDT, /soc/rsc@18900000/drv@2, enables boot
 * proxy consumers at qcom,proxy-consumer-voltage <0x180 0xffff> for
 * pmh0110_d_s7_level (MMCX), pmh0110_f_s9_level (MXC) and
 * pmh0110_i_s10_level (MX). Display /soc/qcom,mdss_mdp@9800000 uses
 * MMCX; /soc/clock-controller@9ba2000 uses MMCX and MX. Stock
 * fc30e54174d2 regulator/proxy-consumer.c and clk/qcom/common.c retain
 * their proxy until sync_state has transferred the active clock votes.
 * This GOP port has no native display-clock takeover. Reestablish these
 * board boot-proxy requests, rather than treating a raw RSC read as the
 * previous APPS vote or choosing a new camera voltage. */
enum {NezhaCameraBootArcCount = 3, NezhaCameraBootArcLevel = 0x180};

#endif
