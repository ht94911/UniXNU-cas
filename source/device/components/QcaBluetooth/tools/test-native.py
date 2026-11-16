#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute native routing, capabilities and root-key paths against the locked bluetoothd."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from tempfile import TemporaryDirectory

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as arm64
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X8,
    UC_ARM64_REG_X9, UC_ARM64_REG_X19, UC_ARM64_REG_X20, UC_ARM64_REG_X29,
    UC_ARM64_REG_X30, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_NZCV)
import native_ios27 as n

STACK = 0x20000000
STACK_BYTES = 0x10000
STOP = STACK + STACK_BYTES
CONTEXT = 0x1234567812345678
REG_X = tuple(getattr(arm64, 'UC_ARM64_REG_X' + str(index)) for index in range(31))
REG_Q = tuple(getattr(arm64, 'UC_ARM64_REG_Q' + str(index)) for index in range(32))
ROOT_ENTRY = 0x10085e5d4
NC_LOAD = 0x10085dcb8
NC_GENERATE = 0x10085de40
ROOT_OBJECT = STACK + 0x1000
ROOT_MODEL = STACK + 0x3000
ROOT_VTABLE = STACK + 0x3100
ROOT_MODEL_KIND = STACK + 0x8400
ROOT_LOG = 0x100cae470
ROOT_READ = 0x1004a07f4
ROOT_STORE = 0x1004a0968
ROOT_RANDOM = 0x10090a100
ROOT_DERIVE = 0x10031b470
ROOT_CLOUD_GENERATE = 0x10031bc4c
ROOT_PUBLISH = 0x1001893b8
RAM_ONLY = -34019
MISSING = -65004  # ROOT_READ maps absent/invalid CFData to this wrapper status
CLOUD_ERROR = 0x41b  # original Cloud generation helper's RNG/export failure

# Fixed donor branches and continuations, independently read from 24A446.
# Each read row is: site, type, length, normal check, valid edge, invalid edge.
ROOT_READ_PATHS = (
    (0x10085e6bc, 2, 16, 0x10085e6e0, 0x10085e758, 0x10085e6f4),
    (0x10085e778, 1, 16, 0x10085e79c, 0x10085e814, 0x10085e7ac),
    (0x10085e830, 5, 96, 0x10085e854, 0x10085e864, 0x10085e8a4),
    (0x10085e880, 6, 64, 0x10085e8e0, 0x10085e8f0, 0x10085e9a0),
    (0x10085e90c, 7, 16, 0x10085e9e8, 0x10085ea90, 0x10085e9f8),
    (0x10085ec00, 16, 6, 0x10085ec24, 0x10085ecc4, 0x10085ec34),
    (0x10085dd04, 19, 6, 0x10085dd38, 0x10085dd8c, 0x10085dd28),
    (0x10085dda8, 17, 16, 0x10085ddd4, 0x10085ddec, 0x10085dd28),
)
# Store rows include the missing read that reaches each distinct generation path.
ROOT_STORE_PATHS = (
    (0x10085e724, 0x340001a0, 0x10085e758, 2),
    (0x10085e7dc, 0x340001a0, 0x10085e810, 1),
    (0x10085e94c, 0x34000880, 0x10085ea5c, 5),
    (0x10085ea28, 0x34000340, 0x10085ea90, 7),
    (0x10085ea6c, 0x34fff420, 0x10085e8f0, 5),
    (0x10085eb14, 0x340003c0, 0x10085eb8c, 6),
    (0x10085eb9c, 0x34ffeaa0, 0x10085e8f0, 6),
    (0x10085ec70, 0x340002a0, 0x10085ecc4, 16),
    (0x10085dedc, 0x340001a0, 0x10085df10, 19),
    (0x10085dfa4, 0x340001a0, 0x10085dfd8, 19),
)
ROOT_LENGTHS = {kind: length for _, kind, length, *_ in ROOT_READ_PATHS}


