#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only XNU NativeAdreno KMD v3 integration auditor & patch-plan generator.

Audits:
  1. NativeAdrenoUserClient selector coverage across v2 (1..19) and v3 (20..26)
  2. IOAcceleratorES / Info.plist MetalPluginName & MetalPluginClassName contract
  3. 54-range a840_protect[] CP_PROTECT hardware protection table in NezhaGpu.c
  4. Complete 18 Phase 2-6 a840_kmd_*.c + 2 XNU C++ bridge build manifest
  5. Zero Legacy Turnip / zero Vulkan static dependency policy across KMD v3
  6. Honest real-device qualification boundary (device_verified=false)

Strictly read-only on --repo, --ref-gpu, and --ref-myron. Never touches
integration088, C:/, /mnt/c, or /tmp.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path
import re
from typing import Any


REQUIRED_KMD_C_SOURCES = (
    "drivers/Adreno840Kernel/a840_kmd_zero_page.c",
    "drivers/Adreno840Kernel/a840_kmd_wait_any.c",
    "drivers/Adreno840Kernel/a840_kmd_fault_capture.c",
    "drivers/Adreno840Kernel/a840_kmd_userclient_v3.c",
    "drivers/Adreno840Kernel/a840_kmd_ib_diag.c",
    "drivers/Adreno840Kernel/a840_kmd_ubwc_surface.c",
    "drivers/Adreno840Kernel/a840_kmd_tier0_pack.c",
    "drivers/Adreno840Kernel/a840_kmd_writer_promise.c",
    "drivers/Adreno840Kernel/a840_kmd_display_handoff.c",
    "drivers/Adreno840Kernel/a840_kmd_sparse_va.c",
    "drivers/Adreno840Kernel/a840_kmd_bo_pool.c",
    "drivers/Adreno840Kernel/a840_kmd_quota_recovery.c",
    "drivers/Adreno840Kernel/a840_kmd_ca_env_policy.c",
    "drivers/Adreno840Kernel/a840_kmd_preempt_ring.c",
    "drivers/Adreno840Kernel/a840_kmd_wakeup_latency.c",
    "drivers/Adreno840Kernel/a840_kmd_gmu_dcvs.c",
    "drivers/Adreno840Kernel/a840_kmd_mdss_sync.c",
    "drivers/Adreno840Kernel/a840_kmd_v3_freeze.c",
)

REQUIRED_XNU_CPP_BRIDGES = (
    "drivers/Adreno840Kernel/xnu/NativeAdrenoV3Glue.cpp",
    "drivers/Adreno840Kernel/xnu/a840_xnu_system_bridge.cpp",
)

REQUIRED_XNU_HEADERS = (
    "drivers/Adreno840Kernel/xnu/NativeAdrenoV3Glue.h",
    "drivers/Adreno840Kernel/xnu/a840_xnu_system_bridge.h",
)

EXPECTED_PROTECT_COUNT = 54
EXPECTED_METAL_PLUGIN_NAME = "NativeAdrenoMetal"
EXPECTED_METAL_PLUGIN_CLASS = "NativeAdrenoMetalDevice"
EXPECTED_ACCELERATOR_BASE = "IOAcceleratorES"

FORBIDDEN_DEP_PATTERNS = (
    ("LIBVULKAN_FREEDRENO", re.compile(r"libvulkan_freedreno", re.I)),
    ("METAL2VULKAN_RUNTIME", re.compile(r"metal2vulkan[-_]runtime", re.I)),
    ("FREEDRENO_VULKAN_SRC", re.compile(r"src/freedreno/vulkan(?:/|$)", re.I)),
    ("VK_CREATE_DEVICE", re.compile(r"\bvkCreateDevice\b")),
    ("VK_CREATE_GRAPHICS_PIPELINES", re.compile(r"\bvkCreateGraphicsPipelines\b")),
    ("NATIVE_BRIDGE_FRONTEND", re.compile(r"NativeBridgeMetalFrontend", re.I)),
    ("TURNIP_DARWIN_PATCH", re.compile(r"turnip-darwin-kmd\.patch", re.I)),
)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def check_safe_output_path(out_path: Path) -> Path:
    raw = str(out_path)
    norm = raw.replace("\\", "/")
    if norm.startswith("/mnt/c/") or norm == "/mnt/c":
        raise ValueError(f"forbidden C-drive output path: {out_path}")
    if len(norm) >= 2 and norm[0].lower() == "c" and norm[1] == ":":
        raise ValueError(f"forbidden C-drive output path: {out_path}")
    if norm == "/tmp" or norm.startswith("/tmp/"):
        raise ValueError(f"forbidden /tmp output path: {out_path}")
    resolved = out_path.resolve()
    res_norm = str(resolved).replace("\\", "/")
    if res_norm.startswith("/mnt/c/") or res_norm == "/mnt/c" or res_norm == "/tmp" or res_norm.startswith("/tmp/"):
        raise ValueError(f"resolved output path enters forbidden root: {resolved}")
    if "integration088" in res_norm:
        raise ValueError(f"forbidden integration088 output path: {resolved}")
    return resolved


