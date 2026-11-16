/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_SENSOR_BUNDLE_H
#define NEZHA_SENSOR_BUNDLE_H
#include "NezhaSensorFiles.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NEZHA_SENSOR_BUNDLE_CAPACITY (8u*1024u*1024u)
#define NEZHA_SENSOR_BUNDLE_ENTRIES 512u
typedef struct {char path[256];const uint8_t *data;uint32_t bytes,directory;} NezhaSensorBundleEntry;
typedef struct {NezhaSensorBundleEntry entry[NEZHA_SENSOR_BUNDLE_ENTRIES];uint32_t count;} NezhaSensorBundle;
/* Standard ustar input; paths remain the exact Android paths, with an optional
 * private properties/ directory. Entries/contents never become source code. */
int nezha_sensor_bundle_prepare(NezhaSensorBundle *,const uint8_t *,uint32_t bytes);
int nezha_sensor_bundle_file(void *,const char *,const uint8_t **,uint32_t *);
int nezha_sensor_bundle_property(void *,const char *,const char **);
int nezha_sensor_bundle_directory(void *,const char *,uint32_t,const char **,int *);
#ifdef __cplusplus
}
#endif
#endif