class Machine:
    def __init__(self, image, slide):
        self.image = image
        self.slide = slide
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for name, va, size, offset, file_bytes in image.segments:
            if size and name != '__PAGEZERO':
                self.uc.mem_map(va + slide, (size + 4095) & ~4095)
                self.uc.mem_write(va + slide, bytes(image.data[offset:offset + file_bytes]))
        self.uc.mem_map(STACK, STACK_BYTES + 4096)
        self.uc.reg_write(UC_ARM64_REG_SP, STACK + STACK_BYTES - 0x100)
        self.uc.reg_write(UC_ARM64_REG_X30, STOP)
        self.initial_sp = self.uc.reg_read(UC_ARM64_REG_SP)
        self.pac = 0
        self.visited = []
        self.stop_at = set()
        self.callbacks = {}
        self.bzero_calls = []
        self.next_calls = []
        self.indirect_calls = []
        self.publish_calls = []
        self.prepare_calls = []
        self.mock_publish = STACK + 0x8000
        self.mock_prepare = STACK + 0x8100
        self.write(n.NATIVE_STATE, struct.pack('<QQ', self.mock_publish, self.mock_prepare))
        self.uc.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, uc, address, size, _):
        if address == STOP or address in self.stop_at:
            uc.emu_stop()
            return
        self.visited.append(address - self.slide)
        callback = self.callbacks.get(address)
        if callback is not None:
            callback()
            return
        if address == self.mock_publish:
            self.publish_calls.append(struct.unpack('<IIQ', self.read(n.NATIVE_STATE + 64, 16)))
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        elif address == self.mock_prepare:
            self.prepare_calls.append(uc.reg_read(UC_ARM64_REG_X0))
            uc.reg_write(UC_ARM64_REG_X0, 0xdeadbeef)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        elif address == 0x10000d938 + self.slide:
            uc.reg_write(UC_ARM64_REG_X0, 0)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        elif address == n.BZERO + self.slide:
            va = uc.reg_read(UC_ARM64_REG_X0)
            count = uc.reg_read(UC_ARM64_REG_X1)
            self.bzero_calls.append((va, count))
            uc.mem_write(va, bytes(count))
            # Exercise the real caller-saved ABI. No hook can rely on bzero's
            # argument values surviving the imported call.
            for reg in range(UC_ARM64_REG_X0, UC_ARM64_REG_X0 + 19):
                uc.reg_write(reg, 0xdeadbeef)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
        elif address == n.CHAIN_NEXT + self.slide:
            self.next_calls.append(uc.reg_read(UC_ARM64_REG_X0))
            uc.emu_stop()
        else:
            word = struct.unpack('<I', uc.mem_read(address, 4))[0]
            # Unicorn has no target PAC keys. Model only the signing operation;
            # execute all stack, register, branch and memory instructions.
            if word == n.PACIBSP:
                self.pac += 1
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif word == 0xd50323ff:  # autibsp
                assert self.pac > 0
                self.pac -= 1
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif word == 0xdac11a30:  # autda x16, x17: fixture vtable pointer
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif word == 0xdac143e9:  # xpaci x9: model stripping, no target PAC keys
                uc.reg_write(UC_ARM64_REG_X9, uc.reg_read(UC_ARM64_REG_X9) & 0x0000ffffffffffff)
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif word == 0xd73f0910:  # blraa x8, x16: native model getter
                target = uc.reg_read(REG_X[8])
                uc.reg_write(UC_ARM64_REG_X30, address + 4)
                uc.reg_write(UC_ARM64_REG_PC, target)
            elif word == 0xd63f0a1f:  # blraaz x16, native-to-library ABI
                from unicorn.arm64_const import UC_ARM64_REG_X16
                target = uc.reg_read(UC_ARM64_REG_X16)
                uc.reg_write(UC_ARM64_REG_X30, address + 4)
                uc.reg_write(UC_ARM64_REG_PC, target)
            elif word == 0xd61f081f:  # braaz x0, authenticated native tail call
                target = uc.reg_read(UC_ARM64_REG_X0)
                self.indirect_calls.append(target)
                uc.reg_write(UC_ARM64_REG_PC, target)
            elif word == 0xd65f0fff:  # retab: native frame is restored already
                assert self.pac > 0
                self.pac -= 1
                uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))

    def run(self, entry, stops=(), max_instructions=4096):
        self.stop_at = {a + self.slide for a in stops}
        self.uc.emu_start(entry + self.slide, STOP + 4, count=max_instructions)
        pc = self.uc.reg_read(UC_ARM64_REG_PC)
        assert pc == STOP or pc in self.stop_at or self.next_calls, hex(pc)
        return self

    def write(self, address, value):
        self.uc.mem_write(address + self.slide, value)

    def read(self, address, size):
        return bytes(self.uc.mem_read(address + self.slide, size))


def seed_callee_saved(machine):
    saved = {}
    for index in range(19, 30):
        value = 0x1234000000000000 + index * 0x10101
        machine.uc.reg_write(REG_X[index], value)
        saved[index] = value
    return saved


def check_frame(machine, saved):
    assert machine.uc.reg_read(UC_ARM64_REG_SP) == machine.initial_sp
    assert machine.uc.reg_read(UC_ARM64_REG_X30) == STOP
    assert machine.pac == 0
    assert all(machine.uc.reg_read(REG_X[index]) == value for index, value in saved.items())


