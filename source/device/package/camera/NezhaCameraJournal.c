/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Vnode open/attribute validation/IO_SYNC/close follow the tested native
 * NezhaPowerLog.c. Camera persists a bounded record before a risky access
 * from its existing worker. Volume and UFS sync precede hardware access;
 * the matching Android reader verifies what actually survived a return. */
#include "NezhaCameraJournal.h"

#include <mach/mach_time.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/errno.h>

extern volatile uint32_t native_data_persist_state;
extern volatile uint64_t native_data_persist_errors;
extern int native_ufs_store_sync(void);

enum { JournalLimit = 8192 };
static char path[64];
static unsigned step;
static bool active;

static int journal_append(const char *line, unsigned bytes, bool create)
{
    const uint32_t state = native_data_persist_state;
    if ((state != 2 && state != 3) || native_data_persist_errors != 0) {
        return EROFS;
    }
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t file = NULLVP;
    const int flags = FWRITE | O_NOFOLLOW | (create ? (O_CREAT | O_EXCL) : O_APPEND);
    int error = vnode_open(path, flags, 0600, 0, &file, ctx);
    if (error != 0) {
        return error;
    }
    struct vnode_attr attr;
    VATTR_INIT(&attr);
    VATTR_WANTED(&attr, va_data_size);
    VATTR_WANTED(&attr, va_uid);
    VATTR_WANTED(&attr, va_nlink);
    error = vnode_getattr(file, &attr, ctx);
    if (error == 0 && (vnode_vtype(file) != VREG ||
        !VATTR_IS_SUPPORTED(&attr, va_data_size) || !VATTR_IS_SUPPORTED(&attr, va_uid) ||
        !VATTR_IS_SUPPORTED(&attr, va_nlink) || attr.va_uid != 0 || attr.va_nlink != 1)) {
        error = EACCES;
    }
    if (error == 0 && (attr.va_data_size > JournalLimit || bytes > JournalLimit - attr.va_data_size)) {
        error = EFBIG;
    }
    if (error == 0) {
        int residual = (int)bytes;
        error = vn_rdwr(UIO_WRITE, file, (caddr_t)line, (int)bytes, 0, UIO_SYSSPACE,
            IO_APPEND | IO_SYNC | IO_NOAUTH, vfs_context_ucred(ctx), &residual, vfs_context_proc(ctx));
        if (error == 0 && residual != 0) {
            error = EIO;
        }
    }
    /* IO_SYNC on file data and the UFS cache sync do not alone commit
     * APFS namespace/checkpoint metadata across an abrupt return. */
    if (error == 0) {
        mount_t mount = vnode_mount(file);
        if (mount == NULL || (vfs_flags(mount) & MNT_RDONLY) != 0) {
            error = EROFS;
        } else {
            error = VFS_SYNC(mount, MNT_WAIT | MNT_VOLUME, ctx);
        }
    }
    const int close_error = vnode_close(file, FWRITE, ctx);
    if (error == 0) {
        error = close_error;
    }
    if (error == 0 && !native_ufs_store_sync()) {
        error = EIO;
    }
    if (error == 0 && native_data_persist_errors != 0) {
        error = EIO;
    }
    return error;
}

int nezha_camera_journal_begin(unsigned token, unsigned mode)
{
    if (token == 0 || active) {
        return EINVAL;
    }
    snprintf(path, sizeof(path), "/private/var/log/nezha-camera-%08x.log", token);
    char line[96];
    const int size = snprintf(line, sizeof(line), "NEZHA_CAMERA_V1 token=%08x mode=%u start=%llu\n",
        token, mode, (unsigned long long)mach_continuous_time());
    if (size <= 0 || (size_t)size >= sizeof(line)) {
        return EINVAL;
    }
    const int error = journal_append(line, (unsigned)size, true);
    active = error == 0;
    return error;
}

int nezha_camera_journal_mark(unsigned phase, unsigned status, unsigned a, unsigned b, unsigned c)
{
    if (!active) {
        return ENOENT;
    }
    char line[112];
    const int size = snprintf(line, sizeof(line), "%u %llu phase=%u status=%u a=%08x b=%08x c=%08x\n",
        ++step, (unsigned long long)mach_continuous_time(), phase, status, a, b, c);
    if (size <= 0 || (size_t)size >= sizeof(line)) {
        return EINVAL;
    }
    const int error = journal_append(line, (unsigned)size, false);
    if (error != 0) {
        active = false;
    }
    return error;
}
