"""Refuse applying the entry patch to the wrong kernel source or patch bytes."""
import io,json,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'ci/xnu'))
import build_source
class EntryTraceTests(unittest.TestCase):
 def test_wrong_commit_is_rejected_without_applying(self):
  with tempfile.TemporaryDirectory() as root,patch.object(build_source,'git',return_value=b'0'*40+b'\n'),patch.object(build_source.subprocess,'run') as run:
   with self.assertRaisesRegex(ValueError,'commit mismatch'):build_source.apply_entry_trace(Path(root),io.StringIO())
   run.assert_not_called()
 def test_wrong_source_is_rejected_without_applying(self):
  profile=json.loads((build_source.HERE/'entry-trace.json').read_text())
  with tempfile.TemporaryDirectory() as root,patch.object(build_source,'git',return_value=profile['xnu_commit'].encode()),patch.object(build_source.subprocess,'run') as run:
   p=Path(root)/profile['source'];p.parent.mkdir(parents=True);p.write_text('different source')
   with self.assertRaisesRegex(ValueError,'hash mismatch'):build_source.apply_entry_trace(Path(root),io.StringIO())
   run.assert_not_called()
if __name__=='__main__':unittest.main()