class RootHelpers:
    """Synthetic imported helpers; all root/NC control flow stays in donor code.

    Material is an in-memory pattern only. No original keychain, crypto result
    or device is consulted, and neither material nor pointers enter the report.
    """
    def __init__(self, machine, reads=None, stores=None, derivations=None,
                 cloud_status=0, diagnostics=0, diagnostic_status=0):
        self.m = machine
        self.reads = reads or {}
        self.stores = stores or {}
        self.derivations = derivations or {}
        self.cloud_status = cloud_status
        self.read_calls = []
        self.store_calls = []
        self.derive_calls = []
        self.cloud_calls = []
        self.random_calls = []
        self.publish_calls = []
        self.saved = seed_callee_saved(machine)
        machine.uc.reg_write(UC_ARM64_REG_X0, ROOT_OBJECT)
        machine.write(ROOT_LOG, struct.pack('<Q', STACK + 0x3200))
        machine.uc.mem_write(ROOT_MODEL, struct.pack('<Q', ROOT_VTABLE))
        machine.uc.mem_write(ROOT_VTABLE + 0x1e8, struct.pack('<Q', ROOT_MODEL_KIND))
        callbacks = {
            ROOT_READ: self.read, ROOT_STORE: self.store, ROOT_RANDOM: self.random,
            ROOT_DERIVE: self.derive, ROOT_CLOUD_GENERATE: self.cloud,
            ROOT_PUBLISH: self.publish,
            0x1000071b8: lambda: self.done(ROOT_MODEL),
            0x100006ef8: lambda: self.done(0),  # scoped lock constructor
            0x100006ec0: lambda: self.done(0),  # scoped lock destructor
            0x10090b650: lambda: self.done(0),  # logging disabled
            0x100299ed4: lambda: self.done(0),  # address notification
            0x1003af800: lambda: self.done(0),  # native address callback
            0x10085e0e4: lambda: self.done(diagnostic_status),
        }
        machine.callbacks.update({address + machine.slide: callback for address, callback in callbacks.items()})
        machine.callbacks[ROOT_MODEL_KIND] = lambda: self.done(diagnostics)

    def caller(self):
        return self.m.uc.reg_read(UC_ARM64_REG_X30) - self.m.slide

    def done(self, result=None):
        # Clobber Darwin's volatile argument/IP registers. In particular, a
        # void RNG call has no synthetic success/error return contract.
        for index in range(18):
            self.m.uc.reg_write(REG_X[index], 0xdead0000 + index)
        if result is not None:
            self.m.uc.reg_write(UC_ARM64_REG_X0, result & 0xffffffffffffffff)
        self.m.uc.reg_write(UC_ARM64_REG_PC, self.m.uc.reg_read(UC_ARM64_REG_X30))

    def read(self):
        uc = self.m.uc
        kind, buffer, size, actual = (uc.reg_read(REG_X[index]) for index in range(4))
        assert ROOT_LENGTHS[kind] == size
        status, length = self.reads.get(kind, (0, size))
        if status == 0:
            uc.mem_write(buffer, bytes([0x30 + kind]) * size)
        uc.mem_write(actual, struct.pack('<I', length))
        self.read_calls.append((kind, size, status, length))
        self.done(status & 0xffffffff)

    def store(self):
        uc = self.m.uc
        kind, buffer, size = (uc.reg_read(REG_X[index]) for index in range(3))
        assert size == ROOT_LENGTHS.get(kind, 16) and any(uc.mem_read(buffer, size))
        caller = self.caller()
        status = self.stores.get(caller, self.stores.get('default', 0))
        self.store_calls.append((caller, kind, size, status))
        self.done(status & 0xffffffff)

    def random(self):
        uc = self.m.uc
        buffer, size = (uc.reg_read(REG_X[index]) for index in range(2))
        assert size in (6, 16)
        self.random_calls.append(size)
        uc.mem_write(buffer, bytes([0x80 + len(self.random_calls)]) * size)
        self.done()  # _arc4random_buf returns void

    def derive(self):
        uc = self.m.uc
        assert uc.reg_read(REG_X[1]) in (1, 3) and uc.reg_read(REG_X[2]) == 0
        caller = self.caller()
        status = self.derivations.get(caller, 0)
        if status == 0:
            uc.mem_write(uc.reg_read(REG_X[3]), b'\xa6' * 16)
        self.derive_calls.append((caller, status))
        self.done(status & 0xffffffff)

    def cloud(self):
        uc = self.m.uc
        public, private, length = (uc.reg_read(REG_X[index]) for index in range(3))
        assert bytes(uc.mem_read(length, 1)) == b'\x60'
        if self.cloud_status == 0:
            uc.mem_write(public, b'\xa7' * 64)
            uc.mem_write(private, b'\xa8' * 96)
        self.cloud_calls.append(self.caller())
        self.done(self.cloud_status & 0xffffffff)

    def publish(self):
        self.publish_calls.append(bytes(self.m.uc.mem_read(ROOT_OBJECT + 0xebe, 1)))
        self.done(0)

    def check_return(self, status, ready):
        assert self.m.uc.reg_read(UC_ARM64_REG_X0) == status
        assert bytes(self.m.uc.mem_read(ROOT_OBJECT + 0xebe, 1)) == bytes([ready])
        assert self.publish_calls == ([b'\x01'] if ready else [])
        check_frame(self.m, self.saved)
        assert ROOT_ENTRY in self.m.visited


