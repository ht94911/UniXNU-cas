# SPDX-License-Identifier: BSD-2-Clause-Patent
import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from cpu0_identity import candidate_tag
class TagTests(unittest.TestCase):
 def setUp(self):self.contract={'profile':'cas-cpu0-002','kernel':{'sha256':'a'*64}};self.sources={'device':'b'*40,'uefi':'c'*40,'framework':'d'*40}
 def test_map_order_does_not_change_identity(self):self.assertEqual(candidate_tag(self.contract,'execute','e'*64,self.sources),candidate_tag(self.contract,'execute','e'*64,dict(reversed(list(self.sources.items())))))
 def test_profile_mode_kernel_and_entropy_identity_are_bound(self):
  base=candidate_tag(self.contract,'execute','e'*64,self.sources)
  for contract,mode,adt in [({'profile':'different','kernel':self.contract['kernel']},'execute','e'*64),(self.contract,'validate','e'*64),(self.contract,'execute','f'*64),({'profile':self.contract['profile'],'kernel':{'sha256':'f'*64}},'execute','e'*64)]:self.assertNotEqual(base,candidate_tag(contract,mode,adt,self.sources))
 def test_every_source_commit_changes_identity(self):
  base=candidate_tag(self.contract,'execute','e'*64,self.sources)
  for name in self.sources:
   changed=dict(self.sources);changed[name]='0'*40;self.assertNotEqual(base,candidate_tag(self.contract,'execute','e'*64,changed))
if __name__=='__main__':unittest.main()
