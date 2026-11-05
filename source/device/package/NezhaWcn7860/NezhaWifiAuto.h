/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NEZHA_WIFI_AUTO_H
#define NEZHA_WIFI_AUTO_H
#include <stddef.h>
#include <stdint.h>
#define NEZHA_WIFI_AUTO_PATH "/private/var/ngpu/wifi-test.txt"
#define NEZHA_WIFI_AUTO_MAX_BYTES 104u
int nezha_wifi_auto_prepare(void);
void nezha_wifi_auto_tick(uint64_t now_us);
void nezha_wifi_auto_stop(int error);
/* Cached, nonblocking rows for the existing BootProgress display owner. */
unsigned native_wifi27_screen_rows(char (*out)[120], unsigned capacity);
#endif
