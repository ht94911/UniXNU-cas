#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Byte-level packager and four-scene acceptance tests using host-only fixtures."""

from contextlib import redirect_stdout
from copy import deepcopy
import hashlib
import io
import json
from pathlib import Path
import plistlib
import struct
import tempfile
import unittest

import system_acceptance_gate as gate
import system_cutover_packager as cut


REPO = Path(__file__).resolve().parents[2]


def reference(path, data):
    path.write_bytes(data)
    return {"path": path.name, "sha256": cut.sha256(data)}


def macho(kind=2, identifier="org.nezha.host-fixture", *, page_shift=12,
          minimum=27 << 16, platform=2, subtype=0x80000002, special_files=None, payload=b""):
    """Build a thin synthetic header, signature load command and hashed code pages."""
    special_files = special_files or {}
    code_size = 5000
    page_bytes = 1 << page_shift
    ident = identifier.encode("utf-8") + b"\0"
    slots = max(special_files, default=0)
    before_pages = b"".join(hashlib.sha256(special_files[i]).digest() if i in special_files else bytes(32)
                            for i in range(slots, 0, -1))
    hash_offset = cut.CODE_DIRECTORY.size + len(ident) + len(before_pages)
    n_pages = (code_size + page_bytes - 1) // page_bytes
    cd = cut.CODE_DIRECTORY.pack(0xFADE0C02, hash_offset + n_pages * 32, 0x20001, 0,
                                 hash_offset, cut.CODE_DIRECTORY.size, slots, n_pages, code_size,
                                 32, 2, 0, page_shift, 0)
    cd += ident + before_pages
    signature_size = len(cut.sign_blobs({0: cd + bytes(32 * n_pages)}))
    code = cut.MACHO_HEADER.pack(0xFEEDFACF, 0x0100000C, subtype, kind, 2, 40, 0, 0)
    code += struct.pack("<6I", 0x32, 24, platform, minimum, 27 << 16, 0)
    code += struct.pack("<4I", 0x1D, 16, code_size, signature_size) + payload
    code = code.ljust(code_size, b"\0")
    hashes = b"".join(hashlib.sha256(code[at:at + page_bytes]).digest()
                      for at in range(0, code_size, page_bytes))
    return code + cut.sign_blobs({0: cd + hashes})


def tier0_pack():
    header = struct.pack("<4sBBHQQII32s", b"8PK1", 129, 1, 1, 1, 2, 0x44050A31, 128, bytes(32))
    data = header.ljust(384, b"\0")
    return data[:32] + hashlib.sha256(data).digest() + data[64:]


def daemon_plist():
    return {"Label": cut.DAEMON_LABEL, "ProgramArguments": [cut.DAEMON_EXECUTABLE, "--pack", cut.PACK_PATH],
            "HardResourceLimits": {"MemoryLimit": cut.MEMORY_LIMIT_KIB}, "KeepAlive": True, "RunAtLoad": True}


def prepare_package(directory: Path) -> dict:
    cache = plistlib.dumps({"VersionNumber": 7, "LaunchDaemons": {
        "/System/Library/LaunchDaemons/org.example.owner.plist": {"Label": "org.example.owner"}},
        "untouched": b"\x01\x02"}, fmt=plistlib.FMT_BINARY)
    original_entries = {
        f"{cut.BUNDLE}/Info.plist": (0o644, b"old bundle metadata"),
        f"{cut.BUNDLE}/NativeAdrenoMetal": (0o755, b"old GPU driver"),
        f"{cut.BUNDLE}/libvulkan_freedreno.dylib": (0o755, b"old icd"),
        "addons/NativeAdrenoMetal.fulltest": (0o644, b"old runtime setting"),
        cut.PUBLISH: (0o644, b"0\n"),
        "lib/libVulkan_freedreno.dylib": (0o755, b"old icd"),
        "bin/metal2vulkan-runtime": (0o755, b"old runtime"),
        "bin/NativeBridgeMetalFrontend": (0o755, b"old frontend"),
        "lib/Turnip.dylib": (0o755, b"old driver"),
        "firmware/wcn7860/peach/amss20.bin": (0o600, b"keep Wi-Fi"),
        cut.LAUNCHD_CACHE: (0o644, cache),
        cut.LAUNCHD_SIGNATURE: (0o644, cut.sign_launchd(cache)),
    }
    base_trust = cut.append_trustcache([], ["ab" * 20])
    info = plistlib.dumps({**cut.BUNDLE_INFO, "CFBundleVersion": "1"})
    resources = plistlib.dumps({"files": {}})
    members = {
        "bundle_info": info, "bundle_code_resources": resources,
        "bundle_executable": macho(8, cut.BUNDLE_INFO["CFBundleIdentifier"], special_files={1: info, 3: resources}),
        "shader_compiler": macho(identifier=cut.DAEMON_LABEL),
        "launch_daemon": plistlib.dumps(daemon_plist()),
        "pack": tier0_pack(),
        "gpu_test": macho(identifier="org.nezha.NezhaGpuTest"),
        "gpu_test_metallib": b"MTLB\x00fixture",
    }
    return {"schema_version": 1, **cut.BOOT_POLICY, "payload_write": False,
            "base_gpuaddon": reference(directory / "base.gpuaddon", cut.encode_addon(original_entries)),
            "base_trustcache": reference(directory / "base.trustcache", base_trust),
            "native_artifacts": {name: reference(directory / name, data) for name, data in members.items()}}


