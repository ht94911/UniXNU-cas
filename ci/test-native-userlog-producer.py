#!/usr/bin/env python3
"""Host-only checks of the actual Native015 producer handler and existing FIFO.

Native proc, CDHash, bootarg and copyin dependencies are explicit host fixtures.
The handler, scalar parser, wire CRC and atomic FIFO algorithms come from the
selected component sources. These checks do not compile or run an XNU target.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "QcomUserLogWire.h"
#include "QcomUsbBuffer.h"
#define CS_CDHASH_LEN 20
struct proc { int root; pid_t pid; const uint8_t *cdhash; };
typedef struct proc *proc_t;
static struct proc process;
static const char *bootarg;
static unsigned copies, records, hashes, parses;
static int copy_error;
static uint8_t actual_hash[20], storage[4096];
static struct soc_usb_buffer ring;
static proc_t current_proc(void) { return &process; }
static int proc_suser(proc_t p) { assert(p == &process); return p->root ? 0 : EPERM; }
static pid_t proc_pid(proc_t p) { assert(p == &process); return p->pid; }
static const uint8_t *cs_get_cdhash(proc_t p) {
    assert(p == &process); ++hashes; return p->cdhash;
}
/* Fixture implements a forced string copy with the native size/termination
 * contract. It deliberately never converts a decimal string to an integer. */
static int PE_parse_boot_arg_str(const char *key, char *out, int capacity) {
    assert(!strcmp(key, "btlog") && capacity == 42); ++parses;
    if (!bootarg) return 0;
    size_t n = strlen(bootarg);
    if (n >= (size_t)capacity) n = (size_t)capacity - 1;
    memcpy(out, bootarg, n); out[n] = 0; return 1;
}
struct request { void *oldptr, *newptr; size_t newlen; };
#define SYSCTL_HANDLER_ARGS (void *oidp, void *arg1, int arg2, struct request *req)
static int host_copyin(struct request *req, void *out, size_t count) {
    ++copies; assert(count == req->newlen);
    if (copy_error) return copy_error;
    memcpy(out, req->newptr, count); return 0;
}
#define SYSCTL_IN(req, out, count) host_copyin(req, out, count)
static size_t qcom_usb_debug_try_record(const void *record, size_t count) {
    ++records; return soc_usb_buffer_try_record(&ring, record, count);
}
'''

CASES = r'''
static const uint8_t fixture_hash[20] = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3
};
static const char fixture_hex[] = "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3";
static unsigned host_cases;
static void completed(const char *name) { ++host_cases; printf("PASS %s\n", name); }
static void reset(void) {
    memset(storage, 0, sizeof storage);
    ring = (struct soc_usb_buffer){ .data = storage, .capacity = sizeof storage };
    memcpy(actual_hash, fixture_hash, sizeof actual_hash);
    process = (struct proc){ .pid = 321, .cdhash = actual_hash };
    bootarg = fixture_hex; copies = records = hashes = parses = 0; copy_error = 0;
}
static struct QcomUserLogWire frame(const char *text) {
    struct QcomUserLogWire w = { .magic = SOC_ULOG_MAGIC, .version = 1,
        .sessionLo = 321, .sessionHi = 0x51425455u, .stream = 1,
        .flags = SOC_ULOG_STATUS };
    size_t n = strlen(text); assert(n && n <= sizeof w.data);
    w.total = w.bytes = (uint32_t)n; memcpy(w.data, text, n);
    w.crc = qcom_ulog_crc(w.data, n); return w;
}
static int submit(struct QcomUserLogWire *w) {
    return qcom_usb_userlog_submit(w, offsetof(struct QcomUserLogWire, data) + w->bytes);
}
static int syscall_frame(struct QcomUserLogWire *w) {
    struct request req = { .newptr = w, .newlen = offsetof(struct QcomUserLogWire, data) + w->bytes };
    return qcom_usb_userlog_sysctl(0, 0, 0, &req);
}
static void denied_identity(void) {
    struct QcomUserLogWire w = frame("[qbt-ui] p=1 pid=321 a=1 b=0\n");
    assert(syscall_frame(&w) == EPERM);
    assert(!copies && !records && !ring.used && !ring.accepted);
    assert(submit(&w) == EPERM && !records);
}
int main(void) {
    struct QcomUserLogWire w;
    reset(); process.root = 1; bootarg = 0; process.cdhash = 0;
    w = frame("arbitrary legacy root payload"); w.flags = SOC_ULOG_GZIP;
    w.sessionLo = 17; w.sessionHi = 93;
    assert(syscall_frame(&w) == 0 && copies == 1 && records == 1 && ring.used);
    assert(!hashes && !parses);
    assert(strstr((char *)storage, "flags=2 data=") && !ring.overflow && !ring.contended);
    completed("root-keeps-original-wire-and-gzip-without-btlog");

    reset(); bootarg = 0; denied_identity();
    completed("missing-btlog-is-EPERM-before-copyin");
    const char *invalid[] = {
        "", "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b",  /* 39 */
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b30", /* 41 */
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b30000000000000",
        "g0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3", /* nonhex */
        "0xa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        reset(); bootarg = invalid[i]; denied_identity(); assert(!hashes);
    }
    completed("invalid-short-long-or-nonhex-btlog-is-EPERM");
    reset(); process.cdhash = 0; denied_identity(); assert(hashes);
    reset(); actual_hash[19] ^= 1; denied_identity(); assert(hashes);
    completed("NULL-and-one-byte-CDHash-mismatch-are-EPERM");

    reset(); bootarg = "A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3";
    w = frame("[qbt-ui] p=1 pid=321 a=1 b=0\n");
    assert(syscall_frame(&w) == 0 && copies == 1 && records == 1 && ring.used);
    assert(hashes == 2 && parses == 2);
    reset(); bootarg = "1111111111111111111111111111111111111111";
    memset(actual_hash, 0x11, sizeof actual_hash);
    w = frame("[qbt-ui] p=1 pid=321 a=1 b=0\n");
    assert(syscall_frame(&w) == 0 && records == 1 && ring.used);
    completed("matching-uppercase-and-all-decimal-40hex-identities");

    const unsigned valid[][2] = {
        {1, 1}, {2, 0}, {2, 1}, {3, 0}, {3, 1}, {4, 1},
        {5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4}, {5, 5}, {5, 7},
        {6, 0}, {6, 1}, {7, 0}, {7, 1},
        {8, 0}, {8, 1}, {8, 2}, {8, 3}, {9, 0}, {9, 1}, {10, 0}, {10, 1}
    };
    char text[160];
    for (size_t i = 0; i < sizeof valid / sizeof *valid; ++i) {
        reset(); snprintf(text, sizeof text, "[qbt-ui] p=%u pid=321 a=%u b=0\n", valid[i][0], valid[i][1]);
        w = frame(text); assert(qcom_ulog_valid(&w, 48 + w.bytes));
        assert(syscall_frame(&w) == 0 && records == 1 && ring.used);
    }
    completed("all-ten-stage-canonical-value-ranges");
    const unsigned invalid_stage[][2] = {
        {0, 1}, {11, 1}, {1, 0}, {1, 2}, {4, 0}, {4, 2},
        {2, 2}, {3, 2}, {6, 2}, {7, 2}, {9, 2}, {10, 2},
        {5, 6}, {5, 8}, {8, 4}
    };
    for (size_t i = 0; i < sizeof invalid_stage / sizeof *invalid_stage; ++i) {
        reset(); snprintf(text, sizeof text, "[qbt-ui] p=%u pid=321 a=%u b=0\n", invalid_stage[i][0], invalid_stage[i][1]);
        w = frame(text); assert(syscall_frame(&w) == EPERM && copies == 1 && !records && !ring.used);
    }
    const char *invalid_text[] = {
        "[qbt-ui] p=1 pid=322 a=1 b=0\n", "[qbt-ui] p=1 pid=321 a=1 b=1\n",
        "[qbt-ui] p=01 pid=321 a=1 b=0\n", "[qbt-ui] p=1 pid=0321 a=1 b=0\n",
        "[qbt-ui] p=1 pid=321 a=01 b=0\n", "[qbt-ui] p=1 pid=321 a=-1 b=0\n",
        "[qbt-ui] p=1 pid=321 a=0x1 b=0\n", "[qbt-ui] p=4294967296 pid=321 a=1 b=0\n",
        "[qbt-ui] p=1 pid=4294967296 a=1 b=0\n", "[qbt-ui] p=1 pid=321 a=4294967296 b=0\n",
        "[qbt-ui] p=1 pid=321 a=1 b=0", "[qbt-ui] p=1 pid=321 a=1 b=0\nextra",
        "other p=1 pid=321 a=1 b=0\n", "[qbt-ui] p=1 pid=321 a=1  b=0\n"
    };
    for (size_t i = 0; i < sizeof invalid_text / sizeof *invalid_text; ++i) {
        reset(); w = frame(invalid_text[i]);
        assert(syscall_frame(&w) == EPERM && copies == 1 && !records && !ring.used);
    }
    completed("reject-other-stage-text-value-pid-or-noncanonical-decimal");

    for (unsigned mode = 0; mode < 6; ++mode) {
        reset(); w = frame("[qbt-ui] p=1 pid=321 a=1 b=0\n");
        switch (mode) {
        case 0: w.flags = SOC_ULOG_GZIP; break;
        case 1: w.sessionHi ^= 1; break;
        case 2: w.sessionLo ^= 1; break;
        case 3: w.offset = 1024; w.sequence = 1; w.total = w.offset + w.bytes; break;
        case 4:
            memset(w.data, 'x', 129); w.bytes = w.total = 129;
            w.crc = qcom_ulog_crc(w.data, w.bytes); break;
        case 5: process.pid = 0; break;
        }
        assert(qcom_ulog_valid(&w, 48 + w.bytes));
        assert(syscall_frame(&w) == EPERM && !records && !ring.used);
    }
    completed("reject-nonSTATUS-wrong-session-multiframe-oversize-or-invalid-proc-pid");

    reset(); w = frame("[qbt-ui] p=1 pid=321 a=1 b=0\n"); w.crc ^= 1;
    assert(syscall_frame(&w) == EINVAL && copies == 1 && !records && !ring.used);
    reset(); w = frame("[qbt-ui] p=1 pid=321 a=1 b=0\n"); copy_error = EFAULT;
    assert(syscall_frame(&w) == EFAULT && copies == 1 && !records && !ring.used);
    reset(); w = frame("[qbt-ui] p=1 pid=321 a=1 b=0\n");
    struct request req = { .newptr = &w, .newlen = 48 + w.bytes, .oldptr = &w };
    assert(qcom_usb_userlog_sysctl(0, 0, 0, &req) == EINVAL && !copies && !records);
    req.oldptr = 0; req.newlen = sizeof w + 1;
    assert(qcom_usb_userlog_sysctl(0, 0, 0, &req) == EINVAL && !copies && !records);
    assert(qcom_usb_userlog_submit(0, 48) == EINVAL && !records);
    completed("preserve-CRC-EINVAL-copyin-EFAULT-and-write-only-bounds");

    for (unsigned mode = 0; mode < 3; ++mode) {
        reset(); w = frame("[qbt-ui] p=1 pid=321 a=1 b=0\n");
        if (mode == 0) ring.busy = 1;
        if (mode == 1) ring.used = ring.capacity - 1;
        if (mode == 2) ring.quiesced = 1;
        size_t before = ring.used;
        assert(syscall_frame(&w) == EAGAIN && copies == 1 && records == 1);
        assert(ring.used == before && !ring.accepted && !ring.overflow && !ring.contended);
        for (size_t i = 0; i < sizeof storage; ++i) assert(!storage[i]);
    }
    completed("full-contended-or-quiesced-FIFO-rejects-entire-record-with-EAGAIN");
    printf("HOST_CASES %u\n", host_cases);
    return 0;
}
'''


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='new host evidence directory outside source')
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    args = parser.parse_args()
    component, out = args.component.resolve(), args.output.resolve()
    if out == component or component in out.parents:
        parser.error('host evidence must be outside the selected component source')
    inputs = ('kernel/QcomUsbUserLog.c', 'include/QcomUserLogWire.h', 'kernel/QcomUsbBuffer.h')
    before = {name: digest(component / name) for name in inputs}
    source = (component / inputs[0]).read_text()
    marker = '\nSYSCTL_PROC('
    if source.count(marker) != 1:
        raise ValueError('select exactly one actual sysctl registration declaration')
    # Retain every actual C function, replacing only native header dependencies
    # and excluding the registration macro (qualified by the target checker).
    actual = re.sub(r'^#include[^\n]*\n', '', source.split(marker)[0], flags=re.MULTILINE)
    out.mkdir(parents=True, exist_ok=False)
    generated = out / 'actual-userlog-host.c'
    generated.write_text(PRELUDE + actual + CASES)
    command = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I' + str(component / 'include'), '-I' + str(component / 'kernel'),
               str(generated), '-o', str(out / 'actual-userlog-host')]
    report = dict(passed=False, target_compiled=False, hardware_verified=False,
                  phone_operations=False, native_dependencies='explicit host fixtures',
                  source_files=before, extracted_functions_sha256=hashlib.sha256(actual.encode()).hexdigest(),
                  compile_command=command, sanitizer='address,undefined',
                  wire_header_bytes=48, scalar_bytes_limit=128)
    try:
        with (out / 'compile.log').open('wb') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        with (out / 'tests.log').open('wb') as log:
            subprocess.run([str(out / 'actual-userlog-host')], stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=15)
        after = {name: digest(component / name) for name in inputs}
        if after != before:
            raise ValueError('selected handler or FIFO source changed during host checks')
        names = [line.removeprefix('PASS ') for line in (out / 'tests.log').read_text().splitlines()
                 if line.startswith('PASS ')]
        report.update(passed=True, host_cases=len(names), named_cases=names,
                      selected_source_unchanged=True)
    finally:
        (out / 'host-producer-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
