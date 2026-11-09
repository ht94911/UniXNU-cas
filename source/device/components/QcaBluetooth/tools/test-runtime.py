#!/usr/bin/env python3
"""Run the original btqca startup and native H4/IBS executor against a controller model."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,sys
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--wcn7860',action='store_true',help='use the separately pinned WCN7860 Linux implementation')
args=parser.parse_args()
linux='out/linux-wcn7860' if args.wcn7860 else 'out/linux'
subprocess.run([sys.executable,root/('tools/prepare-linux-wcn7860.py' if args.wcn7860 else 'tools/prepare-linux.py')],check=True)
out=root/('out/runtime-wcn7860-test' if args.wcn7860 else 'out/runtime-test');out.mkdir(parents=True,exist_ok=True)
sources=['src/qbt_core.c','src/qbt_linux.c',linux+'/btqca.c','tests/runtime.c']
inputs=sources+['include/qbt.h','src/qbt_internal.h','tools/test-runtime.py']
command=[os.environ.get('CC','clang'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-sign-compare','-fsanitize=address,undefined',
    *(['-DQBT_WCN7860_SOURCE=1'] if args.wcn7860 else []),
    *['-I'+str(root/d) for d in ['include','src','shim',linux]],*[str(root/f) for f in sources],'-o',str(out/'runtime')]
subprocess.run(command,check=True)
result=subprocess.run([out/'runtime'],text=True,capture_output=True)
(out/'output.txt').write_text(result.stdout+result.stderr)
if result.returncode:raise SystemExit((result.stdout+result.stderr)[-5000:])
print(result.stdout,end='')
(out/'report.json').write_text(json.dumps(dict(passed=True,radio_simulated=True,hardware_verified=False,wcn7860_source=args.wcn7860,
    runtime_hci_metadata=True,metadata_capacity=24,link_security_metadata=True,
    link_metadata_capacity=32,streamed_SMP_metadata=True,payload_capture=False,command=command,
    inputs={s:hashlib.sha256((root/s).read_bytes()).hexdigest() for s in inputs},output=result.stdout),indent=2)+'\n')
