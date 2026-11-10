# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Version-locked native iOS 27 Bluetooth routing and image metadata."""
import hashlib
import importlib.util
import struct
from pathlib import Path

_spec = importlib.util.spec_from_file_location('qbt_inspect_ios', Path(__file__).with_name('inspect-ios.py'))
_inspect = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_inspect)

ORIGINAL_SHA256 = 'cfd94854d0a2efd3ca4a2de6e55349f43fd0d274a4b8bfa9937f29d9f7208cb1'
ORIGINAL_UUID = '8E49197B-E6BC-3F3B-A8D3-DF4BEDD37F01'
BASE = 0x100000000
CAVE = 0x100b9bba0
CAVE_END = 0x100b9c000
DESCRIPTORS = 0x100c1ce88
DESCRIPTOR_COUNT = 0x118
SUPPORT_TABLE = 0x100cadc50
SUPPORT_BYTES = 64
LE_FEATURES = 0x100cadb2e
FACTORIES = (0x10050a9dc, 0x10050bf00, 0x10050bf48)
CONTROLLER_THUNK = 0x1004a5290
CONTROLLER_INIT = 0x1004a4560
VIRTUAL_MACHINE_CHECK = 0x10056a12c
UNKNOWN_PRODUCT_CHECK = 0x10050c204
HOST_BUFFER_STATUS = 0x10032affc
HOST_FLOW_UNSUPPORTED = 0x10032b034
ERRONEOUS_STATUS = 0x10032b0f0
ADDRESS_REFRESH = 0x1003a77e8
ADV_ADDRESS_CACHED = 0x1000dd7bc
ADDRESS_FALLBACK = 0x1003a7bfc
ADDRESS_FINISH = 0x1003a7b64
ADDRESS_RETURN = 0x1003a7b78
COMMAND_SUBMIT = 0x1000648b8
SET_RANDOM_ADDRESS = 0xa5
REGISTER = 0x100377d28
REGISTERED_CHIP = 0x100c1e994
REGISTERED_CONTEXT = 0x100c2c238
VENDOR_INIT = 0x100377b60
VENDOR_CHIP = 0x100c2b610
VENDOR_OPS = 0x100c2b720
VENDOR_OPS_BYTES = 0xb08
VENDOR_HOOKS = (0x100c2ae40, 0x100c2ae60, 0x100c2aa60, 0x100c2c228)
VENDOR_OPCODES = 0x100c2c230
CALLBACKS = ((0x100377e9c, 0x100377ec0, 0x100377fa8, 0x100c2c278),
             (0x100377fc4, 0x100377fe8, 0x100378098, 0x100c2c428))
OPTIONAL_STEP = 0x100329ec8
CHAIN_NEXT = 0x1003d7848
CHAIN_CONTEXT = 0x100c2a9f0
MODEL_GETTER = 0x1000071f0
MODEL_VTABLE = 0x100bafcf8
# Model slots traced through the target's configuration getters and their logs.
MODEL_CAPABILITIES = dict(ext_adv=0xa30, periodic_adv=0xa38, periodic_sync=0xa40,
    past_sender=0xa48, past_recipient=0xa50, ext_scan=0xac0, enhanced_connection=0xac8,
    spec_adv=0xad8, coded_phy=0xba8, phy_2m=0xbb8, adv_sets=0x1138)
H4_PRESTOP = 0x1003083cc
ROOT_KEY_STORE_CHECK = 0x10085ebcc
ROOT_KEY_FAILURE = 0x10085ec98
ROOT_KEY_LOAD_RESULT = 0x10085ed1c
ROOT_KEY_READS = (0x10085e6bc, 0x10085e778, 0x10085e830, 0x10085e880, 0x10085e90c, 0x10085ec00)
ROOT_KEY_WRITES = (0x10085e724, 0x10085e7dc, 0x10085e94c, 0x10085ea28, 0x10085ea6c, 0x10085eb14,
                   0x10085eb9c, 0x10085ec70)
NC_ROOT_KEY_READS = (0x10085dd04, 0x10085dda8)
NC_ROOT_KEY_WRITES = (0x10085dedc, 0x10085dfa4)
TRANSPORT = 0x100c1cdf8
H4_TABLE = 0x100c1cd50
NATIVE_STATE = 0x100caff80
NATIVE_STATE_BYTES = 128
NATIVE_DYLIB = b'/usr/lib/QcaBTType0.dylib'
ADV_START = 0x1007ce390
ADV_MODE = 0x90
ADV_CAPACITY = 6  # 0x1003892f8..300: seven-byte records, memset_s bound 42
CAP_OFFSETS = {name: 16 + index for index, name in enumerate(MODEL_CAPABILITIES) if name != 'adv_sets'}
CAP_OFFSETS['adv_sets'] = 28
NATIVE_BINDINGS = dict(STATE=NATIVE_STATE, STATE_BYTES=NATIVE_STATE_BYTES,
    DESCRIPTORS=DESCRIPTORS, DESCRIPTOR_COUNT=DESCRIPTOR_COUNT,
    SUPPORT_TABLE=SUPPORT_TABLE, SUPPORT_BYTES=SUPPORT_BYTES, LE_FEATURES=LE_FEATURES,
    SUBMIT=0x1000648b8, ADVANCE=0x10032702c, CHAIN_FAIL=0x1003d793c,
    CHAIN_CONTEXT=CHAIN_CONTEXT, INIT_SUBMIT_FLAG=0x100cadc34, MODEL=MODEL_GETTER,
    SET_EXT_ADV=0x10038987c, SET_SPEC_ADV=0x100389870, SET_ADV_HANDLES=0x100389898,
    SET_ENH_CONNECTION=0x100398f60, SET_EXT_SCAN=0x1003af20c, SET_EXT_SCAN_HCI=0x1003af198,
    SET_CODED_SCAN=0x1003af268,
    SET_PERIODIC_ADV=0x10038ac8c, SET_PERIODIC_SYNC=0x100296dbc,
    CACHE_EXT_ADV=0x100c3d120, CACHE_EXT_SCAN=0x100c3d110, CACHE_SPEC_ADV=0x100c3d288,
    CACHE_ENH_CONNECTION=0x100c3d298, CACHE_PERIODIC_ADV=0x100c3d130,
    CACHE_PERIODIC_SYNC=0x100c3d140, CACHE_PAST_SEND=0x100c3d150,
    CACHE_PAST_RECV=0x100c3d160, CACHE_CODED_PHY=0x100c3d240, CACHE_PHY_2M=0x100c1f140,
    LEGACY_STEP=0x100a0ef96, ALL_STEP=0x100a0ef26, MODEL_MULTIPLE_ADV=0xa0,
    ADV_MODE=ADV_MODE, ADV_CAPACITY=ADV_CAPACITY, ID_ADV_SETS=0xd1)
