#!/usr/bin/env python3
"""Host-only argument routing checks; all target outputs are explicit fixtures."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

WRAPPER = None


class RecordRouting(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        fake_target = types.SimpleNamespace(check_object=lambda *_: None)
        with patch.dict(sys.modules, {'kernel_target': fake_target}):
            spec = importlib.util.spec_from_file_location('host_only_usb_wrapper', WRAPPER)
            self.module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(self.module)
        self.module.ROOT = self.root
        self.component = self.root / 'fixed-usb'
        self.component.mkdir()
        self.pe = self.root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json'
        self.bsd = self.root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/subr_prf.o.json'
        for path, tag in ((self.pe, 'HOST_FAKE_PE'), (self.bsd, 'HOST_FAKE_BSD')):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps({'fixture': tag}))
        self.commands = []
        self.wrong_report = False
        self.wrong_tree = False
        self.unstaged = False
        self.untracked = False

    def output(self, command, **kwargs):
        if 'status' in command and '--porcelain' in command:
            return b''
        if 'write-tree' in command:
            return 'HOST_FAKE_WRONG_TREE\n' if self.wrong_tree else 'HOST_FAKE_OVERLAY_TREE\n'
        if 'diff' in command and '--name-only' in command:
            return b'kernel/unreviewed.c\n' if self.unstaged else b''
        if 'ls-files' in command and '--others' in command:
            return b'kernel/untracked.c\n' if self.untracked else b''
        if any(str(value).endswith('/scripts/xnu_patches.py') for value in command):
            return json.dumps({'state_before': 'applied'})
        return 'HOST_FAKE_SOURCE_REVISION\n'

    def build(self, command, **kwargs):
        self.commands.append(command)
        output = Path(command[command.index('--output') + 1])
        output.mkdir(parents=True)
        archive = output / 'libQcomXnuUsbDebug.a'
        archive.write_bytes(b'HOST-ONLY-FAKE-ARCHIVE')
        (output / 'QcomXnuUsbDebug.o').write_bytes(b'HOST-ONLY-FAKE-OBJECT')
        enabled = '--userlog-compile-record' in command
        digest = hashlib.sha256(self.bsd.read_bytes()).hexdigest() if enabled else None
        if self.wrong_report:
            digest = 'HOST_FAKE_WRONG_BSD_DIGEST'
        (output / 'build-report.json').write_text(json.dumps(dict(
            archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
            userlog_enabled=enabled, userlog_compile_record_sha256=digest)))

    def invoke(self, enabled):
        env = dict(os.environ)
        env.pop('UNIXNU_QCOM_USB_USERLOG', None)
        env.pop('UNIXNU_QCOM_USB_USERLOG_TREE', None)
        if enabled:
            env['UNIXNU_QCOM_USB_USERLOG'] = '1'
            env['UNIXNU_QCOM_USB_USERLOG_TREE'] = 'HOST_FAKE_OVERLAY_TREE'
        argv = ['wrapper', '--component', str(self.component), '--soc', 'soc/Sm8850.h',
                '--profile', 'darwin25-xnu12377-unixnu']
        with patch.dict(os.environ, env, clear=True), patch.object(sys, 'argv', argv), \
                patch.object(self.module.subprocess, 'check_output', self.output), \
                patch.object(self.module.subprocess, 'run', self.build):
            self.module.main()

    def test_original_call_without_feature_is_preserved(self):
        self.invoke(False)
        self.assertEqual(len(self.commands), 1)
        self.assertNotIn('--userlog-compile-record', self.commands[0])
        self.assertEqual(self.commands[0][self.commands[0].index('--compile-record') + 1], self.pe)

    def test_native015_uses_actual_bsd_record(self):
        self.invoke(True)
        self.assertEqual(len(self.commands), 1)
        self.assertEqual(self.commands[0][self.commands[0].index('--userlog-compile-record') + 1], self.bsd)
        self.assertEqual(self.commands[0][self.commands[0].index('--compile-record') + 1], self.pe)

    def test_missing_bsd_record_never_starts_target(self):
        self.bsd.unlink()
        with self.assertRaises(SystemExit):
            self.invoke(True)
        self.assertEqual(self.commands, [])

    def test_report_from_other_bsd_record_is_rejected(self):
        self.wrong_report = True
        with self.assertRaises(SystemExit):
            self.invoke(True)

    def test_unreviewed_overlay_never_starts_target(self):
        self.wrong_tree = True
        with self.assertRaises(SystemExit):
            self.invoke(True)
        self.assertEqual(self.commands, [])

    def test_unstaged_or_untracked_source_never_starts_target(self):
        for flag in ('unstaged', 'untracked'):
            with self.subTest(flag=flag):
                setattr(self, flag, True)
                with self.assertRaises(SystemExit):
                    self.invoke(True)
                self.assertEqual(self.commands, [])
                setattr(self, flag, False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wrapper', type=Path, required=True)
    args = parser.parse_args()
    WRAPPER = args.wrapper.resolve()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(RecordRouting))
    print(json.dumps({'passed': result.wasSuccessful(), 'host_cases': result.testsRun,
                      'target_compiled': False, 'fixtures_only': True, 'phone_operations': False}))
    sys.exit(0 if result.wasSuccessful() else 1)