def check_root_keys(donor, image):
    counts = dict(store_guards=0, read_branches=0, root_flows=0, metadata=0)
    assert tuple(row[0] for row in ROOT_READ_PATHS) == n.ROOT_KEY_READS + n.NC_ROOT_KEY_READS
    assert tuple(row[0] for row in ROOT_STORE_PATHS) == n.ROOT_KEY_WRITES + n.NC_ROOT_KEY_WRITES
    assert donor.stubs['_arc4random_buf'] == ROOT_RANDOM
    assert image.word(n.ROOT_KEY_FAILURE) == donor.word(n.ROOT_KEY_FAILURE) == 0x52801058
    assert image.word(n.ROOT_KEY_LOAD_RESULT) == donor.word(n.ROOT_KEY_LOAD_RESULT) == 0xaa0003f8
    assert image.word(n.ROOT_KEY_STORE_CHECK) == 0xd503201f
    for entry in (ROOT_ENTRY, NC_LOAD, NC_GENERATE):
        assert image.word(entry) == donor.word(entry) == n.PACIBSP
    for exit_ in (0x10085ecc0, 0x10085dd34, 0x10085e01c):
        assert image.word(exit_) == donor.word(exit_) == 0xd65f0fff

    allowed = {row[0] for row in ROOT_READ_PATHS + ROOT_STORE_PATHS} | {n.ROOT_KEY_STORE_CHECK}
    for start, end in ((NC_LOAD, ROOT_ENTRY), (ROOT_ENTRY, 0x10085eefc)):
        assert all(image.word(address) == donor.word(address)
                   for address in range(start, end, 4) if address not in allowed)
    changed_fixups = image.chained_fixups()
    assert changed_fixups.keys() == donor.fixups.keys()
    capability_slots = {n.MODEL_VTABLE + offset for offset in n.MODEL_CAPABILITIES.values()}
    for address, original in donor.fixups.items():
        current = changed_fixups[address]
        if address in capability_slots:
            assert current >> 32 == original >> 32
        else:
            assert current == original
    counts['metadata'] += 1

    for slide in (0, 0x5a000000):
        for site, word, success, _ in ROOT_STORE_PATHS:
            assert donor.word(site) == word
            guard = int(image.entries[f'root_key_store_{site:x}'], 16)
            assert n.CAVE <= guard and guard + 24 <= n.CAVE_END
            assert image.word(site) == n.branch(site, guard)
            for subject, patched in ((donor, False), (image, True)):
                for status in (0, RAM_ONLY, 4, 1, -1, -25300, -34018, -34020):
                    m = Machine(subject, slide)
                    saved = seed_callee_saved(m)
                    m.uc.reg_write(UC_ARM64_REG_X0, status & 0xffffffff)
                    m.run(site, (success, site + 4))
                    accepted = status == 0 or patched and status == RAM_ONLY
                    assert m.uc.reg_read(UC_ARM64_REG_PC) == (success if accepted else site + 4) + slide
                    assert m.uc.reg_read(UC_ARM64_REG_X0) == status & 0xffffffff
                    check_frame(m, saved)
                    if patched:
                        offsets = (0, 4, 8, 16) if status == RAM_ONLY else (0, 4, 8, 12, 16 if accepted else 20)
                        assert m.visited == [site] + [guard + offset for offset in offsets]
                    counts['store_guards'] += 1

        for site, kind, size, normal, valid, invalid in ROOT_READ_PATHS:
            for subject, patched in ((donor, False), (image, True)):
                for status, length in ((0, size), (0, size - 1), (MISSING, 0), (RAM_ONLY, 0), (RAM_ONLY, size)):
                    m = Machine(subject, slide)
                    frame = STACK + 0x5000
                    m.uc.reg_write(UC_ARM64_REG_X29, frame)
                    m.uc.reg_write(UC_ARM64_REG_X19, ROOT_OBJECT)
                    m.uc.reg_write(UC_ARM64_REG_X0, status & 0xffffffff)
                    length_address = frame - 0x54 if kind not in (17, 19) else m.initial_sp + 0xc
                    m.uc.mem_write(length_address, struct.pack('<I', length))
                    m.uc.mem_write(m.initial_sp + 0x5, b'\x01' + b'\x3c' * 6)
                    m.run(site - 8, (valid, invalid, site + 4))
                    expected = site + 4 if not patched and status == RAM_ONLY else valid if status == 0 and length == size else invalid
                    assert m.uc.reg_read(UC_ARM64_REG_PC) == expected + slide
                    assert (normal in m.visited) == (patched or status != RAM_ONLY)
                    assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp and m.pac == 0
                    counts['read_branches'] += 1

        def flow(subject, expected, options=None, ready=None):
            m = Machine(subject, slide)
            helpers = RootHelpers(m, **(options or {}))
            m.run(ROOT_ENTRY)
            helpers.check_return(expected, int(expected == 0) if ready is None else ready)
            counts['root_flows'] += 1
            return m, helpers

        for subject, patched in ((donor, False), (image, True)):
            m, helpers = flow(subject, 0)
            assert not helpers.random_calls and not helpers.cloud_calls and NC_GENERATE not in m.visited
            for kind, offset in ((2, 0x88), (1, 0xa8), (17, 0x98)):
                assert bytes(m.uc.mem_read(ROOT_OBJECT + offset, 16)) == bytes([0x30 + kind]) * 16

            # Actual NC load produces 0x67 and the main function must invoke the
            # original NC generator, not merely publish startup success.
            for kind, result in ((19, (MISSING, 0)), (17, (0, 15)),
                                 (19, (RAM_ONLY, 0)), (17, (RAM_ONLY, 0))):
                expected = 0x82 if not patched and result[0] == RAM_ONLY else 0
                m, helpers = flow(subject, expected, dict(reads={kind: result}))
                assert (NC_GENERATE in m.visited) == (expected == 0)
                if expected == 0:
                    assert helpers.random_calls == [6, 16]
                    assert 0x10085ed30 in m.visited and 0x10085ed38 in m.visited

            m, helpers = flow(subject, 0 if patched else 0x82, dict(
                reads={kind: (RAM_ONLY, 0) for kind in ROOT_LENGTHS}, stores={'default': RAM_ONLY}))
            if patched:
                assert helpers.random_calls == [16, 16, 16, 6, 6, 16]
                assert len(helpers.cloud_calls) == 1 and NC_GENERATE in m.visited

            # Both Cloud generation edges and every checked 16-byte derivation
            # return a real error; none may reach ready/publication.
            for kind, caller in ((5, 0x10085e8bc), (6, 0x10085e9b8)):
                m, helpers = flow(subject, 0x82, dict(reads={kind: (MISSING, 0)}, cloud_status=CLOUD_ERROR))
                assert helpers.cloud_calls == [caller] and n.ROOT_KEY_FAILURE in m.visited
            for caller in (0x10085eaa8, 0x10085eb60, 0x10085de00, 0x10085dfec):
                reads = {19: (MISSING, 0)} if caller == 0x10085dfec else {}
                m, helpers = flow(subject, 0x82, dict(reads=reads, derivations={caller: 4}))
                assert (caller, 4) in helpers.derive_calls
                assert (NC_GENERATE in m.visited) == (caller == 0x10085dfec)

            for site, _, _, missing_kind in ROOT_STORE_PATHS:
                for status in (4, RAM_ONLY):
                    expected = 0 if patched and status == RAM_ONLY else 0x82
                    m, helpers = flow(subject, expected, dict(
                        reads={missing_kind: (MISSING, 0)}, stores={site: status}))
                    assert any(caller == site and result == status for caller, _, _, result in helpers.store_calls)
                    if status == 4 and site in n.ROOT_KEY_WRITES:
                        assert n.ROOT_KEY_FAILURE in m.visited

            # These two stores are unchecked in the donor. Preserve that fact;
            # their success path is not evidence of persistent root/bond state.
            for site, reads in ((0x10085eb48, {}), (0x10085e030, {19: (MISSING, 0)})):
                _, helpers = flow(subject, 0, dict(reads=reads, stores={site: 4}))
                assert any(caller == site and result == 4 for caller, _, _, result in helpers.store_calls)

            # Diagnostics returns its real result directly, bypassing the
            # shared failure constant; no root/NC reads or ready are published.
            m, helpers = flow(subject, 0x82, dict(diagnostics=1, diagnostic_status=0x82))
            assert n.ROOT_KEY_FAILURE not in m.visited and not helpers.read_calls
    return counts