BZERO = 0x10090a160
PACIBSP = 0xd503237f
RET = 0xd65f03c0
DPRINTF = 0x10090a740
ROOT_OBSERVER_FORMAT = b'[qbt-root] p=%d ra=%x t=%d s=%d n=%d\n\0'
ROOT_OBSERVER_FRAME = 0x2e0
# p: global read/store, Root/NC, mode, Cloud, derive, canary and source read status.
# All probes run before the displaced word. Return probes keep their callee FP;
# mode/crypto probes keep the completed helper's original LR.
ROOT_OBSERVER_POINTS = (
    (0x10085e608, 8, 0xb81ac3bf),
    (0x1004a084c, 9, 0xb9400ff5),
    (0x1004a093c, 0, 0xaa1503e0),
    (0x1004a0b64, 1, 0xaa1503e0),
    (0x10085eca4, 2, 0xaa1803e0),
    (0x10085dd28, 3, 0xa9427bfd),
    (0x10085e00c, 4, 0xa9437bfd),
    (0x10085e634, 5, 0xaa0003f4),
    (0x10085e140, 5, 0xaa0003f4),
    (0x10085e8bc, 6, 0x34000400),
    (0x10085e9b8, 6, 0x34000a60),
    (0x10085eaa8, 7, 0xaa0003f7),
    (0x10085eb60, 7, 0xaa0003f8),
    (0x10085de00, 7, 0x34fff940),
    (0x10085dfec, 7, 0x340001a0),
    (0x10085e1d0, 7, 0xaa0003f7),
    (0x10085e258, 7, 0xaa0003f8),
    (0x10085e418, 7, 0xaa0003f8),
)
ROOT_READ_TYPE_CAPTURE = 0x1004a0818
ROOT_READ_TYPE_SLOT = 8  # unused word below the read wrapper's status at sp+12


