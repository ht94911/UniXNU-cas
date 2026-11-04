#!/usr/bin/env python3
"""Bind the tested fake-IOKit harness to the actual prepared sources, then run it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu',type=Path,required=True)
p.add_argument('--logs',type=Path,required=True)
a=p.parse_args();root=a.unixnu.resolve();fixtures=Path(__file__).with_name('fixtures')
bindings=json.loads((fixtures/'source-bindings.json').read_text())
for name,digest in bindings.items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest, ('source binding',name)
def extract(text,marker,kind):
    start=text.index(marker)
    if kind=='function':
        match=re.search(r'^'+re.escape(marker)+r'\([^;{]*\)\s*\n\{',text,re.M);assert match,marker
        start=text.rfind('\n',0,match.start()-1)+1
    opening=text.index('{',start)
    lex=re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',lambda m:' '*len(m[0]),text,flags=re.S)
    depth=0
    for i in range(opening,len(text)):
        if lex[i]=='{':depth+=1
        if lex[i]=='}':
            depth-=1
            if not depth:return text[start:i+1]+(';' if text[i+1:i+2]==';' else '')+'\n'
    raise ValueError(marker)
snippets=json.loads((fixtures/'snippet-bindings.json').read_text())
for row in snippets:
    code=extract((root/row['source']).read_text(),row['marker'],row['kind'])
    assert hashlib.sha256(code.encode()).hexdigest()==row['sha256'],row
    assert code in (fixtures/row['fixture']).read_text(),row
out=a.logs/'device-context-host';out.mkdir(parents=True,exist_ok=False)
mesa=root/'.cache/mesa-work'
subprocess.run(['xcrun','clang','-O1','-g','-fsanitize=address,undefined','-I',str(mesa/'src/util'),
                '-c',str(mesa/'src/util/rb_tree.c'),'-o',str(out/'rb_tree.o')],check=True,timeout=120)
results={}
for name in ['old','new','guards','va-old','va-new','swap']:
    binary=out/name
    command=['xcrun','clang++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
             '-Wno-unused-function','-Wno-unused-parameter','-Wno-missing-field-initializers',
             '-fsanitize=address,undefined','-pthread','-I',str(root/'drivers/Adreno740'),
             '-I',str(mesa/'src/util'),'-I',str(mesa/'include'),
             *(['-DTEST_OLD='+('1' if name=='va-old' else '0')] if name.startswith('va-') else []),
             str(fixtures/(name+'.cpp')),
             *([str(out/'rb_tree.o')] if name=='new' or name.startswith('va-') else []),'-o',str(binary)]
    subprocess.run(command,check=True,timeout=120)
    run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=90,
                       env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
    (out/(name+'.log')).write_text(run.stdout+run.stderr)
    if name=='va-old':
        assert run.returncode!=0 and 'heap-use-after-free' in run.stderr,(name,run.stdout,run.stderr)
        print('PASS: the old destruction fragment reproduced ASan heap-use-after-free')
    else:
        assert run.returncode==0,(name,run.stdout,run.stderr)
    print(run.stdout,end='')
    results[name]=dict(exit_code=run.returncode,source_sha256=hashlib.sha256((fixtures/(name+'.cpp')).read_bytes()).hexdigest())
report=dict(actual_prepared_sources_match=True,ASan_UBSan=True,old_lifetime_error_reproduced=True,
            new_ownership_and_guard_cases_passed=True,host=platform.machine(),bindings=bindings,results=results,
            mock_IOKit_only=True,real_A840_hardware_verified=False,phone_operations=False,
            actual_function_bindings_checked=len(snippets),real_Mesa_RB_used=True,
            old_VA_UAF_reproduced=True,fragmented_VA_and_OOM_passed=True,
            actual_swap_API_and_lifetime_guards_passed=True,
            explicit_port_and_allocation_balance_checked=True,leak_sanitizer='Linux preflight additionally checked;not supported in all Apple ASan runtimes')
(a.logs/'device-context-qualified.json').write_text(json.dumps(report,indent=2)+'\n')
