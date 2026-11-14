/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The sensor daemon's AP services over the original QAIC reverse interface.
 * File data/property values come from one immutable local private bundle. */
#ifndef NEZHA_SENSOR_FILES_H
#define NEZHA_SENSOR_FILES_H
#include "../upstream/QcomRpcListener.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NEZHA_SENSOR_FILE_HANDLES 16u
#define NEZHA_SENSOR_MODULE_HANDLES 16u
typedef struct {
    void *context;
    /* Exact requested path/name. The private provider supplies borrowed bytes
     * for its retained boot lifetime. Missing is39, readonly is0x31. */
    int (*file)(void *,const char *path,const uint8_t **data,uint32_t *bytes);
    int (*property)(void *,const char *name,const char **value);
    int (*directory)(void *,const char *path,uint32_t index,const char **name,int *end);
    void (*observe)(void *,const char *kind,const char *name,uint32_t method,int result);
} NezhaSensorFileOps;
typedef struct {
    NezhaSensorFileOps ops;
    struct {const uint8_t *data;uint32_t bytes,position,used,eof;} files[NEZHA_SENSOR_FILE_HANDLES];
    struct {uint32_t handle,kind,used;} modules[NEZHA_SENSOR_MODULE_HANDLES];
    struct {char path[256];uint32_t position,used;} directories[8];
    uint32_t next_module,requests;
} NezhaSensorFiles;
int nezha_sensor_files_prepare(NezhaSensorFiles *,const NezhaSensorFileOps *);
int32_t nezha_sensor_files_invoke(void *,uint32_t handle,uint32_t scalar,QcomRpcArg *,uint32_t count);
#ifdef __cplusplus
}
#endif
#endif