def cli(main, *args):
    output = io.StringIO()
    with redirect_stdout(output):
        code = main(args)
    return code, json.loads(output.getvalue())


class LocalScratch(unittest.TestCase):
    def setUp(self):
        cut.CACHE_ROOT.mkdir(parents=True, exist_ok=True)
        scratch = tempfile.TemporaryDirectory(prefix="system-cutover-test-", dir=cut.CACHE_ROOT)
        self.addCleanup(scratch.cleanup)
        self.directory = Path(scratch.name)


class PackagerTests(LocalScratch):
    def setUp(self):
        super().setUp()
        self.plan = prepare_package(self.directory)

    def audit(self):
        return cut.audit_plan(self.plan, self.directory)

    def replace(self, name, data):
        self.plan["native_artifacts"][name] = reference(self.directory / name, data)

    def write_plan(self):
        path = self.directory / "plan.json"
        path.write_text(cut.json_output(self.plan), encoding="utf-8")
        return str(path)

    def alter_addon(self, **entries):
        raw = cut.read_pinned(self.plan["base_gpuaddon"], self.directory, "base_gpuaddon")
        self.plan["base_gpuaddon"] = reference(self.directory / "base.gpuaddon",
                                               cut.encode_addon({**cut.read_addon(raw), **entries}))

    def test_native_replacement_retains_discovery_name(self):
        report, files = self.audit()
        addon = cut.read_addon(files["kernel.macho.gpuaddon"])
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["bundle_install_path"], "/System/Library/Extensions/NativeAdrenoMetal.bundle")
        self.assertIn(f"{cut.BUNDLE}/Adreno840Metal", addon)
        self.assertNotIn(f"{cut.BUNDLE}/NativeAdrenoMetal", addon)

    def test_named_legacy_components_are_purged(self):
        report, files = self.audit()
        expected = {"lib/libVulkan_freedreno.dylib", "bin/metal2vulkan-runtime",
                    "bin/NativeBridgeMetalFrontend", "lib/Turnip.dylib"}
        self.assertTrue(expected <= set(report["removed_base_entries"]))
        self.assertFalse(expected & cut.read_addon(files["kernel.macho.gpuaddon"]).keys())
        self.assertEqual(report["legacy_entry_count"], 0)

    def test_unrelated_firmware_and_permissions_preserved(self):
        _, files = self.audit()
        addon = cut.read_addon(files["kernel.macho.gpuaddon"])
        self.assertEqual(addon["firmware/wcn7860/peach/amss20.bin"], (0o600, b"keep Wi-Fi"))

    def test_marker_and_read_only_pack(self):
        _, files = self.audit()
        addon = cut.read_addon(files["kernel.macho.gpuaddon"])
        self.assertEqual(addon[cut.PUBLISH], (0o644, b"1\n"))
        self.assertEqual(addon[cut.PACK_MEMBER], (0o444, tier0_pack()))
        self.assertNotIn("addons/NativeAdrenoMetal.fulltest", addon)

    def test_signed_cache_adds_exact_job(self):
        _, files = self.audit()
        addon = cut.read_addon(files["kernel.macho.gpuaddon"])
        cache, signature = addon[cut.LAUNCHD_CACHE][1], addon[cut.LAUNCHD_SIGNATURE][1]
        self.assertGreater(cut.verify_code_directory(cache, signature)["pages_verified"], 0)
        parsed = plistlib.loads(cache)
        self.assertEqual(parsed["LaunchDaemons"][cut.DAEMON_PLIST], daemon_plist())
        self.assertEqual(len(parsed["LaunchDaemons"]), 2)
        self.assertEqual(parsed["untouched"], b"\x01\x02")

    def test_new_owner_registers_each_verified_macho_and_cache(self):
        report, files = self.audit()
        old = cut.read_pinned(self.plan["base_trustcache"], self.directory, "base_trustcache")
        new = cut.split_trustcache(files["kernel.macho.trustcache"])
        self.assertEqual(new[:-1], cut.split_trustcache(old))
        signatures = report["trustcache"]["registered_signatures"]
        self.assertEqual(len(signatures), 4)
        self.assertEqual({info["page_bytes"] for info in signatures.values()}, {4096})
        entries = list(struct.iter_unpack("<20sBBBB", new[-1][cut.TRUST_HEADER.size:]))
        self.assertEqual({entry[0].hex() for entry in entries}, {row["cdhash"] for row in signatures.values()})

    def test_missing_new_cdhash_refused_on_readback(self):
        modules = cut.split_trustcache(cut.read_pinned(self.plan["base_trustcache"], self.directory, "trust"))
        incomplete = cut.append_trustcache(modules, ["12" * 20])
        with self.assertRaisesRegex(ValueError, "CDHash"):
            cut.verify_trustcache(incomplete, modules, ["12" * 20, "34" * 20])

    def test_existing_trust_module_tamper_rejected(self):
        old = cut.split_trustcache(cut.read_pinned(self.plan["base_trustcache"], self.directory, "trust"))
        replacement = bytes([old[0][0] ^ 1]) + old[0][1:]
        with self.assertRaisesRegex(ValueError, "existing trustcache"):
            cut.verify_trustcache(cut.append_trustcache([replacement], ["12" * 20]), old, ["12" * 20])

    def test_unsigned_macho_rejected(self):
        data = bytearray(macho())
        struct.pack_into("<I", data, 56, 1)
        self.replace("gpu_test", bytes(data))
        with self.assertRaisesRegex(ValueError, "missing LC_CODE_SIGNATURE"):
            self.audit()

    def test_signed_page_tamper_rejected(self):
        data = bytearray(macho())
        data[4200] ^= 1
        self.replace("shader_compiler", bytes(data))
        with self.assertRaisesRegex(ValueError, "4 KiB code page 1 hash mismatch"):
            self.audit()

    def test_bundle_info_must_match_signed_special_slot(self):
        self.replace("bundle_info", plistlib.dumps({**cut.BUNDLE_INFO, "CFBundleVersion": "unsigned edit"}))
        with self.assertRaisesRegex(ValueError, "special slot 1"):
            self.audit()

    def test_bundle_resources_must_match_signed_special_slot(self):
        self.replace("bundle_code_resources", plistlib.dumps({"files": {"unsigned": b"1"}}))
        with self.assertRaisesRegex(ValueError, "special slot 3"):
            self.audit()

    def test_base_launchd_signature_must_verify(self):
        original = cut.read_addon(cut.read_pinned(self.plan["base_gpuaddon"], self.directory, "addon"))
        self.alter_addon(**{cut.LAUNCHD_CACHE: (0o644, original[cut.LAUNCHD_CACHE][1] + b"changed")})
        with self.assertRaisesRegex(ValueError, "CodeDirectory coverage"):
            self.audit()

    def test_preexisting_daemon_label_rejected(self):
        cache = plistlib.dumps({"VersionNumber": 7, "LaunchDaemons": {"/conflict.plist": daemon_plist()}},
                                fmt=plistlib.FMT_BINARY)
        self.alter_addon(**{cut.LAUNCHD_CACHE: (0o644, cache), cut.LAUNCHD_SIGNATURE: (0o644, cut.sign_launchd(cache))})
        with self.assertRaisesRegex(ValueError, "conflicting launchd"):
            self.audit()

    def test_compiler_plist_uses_the_mounted_pack_in_the_output(self):
        source = daemon_plist()
        source["ProgramArguments"][-1] = cut.SOURCE_PACK_PATH
        self.replace("launch_daemon", plistlib.dumps(source))
        report, files = self.audit()
        addon = cut.read_addon(files[cut.ADDON_OUTPUT])
        installed = plistlib.loads(addon[cut.DAEMON_ADDON_PLIST][1])
        cache = plistlib.loads(addon[cut.LAUNCHD_CACHE][1])
        self.assertEqual(installed["ProgramArguments"][-1], cut.PACK_PATH)
        self.assertEqual(cache["LaunchDaemons"][cut.DAEMON_PLIST], installed)
        self.assertEqual(report["pack"]["source_launchd_arg"], cut.SOURCE_PACK_PATH)

    def test_missing_metallib_is_optional(self):
        del self.plan["native_artifacts"]["gpu_test_metallib"]
        report, _ = self.audit()
        self.assertNotIn(cut.ARTIFACTS["gpu_test_metallib"][0], report["entries"])

    def test_cli_audit_only_has_host_flags_and_fixed_policy(self):
        previous_paths = set(self.directory.iterdir())
        plan = self.write_plan()
        code, report = cli(cut.main, "--audit-plan", plan)
        self.assertEqual(code, 0)
        for key, value in {**cut.BOOT_POLICY, **cut.HOST_FLAGS, "phone_operations": False}.items():
            self.assertIs(report[key], value)
        self.assertEqual(set(self.directory.iterdir()) - previous_paths, {Path(plan)})

    def test_cli_emits_only_host_modules_and_report(self):
        target = self.directory / "modules"
        code, report = cli(cut.main, "--audit-plan", self.write_plan(), "--out-dir", str(target))
        self.assertEqual(code, 0, report)
        self.assertEqual({file.name for file in target.iterdir()},
                         {"audit.json", "kernel.macho.gpuaddon", "kernel.macho.trustcache"})
        self.assertEqual(json.loads((target / "audit.json").read_text(encoding="utf-8")), report)
        for name, identity in report["outputs"].items():
            self.assertEqual(cut.sha256((target / name).read_bytes()), identity["sha256"])

    def test_failed_audit_creates_no_output(self):
        self.plan["payload_write"] = True
        output = self.directory / "unused"
        code, report = cli(cut.main, "--audit-plan", self.write_plan(), "--out-dir", str(output))
        self.assertEqual(code, 1)
        self.assertIn("payload_write", report["error"])
        self.assertFalse(output.exists())

    def test_output_cannot_clobber_existing_directory(self):
        with self.assertRaisesRegex(ValueError, "already exists"):
            cut.checked_output_dir(str(self.directory))

    def test_output_symlink_escape(self):
        alias = self.directory / "wrong-drive"
        alias.symlink_to("/tmp", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "forbidden host path"):
            cut.checked_output_dir(str(alias / "modules"))

    def test_payload_image_cannot_be_an_output_path_component(self):
        with self.assertRaisesRegex(ValueError, "payload.img"):
            cut.checked_output_dir(str(self.directory / "PAYLOAD.IMG" / "modules"))

    def test_signatures_are_deterministic(self):
        report, artifacts = self.audit()
        self.assertEqual((report, artifacts), self.audit())


