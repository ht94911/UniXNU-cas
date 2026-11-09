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
   DriverCpuRec maximum[33]{};
   maximum[0]={0x44332211ull,77,1000000000ull,32000000ull,0,0,0,1,178,32,1,1};
   for(uint32_t i=1;i<33;i++)
      maximum[i]={0x44332211ull,77,1000000000ull,1000000ull,0,1000000ull,i,1,178,1,0,1};
   traceDriverCpuProfile(maximum,33,sizeof(maximum[0]));
   const size_t before_rejected=wire.size();
   traceDriverCpuProfile(maximum,34,sizeof(maximum[0]));
   assert(wire.size()==before_rejected);

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
assert result['quality']['complete_groups']==7
maximum=next(c for c in result['calls'] if c['group']==0x44332211)
assert len(maximum['phases'])==32
assert {p['name'] for p in maximum['phases']} >= {'IOKit CPU map','IOKit CPU unmap'}
wide=next(c for c in result['calls'] if c['group']==0x8877665544332211)
assert wide['command']==0xfedcba9876543210
assert (wide['wall_ms'],wide['thread_cpu_ms'],wide['non_running_wall_ms'],wide['unattributed_ms'])==(12000,5000,7000,1000)
broken=out/'incomplete.trace';broken.write_bytes(frames.read_bytes()[:-72])
assert decoder.decode(broken)['quality']['incomplete_or_duplicate_phase_group']==1
paths=[front_path,mesa/'src/freedreno/vulkan/tu_cpu_profile.h',mesa/'src/freedreno/vulkan/tu_cpu_profile.cc']
report=dict(passed=True,actual_profiler_to_actual_frontend=True,actual_decoder=True,
    ASan_UBSan=True,wide_fields_preserved=True,incomplete_groups_rejected=True,
    budget_final_marker_and_limit_checked=True,complete_groups=7,
    bindings={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
    command=command,phone_operations=False,hardware_verified=False)
(args.logs/'driver-profile-host.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))

# This is the real KMD wait function within a deterministic transport/clock
# fixture. Verify its source binding before compiling it on the Apple host.
kmd = (mesa/'src/freedreno/vulkan/tu_knl_darwin.cc').read_text()
start = kmd.index('static VkResult\ndw_wait_fence_raw(')
end = kmd.index('\nstatic VkResult darwin_collect_retired',start)
function = kmd[start:end]
fixture = here/'fixtures/fence-wait-test.cpp'
assert function in fixture.read_text()
wait_binary = out/'fence-wait-test'
wait_command = [args.compiler,'-std=c++20','-O2','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined',str(fixture),'-o',str(wait_binary)]
with (out/'wait-build.log').open('w') as log:
    subprocess.run(wait_command,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=120)
with (out/'wait-test.log').open('w') as log:
    subprocess.run([str(wait_binary)],check=True,stdout=log,stderr=subprocess.STDOUT,timeout=15)
report.update(actual_wait_function_sha256=hashlib.sha256(function.encode()).hexdigest(),
              bounded_wait_deadline_cases_passed=True,single_call_request_max_ns=1000000,
              CPU_map_unmap_phases_checked=True,maximum_phase_group_records=33)
(args.logs/'driver-profile-host.json').write_text(json.dumps(report,indent=2)+'\n')
