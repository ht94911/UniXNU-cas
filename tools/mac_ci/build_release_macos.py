#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phase 6 Final Release Mac CI builder & zero-Turnip build-graph auditor.

Builds 100% Vulkan-free, Turnip-free, zero-fallback arm64e-apple-ios27.0
deliverables from pinned public sources across Phase 1-6:
  1. NativeMetalTranslate (pinned UniXNU/metal2vulkan translator staticlib, PAC-wrapped)
  2. adreno_compile       (pinned Mesa 85fda3b36405 non-Vulkan ir3+fdl + W09/W10 + IO info)
  3. Adreno840Emit        (A840 chip facts + full Phase 1-5 PM4 emitters, 0 CP_PROTECT violations)
  4. Adreno840Metal.bundle (production zero-fallback Metal plugin, arm64e 4 KiB signed)
  5. NezhaShaderCompiler   (arm64e shader compiler daemon, 4 KiB signed)
  6. NezhaGpuTest          (arm64e standalone GPU verification probe + Metal 3.0 metallibs)

Supports --audit-graph-only --repo <repo> [--out-json <path>] on Linux hosts
without Xcode to validate the full build DAG, source presence, and zero-Turnip /
zero-fallback invariants offline.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import plistlib
import re
import shlex
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path
from typing import Any

sys.dont_write_bytecode = True

MESA_COMMIT = "85fda3b36405bcaea8bffc64aa4f2651dc90770d"
MESA_TREE = "b72ff1eb2138026ed6aba524770899faabf7d7c8"
M2V_PUBLIC_COMMIT = "87a869f7fc4b73265d4812c40d8cccb6a83c7b17"
RUST_TOOLCHAIN = os.environ.get("RUST_TOOLCHAIN", "1.98.0")

REQUIRED_TARGETS: tuple[str, ...] = (
    "NativeMetalTranslate",
    "adreno_compile",
    "Adreno840Emit",
    "Adreno840Metal.bundle",
    "NezhaShaderCompiler",
    "NezhaGpuTest",
)

FORBIDDEN_TARGETS: tuple[str, ...] = (
    "libvulkan_freedreno",
    "turnip-darwin-kmd.patch",
    "metal2vulkan-runtime",
    "NativeBridgeMetalFrontend",
    "src/freedreno/vulkan",
)

FORBIDDEN_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("LIBVULKAN_FREEDRENO", re.compile(r"libvulkan_freedreno", re.IGNORECASE)),
    ("TURNIP_DARWIN_KMD_PATCH", re.compile(r"turnip-darwin-kmd\.patch", re.IGNORECASE)),
    ("METAL2VULKAN_RUNTIME", re.compile(r"metal2vulkan[-_]runtime", re.IGNORECASE)),
    ("NATIVE_BRIDGE_METAL_FRONTEND", re.compile(r"NativeBridgeMetalFrontend", re.IGNORECASE)),
    ("SRC_FREEDRENO_VULKAN", re.compile(r"src/freedreno/vulkan(?:/|$)", re.IGNORECASE)),
    ("VK_CREATE_DEVICE", re.compile(r"\bvkCreateDevice\b")),
    ("VK_CREATE_GRAPHICS_PIPELINES", re.compile(r"\bvkCreateGraphicsPipelines\b")),
)

PRODUCTION_FALLBACK_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("DUALPATH_ROUTER_IN_PROD", re.compile(r"a840_dualpath_router", re.IGNORECASE)),
    ("NATIVE_BRIDGE_API", re.compile(r"native_bridge_metal_", re.IGNORECASE)),
    ("NB_METAL_SYMBOL", re.compile(r"\bNBMetal\w*")),
)

MESA_PATCHES: tuple[str, ...] = (
    "ci/patches/mesa/ir3-darwin-build-id.patch",
    "ci/patches/mesa/freedreno-tools-nir.patch",
)

MESA_NON_VULKAN_OPTIONS: tuple[str, ...] = (
    "--backend=ninja", "--wrap-mode=nodownload",
    "-Dplatforms=[]", "-Dvulkan-drivers=[]", "-Dgallium-drivers=[]",
    "-Dtools=freedreno", "-Dopengl=false", "-Dglx=disabled", "-Degl=disabled",
    "-Dgbm=disabled", "-Dgles1=disabled", "-Dgles2=disabled", "-Dllvm=disabled",
    "-Dshared-glapi=disabled", "-Dxmlconfig=disabled", "-Dshader-cache=disabled",
    "-Dspirv-tools=disabled", "-Dbuild-tests=false", "-Dvideo-codecs=",
    "-Dlibunwind=disabled", "-Dlmsensors=disabled", "-Dvalgrind=disabled",
    "-Dexpat=disabled", "-Dzstd=disabled", "-Dbuildtype=release", "-Db_ndebug=true",
)

MESA_CORE_ARCHIVES: tuple[str, ...] = (
    "src/compiler/nir/libnir.a",
    "src/compiler/spirv/libvtn.a",
    "src/freedreno/ir3/libfreedreno_ir3.a",
    "src/freedreno/fdl/libfreedreno_layout.a",
    "src/freedreno/common/libfreedreno_common.a",
    "src/compiler/libcompiler.a",
    "src/freedreno/isa/libir3encode.a",
    "src/freedreno/isa/libir3decode.a",
    "src/compiler/isaspec/libisaspec.a",
    "src/util/libmesa_util.a",
)

MESA_OPTIONAL_SUBLIBS: tuple[str, ...] = (
    "src/util/libmesa_util_clflush.a",
    "src/util/libmesa_util_clflushopt.a",
    "src/util/libmesa_util_simd.a",
    "src/util/blake3/libblake3.a",
    "src/c11/impl/libmesa_util_c11.a",
)

PAC_RENAMES: dict[str, str] = {
    "_pthread_create": "_nmb_pac_pthread_create",
    "__tlv_atexit": "_nmb_pac_tlv_atexit",
    "_atexit": "_nmb_pac_atexit",
    "__dyld_register_func_for_add_image": "_nmb_pac_dyld_register_func_for_add_image",
    "__Unwind_Backtrace": "_nmb_pac_Unwind_Backtrace",
}

INCLUDE_DIRS: tuple[str, ...] = (
    "contracts", "kits/Adreno840", "kits/Adreno840/gen",
    "kits/Adreno840Emit", "kits/Adreno840Emit/gen", "translate/include",
    "adreno_compile/ir3", "adreno_compile/layout", "adreno_compile/cache",
    "adreno_compile/pack", "adreno_compile/service",
    "drivers/Adreno840Metal/core", "drivers/Adreno840Metal/transport",
    "drivers/Adreno840Metal/objc", "drivers/Adreno840Kernel",
    "apps/NezhaGpuTest/ref", "apps/NezhaGpuTest/perf",
)