def add_case(cls, name, assertion, *args):
    def test(self):
        assertion(self, *args)
    test.__name__ = f"test_{name}"
    setattr(cls, test.__name__, test)


def reject_plan(self, key, value):
    self.plan[key] = value
    with self.assertRaisesRegex(ValueError, key):
        self.audit()


for name, key, value in (
    ("forbid_non_temporary_boot", "full_temporary_boot", False),
    ("forbid_missing_strict_boolean", "full_temporary_boot", 1),
    ("forbid_phone_payload_write", "phone_payload_write_required", True),
    ("forbid_payload_int_false", "payload_write", 0),
    ("forbid_Data_generation_change", "Data_generation", 2),
    ("forbid_Data_generation_bool", "Data_generation", True),
    ("forbid_Data_reset", "no_Data_creation_or_reset", False),
    ("forbid_unknown_write_alias", "rawwrite", True),
):
    add_case(PackagerTests, name, reject_plan, key, value)


def reject_job(self, changes, reason):
    self.replace("launch_daemon", plistlib.dumps({**daemon_plist(), **changes}))
    with self.assertRaisesRegex(ValueError, reason):
        self.audit()


for name, changes, reason in (
    ("launchd_MiB", {"HardResourceLimits": {"MemoryLimit": 256}}, "262144 KiB"),
    ("launchd_bytes", {"HardResourceLimits": {"MemoryLimit": 268435456}}, "262144 KiB"),
    ("launchd_string", {"HardResourceLimits": {"MemoryLimit": "262144"}}, "262144 KiB"),
    ("launchd_bool", {"HardResourceLimits": {"MemoryLimit": True}}, "262144 KiB"),
    ("launchd_soft_limit", {"SoftResourceLimits": {"MemoryLimit": 256}}, "conflicting"),
    ("launchd_ngpu", {"StandardOutPath": "/private/var/ngpu/test.log"}, "/private/var/ngpu"),
    ("launchd_ngpu_alias", {"EnvironmentVariables": {"STORE": "/var//./ngpu/write"}}, "/private/var/ngpu"),
    ("launchd_ngpu_traversal", {"EnvironmentVariables": {"STORE": "/var/mobile/../ngpu/store"}}, "/private/var/ngpu"),
    ("launchd_payload_img", {"StandardOutPath": "/data/unixnu/ios27/payload.img"}, "payload.img"),
    ("launchd_legacy", {"EnvironmentVariables": {"RUNTIME": "metal2vulkan-runtime"}}, "legacy"),
    ("launchd_wrong_program", {"Program": "/bin/sh"}, "/usr/sbin/NezhaShaderCompiler"),
    ("launchd_wrong_pack", {"ProgramArguments": [cut.DAEMON_EXECUTABLE, "--pack", "/else/pack.bin"]}, "--pack"),
    ("launchd_missing_pack", {"ProgramArguments": [cut.DAEMON_EXECUTABLE, "--pack"]}, "--pack"),
    ("launchd_wrong_label", {"Label": "org.nezha.other"}, "system label"),
    ("launchd_disabled", {"KeepAlive": False}, "KeepAlive"),
):
    add_case(PackagerTests, name + "_rejected", reject_job, changes, reason)


