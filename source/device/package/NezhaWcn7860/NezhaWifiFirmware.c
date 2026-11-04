/* SPDX-License-Identifier: GPL-2.0-only */
/* VFS lifetime follows sheng4601795 WifiFirmware.c. Only the existing RAM
 * addon may supply files; no raw disk or persistence fallback exists. */
#include "NezhaWifiPlatform.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <kern/kalloc.h>
#include <pexpert/device_tree.h>
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccsha2.h>

static struct { const char *name; uint8_t *bytes; size_t length; } firmware[] = {
    {"peach/amss20.bin", NULL, 0}, {"peach/regdb_xiaomi.bin", NULL, 0},
    {"peach/bd_p1.elf", NULL, 0}, {"peach/phy_ucode20.elf", NULL, 0},
    {"peach/aux_ucode20.elf", NULL, 0},
};
static uint8_t expected[NEZHA_WIFI_FIRMWARE_FILES][32];
static unsigned manifest_ready;
static int manifest(void)
{
    if (manifest_ready) return 0;
    DTEntry chosen; const void *data; unsigned length = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, "nezha-wifi-firmware-sha256", &data, &length) != kSuccess ||
        length != sizeof(expected)) return ENOENT;
    memcpy(expected, data, sizeof(expected));
    for (unsigned i = 0; i < NEZHA_WIFI_FIRMWARE_FILES; i++) {
        unsigned nonzero = 0; for (unsigned j = 0; j < 32; j++) nonzero |= expected[i][j];
        if (!nonzero) return EINVAL;
    }
    manifest_ready = 1; return 0;
}
static int load(unsigned index)
{
    if (firmware[index].bytes) return 0;
    int error = manifest(); if (error) return error;
    char path[160];
    if (snprintf(path, sizeof(path), "%s/%s", NEZHA_WIFI_FIRMWARE_ROOT, firmware[index].name) >= (int)sizeof(path)) return EINVAL;
    vfs_context_t ctx = vfs_context_kernel(); vnode_t file = NULLVP;
    error = vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &file, ctx);
    if (error) return error;
    struct vnode_attr va; VATTR_INIT(&va); VATTR_WANTED(&va, va_data_size);
    error = vnode_getattr(file, &va, ctx);
    uint8_t *buffer = NULL; size_t length = 0;
    if (!error && (!VATTR_IS_SUPPORTED(&va, va_data_size) || vnode_vtype(file) != VREG ||
        !va.va_data_size || va.va_data_size > 32u * 1024 * 1024)) error = EINVAL;
    if (!error) {
        length = (size_t)va.va_data_size;
        buffer = kalloc_data(length, Z_WAITOK);
        if (!buffer) error = ENOMEM;
    }
    if (!error) {
        int residual = (int)length;
        error = vn_rdwr(UIO_READ, file, (caddr_t)buffer, (int)length, 0, UIO_SYSSPACE,
            IO_NOAUTH, vfs_context_ucred(ctx), &residual, vfs_context_proc(ctx));
        if (!error && residual) error = EIO;
    }
    int close_error = vnode_close(file, FREAD, ctx);
    if (!error) error = close_error;
    if (!error) {
        uint8_t digest[32]; ccdigest(ccsha256_di(), length, buffer, digest);
        if (memcmp(digest, expected[index], sizeof(digest))) error = EAUTH;
    }
    if (error) { if (buffer) kfree_data(buffer, length); return error; }
    firmware[index].bytes = buffer; firmware[index].length = length;
    return 0;
}
int nezha_wifi_firmware_prepare(void)
{
    for (unsigned i = 0; i < NEZHA_WIFI_FIRMWARE_FILES; i++) {
        int e = load(i); if (e) return e;
    }
    return 0;
}
int nezha_wifi_firmware_pread(const char *name, uint64_t offset, void *out, size_t bytes, uint64_t *size)
{
    if (!name || (!out && bytes)) return EINVAL;
    for (unsigned i = 0; i < NEZHA_WIFI_FIRMWARE_FILES; i++) if (!strcmp(name, firmware[i].name)) {
        if (!firmware[i].bytes) return ENXIO;
        if (offset > firmware[i].length || bytes > firmware[i].length - offset) return EINVAL;
        if (bytes) memcpy(out, firmware[i].bytes + offset, bytes);
        if (size) *size = firmware[i].length;
        return 0;
    }
    return ENOENT;
}
