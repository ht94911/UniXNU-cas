#!/usr/bin/env python3
"""Check original IOSurface plane routines by full hashes, without exporting code."""
import argparse,hashlib,json,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--kernel',type=Path,required=True)
p.add_argument('--prelink-tools',type=Path,required=True)
p.add_argument('--prelink-report',type=Path)
p.add_argument('--native-report',type=Path)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parent
metadata=json.loads((root/'data/iosurface-plane-iphone-24A446.json').read_text())
abi=json.loads((root/'legacy-display-iphone-24A446.json').read_text())
data=a.kernel.read_bytes();digest=hashlib.sha256(data).hexdigest()
slide=0
if a.prelink_report:
    report=json.loads(a.prelink_report.read_text());slide=int(report['collection_slide'],16)
    assert report['candidate_sha256']==digest and not report['missing'] and not report['stubbed']
else:
    assert digest==metadata['kernelcache_sha256']
sys.path.insert(0,str(a.prelink_tools.resolve()))
from kc_image import Image
image=Image(data);checks={}
compiled=json.loads(a.native_report.read_text()) if a.native_report else None
for name,check in metadata['functions'].items():
    assert abi['bindings'][name]==check['address']
    if compiled:assert compiled['bindings'][name]==check['address']
    address=int(check['address'],16)+slide
    actual=hashlib.sha256(image.read(address,check['bytes'])).hexdigest()
    assert actual==check['sha256'],(name,hex(address),actual)
    checks[name]={'address':hex(address),'bytes':check['bytes'],'sha256':actual}
out={'passed':True,'hardware_verified':False,'kernel':str(a.kernel),'kernel_sha256':digest,
     'collection_slide':hex(slide),'actual_original_routines':checks,
     'compiled_binding_report_checked':bool(compiled),'metadata_only_no_original_code_exported':True}
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(out,indent=2)+'\n')
print(f'PASS {len(checks)} original IOSurface plane/handle routines; exact ABI and code hashes; no phone access')