def reject_binary(self, options, reason):
    self.replace("shader_compiler", macho(**options))
    with self.assertRaisesRegex(ValueError, reason):
        self.audit()


for name, options, reason in (
    ("wrong_16k_pages", {"page_shift": 14}, "4 KiB CodeDirectory"),
    ("wrong_platform", {"platform": 1}, "targeting iOS 27.0"),
    ("wrong_minos", {"minimum": 26 << 16}, "targeting iOS 27.0"),
    ("wrong_arm64_subtype", {"subtype": 0}, "arm64e PAC00"),
    ("wrong_file_type", {"kind": 8}, "expected file type"),
    ("libvulkan_freedreno_in_signed_bytes", {"payload": b"@rpath/libvulkan_freedreno.dylib"}, "legacy"),
    ("metal2vulkan_runtime_in_signed_bytes", {"payload": b"metal2vulkan-runtime"}, "legacy"),
    ("NativeBridgeMetalFrontend_in_signed_bytes", {"payload": b"NativeBridgeMetalFrontend"}, "legacy"),
    ("Vulkan_api_in_symbol_strings", {"payload": b"\0_vkCreateDevice\0"}, "legacy"),
    ("Turnip_symbols", {"payload": b"\0_tu_queue_submit\0"}, "legacy"),
):
    add_case(PackagerTests, name + "_rejected", reject_binary, options, reason)


def reject_artifact(self, name, value, reason):
    self.replace(name, value)
    with self.assertRaisesRegex(ValueError, reason):
        self.audit()


add_case(PackagerTests, "invalid_pack_magic", reject_artifact, "pack", bytes(384), "Tier-0")
add_case(PackagerTests, "invalid_pack_checksum", reject_artifact, "pack", tier0_pack()[:-1] + b"\x01", "integrity")
add_case(PackagerTests, "invalid_metallib", reject_artifact, "gpu_test_metallib", b"ELF", "invalid NezhaGpuTest")
add_case(PackagerTests, "invalid_bundle_principal", reject_artifact, "bundle_info",
         plistlib.dumps({**cut.BUNDLE_INFO, "NSPrincipalClass": "Adreno840MetalPrincipal"}), "NSPrincipalClass")