def check(image):
    slots = image.capability_slots()
    assert len(slots) == 11
    assert all(v['address_diversity'] for v in slots.values())
    assert slots['periodic_sync']['target'] == '0x1004a5c2c'
    assert slots['adv_sets']['target'] == '0x1004c6234'
    cases = 0
    for slide in (0, 0x5a000000):
        # Check both newly accepted zero and every registration boundary. Native
        # nonzero IDs keep the original optimized range calculation.
        for chip in (0, 1, 15, 1999, 2000, 3999, 4000, 5999, 6000, 6001, 65535, 0xffffffff):
            m = Machine(image, slide)
            m.write(n.REGISTERED_CHIP, b'\xa5' * 4)
            m.write(n.REGISTERED_CONTEXT, b'\xa5' * 8)
            m.uc.reg_write(UC_ARM64_REG_X0, chip)
            m.uc.reg_write(UC_ARM64_REG_X1, CONTEXT)
            m.run(n.REGISTER)
            valid = chip <= 6000
            assert m.uc.reg_read(UC_ARM64_REG_X0) == (0 if valid else 101)
            assert m.read(n.REGISTERED_CHIP, 4) == (struct.pack('<I', chip) if valid else b'\xa5' * 4)
            assert m.read(n.REGISTERED_CONTEXT, 8) == (struct.pack('<Q', CONTEXT) if valid else b'\xa5' * 8)
            cases += 1

        m = Machine(image, slide)
        for va, size in ((n.VENDOR_OPS, n.VENDOR_OPS_BYTES), (n.VENDOR_OPCODES, 4)):
            m.write(va, b'\xa5' * size)
        for va in n.VENDOR_HOOKS:
            m.write(va, b'\xa5' * 8)
        m.write(n.VENDOR_OPS - 8, b'\x66' * 8)
        m.write(n.VENDOR_OPCODES + 4, b'\x77' * 4)
        m.uc.reg_write(UC_ARM64_REG_X0, 0)
        m.uc.reg_write(UC_ARM64_REG_X1, CONTEXT)
        m.uc.reg_write(UC_ARM64_REG_X19, 0x19191919)
        m.uc.reg_write(UC_ARM64_REG_X20, 0x20202020)
        m.run(n.VENDOR_INIT)
        assert m.bzero_calls == [(n.VENDOR_OPS + slide, n.VENDOR_OPS_BYTES)]
        assert m.read(n.VENDOR_CHIP, 4) == bytes(4)
        assert m.read(n.VENDOR_OPS, n.VENDOR_OPS_BYTES) == bytes(n.VENDOR_OPS_BYTES)
        assert all(m.read(va, 8) == bytes(8) for va in n.VENDOR_HOOKS)
        assert m.read(n.VENDOR_OPCODES, 4) == bytes(4)
        assert m.read(n.VENDOR_OPS - 8, 8) == b'\x66' * 8
        assert m.read(n.VENDOR_OPCODES + 4, 4) == b'\x77' * 4
        assert m.uc.reg_read(UC_ARM64_REG_X19) == 0x19191919
        assert m.uc.reg_read(UC_ARM64_REG_X20) == 0x20202020
        assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp and m.pac == 0
        assert m.uc.reg_read(UC_ARM64_REG_X0) == 0
        cases += 1

        for chip in (1, 2000, 4000, 6000, 6001):
            m = Machine(image, slide)
            m.uc.reg_write(UC_ARM64_REG_X0, chip)
            m.uc.reg_write(UC_ARM64_REG_X1, CONTEXT)
            m.run(n.VENDOR_INIT, (0x100377b84,))
            assert m.uc.reg_read(UC_ARM64_REG_X8) == chip - 1
            assert m.uc.reg_read(UC_ARM64_REG_X0) == chip
            assert not m.bzero_calls
            cases += 1

        for entry, site, _, generic in n.CALLBACKS:
            for chip in (0, 1, 2000, 4000, 6000):
                m = Machine(image, slide)
                m.write(n.VENDOR_CHIP, struct.pack('<I', chip))
                m.uc.reg_write(UC_ARM64_REG_X0, CONTEXT)
                m.run(entry, (site + 4,) if chip else ())
                assert m.read(generic, 8) == struct.pack('<Q', CONTEXT)
                if chip:
                    assert m.uc.reg_read(UC_ARM64_REG_X9) == chip - 1
                else:
                    assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp and m.pac == 0
                cases += 1

        for byte in (0, 1, 7, 8, 0xff):
            m = Machine(image, slide)
            m.write(n.SUPPORT_TABLE + 18, bytes([byte]))
            m.write(n.CHAIN_CONTEXT, struct.pack('<Q', CONTEXT))
            m.run(n.OPTIONAL_STEP, (n.OPTIONAL_STEP + 8,))
            if byte & 8:
                assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp - 0x30
                assert not m.next_calls and m.pac == 1
            else:
                assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp
                assert m.next_calls == [CONTEXT] and m.pac == 0
            cases += 1

        for transport in (0, 1, 2, 3, 11):
            m = Machine(image, slide)
            table = STACK + 0x100
            m.write(n.TRANSPORT, struct.pack('<Q', table))
            m.uc.mem_write(table, struct.pack('<I', transport))
            callback = 0 if transport == 1 else STOP
            m.uc.mem_write(table + 0x60, struct.pack('<Q', callback))
            m.run(n.H4_PRESTOP)
            assert m.indirect_calls == ([] if transport in (0, 1, 11) else [STOP])
            assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp and m.pac == 0
            cases += 1

        for binding, destination in (
                ('SET_EXT_ADV', 0x100c313f0), ('SET_SPEC_ADV', 0x100c313a9),
                ('SET_ADV_HANDLES', 0x100c2c4b0), ('SET_ENH_CONNECTION', 0x100c3225a),
                ('SET_EXT_SCAN', 0x100c37f85), ('SET_EXT_SCAN_HCI', 0x100c37f87), ('SET_CODED_SCAN', 0x100c37f81),
                ('SET_PERIODIC_ADV', 0x100c32120), ('SET_PERIODIC_SYNC', 0x100c263b0)):
            for value in (0, 6 if binding == 'SET_ADV_HANDLES' else 1):
                m = Machine(image, slide)
                m.write(destination, b'\xa5')
                m.uc.reg_write(UC_ARM64_REG_X0, value)
                m.run(n.NATIVE_BINDINGS[binding])
                assert m.read(destination, 1) == bytes([value])
                assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp
                cases += 1

        for name, offset in n.CAP_OFFSETS.items():
            m = Machine(image, slide)
            value = 6 if name == 'adv_sets' else 1
            m.write(n.NATIVE_STATE + offset, struct.pack('<I', value) if name == 'adv_sets' else bytes([value]))
            getter = int(image.entries['get_' + name], 16)
            m.run(getter)
            assert m.uc.reg_read(UC_ARM64_REG_X0) == value
            slot = n.MODEL_VTABLE + n.MODEL_CAPABILITIES[name]
            raw = struct.unpack_from('<Q', image.data, image.offset(slot))[0]
            assert raw >> 32 == image.fixups[slot] >> 32
            assert n.BASE + (raw & 0xffffffff) == getter
            cases += 1

        for kind, entry in enumerate((0x10032a91c, 0x10032a784)):
            for status, parameters in ((0, STACK + 0x100), (0x12, STACK + 0x100), (0, 0)):
                m = Machine(image, slide)
                features = (1 << 12) | (1 << 11) | (1 << 8)
                m.uc.mem_write(STACK + 0x100, struct.pack('<QQQ', features, 0, 0))
                m.uc.reg_write(UC_ARM64_REG_X0, status)
                if kind:
                    from unicorn.arm64_const import UC_ARM64_REG_X2
                    m.uc.reg_write(UC_ARM64_REG_X1, 1)
                    m.uc.reg_write(UC_ARM64_REG_X2, parameters)
                else:
                    m.uc.reg_write(UC_ARM64_REG_X1, parameters)
                m.run(entry)
                assert m.publish_calls == [(status, kind, parameters)]
                if status == 0 and parameters:
                    assert m.read(n.LE_FEATURES, 8) == struct.pack('<Q', features)
                assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp and m.pac == 0
                assert image.word(entry) == n.PACIBSP
                cases += 1

        m = Machine(image, slide)
        m.uc.reg_write(UC_ARM64_REG_X0, STACK + 0x100)
        m.run(n.ADV_START, (n.ADV_START + 8,))
        assert m.prepare_calls == [STACK + 0x100]
        assert m.uc.reg_read(UC_ARM64_REG_X0) == STACK + 0x100
        assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp - 0x50 and m.pac == 1
        assert image.word(n.ADV_START) == n.PACIBSP
        cases += 1

    # Machine code edits cannot silently touch existing functions or fixups.
    permitted = set(range(image.offset(n.CAVE), image.offset(image.cursor)))
    for patch in image.patches:
        start = image.offset(int(patch['address'], 16))
        permitted.update(range(start, start + 4 * len(patch['patched'])))
    assert all(a == b or at in permitted for at, (a, b) in enumerate(zip(image.original, image.data)))
    for site in n.FACTORIES:
        assert image.word(site) == n.mov_w(1, 0)
    assert image.descriptor(0x203b) == (0xd1, 0x2480)
    assert image.word(n.OPTIONAL_STEP) == n.PACIBSP
    # Every product reaches the original constructor with controller type zero.
    entry = int(image.entries['controller_type'], 16)
    assert image.word(n.CONTROLLER_THUNK) == n.branch(n.CONTROLLER_THUNK, entry)
    assert image.word(entry) == n.mov_w(1, 0) and image.word(entry + 4) == n.branch(entry + 4, n.CONTROLLER_INIT)
    cases += 1
    assert image.word(n.UNKNOWN_PRODUCT_CHECK) == 0xd503201f
    assert image.word(n.HOST_BUFFER_STATUS) == 0xaa1f03e0 and image.word(n.ERRONEOUS_STATUS) == 0xaa1f03e0
    assert image.word(n.HOST_FLOW_UNSUPPORTED) == 0xd503201f
    assert n.struct.unpack_from('<HH', image.data, image.offset(n.DESCRIPTORS + n.SET_RANDOM_ADDRESS * 24) + 2)[0] == 0x2005
    assert image.word(n.ADDRESS_REFRESH) >> 26 == 5
    assert image.word(n.ADV_ADDRESS_CACHED) == 0xd503201f
    assert image.word(n.ADDRESS_FALLBACK) == n.branch(n.ADDRESS_FALLBACK, n.ADDRESS_FINISH)
    cases += 1
    assert image.word(n.VIRTUAL_MACHINE_CHECK) == n.mov_w(0, 0) and image.word(n.VIRTUAL_MACHINE_CHECK + 4) == n.RET
    cases += 1
    # Runtime tests below distinguish permitted RAM fallback from real failure.
    for site in n.ROOT_KEY_READS + n.NC_ROOT_KEY_READS + n.ROOT_KEY_WRITES + n.NC_ROOT_KEY_WRITES:
        assert image.word(site) >> 26 == 0b000101
        cases += 1
    return cases


