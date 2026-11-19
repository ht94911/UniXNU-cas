#!/usr/bin/env python3
"""Execute the actual Submit Busy log policy; preserve every status and count across callers."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[1];a.out.mkdir(parents=True,exist_ok=False)
source=(r/'NativeAdrenoPublish.cpp').read_text();start=source.index('static bool reportRpcResult(');end=source.index('extern "C" int proc_selfpid',start);body=source[start:end]
prefix=r"""
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
#include "NativeAdrenoABI.h"
#include "NativeAdrenoPollLog.h"
using IOReturn=int;
constexpr IOReturn kIOReturnBusy=7,kIOReturnTimeout=8,kIOReturnError=9;
static NativeAdrenoPollLog gpuPollLog;
"""
cases=r"""
int main(){
 std::atomic<unsigned> printed{0};std::vector<std::thread> threads;
 for(unsigned t=0;t<8;++t)threads.emplace_back([&,t]{for(unsigned i=0;i<32768;++i)if(reportRpcResult(t&1?NativeAdrenoMethodSubmit:NativeAdrenoMethodSubmitBatch,kIOReturnBusy))++printed;});
 for(auto &t:threads)t.join();
 assert(gpuPollLog.count(2)==262144 && printed==20 && !gpuPollLog.count(0) && !gpuPollLog.count(1));
 for(uint32_t method:{uint32_t(NativeAdrenoMethodBoImport),uint32_t(NativeAdrenoMethodBoFree),uint32_t(0x7002)})assert(reportRpcResult(method,kIOReturnBusy));
 assert(gpuPollLog.count(2)==262144);
 for(uint32_t method:{uint32_t(NativeAdrenoMethodSubmit),uint32_t(NativeAdrenoMethodSubmitBatch)}){assert(reportRpcResult(method,kIOReturnError));assert(reportRpcResult(method,kIOReturnTimeout));}
 for(unsigned i=0;i<10000;++i){(void)reportRpcResult(NativeAdrenoMethodWaitFence,kIOReturnTimeout);(void)reportRpcResult(0x7002,kIOReturnTimeout);}
 assert(gpuPollLog.count(0)==10000 && gpuPollLog.count(1)==10000);
 puts("PASS actual RPC policy: 262144 concurrent Submit/SubmitBatch Busy returns counted with 20 repeated lines; other errors/refusals and original fence/frame timeout counts retained");
}
"""
cpp=a.out/'actual.cpp';binary=a.out/'actual';cpp.write_text(prefix+body+cases)
cmd=shlex.split(os.environ.get('CXX','clang++'))+['-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(r),str(cpp),'-o',str(binary)]
with (a.out/'build.log').open('x')as f:subprocess.run(cmd,check=True,stdout=f,stderr=subprocess.STDOUT,timeout=120)
result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30);(a.out/'run.log').write_text(result.stdout+result.stderr);assert result.returncode==0,result.stderr
report=dict(passed=True,actual_publisher_policy=True,source_sha256=hashlib.sha256((r/'NativeAdrenoPublish.cpp').read_bytes()).hexdigest(),header_sha256=hashlib.sha256((r/'NativeAdrenoPollLog.h').read_bytes()).hexdigest(),policy_sha256=hashlib.sha256(body.encode()).hexdigest(),concurrent_calls=262144,printed_lines=20,original_error_returns_unchanged=True,existing_counter_sampling=True,ASan=True,UBSan=True,command=cmd,phone_operations=False,physical_frame_improvement_verified=False)
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(result.stdout,end='')
