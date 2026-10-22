"""Actual RAM BlockIO callbacks with real UEFI definitions and host memory."""
import os,subprocess,tempfile,unittest
from pathlib import Path
UEFI=Path(os.environ.get("CAS_UEFI_ROOT",str(Path(__file__).resolve().parents[2]/"uefi-cas")))
class RamDisk(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        cls.exe=Path(cls.tmp.name)/"ramdisk"
        inc=UEFI/"Mu_Basecore/MdePkg/Include";pkg=UEFI/"Platforms/Xiaomi/casPkg"
        subprocess.run(["cc","-O2","-Wall","-Wextra","-Werror","-Wno-unused-parameter","-DMDEPKG_NDEBUG","-fshort-wchar","-I",str(inc),"-I",str(inc/"X64"),"-I",str(pkg/"Include"),"-I",str(pkg/"Drivers/CasRamDiskDxe"),str(Path(__file__).parent/"fixtures/ramdisk_harness.c"),"-o",str(cls.exe)],check=True)
    def run_case(self,name):
        result=subprocess.run([str(self.exe),name],check=True,text=True,capture_output=True,timeout=5)
        self.assertEqual(result.stdout.strip(),"PASS")
    def test_info_rejects_wrong_bounds_identity_and_unaligned_length(self):self.run_case("info")
    def test_io_bounds_and_read_only(self):self.run_case("io")
    def test_installs_only_valid_fat_ram_payload(self):self.run_case("install")