def check_root_observation(image):
    """Execute observed words with synthetic console effects at two slides."""
    production_sha = hashlib.sha256(image.data).hexdigest()
    assert production_sha == '2c2e13a7badcef9b7786c456eee91d29d59afce755a4e60b2f29178defa92226'
    image.observe_root_status(True)
    rows=[]
    for slide in (0,0x1e44000):
     for site,kind,word in n.ROOT_OBSERVER_POINTS:
      for value in (0,-34019,130):
       m=Machine(image,slide)
       for index,reg in enumerate(REG_X):m.uc.reg_write(reg,0x1122000000000000+index*0x101)
       for index,reg in enumerate(REG_Q):m.uc.reg_write(reg,(0xfedc000000000000+index)<<64 | (0x1234000000000000+index))
       sp=m.initial_sp
       fp=sp+0x70
       m.uc.reg_write(REG_X[29],fp)
       m.uc.reg_write(REG_X[30],site+slide)
       ra=n.BASE+0x18ec78+slide
       m.uc.mem_write(fp+8,struct.pack('<Q',ra | 0x1000000000000000))
       status=value & 0xffffffff
       m.uc.reg_write(REG_X[0],status)
       m.uc.reg_write(REG_X[21],status)
       m.uc.reg_write(REG_X[24],status)
       m.uc.reg_write(REG_X[20],sp+0x80)
       m.uc.mem_write(sp+0x80,struct.pack('<I',96))
       m.uc.mem_write(sp+n.ROOT_READ_TYPE_SLOT,struct.pack('<I',5))
       if kind==9:
        m.uc.mem_write(sp+12,struct.pack('<I',status))
        m.uc.reg_write(REG_X[0],STACK+0x4000)  # saved dictionary pointer, not the printed status
        m.uc.reg_write(REG_X[21],0x2b2b2b2b)
       m.uc.reg_write(REG_X[22],6)
       m.uc.reg_write(REG_X[23],64)
       if kind in (3,4):
        offset=0x20 if kind==3 else 0x30
        m.uc.mem_write(sp+offset,struct.pack('<QQ',fp,site+slide))
       m.uc.reg_write(UC_ARM64_REG_NZCV,0xa0000000)
       before_gpr=[m.uc.reg_read(reg) for reg in REG_X]
       before_simd=[m.uc.reg_read(reg) for reg in REG_Q]
       captures=[]
       def dprintf():
        call_sp=m.uc.reg_read(UC_ARM64_REG_SP)
        args=[struct.unpack('<i',m.uc.mem_read(call_sp+slot,4))[0] for slot in (0,8,16,24,32)]
        captures.append({'fd':m.uc.reg_read(REG_X[0]),'args':args,'sp_alignment':call_sp%16,'format_ok':bytes(m.uc.mem_read(m.uc.reg_read(REG_X[1]),len(n.ROOT_OBSERVER_FORMAT)))==n.ROOT_OBSERVER_FORMAT})
        lr=m.uc.reg_read(REG_X[30])
        for index in range(19):m.uc.reg_write(REG_X[index],0xdddd0000+index)
        for index in range(32):
         m.uc.reg_write(REG_Q[index], ((m.uc.reg_read(REG_Q[index]) & ((1<<64)-1)) | ((0xabcdef+index)<<64)) if 8<=index<16 else 0xabcdef+index)
        m.uc.reg_write(UC_ARM64_REG_NZCV,0xf0000000)
        m.uc.reg_write(UC_ARM64_REG_PC,lr)
       m.callbacks[n.DPRINTF+slide]=dprintf
       branch=n.branch_target(site,word)
       stops=[site+4]
       if branch is not None:stops.append(branch)
       m.run(site,stops,max_instructions=2048)
       after_gpr=[m.uc.reg_read(reg) for reg in REG_X]
       expected_gpr=before_gpr[:]
       if word in (0xaa1503e0,0xaa1803e0):expected_gpr[0]=before_gpr[21 if word==0xaa1503e0 else 24]
       elif word==0xaa0003f4:expected_gpr[20]=before_gpr[0]
       elif word==0xaa0003f7:expected_gpr[23]=before_gpr[0]
       elif word==0xaa0003f8:expected_gpr[24]=before_gpr[0]
       elif word==0xb9400ff5:expected_gpr[21]=status
       expected_status=0 if kind==8 else value
       expected_ra=site-n.BASE if kind>=5 else 0x18ec78
       expected_args=[kind,expected_ra,5 if kind==0 else 6 if kind==1 else 0,expected_status,96 if kind==0 else 64 if kind==1 else 0]
       rows.append({'site':hex(site),'kind':kind,'slide':hex(slide),'synthetic_status':value,'darwin_args_expected':expected_args,'darwin_args_observed':captures[0]['args'],'darwin_args_ok':captures[0]['args']==expected_args,'format_ok':captures[0]['format_ok'],'original_branch_semantics_ok':m.uc.reg_read(UC_ARM64_REG_PC)-slide==(branch if branch is not None and status==0 else site+4),'fd2':captures[0]['fd']==2,'stack_alignment_ok':captures[0]['sp_alignment']==0,'gpr_displaced_semantics_ok':after_gpr==expected_gpr,'simd_ok':all(m.uc.reg_read(reg)==before_simd[index] for index,reg in enumerate(REG_Q)),'sp_ok':m.uc.reg_read(UC_ARM64_REG_SP)==sp,'nzcv_ok':m.uc.reg_read(UC_ARM64_REG_NZCV)==0xa0000000,'pac_balanced':m.pac==0,'original_branch_selected':hex(m.uc.reg_read(UC_ARM64_REG_PC)-slide),'branch_ok':m.uc.reg_read(UC_ARM64_REG_PC)-slide==(branch if branch is not None and status==0 else site+4)})
    for row in rows:
        assert all(row[key] for key in ('fd2', 'stack_alignment_ok',
            'gpr_displaced_semantics_ok', 'simd_ok', 'sp_ok', 'nzcv_ok',
            'pac_balanced', 'darwin_args_ok', 'branch_ok')), row
    for slide in (0, 0x1e44000):
        m = Machine(image, slide)
        for kind in (1, 5, 19):
            m.uc.reg_write(REG_X[0], kind)
            m.uc.mem_write(m.initial_sp + n.ROOT_READ_TYPE_SLOT, bytes(8))
            m.run(n.ROOT_READ_TYPE_CAPTURE, (n.ROOT_READ_TYPE_CAPTURE + 4,))
            assert struct.unpack('<I', m.uc.mem_read(m.initial_sp + n.ROOT_READ_TYPE_SLOT, 4))[0] == kind
            assert m.uc.reg_read(REG_X[0]) == kind
            assert m.uc.reg_read(UC_ARM64_REG_SP) == m.initial_sp and m.pac == 0
    return dict(cases=len(rows) + 6, point_cases=len(rows), type_capture_cases=6,
        production_sha256=production_sha,
        observer_sha256=hashlib.sha256(image.data).hexdigest(),
        observer=image.root_observation, original_error_paths_unchanged=True,
        darwin_int_stack_offsets=[0, 8, 16, 24, 32],
        simulated='dprintf reads Darwin stack arguments and changes caller state; target PAC keys absent',
        hardware_verified=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[1] / 'out/native-test')
    parser.add_argument('--root-observation-only', action='store_true',
                        help='run only the new explicit Root/NC console instruction checks')
    args = parser.parse_args()
    image = n.NativeImage(args.image).vendor_routing().volatile_root_keys().h4_shutdown().capability_adapter()
    if args.root_observation_only:
        observer = check_root_observation(image)
        args.output.mkdir(parents=True, exist_ok=True)
        report = dict(passed=True, cases=observer['cases'],
            input_sha256=hashlib.sha256(image.original).hexdigest(),
            scope='new Root/NC console observation only; Native011 checks retained separately',
            root_observation=observer, hardware_verified=False)
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f"Root/NC console observer: {observer['cases']} instruction cases passed")
        return
    cases = check(image)
    root_cases = check_root_keys(n.NativeImage(args.image), image)
    cases += sum(root_cases.values())
    with TemporaryDirectory() as directory:
        wrong = Path(directory) / 'bluetoothd'
        bad = bytearray(image.original)
        bad[0x50a9dc] ^= 1
        wrong.write_bytes(bad)
        try:
            n.NativeImage(wrong)
        except ValueError as error:
            assert 'SHA-256' in str(error)
        else:
            raise AssertionError('accepted a changed target')
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(passed=True, cases=cases, input_sha256=hashlib.sha256(image.original).hexdigest(),
                  simulated='actual original and patched AArch64 instructions at two ASLR slides',
                  pointer_authentication='PAC operations modeled without target keys', hardware_verified=False,
                  root_keys=root_cases,
                  synthetic_helpers='Keychain read/store, void RNG, crypto results, model getter, locking/logging and publication')
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Native routing, capability ABI, H4 shutdown and root keys: {cases} instruction cases passed at two ASLR slides')


if __name__ == '__main__':
    main()
