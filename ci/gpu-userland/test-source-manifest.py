#!/usr/bin/env python3
"""Exercise the production CI source verifier without network or Apple inputs."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


spec = importlib.util.spec_from_file_location('gpu_prepare', Path(__file__).with_name('prepare.py'))
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class SourceManifestTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='gpu-ci-manifest-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        (self.root / 'input.py').write_text('original source\n')
        self.declared = {'input.py': hashlib.sha256((self.root / 'input.py').read_bytes()).hexdigest()}
        self.write_manifest()
        self.stage('input.py', 'source-sha256.json')

    def stage(self, *names):
        subprocess.run(['git', '-C', str(self.root), 'add', '--', *names], check=True)

    def write_manifest(self):
        (self.root / 'source-sha256.json').write_text(json.dumps(self.declared))

    def test_complete_identity(self):
        self.assertEqual(prepare.verify_ci_sources(self.root), self.declared)

    def test_changed_tracked_source_is_rejected(self):
        (self.root / 'input.py').write_text('changed after manifest\n')
        with self.assertRaisesRegex(ValueError, 'identity differs: input.py'):
            prepare.verify_ci_sources(self.root)

    def test_new_tracked_source_requires_manifest_entry(self):
        (self.root / 'new.py').write_text('new prerequisite\n')
        self.stage('new.py')
        with self.assertRaisesRegex(ValueError, 'coverage differs: unlisted='):
            prepare.verify_ci_sources(self.root)

    def test_manifest_must_not_claim_untracked_source(self):
        (self.root / 'extra.py').write_text('not exported\n')
        self.declared['extra.py'] = hashlib.sha256((self.root / 'extra.py').read_bytes()).hexdigest()
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, 'coverage differs:'):
            prepare.verify_ci_sources(self.root)

    def test_missing_tracked_source_is_rejected(self):
        (self.root / 'input.py').unlink()
        with self.assertRaisesRegex(ValueError, 'Unsafe or absent CI source: input.py'):
            prepare.verify_ci_sources(self.root)

    def test_symlink_source_is_rejected(self):
        (self.root / 'input.py').unlink()
        (self.root / 'target').write_text('original source\n')
        (self.root / 'input.py').symlink_to('target')
        with self.assertRaisesRegex(ValueError, 'Unsafe or absent CI source: input.py'):
            prepare.verify_ci_sources(self.root)

    def test_untracked_runtime_output_is_not_an_input(self):
        (self.root / 'runtime.log').write_text('local generated output\n')
        self.assertEqual(prepare.verify_ci_sources(self.root), self.declared)


if __name__ == '__main__':
    unittest.main(verbosity=2)
