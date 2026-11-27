#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Comprehensive stdlib unit tests (>=30) for Phase 6 Release Mac CI & Zero-Turnip Build Graph."""

from __future__ import annotations

import copy
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True

THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parents[1]
if str(THIS_DIR) not in sys.path:
    sys.path.insert(0, str(THIS_DIR))

import build_release_macos as brm
import rust_pac_rewrite as rpr
import stage_release_mac_ci as srm

CACHE_BASE = Path("/home/hua/nezha-work/ios27/adreno840-native/.cache/w11-l2/unit-scratch")
SAMPLE_PUBLIC_CERT = (
    b"-----BEGIN CERTIFICATE-----\n"
    b"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAtestpubliccertonly\n"
    b"-----END CERTIFICATE-----\n"
)


def _fresh_dir(name: str) -> Path:
    d = CACHE_BASE / name
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True, exist_ok=True)
    return d


def _build_synthetic_arm64e_macho(
    work: Path,
    *,
    rust_insns: list[int],
    non_rust_insns: list[int] | None = None,
    missing_trampoline: int | None = None,
    include_text_segment: bool = True,
    tramp_offset_bytes: int = 0x200,
    archive_name: str = "libnative_metal_translate_pac.a",
    include_archive_obj: bool = True,
) -> tuple[Path, Path]:
    """Create a valid 64-bit Mach-O binary + ld64 link map for rust_pac_rewrite tests."""
    non_rust = non_rust_insns or [0xD503201F, 0xD63F0000]  # nop; blr x0 (non-Rust, untouched)
    vmaddr = 0x100000000
    text_vm = vmaddr + 0x1000
    rust_vm = text_vm
    non_rust_vm = rust_vm + len(rust_insns) * 4 + 0x10
    tramp_vm = text_vm + tramp_offset_bytes

    file_size = 0x1000 + max(0x800, tramp_offset_bytes + 32 * 4)
    if tramp_offset_bytes > 0x08000000:
        # Out-of-range test: keep file small while setting high trampoline VM address in map.
        file_size = 0x2000
    buf = bytearray(file_size)

    # Mach-O 64-bit header (32 bytes)
    ncmds = 1 if include_text_segment else 0
    cmd_size = 72 if include_text_segment else 0
    struct.pack_into("<IIIIIIII", buf, 0, 0xFEEDFACF, 0x0100000C, 2, 2, ncmds, cmd_size, 0, 0)
    if include_text_segment:
        # LC_SEGMENT_64 for __TEXT (cmd=0x19, cmdsize=72, segname="__TEXT", vmaddr, vmsize, fileoff=0, filesize)
        struct.pack_into("<II", buf, 32, 0x19, 72)
        buf[40:46] = b"__TEXT"
        struct.pack_into("<QQQQ", buf, 56, vmaddr, file_size, 0, file_size)

    # Write Rust instructions and non-Rust instructions at fileoff = vm - vmaddr
    for idx, word in enumerate(rust_insns):
        struct.pack_into("<I", buf, (rust_vm - vmaddr) + idx * 4, word)
    for idx, word in enumerate(non_rust):
        struct.pack_into("<I", buf, (non_rust_vm - vmaddr) + idx * 4, word)

    img = work / "test_arm64e.bin"
    img.write_bytes(buf)

    lines = ["# Object files:"]
    if include_archive_obj:
        lines.append(f"[  1] /work/{archive_name}(rust_unit.o)")
    lines.append("[  2] /work/A840MTLDevice_mm.o")
    lines.append("[  3] /work/A840RustPacBoundary.o")
    lines.append("# Sections:")
    text_size = max(0x400, (non_rust_vm - text_vm) + len(non_rust) * 4 + 0x100)
    lines.append(f"0x{text_vm:016X}\t0x{text_size:08X}\t__TEXT\t__text")
    lines.append("# Symbols:")
    if include_archive_obj:
        lines.append(f"0x{rust_vm:016X}\t0x{len(rust_insns) * 4:08X}\t[  1] _rust_entry")
    lines.append(f"0x{non_rust_vm:016X}\t0x{len(non_rust) * 4:08X}\t[  2] _objc_entry")
    for reg in range(30):
        if missing_trampoline is not None and reg == missing_trampoline:
            continue
        addr = tramp_vm + reg * 4
        lines.append(f"0x{addr:016X}\t0x00000004\t[  3] _nmb_pac_call_x{reg}")
    lines.append("# Dead Stripped Symbols:")
    lines.append("<<dead>>\t0x00000004\t[  1] _dead_fn")

    lmap = work / "test_arm64e.map"
    lmap.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return img, lmap


