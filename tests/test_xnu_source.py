"""Check corruption refusal and real git-blob verification for source candidates."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'ci/xnu'))
from build_source import kernel_identity, tracked_manifest


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


if __name__ == '__main__':
    unittest.main()