def relative(pc, target, bits):
    delta = target - pc
    if delta % 4 or not -(1 << (bits + 1)) <= delta < (1 << (bits + 1)):
        raise ValueError(f'branch outside aligned range: {pc:#x} -> {target:#x}')
    return (delta // 4) & ((1 << bits) - 1)


def branch(pc, target, link=False):
    return (0x94000000 if link else 0x14000000) | relative(pc, target, 26)


def cbnz(pc, reg, target):
    return 0x35000000 | relative(pc, target, 19) << 5 | reg


def adrp(pc, reg, target):
    pages = ((target & ~4095) - (pc & ~4095)) >> 12
    if not -(1 << 20) <= pages < (1 << 20):
        raise ValueError('ADRP outside range')
    pages &= (1 << 21) - 1
    return 0x90000000 | (pages & 3) << 29 | (pages >> 2) << 5 | reg


def add(reg, source, value):
    if not 0 <= value < 4096:
        raise ValueError('ADD immediate outside range')
    return 0x91000000 | value << 10 | source << 5 | reg


def memory(op, reg, base, offset, scale):
    if offset % scale or not 0 <= offset // scale < 4096:
        raise ValueError('load/store offset outside range')
    return op | (offset // scale) << 10 | base << 5 | reg


def mov_w(reg, value):
    if not 0 <= value <= 65535:
        raise ValueError('MOV immediate outside range')
    return 0x52800000 | value << 5 | reg


def pair(op, first, second, base, offset, scale):
    if offset % scale or not -64 <= offset // scale < 64:
        raise ValueError('pair offset outside range')
    return op | ((offset // scale) & 127) << 15 | second << 10 | base << 5 | first


def branch_target(pc, word):
    if word & 0x7c000000 == 0x14000000:
        bits, shift = 26, 0
    elif word & 0xff000010 == 0x54000000 or word & 0x7e000000 == 0x34000000:
        bits, shift = 19, 5
    elif word & 0x7e000000 == 0x36000000:
        bits, shift = 14, 5
    else:
        return None
    value = word >> shift & ((1 << bits) - 1)
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return pc + value * 4


def displaced_words(original_pc, word):
    target = branch_target(original_pc, word)
    if target is None:
        return [word]
    if word & 0x7e000000 == 0x34000000:
        inverted = word ^ 0x01000000  # CBZ/CBNZ skips the far B on the opposite edge
        return [lambda pc: (inverted & ~(0x7ffff << 5)) | relative(pc, pc + 8, 19) << 5,
                lambda pc: branch(pc, target)]
    raise ValueError('unsupported displaced observation branch')


class NativeImage(_inspect.Image):
    def __init__(self, path):
        super().__init__(path)
        if hashlib.sha256(self.data).hexdigest() != ORIGINAL_SHA256:
            raise ValueError('bluetoothd must match the locked 24A446 original SHA-256')
        if self.uuid != ORIGINAL_UUID or self.subtype != 0x80000002:
            raise ValueError('bluetoothd UUID or arm64e subtype differs')
        self.original = self.data
        self.data = bytearray(self.data)
        self.cursor = CAVE
        self.patches = []
        self.entries = {}
        self.root_observation = None
        self.fixups = self.chained_fixups()
        start, end = self.offset(CAVE), self.offset(CAVE_END - 1) + 1
        if any(self.data[start:end]):
            raise ValueError('executable tail is occupied')
        for section in self.sections:
            if max(CAVE, section['address']) < min(CAVE_END, section['address'] + section['size']):
                raise ValueError('executable tail overlaps a section')
        text = next(s for s in self.segments if s[0] == '__TEXT')
        if CAVE_END > text[1] + text[4]:
            raise ValueError('executable tail is not file-backed __TEXT')
        if self.stubs.get('_bzero') != BZERO:
            raise ValueError('bzero import differs')

    def chained_fixups(self):
        """Enumerate the target's dyld USERLAND24 chains without changing them.

        Layout: Apple's mach-o/fixup-chains.h. Authenticated rebases retain
        diversity, address diversity, key and next when their target changes.
        """
        at = 32
        payload = None
        count = struct.unpack_from('<I', self.data, 16)[0]
        for _ in range(count):
            command, size = struct.unpack_from('<II', self.data, at)
            if command == 0x80000034:
                payload, length = struct.unpack_from('<II', self.data, at + 8)
                if payload + length > len(self.data):
                    raise ValueError('chained fixups extend beyond image')
            at += size
        if payload is None:
            raise ValueError('native image has no chained fixups')
        version, starts = struct.unpack_from('<II', self.data, payload)
        if version != 0:
            raise ValueError('unknown chained fixup version')
        starts += payload
        count = struct.unpack_from('<I', self.data, starts)[0]
        if count != len(self.segments):
            raise ValueError('chained fixup segment count differs')
        result = {}
        for index, segment in enumerate(self.segments):
            delta = struct.unpack_from('<I', self.data, starts + 4 + index * 4)[0]
            if not delta:
                continue
            header = starts + delta
            size, page_size, form, offset, _, pages = struct.unpack_from('<IHHQIH', self.data, header)
            if form != 12 or page_size != 16384 or size < 22 + pages * 2:
                raise ValueError('native USERLAND24 page format differs')
            if self.base + offset != segment[1]:
                raise ValueError('chained fixup segment address differs')
            for page in range(pages):
                first = struct.unpack_from('<H', self.data, header + 22 + page * 2)[0]
                if first == 0xffff:
                    continue
                if first & 0x8000 or first >= page_size:
                    raise ValueError('unsupported multi-start native fixup page')
                page_va = self.base + offset + page * page_size
                va = page_va + first
                while True:
                    if va in result or va + 8 > page_va + page_size:
                        raise ValueError('native fixup chain crosses its page')
                    raw = struct.unpack_from('<Q', self.data, self.offset(va))[0]
                    result[va] = raw
                    step = (raw >> 51) & 0x7ff
                    if not step:
                        break
                    va += step * 8
        return result

    def capability_slots(self):
        result = {}
        for name, slot in MODEL_CAPABILITIES.items():
            address = MODEL_VTABLE + slot
            raw = self.fixups.get(address)
            if raw is None or raw >> 62 != 2 or (raw >> 49) & 3 != 0:
                raise ValueError(f'{name} is not an authenticated IA rebase')
            result[name] = dict(slot=hex(slot), address=hex(address),
                                target=hex(self.base + (raw & 0xffffffff)),
                                diversity=hex((raw >> 32) & 65535),
                                address_diversity=bool((raw >> 48) & 1))
        return result

    def put(self, va, words, expected=None):
        if expected is not None:
            actual = [self.word(va + 4 * n) for n in range(len(expected))]
            if actual != expected:
                raise ValueError(f'instruction mismatch at {va:#x}: {actual!r}')
        for n, word in enumerate(words):
            struct.pack_into('<I', self.data, self.offset(va + 4 * n), word)
        if expected is not None:
            self.patches.append(dict(address=hex(va), original=[hex(w) for w in expected],
                                     patched=[hex(w) for w in words]))

    def emit(self, name, words):
        start = self.cursor
        if start + 4 * len(words) > CAVE_END:
            raise ValueError('native trampoline tail exhausted')
        self.put(start, [word(start + n * 4) if callable(word) else word for n, word in enumerate(words)])
        self.cursor += 4 * len(words)
        self.entries[name] = hex(start)
        return start

    def address_words(self, reg, address):
        return [lambda pc: adrp(pc, reg, address), add(reg, reg, address & 4095)]

    def descriptor(self, opcode):
        matches = []
        for index in range(DESCRIPTOR_COUNT):
            va = DESCRIPTORS + index * 24
            op, bits = struct.unpack_from('<HH', self.data, self.offset(va) + 2)
            if op == opcode:
                matches.append((index, bits))
        if len(matches) != 1:
            raise ValueError(f'opcode {opcode:#x} has no unique native descriptor')
        return matches[0]

    def vendor_routing(self):
        for site in FACTORIES:
            self.put(site, [mov_w(1, 0)], [mov_w(1, 15)])
        # Every product's factory branch reaches this thunk: the board has a standard
        # H4 controller whatever product identity the OS image carries.
        entry = self.emit('controller_type', [mov_w(1, 0),
            lambda pc: branch(pc, CONTROLLER_INIT)])
        self.put(CONTROLLER_THUNK, [branch(CONTROLLER_THUNK, entry)],
                 [branch(CONTROLLER_THUNK, CONTROLLER_INIT)])
        # No factory branch lists the board's product, which would leave the model null
        # and crash the daemon. Fall through into the last constructor call instead.
        self.put(UNKNOWN_PRODUCT_CHECK, [0xd503201f], [0x340002a0])
        # The board's ADT carries vmm-present for other components. This daemon reads it
        # as "skip chip initialization", so the controller would never be booted.
        self.put(VIRTUAL_MACHINE_CHECK, [mov_w(0, 0), RET], [0xf0003688, 0xf942b108])
        # Host flow control and erroneous data reporting are optional for the stack.
        # The callbacks still log a rejection; initialization continues without them.
        self.put(HOST_BUFFER_STATUS, [0xaa1f03e0], [0xaa1303e0])
        self.put(HOST_FLOW_UNSUPPORTED, [0xd503201f], [0x540001c1])
        self.put(ERRONEOUS_STATUS, [0xaa1f03e0], [0xaa1303e0])
        # A scan can be queued while the native refresh still sends its per-set addresses, and
        # its own address is random: enabling it without 0x2005 returns Invalid Parameters.
        # Queue the same address first. The native completion stays on the last 0x2005.
        target = 0x1003a69f8
        start = self.cursor
        skip = start + 8 * 4
        entry = self.emit('address_first', [lambda pc: branch(pc, target, True), 0x39400268,
            lambda pc: 0x34000008 | relative(pc, skip, 19) << 5, 0x91000668, 0xf90003e8,
            0xaa1f03e1, mov_w(0, SET_RANDOM_ADDRESS), lambda pc: branch(pc, COMMAND_SUBMIT, True),
            lambda pc: branch(pc, ADDRESS_REFRESH + 4)])
        self.put(ADDRESS_REFRESH, [branch(ADDRESS_REFRESH, entry)], [branch(ADDRESS_REFRESH, target, True)])
        # The refresh caches each set's address even when the controller rejects it because the
        # set does not exist yet (0x42). Starting that set then skips 0x2035 as already current,
        # and enabling it returns Invalid Parameters forever. Always send the address.
        self.put(ADV_ADDRESS_CACHED, [0xd503201f], [0x54000320])
        # When no resolvable address can be generated, the refresh falls back and returns without
        # clearing its busy flag, so the next advertising update restarts the daemon after 10 s
        # (reason 4074). Leave through the native finish that a refresh without 0x2005 uses.
        self.put(ADDRESS_FALLBACK, [branch(ADDRESS_FALLBACK, ADDRESS_FINISH)],
                 [branch(ADDRESS_FALLBACK, ADDRESS_RETURN)])
        # Preserve the native bound (1..6000), adding exactly type zero.
        start = self.cursor
        entry = self.emit('registration', [lambda pc: cbnz(pc, 0, start + 8),
            lambda pc: branch(pc, REGISTER + 16), 0x51000409,
            lambda pc: branch(pc, REGISTER + 4)])
        self.put(REGISTER, [branch(REGISTER, entry)], [0x51000409])

        # The original frame and PACIBSP are already in effect. Only type zero
        # skips vendor backend setup. Standard HCI initialization stays native.
        start = self.cursor
        words = [None] + self.address_words(0, VENDOR_OPS) + [mov_w(1, VENDOR_OPS_BYTES),
                  lambda pc: branch(pc, BZERO, True)]
        for address in VENDOR_HOOKS:
            words.extend([lambda pc, a=address: adrp(pc, 8, a),
                          memory(0xf9000000, 31, 8, address & 4095, 8)])
        words.extend([lambda pc: adrp(pc, 8, VENDOR_OPCODES),
                      memory(0xb9000000, 31, 8, VENDOR_OPCODES & 4095, 4),
                      mov_w(0, 0), lambda pc: branch(pc, 0x100377d00)])
        original = start + 4 * len(words)
        words[0] = lambda pc: cbnz(pc, 0, original)
        words.extend([0x51000408, lambda pc: branch(pc, 0x100377b84)])
        entry = self.emit('vendor_init', words)
        self.put(0x100377b80, [branch(0x100377b80, entry)], [0x51000408])

        for n, (_, site, epilogue, _) in enumerate(CALLBACKS):
            start = self.cursor
            entry = self.emit(f'callbacks_{n}', [lambda pc, t=start + 8: cbnz(pc, 8, t),
                lambda pc, t=epilogue: branch(pc, t), 0x51000509,
                lambda pc, t=site + 4: branch(pc, t)])
            self.put(site, [branch(site, entry)], [0x51000509])

        # This optional standard command is absent on some QCA revisions.
        # Use the native support bitmap and real chain-next operation.
        index, bits = self.descriptor(0x0c5b)
        if (index, bits) != (0x7c, 0x1208):
            raise ValueError('optional command descriptor differs')
        byte, mask = bits >> 8, bits & 255
        if byte >= SUPPORT_BYTES or mask != 8:
            raise ValueError('optional command support bit differs')
        address = SUPPORT_TABLE + byte
        start = self.cursor
        entry = self.emit('optional_0c5b', [lambda pc: adrp(pc, 16, address),
            memory(0x39400000, 16, 16, address & 4095, 1),
            lambda pc: 0x36180010 | relative(pc, start + 20, 14) << 5,
            0xd100c3ff, lambda pc: branch(pc, OPTIONAL_STEP + 8),
            lambda pc: adrp(pc, 8, CHAIN_CONTEXT),
            memory(0xf9400000, 0, 8, CHAIN_CONTEXT & 4095, 8),
            0xd50323ff,  # authenticate the original PACIBSP before tail call
            lambda pc: branch(pc, CHAIN_NEXT)])
        site = OPTIONAL_STEP + 4  # retain the original indirect-call landing pad
        self.put(site, [branch(site, entry)], [0xd100c3ff])
        return self

    def volatile_root_keys(self):
        # The consumer's RAM-only profile permits errSecUpgradePending (-34019)
        # to enter the original status/length/generation path, including NC
        # identity. Valid stored material and all generation checks stay native.
        for site in ROOT_KEY_READS + NC_ROOT_KEY_READS:
            word = self.word(site)
            if word & 0xff00001f != 0x54000001:
                raise ValueError('root key read branch differs')
            target = site + ((word >> 5 & 0x7ffff) << 2)
            self.put(site, [branch(site, target)], [word])
        for site in ROOT_KEY_WRITES + NC_ROOT_KEY_WRITES:
            word = self.word(site)
            if word & 0xff00001f != 0x34000000:
                raise ValueError('root key write branch differs')
            offset = word >> 5 & 0x7ffff
            offset = offset - (1 << 19) if offset >> 18 else offset
            target = site + (offset << 2)
            start = self.cursor
            # Material has already passed its native generation path. Only a
            # successful store or the selected RAM-only status may continue.
            # The continuations overwrite w8/NZCV before using either; keep w0,
            # the original frame, LR and every callee-saved register intact.
            guard = self.emit(f'root_key_store_{site:x}', [
                0x12909c48,  # mov w8, #-34019 (errSecUpgradePending)
                0x6b08001f,  # cmp w0, w8
                lambda pc, t=start + 16: 0x54000000 | relative(pc, t, 19) << 5,
                lambda pc, t=start + 20: cbnz(pc, 0, t),
                lambda pc, t=target: branch(pc, t),
                lambda pc, t=site + 4: branch(pc, t)])
            self.put(site, [branch(site, guard)], [word])
        # Preserve material failure and NC load's 0x67 repair/0x82 error flow.
        if self.word(ROOT_KEY_FAILURE) != mov_w(24, 0x82) or self.word(ROOT_KEY_LOAD_RESULT) != 0xaa0003f8:
            raise ValueError('root key failure propagation differs')
        # Existing RAM policy suppresses root-change/old-pair cache conflict
        # handling. This is separate from generation and store result handling;
        # persistence and the old records' root generation remain unverified.
        self.put(ROOT_KEY_STORE_CHECK, [0xd503201f], [0x350016c0])
        return self

    def h4_shutdown(self):
        # The stock pre-stop handles none and H4DS, but dereferences +0x60 for
        # H4. The actual H4 table has no such extension. Its regular close
        # callback remains installed and is called by native stack teardown.
        if self.word(H4_TABLE) != 1 or struct.unpack_from('<Q', self.data, self.offset(H4_TABLE) + 0x60)[0]:
            raise ValueError('native H4 table or empty pre-stop extension differs')
        expected = [adrp(H4_PRESTOP, 8, TRANSPORT),
                    memory(0xf9400000, 8, 8, TRANSPORT & 4095, 8),
                    memory(0xb9400000, 9, 8, 0, 4)]
        if [self.word(H4_PRESTOP + i * 4) for i in range(3)] != expected:
            raise ValueError('native pre-stop transport lookup differs')
        start = self.cursor
        entry = self.emit('h4_prestop', [lambda pc: adrp(pc, 8, TRANSPORT), expected[1], expected[2],
            0x7100053f,  # cmp w9, #1
            lambda pc: 0x54000001 | relative(pc, start + 24, 19) << 5,
            RET, lambda pc: branch(pc, H4_PRESTOP + 12)])
        self.put(H4_PRESTOP, [branch(H4_PRESTOP, entry)], expected[:1])
        return self

    def capability_adapter(self):
        # Reserve virtual zero-fill storage; no file section or fixup occupies it.
        segment = next(s for s in self.segments if s[0] == '__DATA')
        if not segment[1] <= NATIVE_STATE or NATIVE_STATE + NATIVE_STATE_BYTES > segment[1] + segment[2]:
            raise ValueError('native capability storage is outside writable data')
        for section in self.sections:
            if max(NATIVE_STATE, section['address']) < min(NATIVE_STATE + NATIVE_STATE_BYTES,
                                                          section['address'] + section['size']):
                raise ValueError('native capability storage overlaps a section')
        self.capability_slots()  # verify authenticated USERLAND24 rebases
        for name, slot in MODEL_CAPABILITIES.items():
            address = NATIVE_STATE + CAP_OFFSETS[name]
            op, scale = (0xb9400000, 4) if name == 'adv_sets' else (0x39400000, 1)
            getter = self.emit('get_' + name, [0xd503245f,
                lambda pc, a=address: adrp(pc, 8, a),
                memory(op, 0, 8, address & 4095, scale), RET])
            raw = self.fixups[MODEL_VTABLE + slot]
            # dyld signs the new rebase using its original key and diversity.
            # Copying a live vtable would invalidate its address-bound signatures.
            new = (raw & 0xffffffff00000000) | (getter - self.base)
            self.put(MODEL_VTABLE + slot, [new & 0xffffffff, new >> 32],
                     [raw & 0xffffffff, raw >> 32])

        publisher = self.emit('publish', [PACIBSP, 0xa9bf7bfd, 0x910003fd,
            lambda pc: adrp(pc, 16, NATIVE_STATE),
            memory(0xf9400000, 16, 16, NATIVE_STATE & 4095, 8),
            0xd63f0a1f,  # blraaz x16; library function pointer is signed IA/0
            0xa8c17bfd, 0xd65f0fff])
        for kind, entry, tail, epilogue, parameter in (
                (0, 0x10032a91c, 0x10032aa08, 0x10032aa18, 1),
                (1, 0x10032a784, 0x10032a8ec, 0x10032a8fc, 2)):
            displaced = self.word(entry + 4)
            if self.word(entry) != PACIBSP or displaced not in (0xd10103ff, 0xd10183ff):
                raise ValueError('native feature callback prologue differs')
            capture = self.emit(f'features_{kind}', [lambda pc: adrp(pc, 8, NATIVE_STATE),
                memory(0xb9000000, 0, 8, (NATIVE_STATE & 4095) + 64, 4),
                mov_w(9, kind), memory(0xb9000000, 9, 8, (NATIVE_STATE & 4095) + 68, 4),
                memory(0xf9000000, parameter, 8, (NATIVE_STATE & 4095) + 72, 8),
                displaced, lambda pc, e=entry: branch(pc, e + 8)])
            self.put(entry + 4, [branch(entry + 4, capture)], [displaced])
            name = NATIVE_BINDINGS['ALL_STEP' if kind else 'LEGACY_STEP']
            expected = [adrp(tail, 1, name), add(1, 1, name & 4095), mov_w(0, 0),
                        branch(tail + 12, NATIVE_BINDINGS['ADVANCE'], True)]
            self.put(tail, [branch(tail, publisher, True), branch(tail + 4, epilogue),
                            0xd503201f, 0xd503201f], expected)

        # Refresh the constructor's pre-probe choice before native GAP init.
        prepare = self.emit('prepare_advertising', [0xa9bf7be0,
            lambda pc: adrp(pc, 16, NATIVE_STATE),
            memory(0xf9400000, 16, 16, (NATIVE_STATE & 4095) + 8, 8), 0xd63f0a1f,
            0xa8c17be0, 0xd10143ff, lambda pc: branch(pc, ADV_START + 8)])
        if self.word(ADV_START) != PACIBSP:
            raise ValueError('native broadcaster entry differs')
        self.put(ADV_START + 4, [branch(ADV_START + 4, prepare)], [0xd10143ff])

        # Appending preserves every existing import ordinal. __TEXT already has
        # executable tail room, so no segment or __LINKEDIT relocation is needed.
        at = self.load_commands_end
        size = (24 + len(NATIVE_DYLIB) + 1 + 7) & ~7
        first_section = min(s['offset'] for s in self.sections if s['segment'] == '__TEXT')
        if at + size > first_section or any(self.data[at:at + size]):
            raise ValueError('no empty load-command room for native library')
        command = struct.pack('<6I', 0xc, size, 24, 2, 0x10000, 0x10000) + NATIVE_DYLIB
        words = struct.unpack('<' + 'I' * (size // 4), command.ljust(size, b'\0'))
        self.put(self.base + at, words, [0] * len(words))
        count, total = struct.unpack_from('<II', self.data, 16)
        self.put(self.base + 16, [count + 1, total + size], [count, total])
        self.entries['capability_state'] = hex(NATIVE_STATE)
        self.entries['library'] = NATIVE_DYLIB.decode()
        return self

    def observer_function_references(self, ranges):
        """Find incoming code/data references before reusing replaced functions.

        Runtime USERLAND24 rebases, all direct branches and address formation,
        defined external symbols and relative metadata are checked. Unwind and
        exception tables describe existing functions; they are not call entries.
        Conservative matches reject a candidate rather than freeing live code.
        """
        result = {start: [] for start, _ in ranges}

        def record(target, source, kind):
            for start, end in ranges:
                if start <= target < end and not start <= source < end:
                    result[start].append(dict(source=hex(source), target=hex(target), kind=kind))

        for source, raw in self.chained_fixups().items():
            if raw & (1 << 62):
                continue  # imported bind, not a local rebase
            targets = [BASE + (raw & 0xffffffff)] if raw >> 63 else [
                raw & ((1 << 43) - 1), BASE + (raw & ((1 << 43) - 1))]
            for target in targets:
                record(target, source, 'USERLAND24 rebase')
        pages = {page for start, end in ranges
                 for page in range(start & ~4095, (end + 4095) & ~4095, 4096)}
        for section in self.sections:
            if section['name'] != '__text':
                continue
            start, end = section['address'], section['address'] + section['size']
            for pc in range(start, end, 4):
                word = self.word(pc)
                target = branch_target(pc, word)
                if target is not None:
                    record(target, pc, 'direct branch')
                if word & 0x9f000000 not in (0x10000000, 0x90000000):
                    continue
                immediate = ((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2)
                if immediate & (1 << 20):
                    immediate -= 1 << 21
                if word & 0x9f000000 == 0x10000000:
                    record(pc + immediate, pc, 'ADR address')
                    continue
                page = (pc & ~4095) + (immediate << 12)
                if page not in pages:
                    continue
                function = self.function(pc)
                limit = int(function[1], 16) if function and function[1] else end
                register = word & 31
                # Looking through the entire containing function deliberately
                # keeps ambiguous register reuse conservative.
                for at in range(pc + 4, min(limit, end), 4):
                    nxt = self.word(at)
                    if nxt & 0xff000000 == 0x91000000 and (nxt >> 5) & 31 == register:
                        target = page + (((nxt >> 10) & 4095) << (12 if nxt & (1 << 22) else 0))
                        record(target, at, 'ADRP+ADD address')
        for section in self.sections:
            name = section['name']
            if name == '__init_offsets':
                for pc in range(section['address'], section['address'] + section['size'], 4):
                    record(BASE + self.word(pc), pc, 'initializer offset')
            if name == '__objc_methlist' or name.startswith('__swift') or name == '__const':
                for pc in range(section['address'], section['address'] + section['size'] - 3, 4):
                    word = self.word(pc)
                    for value in (word, word & ~3):
                        signed = value - (1 << 32) if value & (1 << 31) else value
                        record(pc + signed, pc, 'relative metadata')
        at = 32
        for _ in range(struct.unpack_from('<I', self.data, 16)[0]):
            command, size = struct.unpack_from('<II', self.data, at)
            if command == 2:
                offset, count = struct.unpack_from('<II', self.data, at + 8)
                for index in range(count):
                    _, kind, _, _, target = struct.unpack_from('<IBBHQ', self.data, offset + index * 16)
                    if kind & 0xe0 == 0 and kind & 1 and kind & 0x0e == 0x0e:
                        record(target, BASE + offset + index * 16, 'defined external symbol')
            if command == 0x80000028:
                offset = struct.unpack_from('<Q', self.data, at + 8)[0]
                record(BASE + offset, BASE + at, 'LC_MAIN entry')
            at += size
        return result

    def observe_root_status(self, enabled=False):
        """Opt in to int32 Root diagnostics through the existing dprintf import."""
        if not enabled:
            return self
        if self.root_observation is not None:
            raise ValueError('root observation already installed')
        if 'library' not in self.entries or self.stubs.get('_dprintf') != DPRINTF:
            raise ValueError('root observation requires the native adapter and locked dprintf')
        for site, _, expected in ROOT_OBSERVER_POINTS:
            if self.word(site) != expected:
                raise ValueError(f'root observation point differs at {site:#x}')
        if self.word(ROOT_READ_TYPE_CAPTURE) != 0xb9000fff:
            raise ValueError('root read stack capture differs')
        # The replaced adv_sets getter is one continuous, already-unused body;
        # leave the other old getters and the disabled VMM function untouched.
        original = self.capability_slots()['adv_sets']
        start = int(original['target'], 16)
        function = self.function(start)
        current = self.chained_fixups()
        replacement = int(self.entries['get_adv_sets'], 16)
        if BASE + (current[int(original['address'], 16)] & 0xffffffff) != replacement:
            raise ValueError('the original adv_sets getter is still installed')
        if not function or int(function[0], 16) != start or not function[1]:
            raise ValueError('original adv_sets function boundary differs')
        end = int(function[1], 16)
        references = self.observer_function_references([(start, end)])
        if references[start]:
            raise ValueError('the old adv_sets getter has an incoming reference')
        begin_offset, end_offset = self.offset(start), self.offset(end - 1) + 1
        if self.data[begin_offset:end_offset] != self.original[begin_offset:end_offset]:
            raise ValueError('old adv_sets function body has already changed')
        body_start, body_end = start, end
        pools = [[start, end, 'adv_sets']]
        fmt = ROOT_OBSERVER_FORMAT.ljust((len(ROOT_OBSERVER_FORMAT) + 3) & ~3, b'\0')
        def probe_words(site, kind, original, logger):
            if kind in (8, 9):
                # Save x0 while logging the constant canary or the read helper's
                # out-status. Two pushes put its original sp+12 at current+44.
                before = [pair(0xa9800000, 0, 8, 31, -16, 8),
                    pair(0xa9800000, 9, 30, 31, -16, 8), mov_w(8, kind),
                    mov_w(0, 0) if kind == 8 else memory(0xb9400000, 0, 31, 44, 4),
                    lambda pc: branch(pc, logger, True),
                    pair(0xa8c00000, 9, 30, 31, 16, 8),
                    pair(0xa8c00000, 0, 8, 31, 16, 8)]
            else:
                before = [pair(0xa9800000, 8, 30, 31, -16, 8), mov_w(8, kind),
                    lambda pc: branch(pc, logger, True), pair(0xa8c00000, 8, 30, 31, 16, 8)]
            return before + displaced_words(site, original) + [lambda pc: branch(pc, site + 4)]

        # Conditional points add a local inverse branch and a range-safe B.
        required = sum(len(probe_words(site, kind, word, 0)) * 4
                       for site, kind, word in ROOT_OBSERVER_POINTS) + 12 + len(fmt)
        if end - start < required:
            raise ValueError('old adv_sets body is insufficient for root observation')

        def reserve(name, words):
            length = len(words) * 4
            for pool in pools:
                if pool[0] + length <= pool[1]:
                    start = pool[0]
                    original = [self.word(start + i * 4) for i in range(len(words))]
                    self.put(start, [word(start + i * 4) if callable(word) else word
                                     for i, word in enumerate(words)], original)
                    pool[0] += length
                    self.entries[name] = hex(start)
                    return start
            raise ValueError('root observation getter fragments exhausted')

        fmt_address = reserve('root_status_format',
            list(struct.unpack('<' + 'I' * (len(fmt) // 4), fmt)))
        frame = ROOT_OBSERVER_FRAME
        # Original x8/LR are already saved by the per-point entry. Save every
        # other GPR and all SIMD lanes before preparing Darwin stack varargs.
        words = [PACIBSP, 0xd10003ff | frame << 10]
        for first in range(0, 18, 2):
            words.append(pair(0xa9000000, first, first + 1, 31, 0x30 + first * 8, 8))
        words.append(pair(0xa9000000, 18, 30, 31, 0xc0, 8))
        for first in range(0, 32, 2):
            words.append(pair(0xad000000, first, first + 1, 31, 0xd0 + first * 16, 16))
        words += [0xd53b4209, memory(0xf9000000, 9, 31, 0x2d0, 8),
                  memory(0xb9000000, 8, 31, 0, 4), mov_w(11, 0), mov_w(12, 0),
                  0x7100091f, 0x1a80030a]  # cmp w8,2; csel w10,w24,w0,eq
        common_branch = len(words)
        words.append(None)  # p>=2 already selects w24 for Root or w0 otherwise
        words.append(0x2a1503ea)  # global wrapper's status is w21
        store_branch = len(words)
        words.append(None)
        words += [memory(0xb9400000, 11, 31, frame + 16 + ROOT_READ_TYPE_SLOT, 4),
                  0x1280000c]  # absent optional length pointer: n=-1
        null_branch = len(words)
        words.append(None)
        words.append(memory(0xb9400000, 12, 20, 0, 4))
        read_done = len(words)
        words.append(None)
        store = len(words)
        words += [0x2a1603eb, 0x2a1703ec]  # type=w22, requested length=w23
        common = len(words)
        # Darwin AArch64 variadic arguments use eight-byte stack slots even
        # for int. Keep p/ra/type/status/length at 0/8/16/24/32; GPRs start at48.
        words += [pair(0xa9000000, 11, 10, 31, 16, 8),
                  memory(0xb9000000, 12, 31, 32, 4),
                  memory(0xf9400000, 9, 31, frame + 8, 8), 0x7100151f]
        ra_branch = len(words)
        words.append(None)
        words.append(memory(0xf9400000, 9, 29, 8, 8))
        normalize = len(words)
        words += [0xdac143e9, lambda pc: adrp(pc, 10, BASE), 0xcb0a0129,
                  memory(0xb9000000, 9, 31, 8, 4), mov_w(0, 2)]
        words += self.address_words(1, fmt_address)
        words.append(lambda pc: branch(pc, DPRINTF, True))
        words += [memory(0xf9400000, 9, 31, 0x2d0, 8), 0xd51b4209]
        for first in range(0, 32, 2):
            words.append(pair(0xad400000, first, first + 1, 31, 0xd0 + first * 16, 16))
        for first in range(0, 18, 2):
            words.append(pair(0xa9400000, first, first + 1, 31, 0x30 + first * 8, 8))
        words += [pair(0xa9400000, 18, 30, 31, 0xc0, 8), add(31, 31, frame), 0xd65f0fff]
        start = self.cursor
        words[common_branch] = lambda pc: 0x54000002 | relative(pc, start + common * 4, 19) << 5
        words[store_branch] = lambda pc: cbnz(pc, 8, start + store * 4)
        words[null_branch] = lambda pc: 0xb4000014 | relative(pc, start + common * 4, 19) << 5
        words[read_done] = lambda pc: branch(pc, start + common * 4)
        words[ra_branch] = lambda pc: 0x54000002 | relative(pc, start + normalize * 4, 19) << 5
        if start + len(words) * 4 > CAVE_END:
            raise ValueError('shared Root logger does not fit the original tail')
        logger = self.emit('root_status_logger', words)
        capture = reserve('root_read_type_capture', [
            memory(0xb9000000, 0, 31, ROOT_READ_TYPE_SLOT, 4), 0xb9000fff,
            lambda pc: branch(pc, ROOT_READ_TYPE_CAPTURE + 4)])
        self.put(ROOT_READ_TYPE_CAPTURE, [branch(ROOT_READ_TYPE_CAPTURE, capture)], [0xb9000fff])
        for site, kind, original in ROOT_OBSERVER_POINTS:
            entry = reserve(f'root_status_{site:x}', probe_words(site, kind, original, logger))
            self.put(site, [branch(site, entry)], [original])
        self.root_observation = dict(enabled=True, import_address=hex(DPRINTF),
            sink='dprintf(fd=2); launchd /dev/console; existing kernel console to USB',
            format=ROOT_OBSERVER_FORMAT[:-1].decode(), format_address=hex(fmt_address),
            shared_logger_bytes=len(words) * 4, frame_bytes=frame,
            points=[dict(address=hex(site), kind=kind) for site, kind, _ in ROOT_OBSERVER_POINTS],
            reclaimed_body=dict(name='adv_sets', start=hex(body_start), end=hex(body_end),
                original_sha256=hashlib.sha256(self.original[begin_offset:end_offset]).hexdigest(),
                used_bytes=required, references=references[body_start]),
            statuses='int32 stack varargs; no material or peer/credential fields',
            variadic_int_stack_offsets=[0, 8, 16, 24, 32],
            canary='p=8, s=0 is a constant Root-entry sink record; original x0 is restored',
            source_read_status='p=9 records the read helper out-status before the wrapper maps missing/invalid data',
            return_addresses='component-relative return offset; callsite=ra-4',
            hardware_verified=False)
        return self

    def report(self):
        return dict(input_sha256=ORIGINAL_SHA256, input_uuid=self.uuid,
                    output_sha256=hashlib.sha256(self.data).hexdigest(),
                    stage='native-capability-adapter' if 'library' in self.entries else 'vendor-routing-and-h4-shutdown',
                    adapter_ready='library' in self.entries, hardware_verified=False,
                    cave_start=hex(CAVE), cave_end=hex(self.cursor),
                    entries=self.entries, patches=self.patches,
                    root_observation=self.root_observation,
                    capability_slots=self.capability_slots(),
                    pending=([] if 'library' in self.entries else ['native capability publication']) +
                            ['payload installation and device verification'])


def layout_header():
    """Generate the shared private ABI for the version-locked adapter library."""
    lines = ['/* Generated by native_ios27.py; external Apple image stays local. */',
             '#ifndef QBT_IOS27_LAYOUT_H', '#define QBT_IOS27_LAYOUT_H',
             '#include <stdint.h>', '#include <stddef.h>']
    lines += [f'#define QBT_{name} 0x{address:x}ULL' for name, address in NATIVE_BINDINGS.items()]
    lines += ['struct qbt_native_state {',
              '    void (*publish)(void);', '    void (*prepare)(void *);']
    lines += [f'    uint8_t {name};' for name in MODEL_CAPABILITIES if name != 'adv_sets']
    lines += ['    uint8_t reserved[2];', '    uint32_t adv_sets;',
              '    uint8_t reserved_tail[32];', '    uint32_t status, kind;',
              '    uintptr_t parameters;', '};']
    for name, offset in CAP_OFFSETS.items():
        lines += [f'_Static_assert(offsetof(struct qbt_native_state, {name}) == {offset}, "{name} ABI");']
    lines += ['_Static_assert(offsetof(struct qbt_native_state, status) == 64, "status ABI");',
              '_Static_assert(offsetof(struct qbt_native_state, parameters) == 72, "parameters ABI");',
              '#endif', '']
    return '\n'.join(lines)