def check_safe_input_path(path: Path, label: str) -> Path:
    resolved = path.resolve()
    if "integration088" in str(resolved).replace("\\", "/"):
        raise ValueError(f"forbidden integration088 access in {label}: {resolved}")
    if not resolved.is_dir():
        raise ValueError(f"missing {label} directory: {resolved}")
    return resolved


def audit_protect_table(nezha_gpu_c: Path) -> dict[str, Any]:
    text = nezha_gpu_c.read_text(encoding="utf-8", errors="replace")
    m = re.search(
        r"static\s+const\s+uint32_t\s+a840_protect\s*\[\s*\]\s*=\s*\{(.*?)\};",
        text,
        re.DOTALL,
    )
    if not m:
        return {
            "status": "FAIL",
            "error": "a840_protect[] table not found in NezhaGpu.c",
            "range_count": 0,
        }
    body = m.group(1)
    entries = re.findall(
        r"(PROT_(?:RO|NA))\s*\(\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\)",
        body,
    )
    decoded = []
    ro_count = 0
    na_count = 0
    has_0x9e00_range = False
    for idx, (mode, reg_hex, len_hex) in enumerate(entries):
        reg = int(reg_hex, 16)
        length_m1 = int(len_hex, 16)
        end_reg = reg + length_m1
        if mode == "PROT_RO":
            ro_count += 1
        else:
            na_count += 1
        if reg <= 0x09E00 <= end_reg:
            has_0x9e00_range = True
        decoded.append({
            "index": idx,
            "mode": mode,
            "start_reg_hex": f"0x{reg:05x}",
            "length_minus_1_hex": f"0x{length_m1:04x}",
            "end_reg_hex": f"0x{end_reg:05x}",
        })
    passed = (len(decoded) == EXPECTED_PROTECT_COUNT) and has_0x9e00_range
    return {
        "status": "PASS" if passed else "FAIL",
        "source_file": str(nezha_gpu_c),
        "source_sha256": sha256_file(nezha_gpu_c),
        "expected_range_count": EXPECTED_PROTECT_COUNT,
        "actual_range_count": len(decoded),
        "prot_ro_count": ro_count,
        "prot_na_count": na_count,
        "covers_0x9e00_range": has_0x9e00_range,
        "ranges": decoded,
    }


