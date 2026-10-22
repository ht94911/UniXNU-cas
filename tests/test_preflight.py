"""Offline rejection tests. Fixtures are not a real firmware or hardware result."""
import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import preflight as p


class PreflightTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cas = self.root / "Platforms/Xiaomi/casPkg"
        self.cas.mkdir(parents=True)
        self.source = self.root / "Packages/TestPkg/Minimal"
        self.source.mkdir(parents=True)
        (self.source / "Minimal.c").write_text("int fixture(void) { return 0; }\n")
        self.inf = self.source / "Minimal.inf"
        self.inf.write_text("[Sources]\nMinimal.c\n")
        self.entry = self.cas / "cas.dsc"
        self.entry.write_text("[Components]\nTestPkg/Minimal/Minimal.inf\n")
        self.policy = {"package_roots": ["Packages", "Platforms/Xiaomi"], "pinned_includes": [],
                       "forbidden_prefixes": ["Binaries/", "QcomPkg/Library/PlatformPeiLib/"]}

    def audit(self):
        return p.audit_descriptions(self.root, self.policy, [self.entry])

    def test_source_selection(self):
        report, _ = self.audit()
        self.assertEqual(len(report["modules"]), 1)

    def test_missing_file(self):
        with self.assertRaisesRegex(p.Rejected, "missing input"):
            p.verify_file(self.root, {"path": "missing", "sha256": "0" * 64})

    def test_hash_mismatch(self):
        with self.assertRaisesRegex(p.Rejected, "hash mismatch"):
            p.verify_file(self.cas, {"path": "cas.dsc", "sha256": "0" * 64})

    def test_version_mismatch_even_with_correct_hash(self):
        version = self.root / "version.json"
        version.write_text(json.dumps({"design_id": "wrong-board-001"}))
        with self.assertRaisesRegex(p.Rejected, "version/identity"):
            p.verify_file(self.root, {"path": "version.json", "sha256": p.digest(version),
                                      "json_fields": {"design_id": "cas-memory-001"}})

    def test_parent_escape(self):
        with self.assertRaisesRegex(p.Rejected, "unsafe input path"):
            p.contained(self.cas, "../../private.bin")

    def test_symlink_escape(self):
        (self.cas / "escape").symlink_to(self.source, target_is_directory=True)
        with self.assertRaisesRegex(p.Rejected, "escapes its root"):
            p.contained(self.cas, "escape/Minimal.c")

    def test_forbidden_vendor_policy(self):
        policy = {"schema_version": 1, "board": "cas", "firmware_mode": "source-only",
                  "vendor_inputs": [{"board": "pipa"}]}
        with self.assertRaisesRegex(p.Rejected, "no vendor binary"):
            p.check_inputs(policy, {})

    def test_excluded_scheduler(self):
        self.entry.write_text("[LibraryClasses]\nPlatformPeiLib|QcomPkg/Library/PlatformPeiLib/PlatformPeiLib.inf\n")
        with self.assertRaisesRegex(p.Rejected, "excluded dependency"):
            self.audit()

    def test_missing_module_source(self):
        (self.source / "Minimal.c").unlink()
        with self.assertRaisesRegex(p.Rejected, "missing module source"):
            self.audit()

    def test_literal_inf_macro_and_unknown_macro(self):
        self.inf.write_text("[Defines]\nDEFINE SOURCE_DIR = .\n[Sources]\n$(SOURCE_DIR)/Minimal.c\n")
        report, _ = self.audit()
        self.assertEqual(report["modules"][0]["source_macros"], {"SOURCE_DIR": "."})
        self.inf.write_text("[Sources]\n$(UNRESOLVED_DIR)/Minimal.c\n")
        with self.assertRaisesRegex(p.Rejected, "computed source"):
            self.audit()

    def test_binary_inf_even_with_source(self):
        self.inf.write_text("[Sources]\nMinimal.c\n[Binaries.AARCH64]\nPE32|vendor.efi\n")
        with self.assertRaisesRegex(p.Rejected, "binary INF"):
            self.audit()

    def test_direct_binary_fdf(self):
        self.entry.write_text("[FV.Main]\nSECTION PE32 = vendor.efi\n")
        with self.assertRaisesRegex(p.Rejected, "direct binary input"):
            self.audit()

    def test_unreviewed_conditional_cannot_hide_input(self):
        self.entry.write_text("!if $(VENDOR) == 0\n[Components]\nTestPkg/Minimal/Minimal.inf\n!endif\n")
        with self.assertRaisesRegex(p.Rejected, "directive requires review"):
            self.audit()

    def test_include_cycle(self):
        self.entry.write_text("!include next.inc\n")
        (self.cas / "next.inc").write_text("!include cas.dsc\n")
        with self.assertRaisesRegex(p.Rejected, "include cycle"):
            self.audit()

    def test_trusted_unused_binary_rule_is_not_a_blob(self):
        rules = self.root / "Packages/TestPkg/Rules.inc"
        rules.write_text("[Rule.Common.UEFI_APPLICATION.BINARY]\nFILE APPLICATION = $(NAMED_GUID) {\nPE32 PE32 |.efi\n}\n")
        self.policy["pinned_includes"] = [{"path": "Packages/TestPkg/Rules.inc", "sha256": p.digest(rules)}]
        with self.entry.open("a") as stream:
            stream.write("!include TestPkg/Rules.inc\n")
        report, _ = self.audit()
        self.assertEqual(len(report["modules"]), 1)
        rules.write_text(rules.read_text() + "# changed rules\n")
        with self.assertRaisesRegex(p.Rejected, "unreviewed external include"):
            self.audit()

    def test_missing_and_oversized_artifacts(self):
        memory_path = Path(__file__).resolve().parents[1] / "config/memory-map.json"
        memory = json.loads(memory_path.read_text())
        with self.assertRaisesRegex(p.Rejected, "requires existing"):
            p.check_artifacts(memory, {})
        files = {}
        for name, size in {"fd": 0x800001, "dtb": 40, "shim": 512, "ramdisk": 0, "boot_image": 4096}.items():
            file = self.root / name
            with file.open("wb") as stream:
                stream.truncate(size)
            files[name] = file
        with self.assertRaisesRegex(ValueError, "fd size violates"):
            p.check_artifacts(memory, files)


if __name__ == "__main__":
    unittest.main()