class FormatTests(unittest.TestCase):
    def test_unverified_compiler_IR_names_are_not_loader_symbols(self):
        cut.audit_inventory({"bin/compiler": (0o755, b"\0vk_format\0VulkanMemoryModel\0")})

    def test_pin_must_be_full_hash(self):
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            cut.check_sha256("adreno840", "digest")

    def test_duplicate_json_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate JSON"):
            cut.parse_json(b'{"phone_payload_write_required":true,"phone_payload_write_required":false}')

    def test_nonfinite_json_rejected(self):
        with self.assertRaisesRegex(ValueError, "non-finite"):
            cut.parse_json(b'{"timestamp":NaN}')

    def test_file_directory_collision_rejected(self):
        with self.assertRaisesRegex(ValueError, "file/directory collision"):
            cut.read_addon(cut.encode_addon({"dir": (0o444, b""), "dir/bin": (0o755, b"data")}))

    def test_case_alias_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate NGPUADD2"):
            cut.read_addon(cut.encode_addon({"lib/x": (0o644, b"a"), "lib/X": (0o644, b"b")}))

    def test_trailing_addon_data_rejected(self):
        data = cut.encode_addon({"lib/x": (0o644, b"a")})
        with self.assertRaisesRegex(ValueError, "header"):
            cut.read_addon(data + b"stale data")

    def test_hidden_legacy_sidecar_rejected(self):
        with self.assertRaisesRegex(ValueError, "legacy Turnip/Vulkan"):
            cut.audit_inventory({"lib/renderer.dylib": (0o755, b"@rpath/libvulkan_freedreno.dylib")})

    def test_owner_hashes_must_be_distinct(self):
        with self.assertRaisesRegex(ValueError, "distinct"):
            cut.append_trustcache([], ["de" * 20, "de" * 20])


def reject_output(self, raw):
    with self.assertRaisesRegex(ValueError, "invalid host path|forbidden host path|new directory"):
        cut.checked_output_dir(raw)


for name, raw in (
    ("windows_C_path", r"C:\Users\Hua\cutover"),
    ("windows_C_slash", "C:/Users/Hua/cutover"),
    ("windows_UNC_path", r"\\server\share\cutover"),
    ("mnt_c_path", "/mnt/c/forbidden"),
    ("mnt_upper_C_path", "/mnt/C/forbidden"),
    ("tmp_path", "/tmp/forbidden"),
    ("var_tmp_path", "/var/tmp/forbidden"),
    ("nonapproved_home_path", "/home/hua/forbidden"),
    ("escaped_output_path", str(cut.CACHE_ROOT / "../forbidden")),
):
    add_case(FormatTests, "forbid_" + name, reject_output, raw)


def reject_member(self, name, reason):
    with self.assertRaisesRegex(ValueError, reason):
        cut.read_addon(cut.encode_addon({name: (0o644, b"data")}))


