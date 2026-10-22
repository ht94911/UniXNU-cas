"""Run the production shim validator object at EL1/EL2, with the MMU off.

This catches ARM alignment faults invisible to the x86 host C tests. The FDT
is synthetic and the emulator does not access a phone or Qualcomm registers.
QEMU and the shim compiler are required for this architecture regression.
"""
import contextlib
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UEFI = Path(os.environ.get("CAS_UEFI_ROOT", str(ROOT.parent / "uefi-cas")))
sys.path.insert(0, str(ROOT / "scripts"))
from build_shim import build


def memory_reg_offset(raw):
    """Locate the fixture's memory property without assuming its byte offset."""
    at, strings = struct.unpack_from(">II", raw, 8)
    stack = []
    while True:
        token = struct.unpack_from(">I", raw, at)[0]
        at += 4
        if token == 1:
            end = raw.index(0, at)
            stack.append(raw[at:end].decode())
            at = (end + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, name = struct.unpack_from(">II", raw, at)
            at += 8
            name = raw[strings + name:raw.index(0, strings + name)].decode()
            if stack == ["", "memory"] and name == "reg":
                return at
            at = (at + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            raise ValueError("fixture has no /memory/reg")
        else:
            raise ValueError("invalid fixture token")


class Arm64ShimValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("qemu-system-aarch64") is None:
            raise RuntimeError("qemu-system-aarch64 is required for the ARM64 entry regression")
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        cls.addClassCleanup(cls.temp.cleanup)
        with contextlib.redirect_stdout(io.StringIO()):
            build(UEFI, cls.directory / "shim")
        cls.tree = cls.directory / "fixture.dtb"
        subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(cls.tree),
                        str(UEFI / "Resources/DTBs/cas.dts")], check=True, capture_output=True)
        if memory_reg_offset(cls.tree.read_bytes()) % 8 == 0:
            # A zero-length property consumes 12 bytes and switches mod-8
            # alignment while preserving a valid, four-byte-aligned FDT.
            subprocess.run(["fdtput", str(cls.tree), "/", "probe-alignment-pad"],
                           check=True, capture_output=True)
        assert memory_reg_offset(cls.tree.read_bytes()) % 8 == 4
        cls.bad_tree = cls.directory / "invalid-magic.dtb"
        raw = bytearray(cls.tree.read_bytes())
        raw[0] ^= 1
        cls.bad_tree.write_bytes(raw)
        cls.executables = {}
        fixture = ROOT / "tests/fixtures/arm64-validator"
        for alignment in (0, 1):
            obj = cls.directory / f"probe-{alignment}.o"
            elf = cls.directory / f"probe-{alignment}.elf"
            subprocess.run(["clang", "--target=aarch64-none-elf", f"-DPROBE_ALIGNMENT_CHECK={alignment}",
                            "-c", str(fixture / "entry.S"), "-o", str(obj)], check=True, capture_output=True)
            subprocess.run(["ld.lld", "-T", str(fixture / "probe.lds"), str(obj),
                            str(cls.directory / "shim/CasBootValidate.o"), "-o", str(elf)],
                           check=True, capture_output=True)
            cls.executables[alignment] = elf

    def execute(self, el, alignment, tree, expected):
        command = ["qemu-system-aarch64", "-nodefaults", "-nic", "none", "-machine",
                   f"virt,secure=off,virtualization={'on' if el == 2 else 'off'}",
                   "-cpu", "cortex-a76", "-m", "2048M", "-display", "none", "-serial", "none",
                   "-monitor", "none", "-semihosting-config", "enable=on,target=native",
                   "-device", f"loader,file={self.executables[alignment]},cpu-num=0",
                   "-device", f"loader,file={tree},addr=0xafdfe000,force-raw=on", "-no-reboot"]
        run = subprocess.run(command, capture_output=True, text=True, timeout=15)
        output = run.stdout + run.stderr
        self.assertEqual(run.returncode, 0, output)
        fields = {key: int(value, 16) for key, value in re.findall(r"^(\w+)=([0-9a-f]{16})$", output, re.M)}
        self.assertNotIn("ESR", fields, output)
        self.assertEqual(fields.get("EL"), el * 4, output)
        self.assertEqual(fields["SCTLR"] & 0x1007, alignment * 2, output)
        self.assertEqual(fields.get("CRESULT"), expected, output)

    def test_four_byte_aligned_u64_fields_with_mmu_off(self):
        for el in (1, 2):
            for alignment in (0, 1):
                with self.subTest(el=el, alignment=alignment):
                    self.execute(el, alignment, self.tree, 0)

    def test_invalid_tree_still_rejected_with_alignment_check(self):
        for el in (1, 2):
            with self.subTest(el=el):
                self.execute(el, 1, self.bad_tree, 3)


if __name__ == "__main__":
    unittest.main()