KMD_CHECK_C_SOURCES: tuple[str, ...] = (
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
)

KMD_SIBLING_W11_SOURCES: tuple[str, ...] = (
    "drivers/Adreno840Kernel/a840_kmd_v3_freeze.c",
)

TARGET_GRAPH: dict[str, dict[str, Any]] = {
    "NativeMetalTranslate": {
        "kind": "rust_staticlib",
        "deps": (),
        "zero_fallback": True,
        "sources": (
            "translate/Cargo.toml",
            "translate/include/NativeMetalTranslate.h",
            "translate/src/lib.rs",
            "translate/src/a840_profile.rs",
        ),
        "sibling_w11_sources": (),
        "link_flags": (),
    },
    "adreno_compile": {
        "kind": "c_staticlib",
        "deps": (),
        "zero_fallback": True,
        "ir3_backend_sources": (
            "adreno_compile/ir3/a840_fdl_layout.c",
            "adreno_compile/ir3/a840_spirv_lower.c",
            "adreno_compile/ir3/adreno_compile_ir3.c",
            "adreno_compile/ir3/a840_ir3_stats.c",
            "adreno_compile/ir3/a840_robustness_policy.c",
            "adreno_compile/ir3/a840_metal_opt_pass.c",
        ),
        "sources": (
            "adreno_compile/ir3/a840_ir3_io_info.h",
            "adreno_compile/ir3/a840_fdl_layout.c",
            "adreno_compile/ir3/a840_spirv_lower.c",
            "adreno_compile/ir3/adreno_compile_ir3.c",
            "adreno_compile/ir3/a840_ir3_stats.c",
            "adreno_compile/ir3/a840_robustness_policy.c",
            "adreno_compile/ir3/a840_metal_opt_pass.c",
            "adreno_compile/layout/a840_binding_plan.c",
            "adreno_compile/cache/a840_record_io.c",
            "adreno_compile/pack/a840_pack_v1.c",
            "adreno_compile/pack/a840_pack_sds_preencode.c",
            "adreno_compile/service/a840_shader_compiler_service.c",
            "adreno_compile/service/a840_shader_compiler_client.c",
            "adreno_compile/service/a840_shader_compiler_ir3_backend.c",
            "adreno_compile/service/a840_system_cache_store.c",
            "adreno_compile/pack/a840_system_pack_builder.c",
        ),
        "sibling_w11_sources": (),
        "link_flags": (),
    },
    "Adreno840Emit": {
        "kind": "c_staticlib",
        "deps": (),
        "zero_fallback": True,
        "sources": (
            "kits/Adreno840/chip_facts_a840.c",
            "kits/Adreno840/a840_context_init.c",
            "kits/Adreno840/a840_abi_v3_host.c",
            "kits/Adreno840Emit/a840_cs.c",
            "kits/Adreno840Emit/a840_descriptors.c",
            "kits/Adreno840Emit/a840_emit_blit.c",
            "kits/Adreno840Emit/a840_emit_sync.c",
            "kits/Adreno840Emit/a840_emit_compute.c",
            "kits/Adreno840Emit/a840_emit_3d.c",
            "kits/Adreno840Emit/a840_emit_gmem.c",
            "kits/Adreno840Emit/a840_gmem_planner.c",
            "kits/Adreno840Emit/a840_emit_indirect.c",
            "kits/Adreno840Emit/a840_emit_msaa_tess_vis.c",
            "kits/Adreno840Emit/a840_sync_tracker.c",
        ),
        "sibling_w11_sources": (),
        "link_flags": (),
    },
    "Adreno840Metal.bundle": {
        "kind": "ios_bundle",
        "deps": ("NativeMetalTranslate", "adreno_compile", "Adreno840Emit"),
        "zero_fallback": True,
        "sources": (
            "drivers/Adreno840Metal/core/a840_runtime_device.c",
            "drivers/Adreno840Metal/core/a840_runtime_cmd.c",
            "drivers/Adreno840Metal/core/a840_ubwc_texture.c",
            "drivers/Adreno840Metal/core/a840_render_pass_gmem.c",
            "drivers/Adreno840Metal/core/a840_pack_cache_rt.c",
            "drivers/Adreno840Metal/core/a840_ca_render_bridge.c",
            "drivers/Adreno840Metal/core/a840_heap_allocator.c",
            "drivers/Adreno840Metal/core/a840_frame_arena.c",
            "drivers/Adreno840Metal/core/a840_argbuf_icb.c",
            "drivers/Adreno840Metal/core/a840_phase4_pipeline.c",
            "drivers/Adreno840Metal/core/a840_cb_chunker.c",
            "drivers/Adreno840Metal/core/a840_early_promise_sched.c",
            "drivers/Adreno840Metal/transport/a840_iokit_wire.c",
            "drivers/Adreno840Metal/transport/a840_hwtest_hook.c",
            "drivers/Adreno840Metal/transport/a840_iokit_transport.mm",
            "drivers/Adreno840Metal/objc/A840MTLDevice.mm",
            "drivers/Adreno840Metal/objc/A840MTLResource.mm",
            "drivers/Adreno840Metal/objc/A840MTLCommand.mm",
            "drivers/Adreno840Metal/objc/A840MTLEncoders.mm",
            "drivers/Adreno840Metal/core/a840_system_frontend_core.c",
            "drivers/Adreno840Metal/objc/A840MTLSystemSPI.mm",
            "drivers/Adreno840Metal/objc/A840MTLHeapAndEvents.mm",
            "drivers/Adreno840Metal/objc/A840MTLLibraryAndArchive.mm",
            "drivers/Adreno840Metal/objc/A840MTLScanoutBridge.mm",
        ),
        "sibling_w11_sources": (
            "drivers/Adreno840Metal/core/a840_production_router.c",
            "drivers/Adreno840Metal/objc/A840MTLBundleEntry.mm",
            "drivers/Adreno840Metal/objc/A840MTLPipelineProvider.mm",
            "drivers/Adreno840Metal/transport/A840RustPacBoundary.c",
            "drivers/Adreno840Metal/transport/a840_gfx_bind_sds.c",
        ),
        "link_flags": (
            "-bundle", "-Wl,-dead_strip", "-lobjc", "-lz",
            "-framework", "Foundation", "-framework", "Metal", "-framework", "IOSurface",
        ),
    },
    "NezhaShaderCompiler": {
        "kind": "ios_daemon",
        "deps": ("NativeMetalTranslate", "adreno_compile"),
        "zero_fallback": True,
        "sources": (
            "daemon/NezhaShaderCompiler/main.c",
        ),
        "sibling_w11_sources": (
            "drivers/Adreno840Metal/transport/A840RustPacBoundary.c",
        ),
        "link_flags": ("-Wl,-dead_strip", "-lz", "-lpthread"),
    },
    "NezhaGpuTest": {
        "kind": "ios_probe",
        "deps": ("NativeMetalTranslate", "adreno_compile", "Adreno840Emit"),
        "zero_fallback": True,
        "sources": (
            "apps/NezhaGpuTest/ref/cpu_reference.cpp",
            "apps/NezhaGpuTest/ref/cpu_reference_phase3.cpp",
            "apps/NezhaGpuTest/ref/cpu_reference_ca_glass.cpp",
            "apps/NezhaGpuTest/ref/cpu_reference_phase4.cpp",
            "apps/NezhaGpuTest/ref/cpu_reference_phase4_ext.cpp",
            "apps/NezhaGpuTest/perf/a840_frame_weights.c",
            "apps/NezhaGpuTest/ios/NezhaGpuTestRunner.mm",
            "apps/NezhaGpuTest/ios/NezhaGpuTestPhase3Runner.mm",
            "apps/NezhaGpuTest/ios/main.mm",
            "apps/NezhaGpuTest/shaders/NezhaGpuTest.metal",
            "apps/NezhaGpuTest/shaders/NezhaGpuTestPhase3.metal",
            "apps/NezhaGpuTest/shaders/NezhaGpuTestPhase4.metal",
        ),
        "sibling_w11_sources": (
            "drivers/Adreno840Metal/core/a840_production_router.c",
            "drivers/Adreno840Metal/objc/A840MTLPipelineProvider.mm",
            "drivers/Adreno840Metal/transport/A840RustPacBoundary.c",
            "drivers/Adreno840Metal/transport/a840_gfx_bind_sds.c",
        ),
        "link_flags": (
            "-Wl,-dead_strip", "-lobjc", "-lz",
            "-framework", "Foundation", "-framework", "Metal", "-framework", "IOSurface",
        ),
    },
}

