#!/usr/bin/env python3
"""Host fixtures for the actual Native016 consumed BSD record contract.

The Native015 final raw record and archive report are read-only provenance.
Consumed-0 bytes are an exact SHA-matched restoration; all snapshot files and
new report snapshot keys below are artificial fixture views, never original
collected Native015 data. Fake compiler/ar/nm outputs do not compile a target.
"""
import argparse
import contextlib
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

CTX = None
DISABLED = b'"XNU_BOOT_PROGRESS=0"'
ENABLED = b'"XNU_BOOT_PROGRESS=1"'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def replace_argument(raw, old, new):
    data = json.loads(raw.decode().rstrip().rstrip(','))
    index = data['arguments'].index(old)
    data['arguments'][index] = new
    return (json.dumps(data, indent=2) + '\n').encode()


class RecordContract(unittest.TestCase):
    def view(self, name, snapshot=None, final=None, update_hashes=False):
        directory = CTX['out'] / 'views' / name
        original = Path(CTX['report']['evidence_directory'])
        snapshot_path = directory / 'usb-attempts' / original.parent.name / 'userlog-compile-record.json'
        final_path = directory / 'objects/bsd/DEVELOPMENT/subr_prf.o.json'
        snapshot_path.parent.mkdir(parents=True)
        final_path.parent.mkdir(parents=True)
        snapshot = CTX['consumed'] if snapshot is None else snapshot
        final = CTX['final'] if final is None else final
        snapshot_path.write_bytes(snapshot)
        final_path.write_bytes(final)
        report = copy.deepcopy(CTX['report'])
        report['userlog_compile_record_snapshot'] = str(original / snapshot_path.name)
        report['userlog_compile_record_snapshot_sha256'] = report['userlog_compile_record_sha256']
        if update_hashes:
            report['userlog_compile_record_sha256'] = sha(snapshot)
            report['userlog_compile_record_snapshot_sha256'] = sha(snapshot)
        (directory / 'HOST_FIXTURE.json').write_text(json.dumps(dict(
            artificial_view=True, original_Native015_snapshot_was_collected=False,
            report_snapshot_keys_are_artificial=True, test=name,
            snapshot_derivation='Base is exact SHA-matched progress1->0 restoration; named test mutations apply only to this artificial view',
            source_artifacts=str(CTX['artifacts']),
            original_consumed_SHA256=CTX['report']['userlog_compile_record_sha256'],
            fixture_snapshot_SHA256=sha(snapshot), fixture_final_SHA256=sha(final),
            fixture_report_hashes_updated=update_hashes), indent=2) + '\n')
        return directory, report, snapshot_path, final_path

    def check(self, name, view, report, expected=None):
        try:
            result = CTX['checker'].userlog_records(
                CTX['out'], CTX['component'], CTX['manifest'], report,
                CTX['builder'], artifacts=view)
        except (ValueError, OSError, KeyError, IndexError) as error:
            CTX['cases'].append(dict(name=name, accepted=False, reason=str(error), expected=expected or 'reject'))
            if expected:
                raise
            return
        CTX['cases'].append(dict(name=name, accepted=True, transition=result['transition'], expected=expected or 'reject'))
        if not expected:
            self.fail('Actual userlog_records accepted a forbidden fixture: ' + name)
        self.assertEqual(result['transition'], expected)
        self.assertEqual(result['snapshot_sha256'], report['userlog_compile_record_sha256'])

    def test_exact_consumed0_to_final1(self):
        view, report, _, _ = self.view('exact-consumed0-to-final1')
        self.check('exact-consumed0-to-final1', view, report, 'XNU_BOOT_PROGRESS=0->1')

    def test_raw_unchanged0(self):
        view, report, _, _ = self.view('raw-unchanged0', final=CTX['consumed'])
        self.check('raw-unchanged0', view, report, 'unchanged')

    def test_snapshot_failures(self):
        for name in ('missing-snapshot', 'bad-snapshot-json', 'escaped-snapshot-path',
                     'bad-consumed-SHA', 'bad-snapshot-SHA'):
            with self.subTest(name=name):
                view, report, snapshot, final = self.view(name)
                if name == 'missing-snapshot': snapshot.unlink()
                elif name == 'bad-snapshot-json':
                    snapshot.write_bytes(b'HOST_INVALID_JSON')
                    final.write_bytes(b'HOST_INVALID_JSON')
                    report['userlog_compile_record_sha256'] = sha(b'HOST_INVALID_JSON')
                    report['userlog_compile_record_snapshot_sha256'] = sha(b'HOST_INVALID_JSON')
                elif name == 'escaped-snapshot-path':
                    report['userlog_compile_record_snapshot'] = str(Path(report['evidence_directory']).parent / snapshot.name)
                elif name == 'bad-consumed-SHA': report['userlog_compile_record_sha256'] = '0' * 64
                else: report['userlog_compile_record_snapshot_sha256'] = '0' * 64
                self.check(name, view, report)

    def test_final_changes_beyond_witnessed_transition(self):
        data = json.loads(CTX['final'].decode().rstrip().rstrip(','))
        old_include = next(a for a in data['arguments'] if a.endswith('/EXPORT_HDRS/bsd'))
        mutations = {
            'final-wrong-directory': lambda d: d.update(directory=d['directory'] + '-wrong'),
            'final-wrong-file': lambda d: d.update(file=d['file'].replace('subr_prf.c', 'other.c')),
            'final-wrong-output': lambda d: d.update(output='other.o'),
            'final-wrong-arch': lambda d: d['arguments'].__setitem__(d['arguments'].index('arm64e'), 'arm64'),
            'final-wrong-include': lambda d: d['arguments'].__setitem__(d['arguments'].index(old_include), old_include + '-wrong'),
            'final-other-flags': lambda d: d['arguments'].__setitem__(d['arguments'].index('-O2'), '-O0')}
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                candidate = copy.deepcopy(data); mutate(candidate)
                raw = (json.dumps(candidate, indent=2) + '\n').encode()
                view, report, _, _ = self.view(name, final=raw)
                self.check(name, view, report)
        view, report, _, _ = self.view('final-whitespace-change', final=CTX['final'] + b' ')
        self.check('final-whitespace-change', view, report)

    def test_reverse_transition_is_rejected(self):
        view, report, _, _ = self.view('reverse1-to0', snapshot=CTX['final'], final=CTX['consumed'], update_hashes=True)
        self.check('reverse1-to0', view, report)

    def test_both_record_metadata_changed_with_updated_SHA(self):
        for field, value in (('directory', '/HOST_WRONG_BSD'), ('file', '/HOST_WRONG/subr_prf.c'), ('output', 'other.o')):
            with self.subTest(field=field):
                data = json.loads(CTX['consumed'].decode().rstrip().rstrip(',')); data[field] = value
                raw0 = (json.dumps(data, indent=2) + '\n').encode()
                raw1 = raw0.replace(DISABLED, ENABLED, 1)
                name = 'both-records-wrong-' + field + '-SHA-updated'
                view, report, _, _ = self.view(name, snapshot=raw0, final=raw1, update_hashes=True)
                self.check(name, view, report)

    def test_both_record_flags_changed_with_updated_SHA(self):
        arguments = json.loads(CTX['consumed'].decode().rstrip().rstrip(','))['arguments']
        include = next(a for a in arguments if a.endswith('/EXPORT_HDRS/bsd'))
        for label, old, new in (('arch', 'arm64e', 'arm64'), ('include', include, include + '-wrong'), ('optimization', '-O2', '-O0')):
            with self.subTest(label=label):
                raw0 = replace_argument(CTX['consumed'], old, new)
                name = 'both-records-wrong-' + label + '-SHA-updated'
                view, report, _, _ = self.view(name, snapshot=raw0, final=raw0.replace(DISABLED, ENABLED, 1), update_hashes=True)
                self.check(name, view, report)

    def test_actual_compile_command_changes(self):
        for name in ('command-wrong-BSD-flag', 'command-wrong-output', 'command-double-output', 'command-late-target-override'):
            with self.subTest(name=name):
                view, report, _, _ = self.view(name)
                command = next(c for c in report['commands'] if c[-3].endswith('/kernel/QcomUsbUserLog.c'))
                if name == 'command-wrong-BSD-flag': command[command.index('-O2')] = '-O0'
                elif name == 'command-wrong-output': command[-1] = str(Path(command[-1]).with_name('other.o'))
                elif name == 'command-double-output': command.extend(['-o', '/HOST_WRONG/output.o'])
                else: command.insert(command.index('-c'), '--target=arm64-apple-ios27.0.0')
                self.check(name, view, report)

    def test_builder_reads_BSD_once_and_keeps_initial_flags(self):
        directory = CTX['out'] / 'tmp' / 'builder-fixture'
        directory.mkdir(parents=True)
        fake_source = directory / 'HOST_FAKE_subr_prf.c'; fake_source.write_text('/* HOST FIXTURE */\n')
        fake_pe = directory / 'HOST_FAKE_pe_kprintf.c'; fake_pe.write_text('/* HOST FIXTURE */\n')
        data = json.loads(CTX['consumed'].decode().rstrip().rstrip(','))
        old_source = data['file']; data['file'] = str(fake_source); data['directory'] = str(directory)
        data['arguments'] = [str(fake_source) if a == old_source else a for a in data['arguments']]
        initial = (json.dumps(data, indent=2) + '\n').encode()
        bsd = directory / 'HOST_FAKE_subr_prf.o.json'; bsd.write_bytes(initial)
        pe = directory / 'HOST_FAKE_pe_kprintf.o.json'
        pe.write_text(json.dumps(dict(directory=str(directory), file=str(fake_pe), output='HOST_FAKE_pe.o',
            arguments=['HOST_FAKE_CC', '-target', 'arm64e-apple-ios27.0', '-c', str(fake_pe), '-o', 'HOST_FAKE_pe.o'])))
        module_out = directory / 'module'
        commands, reads = [], []
        original_read_bytes, original_read_text = Path.read_bytes, Path.read_text
        def read_bytes(path):
            if path.resolve() == bsd.resolve(): reads.append('read_bytes')
            return original_read_bytes(path)
        def read_text(path, *args, **kwargs):
            if path.resolve() == bsd.resolve(): reads.append('read_text')
            return original_read_text(path, *args, **kwargs)
        def fake_run(command, **kwargs):
            commands.append([str(a) for a in command])
            if command[0] == 'HOST_FAKE_AR':
                Path(command[2]).write_bytes(b'HOST_FAKE_ARCHIVE_NO_TARGET_CODE')
            else:
                path = Path(command[command.index('-o') + 1])
                path.write_bytes(b'HOST_FAKE_OBJECT_NO_TARGET_CODE')
                if len(commands) == 1: bsd.write_bytes(initial.replace(DISABLED, ENABLED, 1))
            return subprocess.CompletedProcess(command, 0)
        def fake_output(command, **kwargs):
            if command[0] == 'git': return CTX['report']['component_revision'] + '\n'
            if command[0] != 'HOST_FAKE_NM': raise AssertionError('Unexpected fixture tool ' + str(command))
            if '--defined-only' in command:
                return '\n'.join('0000 T ' + name for name in CTX['report']['public_exports']) + '\n'
            if str(command[-1]).endswith('libQcomXnuUsbDebug.a'):
                return '\n'.join('U ' + name for name in CTX['report']['external_imports']) + '\n'
            return ''
        argv = ['builder', '--compile-record', str(pe), '--userlog-compile-record', str(bsd),
                '--soc', 'soc/Sm8850.h', '--xnu-profile', 'darwin25-xnu12377-unixnu',
                '--output', str(module_out), '--ar', 'HOST_FAKE_AR', '--nm', 'HOST_FAKE_NM']
        sys.path.insert(0, str(CTX['component'] / 'scripts'))
        with patch.object(sys, 'argv', argv), patch.object(Path, 'read_bytes', read_bytes), \
                patch.object(Path, 'read_text', read_text), \
                patch.object(CTX['builder'].subprocess, 'run', fake_run), \
                patch.object(CTX['builder'].subprocess, 'check_output', fake_output):
            CTX['builder'].main()
        report = json.loads((module_out / 'build-report.json').read_text())
        self.assertEqual(reads, ['read_bytes'])
        self.assertEqual((module_out / 'userlog-compile-record.json').read_bytes(), initial)
        self.assertEqual(report['userlog_compile_record_sha256'], sha(initial))
        self.assertEqual(report['userlog_compile_record_snapshot_sha256'], sha(initial))
        self.assertEqual(bsd.read_bytes(), initial.replace(DISABLED, ENABLED, 1))
        command = next(c for c in commands if '-c' in c and c[c.index('-c') + 1].endswith('/kernel/QcomUsbUserLog.c'))
        flags, _ = CTX['builder'].compile_flags(data)
        self.assertEqual(command[:len(flags)], flags)
        self.assertEqual(command.count('XNU_BOOT_PROGRESS=0'), 1)
        self.assertNotIn('XNU_BOOT_PROGRESS=1', command)
        fixture = dict(name='builder-BSD-read-once-external-record-rewritten', accepted=True, expected='accept',
            compiler_ar_nm_outputs='HOST_FAKE bytes; no target code or tool execution',
            target_compiled=False, fixtures_only=True, BSD_reads=reads,
            consumed_SHA256=sha(initial), external_rewritten_SHA256=sha(bsd.read_bytes()),
            saved_snapshot_SHA256=sha((module_out / 'userlog-compile-record.json').read_bytes()),
            UserLog_flags_retained_initial0=True, commands=commands)
        CTX['cases'].append(fixture)
        (directory / 'HOST_FIXTURE.json').write_text(json.dumps(fixture, indent=2) + '\n')