class TestReleaseBuildGraphAudit(unittest.TestCase):
    """Tests 1-14: build_release_macos.py --audit-graph-only positive & negative fail-closed."""

    def test_01_audit_graph_only_passes_on_real_repo(self) -> None:
        report = brm.audit_build_graph(REPO_ROOT)
        self.assertEqual(report["status"], "PASS", report["violations"])
        self.assertTrue(report["zero_turnip_verified"])
        self.assertTrue(report["zero_fallback_verified"])
        self.assertFalse(report["device_verified"])
        self.assertFalse(report["hardware_acceptance"])
        self.assertEqual(report["violations"], [])
        self.assertEqual(tuple(report["required_targets"]), brm.REQUIRED_TARGETS)
        self.assertEqual(tuple(report["forbidden_targets"]), brm.FORBIDDEN_TARGETS)

    def test_02_audit_graph_cli_writes_json_to_cache(self) -> None:
        scratch = _fresh_dir("cli-audit")
        out_json = scratch / "release_graph.json"
        res = subprocess.run(
            [sys.executable, "-B", str(THIS_DIR / "build_release_macos.py"),
             "--audit-graph-only", "--repo", str(REPO_ROOT), "--out-json", str(out_json)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertTrue(out_json.is_file())
        parsed = json.loads(out_json.read_text(encoding="utf-8"))
        self.assertEqual(parsed["status"], "PASS")
        self.assertFalse(parsed["device_verified"])
        self.assertGreater(parsed["scanned_source_count"], 40)

    def test_03_required_targets_exact_six(self) -> None:
        self.assertEqual(
            set(brm.REQUIRED_TARGETS),
            {
                "NativeMetalTranslate",
                "adreno_compile",
                "Adreno840Emit",
                "Adreno840Metal.bundle",
                "NezhaShaderCompiler",
                "NezhaGpuTest",
            },
        )

    def test_04_reject_injected_libvulkan_freedreno_target(self) -> None:
        g = copy.deepcopy(brm.TARGET_GRAPH)
        g["libvulkan_freedreno"] = {"kind": "dylib", "deps": (), "sources": (), "link_flags": ()}
        report = brm.audit_build_graph(REPO_ROOT, target_graph=g)
        self.assertEqual(report["status"], "FAIL")
        self.assertFalse(report["zero_turnip_verified"])
        rules = {v["rule"] for v in report["violations"]}
        self.assertIn("LIBVULKAN_FREEDRENO", rules)

    def test_05_reject_injected_libvulkan_freedreno_in_link_flags(self) -> None:
        g = copy.deepcopy(brm.TARGET_GRAPH)
        g["Adreno840Metal.bundle"]["link_flags"] = (*g["Adreno840Metal.bundle"]["link_flags"], "-lvulkan_freedreno", "libvulkan_freedreno.dylib")
        report = brm.audit_build_graph(REPO_ROOT, target_graph=g)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "LIBVULKAN_FREEDRENO" for v in report["violations"]))

    def test_06_reject_injected_turnip_darwin_kmd_patch(self) -> None:
        report = brm.audit_build_graph(
            REPO_ROOT,
            mesa_patches=(*brm.MESA_PATCHES, "ci/patches/mesa/turnip-darwin-kmd.patch"),
        )
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "TURNIP_DARWIN_KMD_PATCH" for v in report["violations"]))

    def test_07_reject_injected_metal2vulkan_runtime_dep(self) -> None:
        g = copy.deepcopy(brm.TARGET_GRAPH)
        g["Adreno840Metal.bundle"]["deps"] = (*g["Adreno840Metal.bundle"]["deps"], "metal2vulkan-runtime")
        report = brm.audit_build_graph(REPO_ROOT, target_graph=g)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "METAL2VULKAN_RUNTIME" for v in report["violations"]))

    def test_08_reject_injected_native_bridge_metal_frontend(self) -> None:
        g = copy.deepcopy(brm.TARGET_GRAPH)
        g["Adreno840Metal.bundle"]["sources"] = (
            *g["Adreno840Metal.bundle"]["sources"],
            "drivers/Adreno840Metal/objc/NativeBridgeMetalFrontend.mm",
        )
        report = brm.audit_build_graph(REPO_ROOT, target_graph=g)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "NATIVE_BRIDGE_METAL_FRONTEND" for v in report["violations"]))

    def test_09_reject_injected_src_freedreno_vulkan_in_extra_flags(self) -> None:
        report = brm.audit_build_graph(REPO_ROOT, extra_flags=("-Isrc/freedreno/vulkan",))
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "SRC_FREEDRENO_VULKAN" for v in report["violations"]))

    def test_10_reject_mesa_vulkan_drivers_enabled(self) -> None:
        opts = [o for o in brm.MESA_NON_VULKAN_OPTIONS if o != "-Dvulkan-drivers=[]"] + ["-Dvulkan-drivers=freedreno"]
        report = brm.audit_build_graph(REPO_ROOT, mesa_options=opts)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "MESA_VULKAN_NOT_DISABLED" for v in report["violations"]))

    def test_11_reject_dualpath_router_in_production_bundle(self) -> None:
        g = copy.deepcopy(brm.TARGET_GRAPH)
        g["Adreno840Metal.bundle"]["sources"] = (
            *g["Adreno840Metal.bundle"]["sources"],
            "drivers/Adreno840Metal/core/a840_dualpath_router.c",
        )
        report = brm.audit_build_graph(REPO_ROOT, target_graph=g)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "DUALPATH_ROUTER_IN_PROD" for v in report["violations"]))

    def test_12_reject_missing_required_target(self) -> None:
        g = copy.deepcopy(brm.TARGET_GRAPH)
        del g["NezhaShaderCompiler"]
        report = brm.audit_build_graph(REPO_ROOT, target_graph=g)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "MISSING_REQUIRED_TARGET" and v["match"] == "NezhaShaderCompiler"
                            for v in report["violations"]))

    def test_13_reject_cyclic_dependency_graph(self) -> None:
        g = copy.deepcopy(brm.TARGET_GRAPH)
        g["adreno_compile"]["deps"] = ("Adreno840Metal.bundle",)
        report = brm.audit_build_graph(REPO_ROOT, target_graph=g)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "CYCLIC_TARGET_DEP" for v in report["violations"]))

    def test_14_reject_missing_source_file(self) -> None:
        g = copy.deepcopy(brm.TARGET_GRAPH)
        g["adreno_compile"]["sources"] = (*g["adreno_compile"]["sources"], "adreno_compile/ir3/does_not_exist.c")
        report = brm.audit_build_graph(REPO_ROOT, target_graph=g)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "MISSING_SOURCE_FILE" for v in report["violations"]))

    def test_15_reject_cargo_toml_with_metal2vulkan_runtime(self) -> None:
        scratch = _fresh_dir("bad-cargo")
        shutil.copytree(REPO_ROOT / "translate", scratch / "translate")
        (scratch / "translate/Cargo.toml").write_text(
            '[package]\nname = "t"\nversion = "0.1.0"\nedition = "2021"\n\n'
            '[dependencies]\nmetal2vulkan-runtime = { path = "../bad" }\n',
            encoding="utf-8",
        )
        g = {"NativeMetalTranslate": brm.TARGET_GRAPH["NativeMetalTranslate"]}
        report = brm.audit_build_graph(scratch, target_graph=g)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "METAL2VULKAN_RUNTIME" for v in report["violations"]))

    def test_16_reject_source_code_containing_vk_create_device(self) -> None:
        scratch = _fresh_dir("bad-vk-src")
        (scratch / "daemon/NezhaShaderCompiler").mkdir(parents=True)
        (scratch / "daemon/NezhaShaderCompiler/main.c").write_text(
            "int main(void) { return vkCreateDevice(0); }\n", encoding="utf-8"
        )
        g = {"NezhaShaderCompiler": {
            "kind": "ios_daemon", "deps": (), "zero_fallback": True,
            "sources": ("daemon/NezhaShaderCompiler/main.c",), "sibling_w11_sources": (), "link_flags": (),
        }}
        report = brm.audit_build_graph(scratch, target_graph=g)
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any(v["rule"] == "VK_CREATE_DEVICE" for v in report["violations"]))