def audit_accelerator_and_plist(ref_gpu: Path, ref_myron: Path) -> dict[str, Any]:
    nezha_cpp = ref_gpu / "NezhaAdrenoGpu.cpp"
    myron_plist = ref_myron / "Info.plist"
    myron_accel = ref_myron / "NativeAdrenoAccelerator.cpp"
    myron_publish = ref_myron / "NativeAdrenoPublish.cpp"

    findings: list[str] = []
    evidence: dict[str, Any] = {}

    for label, p in (
        ("nezha_adreno_gpu_cpp", nezha_cpp),
        ("myron_info_plist", myron_plist),
        ("myron_accelerator_cpp", myron_accel),
        ("myron_publish_cpp", myron_publish),
    ):
        if not p.is_file():
            findings.append(f"missing reference file: {p}")
            evidence[label] = {"present": False, "path": str(p)}
        else:
            evidence[label] = {
                "present": True,
                "path": str(p),
                "sha256": sha256_file(p),
            }

    if findings:
        return {"status": "FAIL", "errors": findings, "evidence": evidence}

    nezha_txt = nezha_cpp.read_text(encoding="utf-8", errors="replace")
    plist_txt = myron_plist.read_text(encoding="utf-8", errors="replace")
    accel_txt = myron_accel.read_text(encoding="utf-8", errors="replace")
    pub_txt = myron_publish.read_text(encoding="utf-8", errors="replace")

    has_plugin_name = f'setProperty("MetalPluginName", "{EXPECTED_METAL_PLUGIN_NAME}")' in nezha_txt
    has_plugin_class = f'setProperty("MetalPluginClassName", "{EXPECTED_METAL_PLUGIN_CLASS}")' in nezha_txt
    has_ioaccel_es = EXPECTED_ACCELERATOR_BASE in nezha_txt and EXPECTED_ACCELERATOR_BASE in accel_txt
    has_plist_provider = "IOProviderClass" in plist_txt and "NativeAdreno" in plist_txt
    has_publish_lookup = '"MetalPluginName"' in pub_txt
    has_hbb_16 = "highest_bank_bit = 16" in nezha_txt

    checks = {
        "nezha_metal_plugin_name_exact": has_plugin_name,
        "nezha_metal_plugin_class_exact": has_plugin_class,
        "ioaccelerator_es_base_verified": has_ioaccel_es,
        "myron_info_plist_present_and_valid": has_plist_provider,
        "myron_publish_metal_plugin_name_hook": has_publish_lookup,
        "nezha_kaanapali_hbb_16_declared": has_hbb_16,
    }
    passed = all(checks.values())
    return {
        "status": "PASS" if passed else "FAIL",
        "expected_metal_plugin_name": EXPECTED_METAL_PLUGIN_NAME,
        "expected_metal_plugin_class": EXPECTED_METAL_PLUGIN_CLASS,
        "expected_accelerator_base": EXPECTED_ACCELERATOR_BASE,
        "checks": checks,
        "evidence": evidence,
    }


