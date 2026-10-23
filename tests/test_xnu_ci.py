"""Exercise actual encryption and rejection of malformed downloaded archives."""
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import tarfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'ci/xnu' / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


artifacts = module('cas_xnu_artifacts', 'artifacts.py')
preflight = module('cas_xnu_preflight', 'preflight.py')


class ArtifactTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.keys = Path(cls.directory.name)
        for label in ('owner', 'other'):
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                            '-keyout', str(cls.keys / (label + '.key')),
                            '-out', str(cls.keys / (label + '.pem')), '-days', '1',
                            '-subj', '/CN=temporary artifact test'],
                           capture_output=True, check=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.source = self.root / 'source'; self.source.mkdir()
        (self.source / 'kernel').write_bytes(b'kernel test\x00' * 5000)
        (self.source / 'symbols').write_bytes(b'matching-symbol test\x00' * 5000)

    def tearDown(self):
        self.tmp.cleanup()

    def seal(self):
        return artifacts.seal(self.source, ['kernel', 'symbols'], self.keys / 'owner.pem',
                              self.root / 'sealed')

    def test_encryption_roundtrip_binds_both_files(self):
        self.seal()
        manifest = artifacts.unseal(self.root / 'sealed', self.keys / 'owner.key', self.root / 'opened')
        self.assertEqual({f['path'] for f in manifest['files']}, {'kernel', 'symbols'})
        for name in ('kernel', 'symbols'):
            self.assertEqual((self.source / name).read_bytes(), (self.root / 'opened' / name).read_bytes())
        self.assertNotIn(b'matching-symbol test', (self.root / 'sealed/bundle.cms').read_bytes())

    def test_wrong_key_has_no_output(self):
        self.seal()
        with self.assertRaises(ValueError):
            artifacts.unseal(self.root / 'sealed', self.keys / 'other.key', self.root / 'opened')
        self.assertFalse((self.root / 'opened').exists())

    def test_ciphertext_corruption_is_rejected(self):
        self.seal()
        cipher = self.root / 'sealed/bundle.cms'
        data = bytearray(cipher.read_bytes()); data[len(data) // 2] ^= 1; cipher.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'Ciphertext identity'):
            artifacts.unseal(cipher.parent, self.keys / 'owner.key', self.root / 'opened')
        self.assertFalse((self.root / 'opened').exists())

    def test_outputs_are_never_overwritten(self):
        self.seal()
        with self.assertRaises(FileExistsError):
            self.seal()
        output = self.root / 'opened'; output.mkdir(); (output / 'keep').write_text('saved')
        with self.assertRaises(FileExistsError):
            artifacts.unseal(self.root / 'sealed', self.keys / 'owner.key', output)
        self.assertEqual((output / 'keep').read_text(), 'saved')

    def test_source_symlink_and_escape_are_rejected(self):
        (self.source / 'alias').symlink_to(self.source / 'kernel')
        for name in ('alias', '../outside', '/absolute', './kernel', 'C:/input', '.', 'manifest.json'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                artifacts.seal(self.source, [name], self.keys / 'owner.pem', self.root / 'sealed')
        self.assertFalse((self.root / 'sealed').exists())

    def raw_archive(self, entries):
        archive = self.root / 'malformed.tar'
        with tarfile.open(archive, 'w') as stream:
            for name, data, kind in entries:
                item = tarfile.TarInfo(name); item.type = kind; item.size = len(data)
                stream.addfile(item, io.BytesIO(data))
        return archive

    def test_traversal_archive_is_rejected_before_extraction(self):
        manifest = json.dumps({'schema_version': 1, 'files': [
            {'path': '../escape', 'bytes': 1, 'sha256': '0' * 64}]}).encode()
        raw = self.raw_archive([('manifest.json', manifest, tarfile.REGTYPE),
                                ('../escape', b'x', tarfile.REGTYPE)])
        with self.assertRaises(ValueError):
            artifacts.unpack_verified(raw, self.root / 'opened')
        self.assertFalse((self.root / 'opened').exists())

    def test_manifest_hash_mismatch_is_rejected_before_extraction(self):
        manifest = json.dumps({'schema_version': 1, 'files': [
            {'path': 'kernel', 'bytes': 1, 'sha256': '0' * 64}]}).encode()
        raw = self.raw_archive([('manifest.json', manifest, tarfile.REGTYPE),
                                ('kernel', b'x', tarfile.REGTYPE)])
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            artifacts.unpack_verified(raw, self.root / 'opened')
        self.assertFalse((self.root / 'opened').exists())

    def test_archive_links_are_rejected(self):
        raw = self.raw_archive([('manifest.json', b'{}', tarfile.REGTYPE),
                                ('link', b'', tarfile.SYMTYPE)])
        with self.assertRaises(ValueError):
            artifacts.unpack_verified(raw, self.root / 'opened')
        self.assertFalse((self.root / 'opened').exists())


class ObjectTests(unittest.TestCase):
    def check_object(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'target.o'; path.write_bytes(data)
            return preflight.inspect_object(path)

    def object_bytes(self, cpu=0x100000c, platform=2, minimum=17 << 16):
        return struct.pack('<8I', 0xfeedfacf, cpu, 0, 1, 1, 24, 0, 0) + \
            struct.pack('<6I', 0x32, 24, platform, minimum, 0, 0)

    def test_arm64_ios17_object(self):
        self.assertEqual(self.check_object(self.object_bytes())['build_version']['platform'], 2)

    def test_wrong_architecture_target_and_truncation(self):
        for data in (self.object_bytes(cpu=0x1000007), self.object_bytes(platform=1),
                     self.object_bytes(minimum=18 << 16), self.object_bytes()[:-1]):
            with self.subTest(data=data.hex()), self.assertRaises(ValueError):
                self.check_object(data)


if __name__ == '__main__':
    unittest.main()
