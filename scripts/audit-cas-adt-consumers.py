#!/usr/bin/env python3
"""Compile fixed XNU/DarwinBoot parser code against generated ADTs on the host.

This verifies wire format and source contracts, not the kernel/IOKit runtime.
Original source notices are retained in the private probe directory.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from adt_codec import u64
from cas_adt import ROOT, audit_sources, audit_tree, build_tree, load_json, load_schema, private_directory, private_write, sha256

SHIM = r"""
#ifndef CAS_ADT_HOST_SHIM
#define CAS_ADT_HOST_SHIM
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
#define SECURITY_READ_ONLY_LATE(type) type
#define CONFIG_SPTM 0
#define __unused __attribute__((unused))
#define Z_WAITOK 0
#define kalloc_type(type, flags) ((type *)calloc(1, sizeof(type)))
#define kfree_type(type, ptr) free(ptr)
static inline bool cas_add3(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t *out) {
  uintptr_t tmp;
  return __builtin_add_overflow(a, b, &tmp) || __builtin_add_overflow(tmp, c, out);
}
#define os_add3_overflow(a,b,c,out) cas_add3((uintptr_t)(a),(uintptr_t)(b),(uintptr_t)(c),(out))
__attribute__((noreturn)) static inline void panic(const char *format, ...) {
  va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args); abort();
}
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef char CHAR8;
typedef bool BOOLEAN;
#define STATIC static
#define CONST const
#define TRUE true
#define FALSE false
#define MAX_UINTN UINTPTR_MAX
#define ScanMem8(base,len,value) memchr((base),(value),(len))
#define CompareMem(a,b,n) memcmp((a),(b),(n))
#define AsciiStrCmp(a,b) strcmp((a),(b))
#define ALIGN_VALUE(x,a) (((x)+(a)-1)&~((a)-1))
static inline UINT32 U32(const UINT8 *p) { UINT32 v; memcpy(&v,p,4); return v; }
static inline UINT64 U64(const UINT8 *p) { UINT64 v; memcpy(&v,p,8); return v; }
typedef int IOReturn;
#define kIOReturnError (-1)
#define kIOReturnSuccess 0
#define kNVRAMVersion1 1
#define kNVRAMVersion2 2
#define NVRAM_CHRP_APPLE_HEADER_NAME_V1 "nvram"
#define NVRAM_CHRP_APPLE_HEADER_NAME_V2 "2nvram"
#define nullptr NULL
#define require(condition,label) do { if (!(condition)) goto label; } while (0)
#define DEBUG_INFO(...) ((void)0)
#endif
"""


def section(text, start, stop):
    begin = text.index(start)
    return text[begin:text.index(stop, begin)]


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", type=Path, required=True)
    parser.add_argument("--xnu", type=Path, required=True, help="Fixed source/patch preparation; read only")
    parser.add_argument("--evidence-root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if sys.byteorder != "little" or struct.calcsize("P") != 8:
        raise ValueError("consumer probe requires a little-endian 64-bit host")
    try:
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    except ImportError:
        pass
    schema, facts = load_schema(), load_json(ROOT / "config/cas-adt-facts.json")
    evidence = audit_sources(facts, evidence_root=args.evidence_root)
    expected = schema["consumers"]
    if git(args.framework, "rev-parse", "HEAD") != expected["framework_commit"]:
        raise ValueError("framework commit mismatch")
    for name, commit in expected["module_commits"].items():
        path = args.framework / "drivers" / name
        if git(path, "rev-parse", "HEAD") != commit or git(path, "status", "--porcelain=v1"):
            raise ValueError("consumer module changed")
    inspected = []
    for item in expected["files"]:
        root = args.framework if item["repository"] == "framework" else args.xnu
        actual = root / item["path"]
        if sha256(actual) != item["sha256"]:
            raise ValueError("consumer source hash mismatch: " + item["path"])
        inspected.append(item)
    compiler = shutil.which("clang-18") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise ValueError("host C compiler is required")
    private_directory(args.out)
    include = args.out / "include"
    include.mkdir()
    fake_headers = ["pexpert/protos.h", "pexpert/boot.h", "mach/mach_types.h", "mach/machine/vm_types.h",
                    "mach/vm_types.h", "kern/debug.h", "kern/kern_types.h", "kern/kalloc.h",
                    "libkern/kernel_mach_header.h", "os/overflow.h", "sys/appleapiopts.h"]
    for name in fake_headers:
        path = include / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("/* Host-only dependency shim; no hardware or kernel behavior. */\n")
    (include / "host_shim.h").write_text(SHIM)
    shutil.copyfile(args.xnu / "pexpert/pexpert/device_tree.h", include / "pexpert/device_tree.h")
    shutil.copyfile(args.xnu / "pexpert/gen/device_tree.c", args.out / "device_tree.c")
    darwin = (args.framework / "drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c").read_text()
    driver = args.framework / "drivers/DarwinBoot/DarwinBootPkg/Driver"
    shutil.copyfile(driver / "WindowSize.h", include / "WindowSize.h")
    adt = section(darwin, "STATIC\nBOOLEAN\nAdtNode (", "STATIC\nBOOLEAN\nValidTrustCaches (")
    (include / "darwin_adt.h").write_text("// SPDX-License-Identifier: BSD-2-Clause-Patent\n#include \"WindowSize.h\"\n" + adt)
    nvram = (args.xnu / "iokit/Kernel/IONVRAMCHRPHandler.cpp").read_text()
    notice = nvram[:nvram.index("#include")]
    types = section(nvram, "typedef struct chrp_nvram_header", "typedef struct {\n\tNVRAMPartitionType")
    helpers = section(nvram, "static const char *\nget_bank_version_string", "static void\nnvram_set_apple_header")
    (include / "nvram_helpers.h").write_text(notice + types + helpers)
    harness = ROOT / "tests/fixtures/adt_consumer.c"
    command = [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
               "-D__APPLE_API_PRIVATE=1", "-I", str(include), "-include", str(include / "host_shim.h"),
               str(args.out / "device_tree.c"), str(harness), "-o", str(args.out / "consumer-probe")]
    compiled = subprocess.run(command, text=True, capture_output=True)
    (args.out / "compile.log").write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise ValueError("actual parser host compilation failed; see compile.log")
    gic, timer = facts["gic"], facts["timer"]
    results = []
    def execute(name, blob, cpus, expected_pass):
        path = args.out / (name + ".adt")
        private_write(path, blob)
        command = [str(args.out / "consumer-probe"), str(path), str(cpus), str(gic["distributor"]["base"]),
                   str(gic["redistributor"]["base"]), str(timer["frequency_hz"]), str(gic["redistributor_stride"])]
        run = subprocess.run(command, text=True, capture_output=True)
        passed = (run.returncode == 0) == expected_pass
        result = {"case": name, "expected": "accept" if expected_pass else "reject", "passed": passed,
                  "returncode": run.returncode, "stdout": run.stdout, "stderr": run.stderr,
                  "adt_sha256": hashlib.sha256(blob).hexdigest()}
        results.append(result)
    for profile, mode in [("cpu0", "fixture"), ("all", "fixture"), ("cpu0", "private")]:
        tree, _ = build_tree(facts, cpu_profile=profile, mode=mode)
        audit_tree(tree, facts, schema, cpu_profile=profile, mode=mode)
        execute(profile + "-" + mode, tree.encode(), 8 if profile == "all" else 1, True)
    tree, _ = build_tree(facts)
    execute("truncated", tree.encode()[:-1], 1, False)
    tree.at("/cpus/cpu0").properties["reg"] = u64(0)
    execute("wrong-cpu-reg-width", tree.encode(), 1, False)
    tree, _ = build_tree(facts)
    tree.at("/arm-io/intc").properties["reg"] = bytes(32)
    execute("wrong-gic-translation", tree.encode(), 1, False)
    tree, _ = build_tree(facts)
    image = bytearray(tree.at("/chosen").properties["nvram-proxy-data"])
    image[1] ^= 1
    tree.at("/chosen").properties["nvram-proxy-data"] = bytes(image)
    execute("wrong-nvram-checksum", tree.encode(), 1, False)
    report = {"result": "pass" if all(case["passed"] for case in results) else "fail",
              "observed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
              "scope": "Real XNU SecureDT parser and CHRP helpers; real DarwinBoot AdtNode. Host shims replace kernel/UEFI dependencies, not parser statements. ArmCpu/ArmDTPlatform contracts checked through hashes and their documented reads; full drivers are not executed.",
              "source_audit": evidence, "consumers": inspected,
              "extracted_hashes": {name: sha256(include / name) for name in ["darwin_adt.h", "nvram_helpers.h", "host_shim.h"]},
              "compiler": {"path": compiler, "sha256": sha256(Path(compiler).resolve()),
                           "version": subprocess.check_output([compiler, "--version"], text=True)},
              "compile_command": command, "compile_returncode": compiled.returncode,
              "harness_sha256": sha256(harness), "cases": results, "runtime": "not_run"}
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"result": report["result"], "cases": len(results), "report": str(args.out / "report.json")}))
    return 0 if report["result"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