class TestStageReleaseMacCi(unittest.TestCase):
    """Tests 17-28: stage_release_mac_ci.py secret, Apple private, Turnip, and SHA-256 checks."""

    def test_17_reject_ipsw_suffix(self) -> None:
        with self.assertRaises(srm.StagingAuditError):
            srm.audit_staging_path("downloads/iPhone17,1_27.0_Restore.ipsw")

    def test_18_reject_metallib_suffix(self) -> None:
        with self.assertRaises(srm.StagingAuditError):
            srm.audit_staging_path("apps/NezhaGpuTest/NezhaGpuTest.metallib")

    def test_19_reject_pem_key_and_p12_suffixes(self) -> None:
        for rel in ("ci/secret.pem", "ci/id_ed25519.key", "ci/signing.p12", "ci/dev.mobileprovision", "run.log"):
            with self.subTest(rel=rel):
                with self.assertRaises(srm.StagingAuditError):
                    srm.audit_staging_path(rel)

    def test_20_reject_private_evidence_runs_segments(self) -> None:
        for rel in ("private/key.txt", "evidence/dump.txt", "runs/r001/out.txt", "backups/old.c"):
            with self.subTest(rel=rel):
                with self.assertRaises(srm.StagingAuditError):
                    srm.audit_staging_path(rel)

    def test_21_reject_turnip_and_vulkan_paths_in_staging(self) -> None:
        for rel in (
            "ci/patches/mesa/turnip-darwin-kmd.patch",
            "third_party/src/freedreno/vulkan/tu_device.cc",
            "libs/libvulkan_freedreno.txt",
            "crates/metal2vulkan-runtime/Cargo.toml",
            "objc/NativeBridgeMetalFrontend.mm",
        ):
            with self.subTest(rel=rel):
                with self.assertRaises(srm.StagingAuditError):
                    srm.audit_staging_path(rel)

    def test_22_reject_private_key_headers_in_source_bytes(self) -> None:
        for kind in (b"", b"RSA ", b"OPENSSH ", b"EC ", b"ENCRYPTED "):
            header = b"-----BEGIN " + kind + b"PRIVATE KEY-----\nabc\n"
            with self.subTest(kind=kind):
                with self.assertRaises(srm.StagingAuditError):
                    srm.audit_staging_bytes("tools/mac_ci/leak.txt", header)

    def test_23_reject_device_identifier_in_source_bytes(self) -> None:
        token = b"serial=" + b"c7fe" + b"a50c" + b"\n"
        with self.assertRaises(srm.StagingAuditError):
            srm.audit_staging_bytes("tools/mac_ci/id.txt", token)

    def test_24_reject_binary_nul_bytes(self) -> None:
        with self.assertRaises(srm.StagingAuditError):
            srm.audit_staging_bytes("tools/mac_ci/blob.txt", b"hello\x00world")

    def test_25_recipient_cert_rejects_private_key_and_non_cert(self) -> None:
        scratch = _fresh_dir("cert-check")
        bad_key = scratch / "priv.pem"
        bad_key.write_bytes(b"-----BEGIN " + b"PRIVATE KEY-----\nsecret\n-----END " + b"PRIVATE KEY-----\n")
        with self.assertRaises(srm.StagingAuditError):
            srm.validate_recipient_cert(bad_key)

        bad_txt = scratch / "notcert.pem"
        bad_txt.write_bytes(b"just plain text\n")
        with self.assertRaises(srm.StagingAuditError):
            srm.validate_recipient_cert(bad_txt)

        good_cert = scratch / "pub.pem"
        good_cert.write_bytes(SAMPLE_PUBLIC_CERT)
        self.assertEqual(srm.validate_recipient_cert(good_cert), SAMPLE_PUBLIC_CERT)

    def test_26_reject_symlink_inside_staged_repo(self) -> None:
        scratch = _fresh_dir("symlink-repo")
        (scratch / "contracts").mkdir(parents=True)
        real_f = scratch / "contracts/ok.h"
        real_f.write_text("/* ok */\n", encoding="utf-8")
        (scratch / "contracts/link.h").symlink_to(real_f)
        with self.assertRaises(srm.StagingAuditError):
            srm.collect_public_release_sources(scratch)

    def test_27_stage_release_repo_end_to_end_and_deterministic_sha(self) -> None:
        scratch = _fresh_dir("stage-e2e")
        cert = scratch / "recipient.pem"
        cert.write_bytes(SAMPLE_PUBLIC_CERT)
        st1 = scratch / "staging1"
        st2 = scratch / "staging2"
        cache = scratch / "cache"
        r1 = srm.stage_release_repo(REPO_ROOT, st1, cache, cert=cert, init_git=True)
        r2 = srm.stage_release_repo(REPO_ROOT, st2, cache, cert=cert, init_git=False)

        self.assertEqual(r1["status"], "STAGING_READY_LOCAL_ONLY")
        self.assertFalse(r1["pushed"])
        self.assertFalse(r1["device_verified"])
        self.assertTrue(r1["zero_turnip_verified"])
        self.assertEqual(r1["manifest_sha256"], r2["manifest_sha256"])
        self.assertEqual(r1["file_count"], r2["file_count"])

        # Verify git pushurl is strictly DISABLED.
        pushurl = subprocess.check_output(
            ["git", "-C", str(st1), "remote", "get-url", "--push", "origin"], text=True
        ).strip()
        self.assertEqual(pushurl, "DISABLED")

    def test_28_stage_release_repo_cli_smoke(self) -> None:
        scratch = _fresh_dir("stage-cli")
        cert = scratch / "recipient.pem"
        cert.write_bytes(SAMPLE_PUBLIC_CERT)
        res = subprocess.run(
            [sys.executable, "-B", str(THIS_DIR / "stage_release_mac_ci.py"),
             "--repo", str(REPO_ROOT), "--staging", str(scratch / "out"),
             "--cache-dir", str(scratch / "cache"), "--cert", str(cert), "--no-git"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        doc = json.loads(res.stdout)
        self.assertEqual(doc["status"], "STAGING_READY_LOCAL_ONLY")
        self.assertEqual(len(doc["manifest_sha256"]), 64)


class TestRustPacRewriteBoundary(unittest.TestCase):
    """Tests 29-36: rust_pac_rewrite.py deterministic rewriting & boundary failures."""

    def test_29_rewrite_blr_and_br_routes_to_exact_trampolines(self) -> None:
        scratch = _fresh_dir("pac-basic")
        # blr x0, br x5, nop, blr x29, br x30 (left), blr x30 (left)
        insns = [
            0xD63F0000 | (0 << 5),   # blr x0 -> bl _nmb_pac_call_x0
            0xD61F0000 | (5 << 5),   # br x5  -> b  _nmb_pac_call_x5
            0xD503201F,              # nop (untouched)
            0xD63F0000 | (29 << 5),  # blr x29 -> bl _nmb_pac_call_x29
            0xD61F0000 | (30 << 5),  # br x30 (must stay untouched)
            0xD63F0000 | (30 << 5),  # blr x30 (must stay untouched)
        ]
        img, lmap = _build_synthetic_arm64e_macho(scratch, rust_insns=insns)
        counts = rpr.rewrite_rust_indirect_branches(img, lmap, "libnative_metal_translate_pac.a")
        self.assertEqual(counts, {"blr": 2, "br": 1, "x30": 2, "objects": 1})

        raw = img.read_bytes()
        # Check instruction 0: bl -> delta = (0x200 - 0) >> 2 = 0x80
        w0 = struct.unpack_from("<I", raw, 0x1000)[0]
        self.assertEqual(w0, 0x94000000 | ((0x200 - 0x0) >> 2))
        # Check instruction 1: b -> tramp x5 at +0x200 + 5*4 = +0x214, pc = +4 -> delta = 0x210 >> 2
        w1 = struct.unpack_from("<I", raw, 0x1004)[0]
        self.assertEqual(w1, 0x14000000 | ((0x214 - 0x4) >> 2))
        # Check instruction 2: nop unchanged
        w2 = struct.unpack_from("<I", raw, 0x1008)[0]
        self.assertEqual(w2, 0xD503201F)
        # Check instructions 4 & 5 (x30): unchanged
        self.assertEqual(struct.unpack_from("<I", raw, 0x1010)[0], insns[4])
        self.assertEqual(struct.unpack_from("<I", raw, 0x1014)[0], insns[5])
        # Check non-Rust blr x0 untouched
        non_rust_off = 0x1000 + len(insns) * 4 + 0x10 + 4
        self.assertEqual(struct.unpack_from("<I", raw, non_rust_off)[0], 0xD63F0000)

    def test_30_rewrite_is_byte_deterministic(self) -> None:
        s1 = _fresh_dir("pac-det1")
        s2 = _fresh_dir("pac-det2")
        insns = [0xD63F0000 | (r << 5) for r in range(10)]
        i1, m1 = _build_synthetic_arm64e_macho(s1, rust_insns=insns)
        i2, m2 = _build_synthetic_arm64e_macho(s2, rust_insns=insns)
        rpr.rewrite_rust_indirect_branches(i1, m1, "libnative_metal_translate_pac.a")
        rpr.rewrite_rust_indirect_branches(i2, m2, "libnative_metal_translate_pac.a")
        self.assertEqual(hashlib.sha256(i1.read_bytes()).hexdigest(),
                         hashlib.sha256(i2.read_bytes()).hexdigest())

    def test_31_rewrite_fails_when_trampoline_missing(self) -> None:
        scratch = _fresh_dir("pac-miss-tramp")
        img, lmap = _build_synthetic_arm64e_macho(scratch, rust_insns=[0xD63F0000], missing_trampoline=17)
        with self.assertRaisesRegex(RuntimeError, "missing PAC trampolines"):
            rpr.rewrite_rust_indirect_branches(img, lmap, "libnative_metal_translate_pac.a")

    def test_32_rewrite_fails_when_archive_absent_from_map(self) -> None:
        scratch = _fresh_dir("pac-no-arch")
        img, lmap = _build_synthetic_arm64e_macho(scratch, rust_insns=[0xD63F0000], include_archive_obj=False)
        with self.assertRaisesRegex(RuntimeError, "names no object"):
            rpr.rewrite_rust_indirect_branches(img, lmap, "libnative_metal_translate_pac.a")

    def test_33_rewrite_fails_when_text_segment_missing(self) -> None:
        scratch = _fresh_dir("pac-no-text")
        img, lmap = _build_synthetic_arm64e_macho(scratch, rust_insns=[0xD63F0000], include_text_segment=False)
        with self.assertRaisesRegex(RuntimeError, "missing __TEXT LC_SEGMENT_64"):
            rpr.rewrite_rust_indirect_branches(img, lmap, "libnative_metal_translate_pac.a")

    def test_34_rewrite_fails_on_branch_out_of_range(self) -> None:
        scratch = _fresh_dir("pac-oob")
        # 140 MiB exceeds ARM64 +-128 MiB B/BL immediate range
        img, lmap = _build_synthetic_arm64e_macho(
            scratch, rust_insns=[0xD63F0000], tramp_offset_bytes=0x08C00000
        )
        with self.assertRaisesRegex(RuntimeError, "out of branch range"):
            rpr.rewrite_rust_indirect_branches(img, lmap, "libnative_metal_translate_pac.a")

    def test_35_rewrite_fails_on_invalid_macho_magic(self) -> None:
        scratch = _fresh_dir("pac-bad-magic")
        img, lmap = _build_synthetic_arm64e_macho(scratch, rust_insns=[0xD63F0000])
        img.write_bytes(b"NOT_A_MACHO_HEADER" + b"\x00" * 64)
        with self.assertRaisesRegex(RuntimeError, "unsupported Mach-O magic"):
            rpr.rewrite_rust_indirect_branches(img, lmap, "libnative_metal_translate_pac.a")

    def test_36_rewrite_cli_main_pass_and_bad_argc(self) -> None:
        self.assertEqual(rpr.main(["rust_pac_rewrite.py"]), 2)
        scratch = _fresh_dir("pac-cli")
        img, lmap = _build_synthetic_arm64e_macho(scratch, rust_insns=[0xD63F0000 | (3 << 5)])
        rc = rpr.main(["rust_pac_rewrite.py", str(img), str(lmap), "libnative_metal_translate_pac.a"])
        self.assertEqual(rc, 0)


class TestOutputPathGuards(unittest.TestCase):
    """Tests 37-40: C drive (/mnt/c, C:\\) and /tmp output path rejection."""

    def test_37_reject_c_drive_paths(self) -> None:
        for bad in ("/mnt/c/out.json", "/mnt/C/Users/Hua/out.json", "C:\\Users\\Hua\\out.json", "c:/tmp/out.json"):
            with self.subTest(bad=bad):
                with self.assertRaises(brm.BuildGraphAuditError):
                    brm.validate_output_path(bad, "--out-json")

    def test_38_reject_tmp_paths(self) -> None:
        for bad in ("/tmp/release.json", "/tmp", "/private/tmp/release.json"):
            with self.subTest(bad=bad):
                with self.assertRaises(brm.BuildGraphAuditError):
                    brm.validate_output_path(bad, "--out-json")

    def test_39_cli_audit_rejects_c_drive_and_tmp_out_json(self) -> None:
        for bad in ("/mnt/c/should_fail.json", "/tmp/should_fail.json"):
            with self.subTest(bad=bad):
                res = subprocess.run(
                    [sys.executable, "-B", str(THIS_DIR / "build_release_macos.py"),
                     "--audit-graph-only", "--repo", str(REPO_ROOT), "--out-json", bad],
                    capture_output=True, text=True, check=False,
                )
                self.assertNotEqual(res.returncode, 0)
                doc = json.loads(res.stdout)
                self.assertEqual(doc["status"], "FAIL")

    def test_40_stage_cli_rejects_c_drive_and_tmp_staging(self) -> None:
        scratch = _fresh_dir("path-guard-stage")
        cert = scratch / "recipient.pem"
        cert.write_bytes(SAMPLE_PUBLIC_CERT)
        for bad in ("/mnt/c/bad-stage", "/tmp/bad-stage"):
            with self.subTest(bad=bad):
                res = subprocess.run(
                    [sys.executable, "-B", str(THIS_DIR / "stage_release_mac_ci.py"),
                     "--repo", str(REPO_ROOT), "--staging", bad,
                     "--cache-dir", str(scratch / "cache"), "--cert", str(cert), "--no-git"],
                    capture_output=True, text=True, check=False,
                )
                self.assertNotEqual(res.returncode, 0)
                doc = json.loads(res.stdout)
                self.assertEqual(doc["status"], "FAIL")


if __name__ == "__main__":
    unittest.main()
