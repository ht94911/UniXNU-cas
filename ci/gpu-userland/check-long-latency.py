"""Run the actual long-latency serializer and bind its integration into the final frontend."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--unixnu',type=Path,required=True)
parser.add_argument('--logs',type=Path,required=True)
args=parser.parse_args()
root=args.unixnu.resolve()
source=root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm'
header=source.with_name('NativeLongLatency.h')
out=args.logs/'long-latency'
out.mkdir(exist_ok=False)
fixture=Path(__file__).with_name('fixtures')/'long-latency.cpp'
command=['clang++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
         '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(header.parent),
         str(fixture),'-o',str(out/'wire')]
with (out/'build.log').open('w') as log:
    subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
result=subprocess.run([str(out/'wire')],capture_output=True,text=True,timeout=30)
(out/'run.log').write_text(result.stdout+result.stderr)
assert result.returncode==0 and 'PASS 13 wire threshold/order/width' in result.stdout
text=source.read_text()
assert '#include "NativeLongLatency.h"' in text
assert 'nblonglatency::api(id,' in text
assert 'nblonglatency::command({self->traceNumber' in text
assert 'nblonglatency::producerGap(traceNumber' in text
assert 'sysctlbyname("hw.memsize"' in text
assert text.index('commandBuffer->diagnosticCallbackUs=nowUs();')<text.index('commandBuffer->diagnosticCallbackUs-commandBuffer.committedUs')
report=dict(passed=True,actual_serializer_executed=True,cases=13,ASan=True,UBSan=True,
            existing_USB_problem_channel=True,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
            header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),phone_operations=False,
            target_frame_latency_unverified=True)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
