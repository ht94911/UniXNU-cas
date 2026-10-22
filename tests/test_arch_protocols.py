"""Exercise missing architecture providers with deliberately synthetic sources."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import preflight as p


class ArchitectureProtocols(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.cas = self.root / "Platforms/Xiaomi/casPkg"
        self.cas.mkdir(parents=True)
        self.table = self.root / "Packages/TestPkg/Core.c"
        self.table.parent.mkdir(parents=True)
        self.table.write_text("EFI_CORE_PROTOCOL_NOTIFY_ENTRY mArchProtocols[] = {\n"
                             " { &gEfiCpuArchProtocolGuid, 0 },\n"
                             " { &gEfiCapsuleArchProtocolGuid, 0 },\n"
                             " { NULL, 0 }\n};\n")
        self.providers = {}
        for name in ("Cpu", "Capsule"):
            relative = f"TestPkg/{name}/{name}.inf"
            folder = self.root / "Packages" / "TestPkg" / name
            folder.mkdir()
            (folder / f"{name}.c").write_text("int synthetic_fixture;\n")
            (folder / f"{name}.inf").write_text(
                f"[Defines]\nBASE_NAME = {name}\nMODULE_TYPE = DXE_DRIVER\n"
                f"[Sources]\n{name}.c\n[Protocols]\ngEfi{name}ArchProtocolGuid ## PRODUCES\n")
            self.providers[f"gEfi{name}ArchProtocolGuid"] = relative
        self.dsc = self.cas / "cas.dsc"
        self.fdf = self.cas / "cas.fdf"
        self.dsc.write_text("[Components]\n" + "\n".join(self.providers.values()) + "\n")
        self.fdf.write_text("[FV.Main]\n" + "\n".join("INF " + s for s in self.providers.values()) + "\n")
        self.policy = {"package_roots": ["Packages", "Platforms/Xiaomi"],
                       "pinned_includes": [], "forbidden_prefixes": [],
                       "architecture_protocol_table": self.table.relative_to(self.root).as_posix(),
                       "required_architectural_protocols": self.providers.copy()}

    def audit(self):
        audit, _ = p.audit_descriptions(self.root, self.policy, [self.dsc, self.fdf])
        return p.check_architectural_protocols(self.root, self.policy, audit)

    def remove(self, path, name):
        path.write_text("\n".join(s for s in path.read_text().splitlines() if name not in s) + "\n")

    def test_all_core_requirements_have_actual_selected_providers(self):
        result = self.audit()
        self.assertEqual({s["protocol"] for s in result["providers"]}, set(self.providers))

    def test_missing_capsule_in_both_descriptions_is_rejected(self):
        for path in (self.dsc, self.fdf):
            self.remove(path, "Capsule")
        with self.assertRaisesRegex(p.Rejected, "missing architectural provider: gEfiCapsule"):
            self.audit()

    def test_fdf_and_dsc_omissions_are_distinguished(self):
        for path in (self.dsc, self.fdf):
            with self.subTest(path=path.name):
                original = path.read_text()
                self.remove(path, "Capsule")
                with self.assertRaisesRegex(p.Rejected, "DSC/FDF component mismatch"):
                    self.audit()
                path.write_text(original)

    def test_apriori_reference_is_not_a_fv_module(self):
        self.remove(self.fdf, "Capsule")
        with self.fdf.open("a") as stream:
            stream.write("APRIORI DXE {\nINF TestPkg/Capsule/Capsule.inf\n}\n")
        with self.assertRaisesRegex(p.Rejected, "DSC/FDF component mismatch"):
            self.audit()

    def test_library_reference_does_not_satisfy_provider_selection(self):
        for path in (self.dsc, self.fdf):
            self.remove(path, "Capsule")
        with self.dsc.open("a") as stream:
            stream.write("[LibraryClasses]\nFakeLib|TestPkg/Capsule/Capsule.inf\n")
        with self.assertRaisesRegex(p.Rejected, "missing architectural provider"):
            self.audit()

    def test_policy_must_match_actual_core_table(self):
        for action in ("remove", "add"):
            with self.subTest(action=action):
                mapping = self.providers.copy()
                if action == "remove":
                    mapping.pop("gEfiCapsuleArchProtocolGuid")
                else:
                    mapping["gEfiCpuIo2ProtocolGuid"] = mapping["gEfiCpuArchProtocolGuid"]
                self.policy["required_architectural_protocols"] = mapping
                with self.assertRaisesRegex(p.Rejected, "policy disagrees"):
                    self.audit()

    def test_provider_must_declare_the_protocol(self):
        inf = self.root / "Packages/TestPkg/Capsule/Capsule.inf"
        self.remove(inf, "gEfiCapsuleArchProtocolGuid")
        with self.assertRaisesRegex(p.Rejected, "does not declare"):
            self.audit()

    def test_table_parse_failure_cannot_silently_skip_the_gate(self):
        self.table.write_text("// mArchProtocols[] = { &gEfiCpuArchProtocolGuid };\n")
        with self.assertRaisesRegex(p.Rejected, "cannot read pinned DXE"):
            self.audit()


if __name__ == "__main__":
    unittest.main()