def audit_selector_coverage(repo: Path, ref_myron: Path, ref_gpu: Path) -> dict[str, Any]:
    abi_v3 = repo / "contracts/NativeAdrenoABI_v3.h"
    freeze_h = repo / "drivers/Adreno840Kernel/a840_kmd_v3_freeze.h"
    bridge_cpp = repo / "drivers/Adreno840Kernel/xnu/a840_xnu_system_bridge.cpp"
    myron_uc = ref_myron / "NativeAdrenoUserClient.cpp"
    myron_abi = ref_myron / "NativeAdrenoABI.h"
    nezha_cpp = ref_gpu / "NezhaAdrenoGpu.cpp"

    for p in (abi_v3, freeze_h, bridge_cpp, myron_uc, myron_abi, nezha_cpp):
        if not p.is_file():
            return {"status": "FAIL", "error": f"missing file: {p}"}

    abi_v3_txt = abi_v3.read_text(encoding="utf-8", errors="replace")
    freeze_txt = freeze_h.read_text(encoding="utf-8", errors="replace")
    bridge_txt = bridge_cpp.read_text(encoding="utf-8", errors="replace")
    myron_uc_txt = myron_uc.read_text(encoding="utf-8", errors="replace")

    v2_methods_myron = sorted(set(re.findall(r"case\s+(NativeAdrenoMethod\w+)\s*:", myron_uc_txt)))
    v3_methods_contract = {
        name: int(num)
        for name, num in re.findall(r"(NativeAdrenoMethod\w+V3)\s*=\s*(\d+)u?", abi_v3_txt)
    }

    bridge_cases_v2 = sorted(set(re.findall(r"case\s+(A840_KMD_V3_METHOD_\w+)\s*:", bridge_txt)))
    bridge_cases_v3 = sorted(set(re.findall(r"case\s+(NativeAdrenoMethod\w+V3)\s*:", bridge_txt)))

    expected_v2_cases = [
        "A840_KMD_V3_METHOD_QUERY",
        "A840_KMD_V3_METHOD_BO_ALLOC",
        "A840_KMD_V3_METHOD_BO_MAP",
        "A840_KMD_V3_METHOD_BO_FREE",
        "A840_KMD_V3_METHOD_SUBMIT",
        "A840_KMD_V3_METHOD_WAIT_FENCE",
        "A840_KMD_V3_METHOD_GET_TIMESTAMP",
        "A840_KMD_V3_METHOD_GET_STATUS",
        "A840_KMD_V3_METHOD_SET_METADATA",
        "A840_KMD_V3_METHOD_GET_METADATA",
        "A840_KMD_V3_METHOD_CREATE_QUEUE",
        "A840_KMD_V3_METHOD_CLOSE_QUEUE",
        "A840_KMD_V3_METHOD_BO_CPU_MAP",
        "A840_KMD_V3_METHOD_BO_CPU_UNMAP",
        "A840_KMD_V3_METHOD_GET_DEVICE_INFO",
        "A840_KMD_V3_METHOD_BO_IMPORT",
        "A840_KMD_V3_METHOD_SET_PRIORITY",
        "A840_KMD_V3_METHOD_NOTE",
        "A840_KMD_V3_METHOD_SUBMIT_BATCH",
    ]
    expected_v3_cases = [
        "NativeAdrenoMethodWaitAnyV3",
        "NativeAdrenoMethodGetZeroPageV3",
        "NativeAdrenoMethodMapSparseV3",
        "NativeAdrenoMethodMapSharedPackV3",
        "NativeAdrenoMethodWriterPromiseV3",
        "NativeAdrenoMethodSurfaceMetaV3",
        "NativeAdrenoMethodQueueConfigV3",
    ]

    missing_v2 = [m for m in expected_v2_cases if m not in bridge_cases_v2]
    missing_v3 = [m for m in expected_v3_cases if m not in bridge_cases_v3]
    has_freeze_bounds = (
        "A840_KMD_V3_PROD_SELECTOR_MIN  1u" in freeze_txt
        and "A840_KMD_V3_PROD_SELECTOR_MAX  26u" in freeze_txt
    )
    has_zero_legacy_check = "a840_kmd_v3_freeze_policy_check_selector" in bridge_txt

    passed = (
        not missing_v2
        and not missing_v3
        and len(v3_methods_contract) == 7
        and has_freeze_bounds
        and has_zero_legacy_check
        and len(v2_methods_myron) >= 16
    )
    return {
        "status": "PASS" if passed else "FAIL",
        "myron_v2_dispatch_case_count": len(v2_methods_myron),
        "myron_v2_dispatch_cases": v2_methods_myron,
        "contract_v3_methods": v3_methods_contract,
        "bridge_v2_case_count": len(bridge_cases_v2),
        "bridge_v3_case_count": len(bridge_cases_v3),
        "missing_v2_cases": missing_v2,
        "missing_v3_cases": missing_v3,
        "freeze_bounds_1_to_26": has_freeze_bounds,
        "bridge_enforces_selector_whitelist": has_zero_legacy_check,
    }


def audit_build_manifest_and_zero_turnip(repo: Path) -> dict[str, Any]:
    c_manifest: list[dict[str, Any]] = []
    cpp_manifest: list[dict[str, Any]] = []
    hdr_manifest: list[dict[str, Any]] = []
    missing_files: list[str] = []
    policy_violations: list[dict[str, Any]] = []

    def inspect_source(rel: str, target_list: list[dict[str, Any]], check_kernel_guard: bool = False) -> None:
        full = repo / rel
        if not full.is_file():
            missing_files.append(rel)
            return
        text = full.read_text(encoding="utf-8", errors="replace")
        for line_no, line in enumerate(text.splitlines(), 1):
            for rule_name, pat in FORBIDDEN_DEP_PATTERNS:
                m = pat.search(line)
                if m is not None:
                    policy_violations.append({
                        "file": rel,
                        "line": line_no,
                        "rule": rule_name,
                        "match": m.group(0),
                    })
        entry: dict[str, Any] = {
            "path": rel,
            "bytes": full.stat().st_size,
            "sha256": sha256_file(full),
        }
        if check_kernel_guard:
            entry["has_kernel_ifdef"] = ("#ifdef KERNEL" in text) or ("defined(KERNEL)" in text)
        target_list.append(entry)

    for rel in REQUIRED_KMD_C_SOURCES:
        inspect_source(rel, c_manifest, check_kernel_guard=False)
    for rel in REQUIRED_XNU_CPP_BRIDGES:
        inspect_source(rel, cpp_manifest, check_kernel_guard=True)
    for rel in REQUIRED_XNU_HEADERS:
        inspect_source(rel, hdr_manifest, check_kernel_guard=True)

    all_kernel_guarded = all(item.get("has_kernel_ifdef", False) for item in cpp_manifest + hdr_manifest)
    passed = (
        not missing_files
        and len(c_manifest) == 18
        and len(cpp_manifest) == 2
        and len(hdr_manifest) == 2
        and all_kernel_guarded
        and not policy_violations
    )
    return {
        "status": "PASS" if passed else "FAIL",
        "kmd_c_source_count": len(c_manifest),
        "xnu_cpp_bridge_count": len(cpp_manifest),
        "xnu_header_count": len(hdr_manifest),
        "all_xnu_bridges_kernel_guarded": all_kernel_guarded,
        "required_xnu_compile_defines": ["KERNEL", "A840_KMD_CUSTOM_SYNC"],
        "missing_files": missing_files,
        "zero_legacy_dependency_violations": policy_violations,
        "kmd_c_sources": c_manifest,
        "xnu_cpp_bridges": cpp_manifest,
        "xnu_headers": hdr_manifest,
    }


