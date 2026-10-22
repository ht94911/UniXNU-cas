"""Run the real key state machine and SPMI reader with synthetic time/MMIO.

Callback writes are recorded in Python RAM; no device address is dereferenced.
"""
import ctypes as c
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

UEFI = Path(os.environ.get("CAS_UEFI_ROOT", str(Path(__file__).resolve().parents[2] / "uefi-cas")))
U, Q = c.c_uint32, c.c_uint64
READ = c.CFUNCTYPE(U, c.c_void_p, Q)
WRITE = c.CFUNCTYPE(None, c.c_void_p, Q, U)
DELAY = c.CFUNCTYPE(None, c.c_void_p, U)


class Bus(c.Structure):
    _fields_ = [("read", READ), ("write", WRITE), ("delay", DELAY), ("context", c.c_void_p)]


class Spmi(c.Structure):
    _fields_ = [(key, U) for key in "version ready pon gpio status address".split()]


class Keys(c.Structure):
    _fields_ = [(key, U) for key in "initialized armed raw stable power_long repeating".split()] + [
        (key, Q) for key in "now raw_since press_since repeat_since".split()]


class Input(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        library = Path(cls.temp.name) / "input.so"
        root = UEFI / "Platforms/Xiaomi/casPkg"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
            "-I", str(root / "Include"), str(root / "Library/CasInputLib/CasSpmi.c"),
            str(root / "Library/CasInputLib/CasKeys.c"), "-o", str(library)], check=True, capture_output=True)
        cls.lib = c.CDLL(str(library))
        cls.init = cls.lib.CasSpmiInitialize; cls.init.argtypes = [c.POINTER(Bus), c.POINTER(Spmi)]; cls.init.restype = U
        cls.read = cls.lib.CasSpmiReadByte; cls.read.argtypes = [c.POINTER(Bus), c.POINTER(Spmi), U, c.POINTER(U)]; cls.read.restype = U
        cls.update = cls.lib.CasKeysUpdate; cls.update.argtypes = [c.POINTER(Keys), U, Q]; cls.update.restype = U
        cls.sample = cls.lib.CasKeySample; cls.sample.argtypes = [U, U]; cls.sample.restype = U

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def setUp(self):
        self.keys = Keys(); self.spmi = Spmi()
        self.maps = {4: 0x00800, 17: 0x0c500}
        self.owners = {4: 1, 17: 0}
        self.version, self.status, self.value = 0x50010000, 1, 0xa5
        self.writes, self.reads, self.invalid = [], [], []
        self.delays = 0

        def read(context, addr):
            self.reads.append(addr)
            if addr == 0xc440000: return self.version
            if 0xc440900 <= addr < 0xc441100 and addr % 4 == 0: return self.maps.get((addr - 0xc440900) // 4, 0)
            if 0xc40a700 <= addr < 0xc40af00 and addr % 4 == 0: return self.owners.get((addr - 0xc40a700) // 4, 0)
            if self.writes:
                if addr == self.writes[-1][0] + 8: return self.status
                if addr == self.writes[-1][0] + 0x18: return self.value
            self.invalid.append(addr); return 0

        def write(context, addr, value): self.writes.append((addr, value))
        def delay(context, value): self.delays += value
        self.bus = Bus(READ(read), WRITE(write), DELAY(delay), None)

    def tearDown(self): self.assertEqual(self.invalid, [])

    def ready(self): self.assertEqual(self.init(c.byref(self.bus), c.byref(self.spmi)), 0)
    def get(self, address):
        out = U(0xdeadbeef)
        code = self.read(c.byref(self.bus), c.byref(self.spmi), address, c.byref(out))
        return code, out.value
    def tick(self, raw, time): return self.update(c.byref(self.keys), raw, time)
    def arm(self): self.assertEqual(self.tick(0, 0), 0); self.assertEqual(self.tick(0, 20), 0)

    def test_version_gate_before_any_command(self):
        for version in (0, 0x30000000, 0x70000000, 0xffffffff):
            self.version = version
            self.assertEqual(self.init(c.byref(self.bus), c.byref(self.spmi)), 1)
        self.assertEqual(self.writes, [])
        self.assertEqual(set(self.reads), {0xc440000})

    def test_missing_mapping_does_not_issue_command(self):
        self.maps.pop(17)
        self.assertEqual(self.init(c.byref(self.bus), c.byref(self.spmi)), 2)
        self.assertEqual(self.get(0xc510)[0], 1)
        self.assertEqual(self.writes, [])

    def test_mapping_prefers_ee0_and_stays_inside_dt(self):
        self.maps.update({9: 0x00800, 12: 0x1000800, 511: 0x00800})
        self.owners.update({9: 0, 12: 1, 511: 0})
        self.ready()
        self.assertEqual((self.spmi.pon, self.spmi.gpio), (511, 17))
        self.assertEqual(self.get(0x810), (0, 0xa5))
        self.assertEqual(self.writes, [(0xe600000 + 511 * 0x80, 0x08000100)])

    def test_gpio_read_command_and_byte_mask(self):
        self.ready(); self.value = 0x123456ab
        self.assertEqual(self.get(0xc510), (0, 0xab))
        self.assertEqual(self.writes, [(0xe600000 + 17 * 0x80, 0x08000100)])

    def test_forbidden_address_and_invalid_apid(self):
        self.ready()
        for address in (0x800, 0x850, 0x1840, 0x1c510, 0xffffffff): self.assertEqual(self.get(address)[0], 3)
        self.spmi.pon = 512; self.assertEqual(self.get(0x810)[0], 2)
        self.assertEqual(self.writes, [])

    def test_reset_registers_are_read_only_commands(self):
        self.ready()
        for address in range(0x840, 0x84c): self.assertEqual(self.get(address)[0], 0)
        self.assertTrue(all(value >> 27 == 1 and addr == 0xe600200 for addr, value in self.writes))

    def test_denied_failed_and_timeout_never_return_stale_data(self):
        self.ready()
        for status, code in ((5, 4), (3, 5), (9, 5), (0, 6)):
            self.status = status; self.assertEqual(self.get(0x810), (code, 0))
        self.assertEqual(self.delays, 1000)

    def test_button_polarity(self):
        self.assertEqual(self.sample(0, 1), 0)
        self.assertEqual(self.sample(1, 1), 4)
        self.assertEqual(self.sample(2, 1), 2)
        self.assertEqual(self.sample(0, 0), 1)

    def test_held_key_at_entry_is_suppressed_until_release(self):
        for time in (0, 20, 500, 1500): self.assertEqual(self.tick(4, time), 0)
        self.assertEqual(self.tick(0, 1600), 0); self.assertEqual(self.tick(0, 1620), 0)
        self.assertEqual(self.tick(4, 1700), 0); self.assertEqual(self.tick(4, 1720), 0)
        self.assertEqual(self.tick(0, 1800), 0); self.assertEqual(self.tick(0, 1820), 3)

    def test_bounce_and_navigation_repeat(self):
        self.arm()
        for raw, time in ((1, 100), (0, 105), (1, 110), (1, 129)): self.assertEqual(self.tick(raw, time), 0)
        self.assertEqual(self.tick(1, 130), 1)
        self.assertEqual(self.tick(1, 629), 0); self.assertEqual(self.tick(1, 630), 1)
        self.assertEqual(self.tick(1, 749), 0); self.assertEqual(self.tick(1, 750), 1)
        self.assertEqual(self.tick(1, 10000), 1)
        self.assertEqual(self.tick(1, 10001), 0)  # no catch-up event burst

    def test_power_short_press_only_on_release(self):
        self.arm()
        for raw, time in ((4, 100), (4, 120), (4, 500), (0, 510), (0, 529)): self.assertEqual(self.tick(raw, time), 0)
        self.assertEqual(self.tick(0, 530), 3)
        self.assertEqual(self.tick(0, 900), 0)

    def test_power_long_press_once_and_no_confirm_on_release(self):
        self.arm()
        self.tick(4, 100); self.tick(4, 120)
        self.assertEqual(self.tick(4, 819), 0); self.assertEqual(self.tick(4, 820), 4)
        self.assertEqual(self.tick(4, 3000), 0)
        self.tick(0, 3010); self.assertEqual(self.tick(0, 3030), 0)

    def test_chord_flushes_and_needs_all_keys_released(self):
        self.arm(); self.tick(2, 100); self.assertEqual(self.tick(2, 120), 2)
        self.assertEqual(self.tick(6, 200), 5)
        for raw, time in ((4, 220), (4, 240), (4, 1000), (0, 1020), (0, 1040)): self.assertEqual(self.tick(raw, time), 0)
        self.tick(2, 1100); self.assertEqual(self.tick(2, 1120), 2)

    def test_error_or_clock_rewind_cannot_generate_ghost_confirm(self):
        self.arm(); self.tick(4, 100); self.tick(4, 120)
        self.assertEqual(self.tick(0xffffffff, 150), 5)
        self.assertEqual(self.tick(0, 160), 0); self.assertEqual(self.tick(0, 180), 0)
        self.tick(4, 200); self.tick(4, 220)
        self.assertEqual(self.tick(0, 10), 0); self.assertEqual(self.tick(0, 30), 0)