for name, path, reason in (
    ("parent_path", "../kernel", "unsafe"),
    ("absolute_path", "/private/volume", "unsafe"),
    ("repeated_separator", "addons//file", "unsafe"),
    ("embedded_NUL", "addons/x\0y", "unsafe"),
    ("payload_member", "EFI/PAYLOAD.IMG", "payload.img"),
):
    add_case(FormatTests, "forbid_" + name, reject_member, path, reason)


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.run_doc, self.boot = gate.host_fixture()
        self.run_doc["source_kind"] = gate.DEVICE_CAPTURE
        for row in (*self.documents(), self.run_doc["runtime_log"], self.run_doc["regression_log"]):
            row["source_kind"] = gate.DEVICE_CAPTURE

    def documents(self):
        return [row for scene in self.run_doc["scenes"].values() for row in scene.values()]

    def evaluate(self):
        return gate.evaluate(self.run_doc, self.boot, lambda doc, label: doc,
                             source_kind=gate.DEVICE_CAPTURE)

    def action(self, scene="settings_scroll", action="scroll"):
        return self.run_doc["scenes"][scene][action]

    def set_intervals(self, row, deltas):
        timestamp = row["action_start_ns"]
        row["frames"] = [{"sequence": 0, "present_ns": timestamp, "content_id": "0"}]
        for seq, interval in enumerate(deltas, 1):
            timestamp += interval
            row["frames"].append({"sequence": seq, "present_ns": timestamp, "content_id": str(seq)})
        row["action_end_ns"] = timestamp

    def result(self, scene="settings_scroll", action="scroll"):
        return self.evaluate()["scenes"][scene]["actions"][action]

    def test_all_four_scenes_device_branch(self):
        report = self.evaluate()
        self.assertEqual(report["status"], "PASS")
        self.assertTrue(report["device_verified"])
        self.assertTrue(report["hardware_acceptance"])
        self.assertEqual(set(report["scenes"]), set(gate.SCENES))

    def test_open_and_close_are_independent(self):
        self.set_intervals(self.action("app_open_close", "close"), [gate.BUDGET_NS + 1])
        scene = self.evaluate()["scenes"]["app_open_close"]
        self.assertEqual({key: row["status"] for key, row in scene["actions"].items()},
                         {"hot_open": "PASS", "close": "FAIL"})
        self.assertFalse(self.evaluate()["hardware_acceptance"])

    def test_exact_physical_budget_passes(self):
        self.set_intervals(self.action(), [gate.BUDGET_NS] * 3)
        self.assertEqual(self.result()["status"], "PASS")

    def test_one_nanosecond_over_budget_fails(self):
        self.set_intervals(self.action(), [gate.BUDGET_NS + 1])
        self.assertEqual(self.result()["status"], "FAIL")
        self.assertEqual(self.result()["physical"]["ge_100_ms_operation_stops"], 0)

    def test_exact_drop_threshold_counts_separately(self):
        self.set_intervals(self.action(), [gate.DROP_NS - 1, gate.DROP_NS, gate.BUDGET_NS])
        metrics = self.result()["physical"]
        self.assertEqual(metrics["ge_16_667_ms_count"], 1)
        self.assertAlmostEqual(metrics["ge_16_667_ms_rate_pct"], 100 / 3)

    def test_nearest_rank_with_exactly_five_percent_drops(self):
        self.set_intervals(self.action(), [gate.BUDGET_NS] * 19 + [gate.DROP_NS])
        metrics = self.result()["physical"]
        self.assertEqual(metrics["p95_ms"], gate.BUDGET_NS / 1e6)
        self.assertEqual(metrics["ge_16_667_ms_rate_pct"], 5.0)
        self.assertEqual(metrics["max_ms"], gate.DROP_NS / 1e6)

    def test_exact_100ms_stops_even_when_p95_is_in_budget(self):
        self.set_intervals(self.action(), [gate.BUDGET_NS] * 21 + [gate.STOP_NS])
        metrics = self.result()["physical"]
        self.assertLessEqual(metrics["p95_ms"], gate.BUDGET_NS / 1e6)
        self.assertEqual(metrics["ge_100_ms_operation_stops"], 1)
        self.assertFalse(self.evaluate()["hardware_acceptance"])

    def test_subthreshold_hold_is_not_counted_as_100ms_stop(self):
        self.set_intervals(self.action(), [gate.BUDGET_NS] * 21 + [gate.STOP_NS - 1])
        self.assertEqual(self.result()["physical"]["ge_100_ms_operation_stops"], 0)

    def test_edge_idle_only_is_excluded(self):
        row = self.action()
        first = row["frames"][0]["present_ns"]
        row["frames"].insert(0, {"sequence": 0, "present_ns": first - 500_000_000, "content_id": "idle"})
        for seq, frame in enumerate(row["frames"]):
            frame["sequence"] = seq
        row["f07"][0]["sequence"] = 2
        self.assertEqual(self.result()["status"], "PASS")
        self.assertEqual(self.result()["physical"]["sample_count"], 3)

    def test_clipped_edge_stall_is_included(self):
        row = self.action()
        row["frames"].append({"sequence": 4, "present_ns": row["action_end_ns"] + 200_000_000,
                              "content_id": "next"})
        row["action_end_ns"] += gate.STOP_NS
        result = self.result()
        self.assertEqual(result["physical"]["ge_100_ms_operation_stops"], 1)
        self.assertEqual(result["physical"]["sample_count"], 3)
        self.assertEqual(result["status"], "FAIL")

    def test_f07_has_separate_software_denominator(self):
        row = self.action()
        row["f07"].append({"sequence": 2, "pre_create_ns": 0, "pipeline_ns": 2_000_000})
        result = self.result()
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["f07_pre_create"]["pre_create_share_pct"], 75.0)
        self.assertEqual(result["physical"]["max_ms"], 8.3)

    def test_physical_stall_is_not_hidden_by_zero_software_wait(self):
        self.set_intervals(self.action(), [gate.STOP_NS])
        self.action()["f07"][0]["pre_create_ns"] = 0
        result = self.result()
        self.assertEqual(result["f07_pre_create"]["pre_create_share_pct"], 0.0)
        self.assertEqual(result["status"], "FAIL")

    def test_capturing_only_repeating_same_content_cannot_pass(self):
        self.action()["frames"][1]["content_id"] = self.action()["frames"][0]["content_id"]
        with self.assertRaisesRegex(ValueError, "repeated content"):
            self.evaluate()

    def test_physical_timestamp_sequence_must_be_contiguous(self):
        self.action()["frames"][2]["sequence"] += 1
        with self.assertRaisesRegex(ValueError, "gapped"):
            self.evaluate()

    def test_software_zero_stall_and_gpu_jobs_cannot_replace_frames(self):
        row = self.action()
        row["frames"] = []
        row["f07"][0]["pre_create_ns"] = 0
        with self.assertRaisesRegex(ValueError, "physical content timestamps"):
            self.evaluate()

    def test_single_folder_substitution_refused(self):
        row = self.action("dense_liquid_glass_pages", "page_move")
        row.update(motion_kind="folder_open", single_folder_opened=True)
        with self.assertRaisesRegex(ValueError, "dense Liquid Glass"):
            self.evaluate()

    def test_whole_page_movement_is_valid(self):
        self.action("dense_liquid_glass_pages", "page_move")["motion_kind"] = "whole_page_motion"
        self.assertEqual(self.evaluate()["status"], "PASS")

    def test_test_end_must_be_covered_by_runtime_log(self):
        self.run_doc["runtime_log"]["end_ns"] = 2_100_000_000
        with self.assertRaisesRegex(ValueError, "cover boot"):
            self.evaluate()

    def test_actual_legacy_load_rejected_even_with_zero_counters(self):
        self.run_doc["runtime_log"]["loaded_images"].append("/usr/lib/libVULKAN_freedreno.dylib")
        report = self.evaluate()
        self.assertTrue(report["device_verified"])
        self.assertFalse(report["hardware_acceptance"])
        self.assertEqual(len(report["runtime"]["legacy_loaded_images"]), 1)

    def test_native_load_must_be_present(self):
        self.run_doc["runtime_log"]["loaded_images"] = []
        with self.assertRaisesRegex(ValueError, "native plugin"):
            self.evaluate()

    def test_independent_boot_sha_must_match(self):
        self.boot = "ab" * 32
        with self.assertRaisesRegex(ValueError, "boot image SHA-256 mismatch"):
            self.evaluate()

    def test_runtime_log_from_other_boot_rejected(self):
        self.run_doc["runtime_log"]["boot_id"] = "00000000-0000-4000-9000-000000000013"
        with self.assertRaisesRegex(ValueError, "runtime log boot_id mismatch"):
            self.evaluate()

    def test_regression_log_from_other_boot_rejected(self):
        self.run_doc["regression_log"]["boot_sha256"] = "cd" * 32
        with self.assertRaisesRegex(ValueError, "regression log boot_sha256 mismatch"):
            self.evaluate()


