"""Exercise the current vnode bitmap path and text applicator after page reclamation."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--vm-fault',type=Path,required=True)
p.add_argument('--ubc-subr',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
vm=a.vm_fault.read_text();ubc=a.ubc_subr.read_text()
start=vm.index('vm_page_validate_cs_fast(\n');end=vm.index('\nvoid\nvm_page_validate_cs_mapped_slow',start)
current=vm[start:end]
marker='\n#if __arm64__\n\t\t/* A validated vnode bitmap survives page reclamation.'
assert marker in current and current.count('cs_text_fixup_page(')==1
begin=current.index(marker);finish=current.index('\n#endif',begin)+len('\n#endif')
previous=current[:begin]+current[finish:]
assert 'cs_text_fixup_page(' not in previous and current.index('vnode_pager_cs_check_validation_bitmap(')<current.index('cs_text_fixup_page(')
first=ubc.index('#define CS_TEXT_FIXUP_MAX');last=ubc.index('\nboolean_t\ncs_validate_range',first)
applicator=ubc[first:last]
# The host uses uint64_t=unsigned long; normalize only these diagnostic prints.
applicator=applicator.replace('f->offset, f->length);','(unsigned long long)f->offset, f->length);')
current=current.replace('page->vmp_offset);','(unsigned long long)page->vmp_offset);')
previous=previous.replace('page->vmp_offset);','(unsigned long long)page->vmp_offset);')
validation='static boolean_t\n'+previous.replace('vm_page_validate_cs_fast(', 'before_validate(')+'\nstatic boolean_t\n'+current.replace('vm_page_validate_cs_fast(', 'after_validate(')
with tempfile.TemporaryDirectory(prefix='text-reclaimed-')as folder:
 w=Path(folder);(w/'applicator.inc').write_text(applicator);(w/'validation.inc').write_text(validation)
 binary=w/'test';compiler=os.environ.get('CC','cc');mac=(os.uname().sysname=='Darwin')
 harness=Path(__file__).with_name('text-fixup-reclaimed-page.c')
 source=harness.read_text()
 if mac:source=source.replace('#include <bsd/string.h>','')
 (w/'test.c').write_text(source)
 flags=['-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
 if not mac:flags+=['-no-pie']
 command=[compiler,*flags,'-I',str(w),str(w/'test.c'),'-o',str(binary)]
 if not mac:command+=['-lbsd']
 subprocess.run(command,check=True)
 output=subprocess.check_output([str(binary)],text=True,timeout=15)
 assert 'regression reproduced' in output and 'PASS reclaimed-page cases 11' in output
 result=dict(passed=True,source_extracted=True,before_regression_reproduced=True,cases=11,ASan_UBSan=True,
  vm_fault_sha256=hashlib.sha256(a.vm_fault.read_bytes()).hexdigest(),ubc_subr_sha256=hashlib.sha256(a.ubc_subr.read_bytes()).hexdigest(),
  original_validation_bitmap_retained=True,original_byte_match_required=True,cache_sync_on_actual_write=True,
  already_resident_no_repeat=True,tainted_or_writable_unchanged=True,bitmap_failure_uses_original_slow_validation=True,
  physical_kernel_test=False,phone_operations=False,output=output)
 a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps(result,indent=2)+'\n');print(output,end='')
