"""Check corruption refusal and real git-blob verification for source candidates."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'ci/xnu'))
from build_source import kernel_identity, tracked_manifest, select_kernel_patches


def fixture(cpu=0x100000c, count=1, symoff=80):
    header = struct.pack('<8I', 0xfeedfacf, cpu, 0, 2, 2, 48, 0, 0)
    uuid = struct.pack('<II', 0x1b, 24) + bytes(range(16))
    symbols = struct.pack('<6I', 2, 24, symoff, count, 96, 8)
    return header + uuid + symbols + bytes(16) + b'\0_symbol'


class SourceTests(unittest.TestCase):
    def inspect(self, data):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'kernel'
            path.write_bytes(data)
            return kernel_identity(path)

    def test_unstripped_arm64_identity_binds_bytes(self):
        data = fixture()
        result = self.inspect(data)
        self.assertEqual(result['sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(result['uuid'], bytes(range(16)).hex())
        self.assertEqual(result['symbol_count'], 1)

    def test_wrong_architecture_and_missing_symbols_refused(self):
        for data in (fixture(cpu=0x1000007), fixture(count=0)):
            with self.subTest(data=data[:16]):
                with self.assertRaises(ValueError):
                    self.inspect(data)

    def test_truncated_or_out_of_file_symbols_refused(self):
        for data in (fixture()[:40], fixture(symoff=4096)):
            with self.assertRaises(ValueError):
                self.inspect(data)

    def test_real_git_blob_verification_rejects_changed_worktree(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root)
            subprocess.run(['git', '-C', root, 'init', '-q'], check=True)
            (path / 'source.c').write_text('int example;\n')
            subprocess.run(['git', '-C', root, 'add', 'source.c'], check=True)
            subprocess.run(['git', '-C', root, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@invalid',
                            'commit', '-qm', 'fixture'], check=True)
            result = tracked_manifest(path)
            self.assertEqual(len(result), 1)
            self.assertEqual(result[0]['sha256'], hashlib.sha256(b'int example;\n').hexdigest())
            (path / 'source.c').write_text('int changed;\n')
            with self.assertRaises(ValueError):
                tracked_manifest(path)

    def test_patch_selection_checks_full_baseline_before_exclusion(self):
        with tempfile.TemporaryDirectory() as root:
            tree = Path(root) / 'tree'
            patches = tree / 'patches/xnu'
            patches.mkdir(parents=True)
            baseline = []
            for name in ('core.patch', 'optional.patch'):
                data = ('patch ' + name).encode()
                (patches / name).write_bytes(data)
                baseline.append({'path': 'patches/xnu/' + name, 'sha256': hashlib.sha256(data).hexdigest()})
            spec = {'kernel_patches': baseline, 'excluded_kernel_patches': [
                {'path': baseline[1]['path'], 'reason': 'Optional feature disabled'}]}
            (patches / 'optional.patch').write_bytes(b'changed')
            with self.assertRaises(ValueError):
                select_kernel_patches(tree, Path(root), spec)
            self.assertTrue((patches / 'core.patch').is_file())
            (patches / 'optional.patch').write_bytes(b'patch optional.patch')
            selection = select_kernel_patches(tree, Path(root), spec)
            self.assertEqual(selection['effective'], baseline[:1])
            self.assertTrue((Path(root) / 'excluded-kernel-patches/optional.patch').is_file())
            self.assertFalse((patches / 'optional.patch').exists())


if __name__ == '__main__':
    unittest.main()