def reject_scene(self, scene):
    del self.run_doc["scenes"][scene]
    with self.assertRaisesRegex(ValueError, scene):
        self.evaluate()


for scene in gate.SCENES:
    add_case(AcceptanceTests, f"missing_{scene}_rejected", reject_scene, scene)


def fail_regression(self, group, item):
    self.run_doc["regression_log"][group][item] = "FAIL"
    report = self.evaluate()
    self.assertEqual(report["regressions"]["status"], "FAIL")
    self.assertFalse(report["hardware_acceptance"])


for group, names in gate.REGRESSIONS.items():
    for item in names:
        add_case(AcceptanceTests, f"{group}_{item}_regression_rejected", fail_regression, group, item)


def runtime_not_clean(self, field):
    self.run_doc["runtime_log"][field] = 1
    report = self.evaluate()
    self.assertEqual(report["runtime"]["status"], "FAIL")
    self.assertFalse(report["hardware_acceptance"])


for field in gate.RUNTIME_COUNTERS:
    add_case(AcceptanceTests, f"nonzero_{field}_rejected", runtime_not_clean, field)


def reject_action(self, field, value, reason):
    self.action()[field] = value
    with self.assertRaisesRegex(ValueError, reason):
        self.evaluate()


for name, field, value, reason in (
    ("software_only", "physical_source", "software_wait", "physical content"),
    ("refresh_counter_only", "physical_source", "dpu_autorefresh", "physical content"),
    ("gpu_jobs_only", "physical_source", "gpu_job_count", "physical content"),
    ("content_source_unconfirmed", "physical_source", "copy_complete", "physical content"),
    ("input_motion_unknown", "action_observer", "unknown", "continuity"),
    ("partial_hardware_capture", "capture_complete", False, "coverage"),
    ("lost_physical_updates", "lost_records", 1, "lost records"),
    ("wrong_boot", "boot_id", "00000000-0000-4000-9000-000000000013", "boot_id mismatch"),
    ("wrong_timebase", "timebase", "wall_clock", "timebase mismatch"),
    ("action_extends_beyond_capture", "action_end_ns", 1_200_000_000, "bracket"),
    ("no_f07_evidence", "f07", [], "F07"),
    ("unmatched_f07", "f07", [{"sequence": 50, "pre_create_ns": 2, "pipeline_ns": 3}], "F07"),
    ("invalid_f07_share", "f07", [{"sequence": 1, "pre_create_ns": 3, "pipeline_ns": 2}], "F07"),
):
    add_case(AcceptanceTests, name + "_rejected", reject_action, field, value, reason)


class EvidenceCliTests(LocalScratch):
    def setUp(self):
        super().setUp()
        self.run_doc, _ = gate.host_fixture()
        self.boot_image = self.directory / "boot-fixture.img"
        self.boot_image.write_bytes(b"Wave 12 synthetic boot fixture")

    def write_evidence(self, source):
        run = deepcopy(self.run_doc)
        run["source_kind"] = source
        for name, actions in run["scenes"].items():
            for action, row in actions.items():
                row["source_kind"] = source
                actions[action] = reference(self.directory / f"{name}-{action}.json", cut.json_output(row).encode())
        for name in ("runtime_log", "regression_log"):
            run[name]["source_kind"] = source
            run[name] = reference(self.directory / (name + ".json"), cut.json_output(run[name]).encode())
        path = self.directory / "run.json"
        path.write_text(cut.json_output(run), encoding="utf-8")
        return str(path)

    def device_cli(self, evidence):
        return cli(gate.main, "--evidence", evidence, "--boot-image", str(self.boot_image))

    def test_self_check_never_promotes(self):
        code, report = cli(gate.main, "--self-check")
        self.assertEqual(code, 0)
        self.assertEqual(report["mode"], "host_self_check")
        self.assertFalse(report["device_verified"])
        self.assertFalse(report["hardware_acceptance"])

    def test_explicit_self_check_refuses_boot_image(self):
        code, report = cli(gate.main, "--self-check", "--boot-image", str(self.boot_image))
        self.assertEqual(code, 1)
        self.assertFalse(report["device_verified"])
        self.assertFalse(report["hardware_acceptance"])

    def test_missing_boot_image_fails_closed(self):
        code, report = cli(gate.main, "--evidence", self.write_evidence(gate.DEVICE_CAPTURE))
        self.assertEqual(code, 1)
        self.assertFalse(report["device_verified"])
        self.assertFalse(report["hardware_acceptance"])

    def test_host_labeled_evidence_fails_device_mode(self):
        code, report = self.device_cli(self.write_evidence(gate.HOST_FIXTURE))
        self.assertEqual(code, 1)
        self.assertFalse(report["device_verified"])
        self.assertFalse(report["hardware_acceptance"])

    def test_device_branch_checks_all_digest_pinned_fixture_files(self):
        code, report = self.device_cli(self.write_evidence(gate.DEVICE_CAPTURE))
        self.assertEqual(code, 0, report)
        self.assertTrue(report["device_verified"])
        self.assertTrue(report["hardware_acceptance"])

    def test_raw_capture_tamper_fails_closed(self):
        evidence = self.write_evidence(gate.DEVICE_CAPTURE)
        (self.directory / "settings_scroll-scroll.json").write_text("{}", encoding="utf-8")
        code, report = self.device_cli(evidence)
        self.assertEqual(code, 1)
        self.assertIn("SHA-256 mismatch", report["error"])
        self.assertFalse(report["device_verified"])
        self.assertFalse(report["hardware_acceptance"])

    def test_independent_image_tamper_fails_closed(self):
        evidence = self.write_evidence(gate.DEVICE_CAPTURE)
        self.boot_image.write_bytes(b"different image")
        code, report = self.device_cli(evidence)
        self.assertEqual(code, 1)
        self.assertIn("boot image SHA-256 mismatch", report["error"])
        self.assertFalse(report["device_verified"])
        self.assertFalse(report["hardware_acceptance"])


class SystemTargetsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = cut.parse_json((REPO / "tools/mac_ci/system_cutover_targets.json").read_bytes())
        cls.targets = cls.doc["targets"]

    def test_wave_and_target_triple(self):
        self.assertEqual((self.doc["schema_version"], self.doc["wave"], self.doc["target_triple"]),
                         (1, 12, "arm64e-apple-ios27.0"))

    def test_discovery_Metadata(self):
        bundle = self.targets["NativeAdrenoMetal.bundle"]
        self.assertEqual(bundle["build_artifact"], "NativeAdrenoMetal.bundle")
        self.assertEqual(bundle["phase6_artifact"], "Adreno840Metal.bundle")
        self.assertEqual(bundle["info_plist"], cut.BUNDLE_INFO)
        self.assertEqual(bundle["discovery"],
                         {"MetalPluginName": "NativeAdrenoMetal", "MetalPluginClassName": "NativeAdrenoMetalDevice"})
        self.assertEqual(bundle["gpuaddon_path"], cut.BUNDLE)
        self.assertEqual(bundle["install_path"], cut.BUNDLE_INSTALL_PATH)
        self.assertEqual(bundle["publish_marker"]["gpuaddon_path"], cut.PUBLISH)

    def test_daemon_paths_and_KiB_match_packager(self):
        daemon = self.targets["NezhaShaderCompiler"]
        self.assertEqual(daemon["install_path"], cut.DAEMON_EXECUTABLE)
        self.assertEqual(daemon["launch_daemon"]["install_path"], cut.DAEMON_PLIST)
        self.assertEqual(daemon["launch_daemon"]["gpuaddon_path"], cut.DAEMON_ADDON_PLIST)
        self.assertEqual(daemon["launch_daemon"]["HardResourceLimits"], {"MemoryLimit": cut.MEMORY_LIMIT_KIB})
        self.assertEqual(daemon["launch_daemon"]["memory_limit_unit"], "KiB")

    def test_pack_paths_and_readonly_mode_match_packager(self):
        pack = self.targets["pack.bin"]
        self.assertEqual((pack["install_path"], pack["gpuaddon_path"], pack["mode"]),
                         (cut.PACK_PATH, cut.PACK_MEMBER, "0444"))
        self.assertEqual(pack["source_launchd_pack_arg"], cut.SOURCE_PACK_PATH)
        self.assertEqual(pack["install_path"], "/System/Library/Extensions/NativeAdrenoMetal.pack.bin")

    def test_new_compile_sources_cover_all_three_lanes(self):
        sources = {Path(path).name for target in self.targets.values() for path in target.get("new_sources", [])}
        self.assertEqual(sources, {"a840_xnu_system_bridge.cpp", "a840_system_cache_store.c",
                                   "a840_system_pack_builder.c", "A840MTLSystemSPI.mm",
                                   "A840MTLHeapAndEvents.mm", "A840MTLLibraryAndArchive.mm",
                                   "A840MTLScanoutBridge.mm", "a840_system_frontend_core.c"})
        self.assertEqual(Path(self.targets["NezhaShaderCompiler"]["launch_daemon"]["source"]).name,
                         "org.unixnu.nezha.shader-compiler.plist")

    def test_manifest_lists_updated_entrypoints(self):
        self.assertIn("drivers/Adreno840Kernel/xnu/NativeAdrenoV3Glue.cpp",
                      self.targets["NativeAdrenoKMDv3"]["updated_sources"])
        self.assertIn("daemon/NezhaShaderCompiler/main.c",
                      self.targets["NezhaShaderCompiler"]["updated_sources"])
        plugin = self.targets["NativeAdrenoMetal.bundle"]["updated_sources"]
        self.assertIn("drivers/Adreno840Metal/objc/A840MTLBundleEntry.mm", plugin)
        self.assertIn("drivers/Adreno840Metal/transport/a840_hwtest_hook.c", plugin)

    def test_manifest_lists_three_independent_host_tests(self):
        self.assertEqual({Path(path).name for path in self.doc["host_test_sources"]},
                         {"test_xnu_system_integration.cpp", "test_system_daemon_and_pack.cpp",
                          "test_system_frontend_bridge.cpp"})

    def test_device_test_and_signature_page_size(self):
        self.assertEqual(self.targets["NezhaGpuTest"]["gpuaddon_path"], cut.ARTIFACTS["gpu_test"][0])
        self.assertEqual(self.doc["code_signature_page_bytes"], 4096)


if __name__ == "__main__":
    unittest.main()
