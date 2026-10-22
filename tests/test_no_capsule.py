"""Run the real no-capsule runtime driver against mock UEFI service tables."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

UEFI = Path(os.environ.get("CAS_UEFI_ROOT", str(Path(__file__).resolve().parents[2] / "uefi-cas")))


class NoCapsule(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.program = Path(cls.temp.name) / "no-capsule"
        include = UEFI / "Mu_Basecore/MdePkg/Include"
        driver = UEFI / "Platforms/Xiaomi/casPkg/Drivers/CasNoCapsuleDxe"
        source = Path(__file__).parent / "fixtures/no_capsule_harness.c"
        result = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-fshort-wchar", "-I", str(include), "-I", str(include / "X64"),
                        "-I", str(driver), str(source), "-o", str(cls.program)],
                                text=True, capture_output=True)
        if result.returncode:
            raise AssertionError("host harness compilation failed:\n" + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.program), name], check=True, text=True,
                                capture_output=True, timeout=5)
        self.assertEqual(result.stdout.strip(), "PASS")

    def test_rejects_all_requests_without_reading_buffers_or_calling_services(self):
        self.run_case("reject")

    def test_runtime_pointers_are_set_before_null_interface_notification(self):
        self.run_case("install")

    def test_install_failure_restores_previous_runtime_functions(self):
        self.run_case("failure")


if __name__ == "__main__":
    unittest.main()
