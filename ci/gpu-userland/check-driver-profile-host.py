"""Run the actual driver profiler and frontend packing against host fixtures."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--unixnu',type=Path,required=True)
parser.add_argument('--logs',type=Path,required=True)
parser.add_argument('--compiler',default='clang++')
args=parser.parse_args()
root=args.unixnu.resolve()
here=Path(__file__).resolve().parent
mesa=root/'.cache/mesa-work'
front_path=root/'drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm'
front=front_path.read_text()
a=front.index('struct DriverCpuRec {')
b=front.index("// The runtime's compact stage ring",a)
fragment=front[a:b]
test=(here/'fixtures/driver-profile-test.cpp').read_text()
prefix='''struct TraceRec {uint32_t type,idx;uint64_t t0,t1,t2,t3;uint32_t a,b,c,d,tid,cpu0,cpu1,tid2;};
static std::vector<TraceRec> wire;
static std::mutex wire_lock;
static bool traceWanted(){return true;}
static void tracePush(const TraceRec &r){std::lock_guard<std::mutex> l(wire_lock);wire.push_back(r);wire.back().idx=(uint32_t)wire.size();}
'''
test=test.replace('static std::mutex output_lock;',prefix+fragment+'\nstatic std::mutex output_lock;')
test=test.replace('assert(size == sizeof(TuCpuProfileRecord));',
    'assert(size == sizeof(TuCpuProfileRecord));\n   traceDriverCpuProfile(raw,count,size);')
test=test.replace('int main() {','int main(int argc,char **argv) {')
end=test.rfind('}')
tail='''
   DriverCpuRec sentinel[2]={
      {0x8877665544332211ull,0xfedcba9876543210ull,1122334455667788ull,12000000000ull,5000000000ull,1000000000ull,0,1,177,1,1,1},
      {0x8877665544332211ull,0xfedcba9876543210ull,1122334455667788ull,11000000000ull,0,11000000000ull,1,1,177,1,0,1}};
   traceDriverCpuProfile(sentinel,2,sizeof(sentinel[0]));
   assert(argc==2);FILE *f=fopen(argv[1],"wb");assert(f);
   assert(fwrite(wire.data(),sizeof(TraceRec),wire.size(),f)==wire.size());fclose(f);
'''
test=test[:end]+tail+test[end:]
out=args.logs/'driver-profile'
out.mkdir(parents=True,exist_ok=False)
cpp=out/'test.cpp';cpp.write_text(test)
binary=out/'test'
command=[args.compiler,'-std=c++17','-O2','-g','-pthread','-Wall','-Wextra','-Werror',
    '-fsanitize=address,undefined','-I'+str(mesa/'src/freedreno/vulkan'),str(cpp),'-o',str(binary)]
with (out/'build.log').open('w') as log:
    subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
frames=out/'frames.trace'
with (out/'test.log').open('w') as log:
    subprocess.run([str(binary),str(frames)],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30)
spec=importlib.util.spec_from_file_location('driver_profile_decoder',here/'decode-driver-profile.py')
decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
result=decoder.decode(frames)
assert result['quality']['complete_groups']==6
wide=next(c for c in result['calls'] if c['group']==0x8877665544332211)
assert wide['command']==0xfedcba9876543210
assert (wide['wall_ms'],wide['thread_cpu_ms'],wide['non_running_wall_ms'],wide['unattributed_ms'])==(12000,5000,7000,1000)
broken=out/'incomplete.trace';broken.write_bytes(frames.read_bytes()[:-72])
assert decoder.decode(broken)['quality']['incomplete_or_duplicate_phase_group']==1
paths=[front_path,mesa/'src/freedreno/vulkan/tu_cpu_profile.h',mesa/'src/freedreno/vulkan/tu_cpu_profile.cc']
report=dict(passed=True,actual_profiler_to_actual_frontend=True,actual_decoder=True,
    ASan_UBSan=True,wide_fields_preserved=True,incomplete_groups_rejected=True,
    budget_final_marker_and_limit_checked=True,complete_groups=6,
    bindings={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
    command=command,phone_operations=False,hardware_verified=False)
(args.logs/'driver-profile-host.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