def build_patch_plan(repo: Path, ref_gpu: Path, ref_myron: Path) -> dict[str, Any]:
    repo_r = check_safe_input_path(repo, "repo")
    ref_gpu_r = check_safe_input_path(ref_gpu, "ref-gpu")
    ref_myron_r = check_safe_input_path(ref_myron, "ref-myron")

    protect = audit_protect_table(ref_gpu_r / "NezhaGpu.c")
    accel = audit_accelerator_and_plist(ref_gpu_r, ref_myron_r)
    selectors = audit_selector_coverage(repo_r, ref_myron_r, ref_gpu_r)
    manifest = audit_build_manifest_and_zero_turnip(repo_r)

    all_pass = all(
        sec.get("status") == "PASS"
        for sec in (protect, accel, selectors, manifest)
    )

    return {
        "schema_version": "a840.xnu_kmd_v3_patch_plan.v1",
        "status": "PASS" if all_pass else "FAIL",
        "target_soc": "SM8850 (Kaanapali)",
        "target_gpu": "Adreno 840 v2 (0x44050a31)",
        "abi_freeze_version": 3,
        "host_audit_only": True,
        "device_verified": False,
        "hardware_acceptance": False,
        "read_only_inputs": {
            "repo": str(repo_r),
            "ref_gpu": str(ref_gpu_r),
            "ref_myron": str(ref_myron_r),
        },
        "zero_turnip_policy": {
            "enforced": True,
            "legacy_vulkan_dependencies": 0 if manifest["status"] == "PASS" else len(manifest["zero_legacy_dependency_violations"]),
            "production_selector_range": [1, 26],
        },
        "audits": {
            "selector_coverage": selectors,
            "accelerator_and_plist": accel,
            "cp_protect_table": protect,
            "kmd_v3_build_manifest": manifest,
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--ref-gpu", required=True, type=Path)
    parser.add_argument("--ref-myron", required=True, type=Path)
    parser.add_argument("--out-json", required=True, type=Path)
    args = parser.parse_args(argv)

    try:
        out_path = check_safe_output_path(args.out_json)
        plan = build_patch_plan(args.repo, args.ref_gpu, args.ref_myron)
    except (OSError, ValueError) as exc:
        err_payload = {
            "status": "FAIL",
            "error": str(exc),
            "device_verified": False,
            "hardware_acceptance": False,
        }
        sys.stderr.write(json.dumps(err_payload, indent=2, sort_keys=True) + "\n")
        return 1

    out_path.parent.mkdir(parents=True, exist_ok=True)
    rendered = json.dumps(plan, indent=2, sort_keys=True) + "\n"
    out_path.write_text(rendered, encoding="utf-8")
    sys.stdout.write(json.dumps({
        "status": plan["status"],
        "out_json": str(out_path),
        "kmd_c_source_count": plan["audits"]["kmd_v3_build_manifest"]["kmd_c_source_count"],
        "xnu_cpp_bridge_count": plan["audits"]["kmd_v3_build_manifest"]["xnu_cpp_bridge_count"],
        "cp_protect_ranges": plan["audits"]["cp_protect_table"]["actual_range_count"],
        "device_verified": plan["device_verified"],
        "hardware_acceptance": plan["hardware_acceptance"],
    }, indent=2, sort_keys=True) + "\n")
    return 0 if plan["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
