"""Exercise the exact diagnostic reader with bounded VFS stand-ins."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='clang')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'source/device/package/nezha27-input/ResourceProbe.c').read_text()
body = 'static void read_appsync_log(' + source.split('static void read_appsync_log(', 1)[1].split('\nstatic void resource_thread(', 1)[0]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
typedef void *vnode_t;
typedef void *vfs_context_t;
#define NULLVP NULL
#define FREAD 1
#define UIO_READ 0
#define UIO_SYSSPACE 0
struct vnode_attr { uint64_t va_data_size; };
static unsigned opens, closes, reads, allocations, frees;
static int open_error, attr_error, read_error, close_error, allocation_error;
static int supported = 1, residual_mode;
static size_t file_size, asked;
static off_t asked_offset;
static char file_data[10000], output[30000];
static size_t output_size;
#define VATTR_INIT(x) memset(x, 0, sizeof(*(x)))
#define VATTR_WANTED(x, y) ((void)0)
#define VATTR_IS_SUPPORTED(x, y) supported
static int capture_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int count = vsnprintf(output + output_size, sizeof(output) - output_size, format, args);
    va_end(args);
    assert(count >= 0 && (size_t)count < sizeof(output) - output_size);
    output_size += (size_t)count;
    return count;
}
#define printf capture_printf
static int vnode_open(const char *path, int flags, int mode, int extra, vnode_t *file, vfs_context_t ctx) {
    assert(!strcmp(path, "fixed") && flags == FREAD && mode == 0 && extra == 0 && ctx);
    opens++;
    if (open_error) return open_error;
    *file = (void *)1;
    return 0;
}
static int vnode_getattr(vnode_t file, struct vnode_attr *value, vfs_context_t ctx) {
    assert(file && ctx);
    value->va_data_size = file_size;
    return attr_error;
}
static char *IOMallocData(size_t bytes) {
    assert(bytes == 8192);
    allocations++;
    if (allocation_error) return NULL;
    return malloc(bytes);
}
static void IOFreeData(void *data, size_t bytes) {
    assert(data && bytes == 8192);
    frees++;
    free(data);
}
static void *vfs_context_ucred(vfs_context_t ctx) { return ctx; }
static void *vfs_context_proc(vfs_context_t ctx) { return ctx; }
static int vnode_close(vnode_t file, int flags, vfs_context_t ctx) {
    assert(file && flags == FREAD && ctx);
    closes++;
    return close_error;
}
static int vn_rdwr(int op, vnode_t file, char *buffer, int count, off_t offset,
                  int space, int flags, void *cred, int *resid, void *proc) {
    assert(op == UIO_READ && file && count >= 0 && count <= 8192 && offset >= 0);
    assert((uint64_t)offset + (unsigned)count == file_size);
    assert(space == UIO_SYSSPACE && flags == 0 && cred && proc);
    reads++;
    asked = (size_t)count;
    asked_offset = offset;
    if (read_error) return read_error;
    memcpy(buffer, file_data + (size_t)offset, (size_t)count);
    *resid = 0;
    if (residual_mode == 1) *resid = -1;
    if (residual_mode == 2) *resid = count + 1;
    if (residual_mode == 3) *resid = 4;
    return 0;
}
static void reset(void) {
    opens = closes = reads = allocations = frees = 0;
    open_error = attr_error = read_error = close_error = allocation_error = residual_mode = 0;
    supported = 1;
    file_size = 10000;
    asked = 0;
    asked_offset = 0;
    output_size = 0;
    output[0] = 0;
    memset(file_data, 'X', sizeof(file_data));
}
'''
tests = r'''
#undef printf
int main(void) {
    reset(); memcpy(file_data + 9992, "TAIL_END", 8); read_appsync_log("fixed", (void *)1);
    assert(opens == 1 && closes == 1 && reads == 1 && asked == 8192 && asked_offset == 1808);
    assert(allocations == frees && strstr(output, "bytes=8192 offset=1808") && strstr(output, "TAIL_END"));
    reset(); file_size = 7; memcpy(file_data, "x %n yz", 7); read_appsync_log("fixed", (void *)1);
    assert(asked == 7 && asked_offset == 0 && strstr(output, "x %n yz") && allocations == frees);
    reset(); file_size = 0; read_appsync_log("fixed", (void *)1);
    assert(reads == 1 && asked == 0 && strstr(output, "bytes=0") && allocations == frees && closes == 1);
    reset(); open_error = ENOENT; read_appsync_log("fixed", (void *)1);
    assert(!reads && !closes && !allocations);
    reset(); attr_error = EIO; read_appsync_log("fixed", (void *)1);
    assert(closes == 1 && !allocations && !reads);
    reset(); supported = 0; read_appsync_log("fixed", (void *)1);
    assert(closes == 1 && !allocations && !reads);
    reset(); allocation_error = 1; read_appsync_log("fixed", (void *)1);
    assert(closes == 1 && allocations == 1 && frees == 0 && reads == 0);
    reset(); read_error = EIO; read_appsync_log("fixed", (void *)1);
    assert(closes == 1 && allocations == frees && !strstr(output, "APPSYNC LOG DATA"));
    reset(); residual_mode = 1; read_appsync_log("fixed", (void *)1);
    assert(closes == 1 && allocations == frees && !strstr(output, "APPSYNC LOG DATA"));
    reset(); residual_mode = 2; read_appsync_log("fixed", (void *)1);
    assert(closes == 1 && allocations == frees && !strstr(output, "APPSYNC LOG DATA"));
    reset(); file_size = 10; residual_mode = 3; read_appsync_log("fixed", (void *)1);
    assert(closes == 1 && allocations == frees && strstr(output, "bytes=6"));
    reset(); close_error = EIO; read_appsync_log("fixed", (void *)1);
    assert(closes == 1 && allocations == frees && strstr(output, "read=0 close=5"));
    puts("PASS: exact production reader, 12 VFS cases under ASan/UBSan.");
}
'''
with tempfile.TemporaryDirectory(prefix='appsync-cache-log-') as temporary:
    work = Path(temporary)
    (work / 'reader.c').write_text(prefix + body + tests)
    subprocess.run([args.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(work / 'reader.c'), '-o', str(work / 'reader')], check=True)
    result = subprocess.run([str(work / 'reader')], capture_output=True, text=True)
    print(result.stdout, end='')
    print(result.stderr, end='')
    if result.returncode:
        raise SystemExit(result.returncode)
args.output.write_text(json.dumps(dict(passed=True, scenarios=12, VFS_stand_ins=True,
    source_sha256=hashlib.sha256(body.encode()).hexdigest(), exact_production_function=True,
    kernel_compilation_separate=True, output=result.stdout), indent=2) + '\n')