def main():
    global CTX
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checker', type=Path, required=True)
    parser.add_argument('--component', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True, help='read-only actual Native015 artifacts')
    parser.add_argument('--output', type=Path, required=True, help='new synthetic host evidence directory')
    args = parser.parse_args()
    out, component, artifacts, checker_path = [p.resolve() for p in (args.output, args.component, args.artifacts, args.checker)]
    for source in (component, artifacts, checker_path.parent.parent):
        if out == source or source in out.parents: parser.error('output must be outside the selected sources/artifacts')
    report_path = artifacts / 'archives/qcom-usb-debug/build-report.json'
    final_path = artifacts / 'objects/bsd/DEVELOPMENT/subr_prf.o.json'
    input_path = artifacts / 'inputs.json'
    watched = (report_path, final_path, input_path, checker_path, component / 'scripts/build.py')
    before = {str(p): sha(p.read_bytes()) for p in watched}
    report = json.loads(report_path.read_text())
    final = final_path.read_bytes()
    if final.count(ENABLED) != 1 or final.count(DISABLED):
        raise ValueError('Require the retained Native015 final raw record with one progress1 token')
    consumed = final.replace(ENABLED, DISABLED, 1)
    if sha(consumed) != report['userlog_compile_record_sha256']:
        raise ValueError('Consumed0 restoration does not match the actual original Native015 e30 record SHA')
    out.mkdir(parents=True, exist_ok=False)
    CTX = dict(out=out, component=component, artifacts=artifacts, report=report,
               manifest=json.loads(input_path.read_text())['userlog_target'], final=final, consumed=consumed,
               checker=load('actual_record_checker', checker_path),
               builder=load('actual_record_builder', component / 'scripts/build.py'), cases=[])
    with (out / 'tests.log').open('w') as log, contextlib.redirect_stdout(log):
        result = unittest.TextTestRunner(stream=log, verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(RecordContract))
    after = {str(p): sha(p.read_bytes()) for p in watched}
    selected_unchanged = before == after
    summary = dict(passed=result.wasSuccessful() and selected_unchanged,
        target_compiled=False, fixtures_only=True, phone_operations=False,
        actual_Native015_CI_status='failure remains retained; these are new synthetic Native016 record-contract fixtures',
        original_Native015_snapshot_collected=False, new_report_snapshot_keys_are_artificial=True,
        consumed0_derivation='Exact single raw progress1->0 restoration, matching original e30 consumed SHA',
        retained_final_record_SHA256=sha(final), derived_consumed_record_SHA256=sha(consumed),
        original_report_consumed_SHA256=report['userlog_compile_record_sha256'],
        tests_run=result.testsRun, fixture_cases=len(CTX['cases']), failures=len(result.failures), errors=len(result.errors),
        selected_source_and_artifact_bytes_unchanged=selected_unchanged, input_SHA256=before,
        fixture_results=CTX['cases'], runner_SHA256=sha(Path(__file__).read_bytes()))
    (out / 'host-record-report.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({k: v for k, v in summary.items() if k not in ('fixture_results', 'input_SHA256')}, indent=2))
    sys.exit(0 if summary['passed'] else 1)


if __name__ == '__main__':
    main()
