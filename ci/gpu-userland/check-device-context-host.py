#!/usr/bin/env python3
"""Bind the tested fake-IOKit harness to the actual prepared sources, then run it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu',type=Path,required=True)
p.add_argument('--logs',type=Path,required=True)
a=p.parse_args();root=a.unixnu.resolve();fixtures=Path(__file__).with_name('fixtures')
bindings=json.loads((fixtures/'source-bindings.json').read_text())
for name,digest in bindings.items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest, ('source binding',name)
out=a.logs/'device-context-host';out.mkdir(parents=True,exist_ok=False)
results={}
for name in ['old','new','guards']:
    binary=out/name
    command=['xcrun','clang++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
             '-Wno-unused-function','-Wno-unused-parameter','-Wno-missing-field-initializers',
             '-fsanitize=address,undefined','-pthread','-I',str(root/'drivers/Adreno740'),
             str(fixtures/(name+'.cpp')),'-o',str(binary)]
    subprocess.run(command,check=True,timeout=120)
    run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=90,
                       env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
    (out/(name+'.log')).write_text(run.stdout+run.stderr)
    assert run.returncode==0,(name,run.stdout,run.stderr)
    print(run.stdout,end='')
    results[name]=dict(exit_code=run.returncode,source_sha256=hashlib.sha256((fixtures/(name+'.cpp')).read_bytes()).hexdigest())
report=dict(actual_prepared_sources_match=True,ASan_UBSan=True,old_lifetime_error_reproduced=True,
            new_ownership_and_guard_cases_passed=True,host=platform.machine(),bindings=bindings,results=results,
            mock_IOKit_only=True,real_A840_hardware_verified=False,phone_operations=False,
            explicit_port_and_allocation_balance_checked=True,leak_sanitizer='Linux preflight additionally checked;not supported in all Apple ASan runtimes')
(a.logs/'device-context-qualified.json').write_text(json.dumps(report,indent=2)+'\n')
