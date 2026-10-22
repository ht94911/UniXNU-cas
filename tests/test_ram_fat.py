"""FAT output is checked with an independent filesystem reader, not our writer."""
import json,struct,subprocess,sys,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/"scripts"))
from make_ram_fat import build
class RamFat(unittest.TestCase):
    def setUp(self):
        self.config=(ROOT/"config/unixnu.toml").read_bytes()
        self.program=bytearray(1024);self.program[:2]=b"MZ";struct.pack_into("<I",self.program,60,128)
        self.program[128:132]=b"PE\0\0";struct.pack_into("<H",self.program,132,0xaa64);struct.pack_into("<H",self.program,220,10)
    def test_determinism_and_independent_file_readback(self):
        a=build(self.config,bytes(self.program));self.assertEqual(a,build(self.config,bytes(self.program)))
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);disk=root/"disk.img";disk.write_bytes(a)
            subprocess.run(["fsck.fat","-n",str(disk)],check=True,capture_output=True)
            for path,expected,name in [("::/EFI/UniXNU/unixnu.toml",self.config,"config"),("::/EFI/Tools/CasSmoke.efi",self.program,"efi")]:
                subprocess.run(["mcopy","-i",str(disk),path,str(root/name)],check=True,capture_output=True)
                self.assertEqual((root/name).read_bytes(),expected)
    def test_rejects_wrong_machine_and_subsystem(self):
        for offset in (132,220):
            bad=self.program.copy();struct.pack_into("<H",bad,offset,0)
            with self.assertRaises(ValueError):build(self.config,bytes(bad))
    def test_rejects_automatic_boot_and_xnu_config(self):
        for a,b in [(b"timeout = -1",b"timeout = 0"),(b'type = "efi"',b'type = "xnu"')]:
            with self.assertRaises(ValueError):build(self.config.replace(a,b),bytes(self.program))