_C_COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


class BuildGraphAuditError(ValueError):
    """Raised when an output path or build-graph contract is violated."""


def validate_output_path(path: str | Path, label: str = "output path") -> Path:
    raw = str(path).strip()
    if not raw:
        raise BuildGraphAuditError(f"EMPTY_OUTPUT_PATH: {label}")
    norm_raw = raw.replace("\\", "/")
    if re.match(r"^[A-Za-z]:", norm_raw):
        raise BuildGraphAuditError(f"FORBIDDEN_WINDOWS_DRIVE_PATH ({label}): {raw}")
    lower_raw = norm_raw.lower()
    if lower_raw == "/mnt/c" or lower_raw.startswith("/mnt/c/"):
        raise BuildGraphAuditError(f"FORBIDDEN_C_DRIVE_PATH ({label}): {raw}")
    if (lower_raw == "/tmp" or lower_raw.startswith("/tmp/")
            or lower_raw == "/private/tmp" or lower_raw.startswith("/private/tmp/")):
        raise BuildGraphAuditError(f"FORBIDDEN_TMP_PATH ({label}): {raw}")
    resolved = Path(path).expanduser().resolve()
    res_lower = resolved.as_posix().lower()
    if res_lower == "/mnt/c" or res_lower.startswith("/mnt/c/"):
        raise BuildGraphAuditError(f"FORBIDDEN_C_DRIVE_RESOLVED ({label}): {resolved}")
    if (res_lower == "/tmp" or res_lower.startswith("/tmp/")
            or res_lower == "/private/tmp" or res_lower.startswith("/private/tmp/")):
        raise BuildGraphAuditError(f"FORBIDDEN_TMP_RESOLVED ({label}): {resolved}")
    return resolved


def _strip_comments(text: str) -> str:
    return _C_COMMENT_RE.sub(" ", text)


def _scan_text_for_rules(
    value: str,
    location: str,
    rules: tuple[tuple[str, re.Pattern[str]], ...],
    violations: list[dict[str, str]],
) -> None:
    normalized = value.replace("\\", "/")
    for rule_name, pattern in rules:
        m = pattern.search(normalized)
        if m is not None:
            violations.append({
                "rule": rule_name,
                "location": location,
                "match": m.group(0),
                "value": value[:240],
            })


def _check_dag_cycles(graph: dict[str, dict[str, Any]], violations: list[dict[str, str]]) -> None:
    color: dict[str, int] = {}

    def dfs(node: str, stack: list[str]) -> None:
        color[node] = 1
        for dep in graph.get(node, {}).get("deps", ()):
            if dep not in graph:
                violations.append({
                    "rule": "UNKNOWN_TARGET_DEP",
                    "location": f"target:{node}/deps",
                    "match": str(dep),
                    "value": f"{node} -> {dep}",
                })
                continue
            state = color.get(dep, 0)
            if state == 1:
                violations.append({
                    "rule": "CYCLIC_TARGET_DEP",
                    "location": f"target:{node}/deps",
                    "match": dep,
                    "value": " -> ".join(stack + [dep]),
                })
            elif state == 0:
                dfs(dep, stack + [dep])
        color[node] = 2

    for name in sorted(graph):
        if color.get(name, 0) == 0:
            dfs(name, [name])


