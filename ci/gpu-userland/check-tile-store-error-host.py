"""Compile the actual selected tile-store error path with host API peers."""
from pathlib import Path
import argparse,hashlib,json,os,re,subprocess
p=argparse.ArgumentParser();p.add_argument('--unixnu',type=Path,required=True);p.add_argument('--logs',type=Path,required=True);a=p.parse_args()
here=Path(__file__).resolve().parent;root=a.unixnu.resolve();a.logs.mkdir(parents=True,exist_ok=True)
source=root/'.cache/mesa-work/src/freedreno/vulkan/tu_cmd_buffer.cc'
text=source.read_text();bindings=json.loads((here/'fixtures/tile-store-bindings.json').read_text());fixture=here/'fixtures/tile-store-error.cpp'
assert hashlib.sha256(source.read_bytes()).hexdigest()==bindings['source_sha256']
def extract(name):
    m=re.search(r'^'+re.escape(name)+r'\([^;{]*\)\s*\n\{',text,re.M);assert m,name
    start=text.rfind('\n',0,m.start()-1)+1;template=text.rfind('template <chip CHIP>',0,start)
    if template>=0 and start-template<80:start=template
    opening=text.index('{',m.start());lex=re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',lambda m:' '*len(m[0]),text,flags=re.S);depth=0
    for i in range(opening,len(text)):
        depth+=(lex[i]=='{')-(lex[i]=='}')
        if not depth:return text[start:i+1]+'\n'
    raise ValueError(name)
for row in bindings['functions']:
    actual=extract(row['function']);assert hashlib.sha256(actual.encode()).hexdigest()==row['sha256'];assert actual in fixture.read_text()
command=['clang++','-std=c++23','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation',
         '-fsanitize=address,undefined',str(fixture),'-o',str(a.logs/'tile-store-error-host')]
subprocess.run(command,check=True)
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
result=subprocess.run([str(a.logs/'tile-store-error-host')],env=env,text=True,capture_output=True,timeout=30)
(a.logs/'tile-store-error-host.log').write_text(result.stdout+result.stderr);assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
report=dict(passed=True,actual_prepared_sources_match=True,source_sha256=bindings['source_sha256'],functions=bindings['functions'],
    command=command,result=result.stdout,ASan_UBSan=True,simulated_GPU_writers_and_Vulkan_owners=True,
    kernel_IO_error_root_cause_fixed=False,phone_operations=False)
(a.logs/'tile-store-error-host.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
