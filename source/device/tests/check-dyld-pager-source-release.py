"""Compile the exact assembly/cleanup blocks against a verification-UPL stand-in."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--before',type=Path,required=True);p.add_argument('--after',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
harness=Path(__file__).with_suffix('.c').with_name('dyld-pager-source-release.c')
results=[]
with tempfile.TemporaryDirectory(prefix='aa27-dyld-release-') as tmp:
    for name,source,old in [('before',a.before,1),('after',a.after,0)]:
        text=source.read_text();begin=text.index('} else if (mwli_page_size > PAGE_SIZE) {')+len('} else if (mwli_page_size > PAGE_SIZE) {')
        end=text.index('} else if (fixup_page(dst_vaddr',begin)
        assembly=text[begin:end]
        start=text.index('\n\t\tassert(VM_PAGE_OBJECT(src_page)',end)
        if not old:start=text.index('\n\t\tif (src_page == VM_PAGE_NULL)',end)
        stop=text.index('\n\t}\n\ndone:',start)
        work=Path(tmp)/name;work.mkdir();(work/'assemble.inc').write_text(assembly);(work/'release.inc').write_text(text[start:stop])
        exe=work/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-D_DEFAULT_SOURCE','-DEXPECT_OLD='+str(old),'-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-I',str(work),str(harness),'-o',str(exe)],check=True)
        output=subprocess.check_output([str(exe)],text=True,timeout=10)
        print(output,end='');results.append(dict(version=name,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),output=output.strip(),cases=44))
a.out.write_text(json.dumps(dict(passed=True,source_extracted=True,VM_APFS_standin=True,real_kernel_or_phone_test=False,results=results),indent=2)+'\n')