def audit_build_graph(
    repo: str | Path,
    *,
    target_graph: dict[str, dict[str, Any]] | None = None,
    mesa_options: tuple[str, ...] | list[str] = MESA_NON_VULKAN_OPTIONS,
    mesa_patches: tuple[str, ...] | list[str] = MESA_PATCHES,
    extra_flags: tuple[str, ...] | list[str] = (),
    strict_merged: bool = False,
) -> dict[str, Any]:
    """Audit the Phase 6 release build graph offline (zero Xcode required)."""
    repo_path = Path(repo).resolve()
    graph = target_graph if target_graph is not None else TARGET_GRAPH
    violations: list[dict[str, str]] = []
    pending_siblings: list[str] = []
    target_summaries: dict[str, dict[str, Any]] = {}
    scanned_sources: set[str] = set()

    if not repo_path.is_dir():
        violations.append({
            "rule": "MISSING_REPO", "location": "repo",
            "match": str(repo_path), "value": str(repo_path),
        })

    present_targets = tuple(sorted(graph.keys()))
    for req in REQUIRED_TARGETS:
        if req not in graph:
            violations.append({
                "rule": "MISSING_REQUIRED_TARGET", "location": "target_graph",
                "match": req, "value": f"missing required target {req}",
            })
    for name in present_targets:
        _scan_text_for_rules(name, f"target:{name}", FORBIDDEN_PATTERNS, violations)
        if name in FORBIDDEN_TARGETS:
            violations.append({
                "rule": "FORBIDDEN_TARGET_DECLARED", "location": f"target:{name}",
                "match": name, "value": name,
            })

    _check_dag_cycles(graph, violations)

    opt_list = list(mesa_options)
    if "-Dvulkan-drivers=[]" not in opt_list:
        violations.append({
            "rule": "MESA_VULKAN_NOT_DISABLED", "location": "mesa_options",
            "match": "-Dvulkan-drivers=[]", "value": " ".join(opt_list),
        })
    for idx, opt in enumerate(opt_list):
        _scan_text_for_rules(opt, f"mesa_options[{idx}]", FORBIDDEN_PATTERNS, violations)
    for idx, patch in enumerate(mesa_patches):
        _scan_text_for_rules(patch, f"mesa_patches[{idx}]", FORBIDDEN_PATTERNS, violations)
    for idx, flag in enumerate(extra_flags):
        _scan_text_for_rules(flag, f"extra_flags[{idx}]", FORBIDDEN_PATTERNS, violations)
        _scan_text_for_rules(flag, f"extra_flags[{idx}]", PRODUCTION_FALLBACK_PATTERNS, violations)

    for name in present_targets:
        spec = graph[name]
        deps = tuple(spec.get("deps", ()))
        sources = tuple(spec.get("sources", ()))
        siblings = tuple(spec.get("sibling_w11_sources", ()))
        flags = tuple(spec.get("link_flags", ()))

        for dep in deps:
            _scan_text_for_rules(str(dep), f"target:{name}/deps", FORBIDDEN_PATTERNS, violations)
        for flag in flags:
            _scan_text_for_rules(str(flag), f"target:{name}/link_flags", FORBIDDEN_PATTERNS, violations)
            _scan_text_for_rules(str(flag), f"target:{name}/link_flags", PRODUCTION_FALLBACK_PATTERNS, violations)

        active_sources: list[str] = []
        missing_sources: list[str] = []
        target_pending: list[str] = []

        for rel in sources:
            _scan_text_for_rules(rel, f"target:{name}/sources", FORBIDDEN_PATTERNS, violations)
            if spec.get("zero_fallback", True):
                _scan_text_for_rules(rel, f"target:{name}/sources", PRODUCTION_FALLBACK_PATTERNS, violations)
            full = repo_path / rel
            if full.is_symlink() or not full.is_file():
                missing_sources.append(rel)
                violations.append({
                    "rule": "MISSING_SOURCE_FILE", "location": f"target:{name}/sources",
                    "match": rel, "value": str(full),
                })
            else:
                active_sources.append(rel)
                scanned_sources.add(rel)

        for rel in siblings:
            _scan_text_for_rules(rel, f"target:{name}/sibling_w11_sources", FORBIDDEN_PATTERNS, violations)
            if spec.get("zero_fallback", True):
                _scan_text_for_rules(rel, f"target:{name}/sibling_w11_sources", PRODUCTION_FALLBACK_PATTERNS, violations)
            full = repo_path / rel
            if full.is_file() and not full.is_symlink():
                active_sources.append(rel)
                scanned_sources.add(rel)
            else:
                target_pending.append(rel)
                if rel not in pending_siblings:
                    pending_siblings.append(rel)
                if strict_merged:
                    violations.append({
                        "rule": "MISSING_SIBLING_W11_SOURCE", "location": f"target:{name}/sibling_w11_sources",
                        "match": rel, "value": str(full),
                    })

        if name in {"Adreno840Metal.bundle", "NezhaShaderCompiler", "NativeMetalTranslate", "adreno_compile"}:
            for rel in active_sources:
                full = repo_path / rel
                try:
                    raw_text = full.read_text(encoding="utf-8")
                except (OSError, UnicodeDecodeError) as exc:
                    violations.append({
                        "rule": "UNREADABLE_SOURCE", "location": f"source:{rel}",
                        "match": rel, "value": str(exc),
                    })
                    continue
                code_only = _strip_comments(raw_text)
                _scan_text_for_rules(code_only, f"source:{rel}", FORBIDDEN_PATTERNS, violations)
                if name in {"Adreno840Metal.bundle", "NezhaShaderCompiler"}:
                    _scan_text_for_rules(code_only, f"source:{rel}", PRODUCTION_FALLBACK_PATTERNS, violations)

        target_summaries[name] = {
            "kind": spec.get("kind", "unknown"),
            "deps": list(deps),
            "zero_fallback": bool(spec.get("zero_fallback", True)),
            "active_source_count": len(active_sources),
            "missing_sources": missing_sources,
            "pending_sibling_sources": target_pending,
        }

    for tool_rel in (
        "tools/mac_ci/rust_pac_rewrite.py",
        "tools/verify/no_fallback_test.py",
        "drivers/Adreno840Kernel/xnu/NativeAdrenoV3Glue.cpp",
        *KMD_CHECK_C_SOURCES,
    ):
        full = repo_path / tool_rel
        if full.is_symlink() or not full.is_file():
            violations.append({
                "rule": "MISSING_REQUIRED_INPUT", "location": "support_files",
                "match": tool_rel, "value": str(full),
            })
        else:
            scanned_sources.add(tool_rel)
    for rel in KMD_SIBLING_W11_SOURCES:
        full = repo_path / rel
        if full.is_file() and not full.is_symlink():
            scanned_sources.add(rel)
        else:
            if rel not in pending_siblings:
                pending_siblings.append(rel)
            if strict_merged:
                violations.append({
                    "rule": "MISSING_SIBLING_W11_SOURCE", "location": "kmd_sibling_sources",
                    "match": rel, "value": str(full),
                })

    cargo_toml = repo_path / "translate/Cargo.toml"
    if cargo_toml.is_file():
        try:
            cargo_doc = tomllib.loads(cargo_toml.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            violations.append({
                "rule": "INVALID_CARGO_TOML", "location": "translate/Cargo.toml",
                "match": "Cargo.toml", "value": str(exc),
            })
        else:
            for section in ("dependencies", "dev-dependencies", "build-dependencies"):
                deps_tbl = cargo_doc.get(section, {})
                if isinstance(deps_tbl, dict):
                    for dep_key, dep_val in deps_tbl.items():
                        _scan_text_for_rules(dep_key, f"translate/Cargo.toml:{section}", FORBIDDEN_PATTERNS, violations)
                        _scan_text_for_rules(str(dep_val), f"translate/Cargo.toml:{section}.{dep_key}", FORBIDDEN_PATTERNS, violations)

    mesa_manifest = repo_path / "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json"
    if mesa_manifest.is_file():
        try:
            mf = json.loads(mesa_manifest.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            violations.append({
                "rule": "INVALID_MESA_MANIFEST", "location": "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json",
                "match": "MESA_85FDA3_IR3_MANIFEST.json", "value": str(exc),
            })
        else:
            excluded = mf.get("excluded_source_prefixes", [])
            if "src/freedreno/vulkan" not in excluded:
                violations.append({
                    "rule": "MESA_MANIFEST_MISSING_VULKAN_EXCLUSION",
                    "location": "adreno_compile/docs/MESA_85FDA3_IR3_MANIFEST.json",
                    "match": "src/freedreno/vulkan", "value": str(excluded),
                })
            for lib_name, lib_meta in sorted(mf.get("libraries", {}).items()):
                _scan_text_for_rules(lib_name, f"mesa_manifest:{lib_name}", FORBIDDEN_PATTERNS, violations)
                for src_rel in lib_meta.get("sources", []):
                    _scan_text_for_rules(src_rel, f"mesa_manifest:{lib_name}.sources", FORBIDDEN_PATTERNS, violations)

    status = "PASS" if not violations else "FAIL"
    return {
        "schema_version": 1,
        "status": status,
        "audit_only": True,
        "device_verified": False,
        "hardware_acceptance": False,
        "target_triple": "arm64e-apple-ios27.0",
        "mesa_commit": MESA_COMMIT,
        "mesa_tree": MESA_TREE,
        "m2v_public_commit": M2V_PUBLIC_COMMIT,
        "required_targets": list(REQUIRED_TARGETS),
        "forbidden_targets": list(FORBIDDEN_TARGETS),
        "zero_turnip_verified": status == "PASS",
        "zero_fallback_verified": status == "PASS",
        "strict_merged": strict_merged,
        "scanned_source_count": len(scanned_sources),
        "pending_sibling_sources": sorted(pending_siblings),
        "targets": target_summaries,
        "violations": violations,
    }


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd: list[str | Path], *, cwd: Path | None = None, env: dict[str, str] | None = None,
        log: Path | None = None) -> str:
    argv = [str(c) for c in cmd]
    print(f"$ {shlex.join(argv)}", flush=True)
    res = subprocess.run(argv, cwd=cwd, env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    if log is not None:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(res.stdout, encoding="utf-8")
    if res.returncode != 0:
        tail = "\n".join(res.stdout.splitlines()[-60:])
        raise RuntimeError(f"exit {res.returncode}: {shlex.join(argv)}\n{tail}")
    return res.stdout


def read_tool(cmd: list[str | Path]) -> str:
    return subprocess.check_output([str(c) for c in cmd], text=True).strip()


def meson_include_flags(repo: Path, mesa_src: Path, mesa_build: Path) -> list[str]:
    flags = [
        "-I", str(repo / "contracts"),
        "-I", str(repo / "kits/Adreno840"),
        "-I", str(repo / "adreno_compile/ir3"),
    ]
    mesa_dirs = (
        mesa_src / "include", mesa_src / "src", mesa_src / "src/compiler",
        mesa_src / "src/compiler/isaspec", mesa_src / "src/compiler/nir",
        mesa_src / "src/compiler/spirv", mesa_src / "src/freedreno",
        mesa_src / "src/freedreno/common", mesa_src / "src/freedreno/ir3",
        mesa_src / "src/freedreno/fdl", mesa_src / "src/freedreno/isa",
        mesa_src / "src/freedreno/registers", mesa_src / "src/util",
        mesa_src / "src/util/format", mesa_build / "src", mesa_build / "src/compiler",
        mesa_build / "src/compiler/nir", mesa_build / "src/compiler/spirv",
        mesa_build / "src/freedreno/common", mesa_build / "src/freedreno/registers",
        mesa_build / "src/freedreno/registers/adreno", mesa_build / "src/freedreno/isa",
        mesa_build / "src/util", mesa_build / "src/util/format",
    )
    for d in mesa_dirs:
        flags.extend(["-isystem", str(d)])
    return flags


def extract_mesa_defines(mesa_build: Path) -> list[str]:
    compdb = json.loads((mesa_build / "compile_commands.json").read_text(encoding="utf-8"))
    for entry in compdb:
        if "freedreno/ir3/" in entry.get("file", "").replace("\\", "/"):
            tokens = shlex.split(entry["command"])
            defs = ["-DA840_BUILD_MESA_IR3_BACKEND=1"]
            for tok in tokens:
                if tok.startswith("-D") and tok not in defs:
                    defs.append(tok)
            return defs
    raise RuntimeError("could not find freedreno/ir3 entry in Mesa compile_commands.json")


def build_mesa_ir3(repo: Path, mesa_src: Path, work: Path, meson_bin: Path, target: str,
                   sdk: str | None, jobs: int) -> tuple[Path, list[Path], Path]:
    build_dir = work / f"mesa-ir3-{target}"
    if build_dir.exists():
        shutil.rmtree(build_dir)
    setup_cmd: list[str | Path] = [meson_bin, "setup", build_dir, mesa_src, *MESA_NON_VULKAN_OPTIONS]
    env = os.environ.copy()
    macos_sdk = read_tool(["xcrun", "--sdk", "macosx", "--show-sdk-path"])
    if target == "ios27":
        assert sdk is not None
        cc = read_tool(["xcrun", "--sdk", "iphoneos", "--find", "clang"])
        cxx = read_tool(["xcrun", "--sdk", "iphoneos", "--find", "clang++"])
        ar = read_tool(["xcrun", "--sdk", "iphoneos", "--find", "ar"])
        strip = read_tool(["xcrun", "--sdk", "iphoneos", "--find", "strip"])
        flags = ["-target", "arm64e-apple-ios27.0", "-isysroot", sdk, "-fstack-protector-strong"]
        cross = work / "mesa-ir3-ios27.cross"
        cross.write_text(
            f"[binaries]\nc = {cc!r}\ncpp = {cxx!r}\nobjc = {cc!r}\nar = {ar!r}\nstrip = {strip!r}\n"
            f"pkg-config = '/usr/bin/false'\n"
            f"[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n"
            f"[properties]\nneeds_exe_wrapper = true\n[built-in options]\n"
            + "".join(f"{k} = {flags!r}\n" for k in ("c_args", "cpp_args", "objc_args",
                                                     "c_link_args", "cpp_link_args", "objc_link_args")),
            encoding="utf-8",
        )
        setup_cmd += ["--cross-file", cross]
        env.update(CC="/usr/bin/clang", CXX="/usr/bin/clang++", SDKROOT=macos_sdk,
                   CFLAGS="", CXXFLAGS="", CPPFLAGS="", LDFLAGS="")
    else:
        cc, cxx, ar = "/usr/bin/clang", "/usr/bin/clang++", "/usr/bin/ar"
        env.update(CC=cc, CXX=cxx, SDKROOT=macos_sdk)

    run(setup_cmd, env=env, log=work / f"logs/meson-setup-{target}.log")

    ninja_text = (build_dir / "build.ninja").read_text(encoding="utf-8", errors="replace")
    if "src/freedreno/vulkan/" in ninja_text or "libvulkan_freedreno" in ninja_text:
        raise RuntimeError("FORBIDDEN Turnip/Vulkan target detected in Meson build.ninja")
    targets = list(MESA_CORE_ARCHIVES) + [s for s in MESA_OPTIONAL_SUBLIBS if s in ninja_text]
    run(["ninja", "-C", build_dir, f"-j{jobs}", *targets], env=env,
        log=work / f"logs/ninja-mesa-{target}.log")

    libs = [build_dir / t for t in targets]
    defs = extract_mesa_defines(build_dir)
    inc = meson_include_flags(repo, mesa_src, build_dir)
    obj_dir = work / f"ir3-obj-{target}"
    obj_dir.mkdir(parents=True, exist_ok=True)
    objs = []
    compile_env = env.copy()
    if target == "ios27":
        compile_env.pop("SDKROOT", None)
    target_flags = ["-target", "arm64e-apple-ios27.0", "-isysroot", sdk, "-fstack-protector-strong"] if target == "ios27" else []
    for rel in TARGET_GRAPH["adreno_compile"]["ir3_backend_sources"]:
        src = repo / rel
        obj = obj_dir / (src.stem + ".o")
        run([cc, *target_flags, "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", *defs, *inc, "-c", src, "-o", obj], env=compile_env)
        objs.append(obj)
    archive = work / f"libadreno_compile_ir3_{target}.a"
    archive.unlink(missing_ok=True)
    run([ar, "rcs", archive, *objs], env=compile_env)
    return archive, libs, build_dir


def build_rust_translate(repo: Path, m2v_dir: Path, work: Path, target: str) -> Path:
    build_src = work / f"translate-src-{target}"
    if build_src.exists():
        shutil.rmtree(build_src)
    shutil.copytree(repo / "translate", build_src)
    cargo_toml = build_src / "Cargo.toml"
    text = cargo_toml.read_text(encoding="utf-8")
    crate_path = (m2v_dir / "crates/metal2vulkan").resolve()
    text = re.sub(
        r"metal2vulkan\s*=\s*\{[^}]*\}",
        f'metal2vulkan = {{ path = {json.dumps(str(crate_path))}, default-features = false }}',
        text,
    )
    if "[workspace]" not in text:
        text = text.rstrip() + "\n\n[workspace]\n"
    cargo_toml.write_text(text, encoding="utf-8")

    host_arch = platform.machine()
    host_triple = "x86_64-apple-darwin" if host_arch == "x86_64" else "aarch64-apple-darwin"
    triple = "arm64e-apple-ios" if target == "ios27" else host_triple
    env = os.environ.copy()
    env.update(
        CARGO_TARGET_DIR=str(work / "cargo-target"),
        MACOSX_DEPLOYMENT_TARGET="14.0",
        IPHONEOS_DEPLOYMENT_TARGET="27.0",
    )
    cargo_cmd: list[str | Path] = ["cargo", f"+{RUST_TOOLCHAIN}"]
    if target == "ios27":
        env["RUSTC_BOOTSTRAP"] = "1"
        cargo_cmd += ["-Z", "build-std=std,panic_unwind"]
    cargo_cmd += ["build", "--release", "--manifest-path", cargo_toml, "--target", triple]
    run(cargo_cmd, cwd=build_src, env=env, log=work / f"logs/cargo-{target}.log")
    lib = work / "cargo-target" / triple / "release/libnative_metal_translate.a"
    if not lib.is_file():
        raise RuntimeError(f"missing Rust staticlib: {lib}")
    return lib


def sign_4k(target: Path, identifier: str, entitlements: Path | None = None) -> str:
    cmd: list[str | Path] = ["codesign", "--force", "--sign", "-", "--pagesize", "4096",
                             "--identifier", identifier]
    if entitlements is not None:
        cmd += ["--entitlements", entitlements]
    cmd.append(target)
    run(cmd)
    run(["codesign", "--verify", "--strict", target])
    detail = subprocess.run(["codesign", "--display", "--verbose=4", str(target)],
                            capture_output=True, text=True, check=True).stderr
    if not re.search(r"^Page size=4096$", detail, re.M):
        raise RuntimeError(f"4 KiB page signature missing on {target}:\n{detail}")
    m = re.search(r"^CDHash=([a-f0-9]{40})$", detail, re.M)
    if not m:
        raise RuntimeError(f"CDHash missing on {target}:\n{detail}")
    return m.group(1)


def _find_llvm_tool(tool_name: str) -> str | None:
    candidates = [
        f"/opt/homebrew/opt/llvm/bin/{tool_name}",
        f"/usr/local/opt/llvm/bin/{tool_name}",
        shutil.which(tool_name) or "",
    ]
    return next((p for p in candidates if p and Path(p).is_file()), None)


def _compile_ios_units(repo: Path, rel_paths: list[str], out_dir: Path,
                       ios_flags: list[str], inc: list[str]) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    objs: list[Path] = []
    seen: set[str] = set()
    for rel in rel_paths:
        if rel in seen:
            continue
        seen.add(rel)
        src = repo / rel
        if src.suffix not in {".c", ".cpp", ".mm"}:
            continue
        safe_stem = rel.replace("/", "__").rsplit(".", 1)[0]
        obj = out_dir / f"{safe_stem}.o"
        extra_defs = ["-DA840_SC_IR3_ENABLE_TRANSLATE=1"] if rel == "adreno_compile/service/a840_shader_compiler_ir3_backend.c" else []
        if src.suffix == ".c":
            run(["xcrun", "clang", *ios_flags, "-std=c11", *extra_defs, *inc, "-c", src, "-o", obj])
        elif src.suffix == ".mm":
            run(["xcrun", "clang++", *ios_flags, "-std=c++17", "-fobjc-arc",
                 "-Wno-deprecated-declarations", *inc, "-x", "objective-c++", "-c", src, "-o", obj])
        else:
            run(["xcrun", "clang++", *ios_flags, "-std=c++17", *inc, "-c", src, "-o", obj])
        objs.append(obj)
    return objs


def run_macos_release_build(repo: Path, mesa: Path, m2v: Path, meson: Path,
                            work: Path, out: Path, jobs: int) -> dict[str, Any]:
    if platform.system() != "Darwin":
        raise SystemExit("Full arm64e build requires macOS with Xcode; use --audit-graph-only on Linux")

    audit = audit_build_graph(repo, strict_merged=True)
    if audit["status"] != "PASS":
        raise RuntimeError(f"Release build graph audit failed: {json.dumps(audit['violations'], indent=2)}")

    (work / "logs").mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)

    if read_tool(["git", "-C", mesa, "rev-parse", "HEAD"]) != MESA_COMMIT:
        raise RuntimeError("Mesa commit mismatch")
    if read_tool(["git", "-C", m2v, "rev-parse", "HEAD"]) != M2V_PUBLIC_COMMIT:
        raise RuntimeError("metal2vulkan public commit mismatch")

    sdk = read_tool(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"])
    inc = [f"-I{repo / d}" for d in INCLUDE_DIRS]
    ios_flags = ["-target", "arm64e-apple-ios27.0", "-isysroot", sdk, "-O2", "-g",
                 "-Wall", "-Wextra", "-Werror", "-fstack-protector-strong"]

    metallibs: dict[str, dict[str, Any]] = {}
    for shader_rel in (
        "apps/NezhaGpuTest/shaders/NezhaGpuTest.metal",
        "apps/NezhaGpuTest/shaders/NezhaGpuTestPhase3.metal",
        "apps/NezhaGpuTest/shaders/NezhaGpuTestPhase4.metal",
    ):
        stem = Path(shader_rel).stem
        air = work / f"{stem}.air"
        mlib = out / f"{stem}.metallib"
        run(["xcrun", "--sdk", "iphoneos", "metal", "-std=metal3.0", "-O2",
             "-c", repo / shader_rel, "-o", air])
        run(["xcrun", "--sdk", "iphoneos", "metallib", air, "-o", mlib])
        metallibs[f"{stem}.metallib"] = {"bytes": mlib.stat().st_size, "sha256": sha256_file(mlib)}

    kmd_dir = work / "kmd-check"
    kmd_dir.mkdir(parents=True, exist_ok=True)
    for rel in (*KMD_CHECK_C_SOURCES, *KMD_SIBLING_W11_SOURCES):
        run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             *inc, "-c", repo / rel, "-o", kmd_dir / (Path(rel).stem + ".o")])
    for cpp_rel in (
        "drivers/Adreno840Kernel/xnu/NativeAdrenoV3Glue.cpp",
        "drivers/Adreno840Kernel/xnu/a840_xnu_system_bridge.cpp",
    ):
        run(["xcrun", "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             *inc, "-c", repo / cpp_rel, "-o", kmd_dir / (Path(cpp_rel).stem + ".o")])

    ios_ir3, ios_mesa_libs, _ = build_mesa_ir3(repo, mesa, work, meson, "ios27", sdk, jobs)
    ios_translate = build_rust_translate(repo, m2v, work, "ios27")

    objcopy = _find_llvm_tool("llvm-objcopy")
    if not objcopy:
        raise RuntimeError("llvm-objcopy required for arm64e Rust PAC symbol redefinition")
    pac_translate = work / "libnative_metal_translate_pac.a"
    run([objcopy, *[f"--redefine-sym={a}={b}" for a, b in PAC_RENAMES.items()], ios_translate, pac_translate])

    llvm_nm = _find_llvm_tool("llvm-nm")
    nofb_env = os.environ.copy()
    if llvm_nm:
        nm_shim = work / "llvm-nm-bin"
        nm_shim.mkdir(parents=True, exist_ok=True)
        nm_link = nm_shim / "nm"
        nm_link.unlink(missing_ok=True)
        nm_link.write_text(
            "#!/bin/sh\n"
            "exec python3 -c \"import os,sys; "
            "os.execv(sys.argv[1], [sys.argv[1]] + [a for a in sys.argv[2:] if a != '--'])\" "
            + shlex.quote(llvm_nm) + ' "$@"\n',
            encoding="utf-8",
        )
        nm_link.chmod(0o755)
        nofb_env["PATH"] = f"{nm_shim}:{nofb_env.get('PATH', '')}"

    ar_ios = read_tool(["xcrun", "--sdk", "iphoneos", "--find", "ar"])
    emit_objs = _compile_ios_units(repo, list(TARGET_GRAPH["Adreno840Emit"]["sources"]),
                                   work / "obj-emit", ios_flags, inc)
    emit_archive = work / "libAdreno840Emit.a"
    emit_archive.unlink(missing_ok=True)
    run([ar_ios, "rcs", emit_archive, *emit_objs])

    ac_non_ir3 = [s for s in TARGET_GRAPH["adreno_compile"]["sources"]
                  if s not in TARGET_GRAPH["adreno_compile"]["ir3_backend_sources"] and s.endswith(".c")]
    ac_objs = _compile_ios_units(repo, ac_non_ir3, work / "obj-adreno-compile", ios_flags, inc)
    ac_archive = work / "libadreno_compile_support.a"
    ac_archive.unlink(missing_ok=True)
    run([ar_ios, "rcs", ac_archive, *ac_objs])

    bundle_sources = list(TARGET_GRAPH["Adreno840Metal.bundle"]["sources"]) + list(
        TARGET_GRAPH["Adreno840Metal.bundle"]["sibling_w11_sources"]
    )
    bundle_objs = _compile_ios_units(repo, bundle_sources, work / "obj-bundle", ios_flags, inc)
    daemon_sources = list(TARGET_GRAPH["NezhaShaderCompiler"]["sources"]) + list(
        TARGET_GRAPH["NezhaShaderCompiler"]["sibling_w11_sources"]
    )
    daemon_objs = _compile_ios_units(repo, daemon_sources, work / "obj-daemon", ios_flags, inc)
    probe_metal_objs = [o for o in bundle_objs if "A840MTLBundleEntry" not in o.name]
    probe_app_objs = _compile_ios_units(
        repo,
        [s for s in TARGET_GRAPH["NezhaGpuTest"]["sources"] if not s.endswith(".metal")],
        work / "obj-probe", ios_flags, inc,
    )

    def _link_and_gate(exe: Path, map_path: Path, flags: tuple[str, ...], objs: list[Path],
                       extra_archives: list[Path], ident: str, entitlements: Path | None = None,
                       sign_target: Path | None = None) -> str:
        run(["xcrun", "clang++", *ios_flags, *flags, *objs, *extra_archives,
             ios_ir3, *ios_mesa_libs, pac_translate, f"-Wl,-map,{map_path}", "-o", exe])
        dsym = Path(f"{exe}.dSYM")
        if dsym.exists():
            dst_dsym = work / f"{exe.name}.dSYM"
            shutil.rmtree(dst_dsym, ignore_errors=True)
            shutil.move(dsym, dst_dsym)
        run(["python3", "-B", repo / "tools/mac_ci/rust_pac_rewrite.py", exe, map_path, pac_translate.name])
        run(["xcrun", "strip", "-x", exe])
        run(["python3", "-B", repo / "tools/verify/no_fallback_test.py", "--profile", "final", "--artifact", exe],
            env=nofb_env)
        return sign_4k(sign_target or exe, ident, entitlements=entitlements)

    bundle = out / "Adreno840Metal.bundle"
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    bundle_exe = bundle / "Adreno840Metal"
    (bundle / "Info.plist").write_bytes(plistlib.dumps({
        "CFBundleIdentifier": "org.nezha.Adreno840Metal", "CFBundleExecutable": "Adreno840Metal",
        "CFBundlePackageType": "BNDL", "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0.0",
        "NSPrincipalClass": "NativeAdrenoMetalDevice", "MinimumOSVersion": "27.0",
    }, fmt=plistlib.FMT_XML))
    bundle_cdhash = _link_and_gate(
        bundle_exe, work / "Adreno840Metal.map", TARGET_GRAPH["Adreno840Metal.bundle"]["link_flags"],
        bundle_objs, [emit_archive, ac_archive], "org.nezha.Adreno840Metal", sign_target=bundle,
    )

    daemon_exe = out / "NezhaShaderCompiler"
    daemon_ent = work / "NezhaShaderCompiler.entitlements"
    daemon_ent.write_bytes(plistlib.dumps({
        "platform-application": True, "com.apple.private.security.no-sandbox": True,
        "com.apple.private.security.no-container": True,
    }, fmt=plistlib.FMT_XML))
    daemon_cdhash = _link_and_gate(
        daemon_exe, work / "NezhaShaderCompiler.map", TARGET_GRAPH["NezhaShaderCompiler"]["link_flags"],
        daemon_objs, [emit_archive, ac_archive], "org.nezha.NezhaShaderCompiler", entitlements=daemon_ent,
    )

    probe_exe = out / "NezhaGpuTest"
    probe_ent = work / "NezhaGpuTest.entitlements"
    probe_ent.write_bytes(plistlib.dumps({
        "platform-application": True, "com.apple.private.security.no-sandbox": True,
        "com.apple.private.security.no-container": True,
        "com.apple.security.iokit-user-client-class": ["AGXDeviceUserClient"],
    }, fmt=plistlib.FMT_XML))
    probe_cdhash = _link_and_gate(
        probe_exe, work / "NezhaGpuTest.map", TARGET_GRAPH["NezhaGpuTest"]["link_flags"],
        [*probe_metal_objs, *probe_app_objs], [emit_archive, ac_archive],
        "org.nezha.NezhaGpuTest", entitlements=probe_ent,
    )

    sys_bundle = out / "NativeAdrenoMetal.bundle"
    if sys_bundle.exists():
        shutil.rmtree(sys_bundle)
    shutil.copytree(bundle, sys_bundle)
    sys_bundle_cdhash = sign_4k(sys_bundle, "org.nezha.Adreno840Metal")
    plist_src = repo / "daemon/NezhaShaderCompiler/org.unixnu.nezha.shader-compiler.plist"
    if plist_src.is_file():
        shutil.copyfile(plist_src, out / plist_src.name)

    summary = {
        "schema_version": 1, "status": "PASS", "device_verified": False, "hardware_acceptance": False,
        "target_triple": "arm64e-apple-ios27.0", "mesa_commit": MESA_COMMIT,
        "m2v_public_commit": M2V_PUBLIC_COMMIT, "required_targets": list(REQUIRED_TARGETS),
        "forbidden_targets": list(FORBIDDEN_TARGETS), "zero_turnip_verified": True,
        "zero_fallback_verified": True, "kmd_v3_appleclang": "PASS",
        "outputs": {
            "Adreno840Metal.bundle": {
                "cdhash": bundle_cdhash, "page_size": 4096,
                "exe_bytes": bundle_exe.stat().st_size, "exe_sha256": sha256_file(bundle_exe),
            },
            "NativeAdrenoMetal.bundle": {
                "cdhash": sys_bundle_cdhash, "page_size": 4096,
                "exe_bytes": (sys_bundle / "Adreno840Metal").stat().st_size,
                "exe_sha256": sha256_file(sys_bundle / "Adreno840Metal"),
                "info_plist_sha256": sha256_file(sys_bundle / "Info.plist"),
                "code_resources_sha256": sha256_file(sys_bundle / "_CodeSignature/CodeResources"),
            },
            "NezhaShaderCompiler": {
                "cdhash": daemon_cdhash, "page_size": 4096,
                "bytes": daemon_exe.stat().st_size, "sha256": sha256_file(daemon_exe),
            },
            "NezhaGpuTest": {
                "cdhash": probe_cdhash, "page_size": 4096,
                "bytes": probe_exe.stat().st_size, "sha256": sha256_file(probe_exe),
            },
            "metallibs": metallibs,
        },
    }
    (out / "release_mac_build_summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--audit-graph-only", action="store_true",
                        help="audit Phase 6 release build graph offline without Xcode")
    parser.add_argument("--strict-merged", action="store_true",
                        help="require all Wave 11 sibling sources (L1/L3) to be present")
    parser.add_argument("--out-json", type=Path, default=None,
                        help="optional JSON output path for --audit-graph-only")
    parser.add_argument("--mesa", type=Path, default=None)
    parser.add_argument("--m2v", type=Path, default=None)
    parser.add_argument("--meson", type=Path, default=None)
    parser.add_argument("--work", type=Path, default=None)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args(argv)

    try:
        if args.audit_graph_only:
            out_json = validate_output_path(args.out_json, "--out-json") if args.out_json is not None else None
            report = audit_build_graph(args.repo, strict_merged=args.strict_merged)
            payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
            if out_json is not None:
                out_json.parent.mkdir(parents=True, exist_ok=True)
                out_json.write_text(payload, encoding="utf-8")
            print(payload, end="")
            return 0 if report["status"] == "PASS" else 1

        if not (args.mesa and args.m2v and args.meson and args.work and args.out):
            raise BuildGraphAuditError("Full build requires --mesa, --m2v, --meson, --work, and --out")
        work = validate_output_path(args.work, "--work")
        out = validate_output_path(args.out, "--out")
        summary = run_macos_release_build(
            args.repo.resolve(), args.mesa.resolve(), args.m2v.resolve(),
            args.meson.resolve(), work, out, args.jobs,
        )
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except (BuildGraphAuditError, RuntimeError, OSError, ValueError) as exc:
        print(json.dumps({
            "status": "FAIL", "device_verified": False,
            "hardware_acceptance": False, "error": str(exc),
        }, indent=2, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
