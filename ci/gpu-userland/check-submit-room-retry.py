#!/usr/bin/env python3
"""Exercise the actual Darwin Submit/SubmitBatch Busy retry over controlled IOKit peers."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path)
p.add_argument('--unixnu',type=Path)
p.add_argument('--native',type=Path)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--control',type=Path)
a=p.parse_args()
root=a.unixnu.resolve() if a.unixnu else None
source=a.source or root/'.cache/mesa-work/src/freedreno/vulkan/tu_knl_darwin.cc'
native=a.native or root/'drivers/Adreno740'
a.out.mkdir(parents=True,exist_ok=False)
code=source.read_text()
header=r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <pthread.h>
#include <vector>
#include "NativeAdrenoABI.h"
using VkResult=int; using kern_return_t=int;
constexpr int VK_SUCCESS=0,VK_ERROR_DEVICE_LOST=-4;
constexpr int KERN_SUCCESS=0,kIOReturnBusy=7,kIOReturnError=8;
#define MIN2(a,b) ((a)<(b)?(a):(b))
struct dw_connection {pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;uint32_t inflight[8]{};unsigned inflight_count=0;NativeAdrenoAbiParams params{};};
struct Peer;
struct tu_device {dw_connection dw{};int vk=0;Peer *peer=nullptr;};
struct tu_queue {tu_device *device=nullptr;};
struct tu_darwin_submit_rec {uint64_t ns=0;uint32_t seq=0,fence=0,sync_wait_us=0,call_us=0,space_wait_us=0;uint16_t busy=0;uint8_t chunks=0,buffers=0;};
static dw_connection &dw_device(tu_device *d){return d->dw;}
static uint64_t ticks=1;
static uint64_t tu_trace_clock_ns(){return ticks+=1000;}
struct Peer {
 tu_device device{};tu_queue queue{&device};
 unsigned submitCalls=0,begins=0,appends=0,commits=0,sleeps=0,yields=0,waits=0,published=0;
 unsigned roomAfterSleeps=3,busyReplies=0,failOnCall=0;
 bool ready=false,waitFailure=false,wrongBatch=false,dropDuringWait=false;
 uint32_t nextFence=90,expected=0;
 std::vector<uint32_t> waited;std::vector<NativeAdrenoAbiSubmitBuffer> uploaded;
 std::function<void()> onWait;
 Peer(){device.peer=this;device.dw.params.flags=NativeAdrenoParamsSubmitBatch;}
};
static Peer *active;
static void lockAvailable(dw_connection &dw){assert(pthread_mutex_trylock(&dw.lock)==0);pthread_mutex_unlock(&dw.lock);}
[[maybe_unused]] static int fixture_sleep(unsigned us){assert(us==200);assert(active);lockAvailable(active->device.dw);++active->sleeps;ticks+=uint64_t(us)*1000;active->ready=active->sleeps>=active->roomAfterSleeps;return 0;}
[[maybe_unused]] static int fixture_yield(){assert(active);lockAvailable(active->device.dw);++active->yields;return 0;}
#define usleep fixture_sleep
#define sched_yield fixture_yield
static VkResult vk_device_set_lost(int*,const char*,...){return VK_ERROR_DEVICE_LOST;}
static VkResult dw_wait_fence_locked_free(tu_device *d,uint32_t fence,uint64_t timeout){
 Peer &p=*d->peer;lockAvailable(d->dw);assert(timeout==UINT64_MAX);++p.waits;p.waited.push_back(fence);
 if(p.onWait)p.onWait();
 return p.waitFailure?VK_ERROR_DEVICE_LOST:VK_SUCCESS;
}
static kern_return_t dw_call(dw_connection &dw,uint32_t method,const void *input,size_t input_bytes,void *output,size_t output_bytes){
 assert(active && &dw==&active->device.dw);lockAvailable(dw);Peer &p=*active;
 if(method==NativeAdrenoMethodSubmit){
  assert(input_bytes==sizeof(NativeAdrenoAbiSubmit) && output_bytes==sizeof(NativeAdrenoAbiSubmitReply));
  const auto &r=*static_cast<const NativeAdrenoAbiSubmit*>(input);assert(r.buffer_count==p.expected && r.queue==0);
  ++p.submitCalls;
  if(p.failOnCall && p.submitCalls>=p.failOnCall)return kIOReturnError;
  if(!p.ready){++p.busyReplies;return kIOReturnBusy;}
  static_cast<NativeAdrenoAbiSubmitReply*>(output)->fence=p.nextFence;++p.published;return KERN_SUCCESS;
 }
 assert(method==NativeAdrenoMethodSubmitBatch && input_bytes==sizeof(NativeAdrenoAbiSubmitBatch) && output_bytes==sizeof(NativeAdrenoAbiSubmitBatchReply));
 const auto &r=*static_cast<const NativeAdrenoAbiSubmitBatch*>(input);auto &reply=*static_cast<NativeAdrenoAbiSubmitBatchReply*>(output);
 if(r.action==NativeAdrenoBatchBegin){assert(!r.batch && r.total_buffers==p.expected);++p.begins;reply.batch=77;return KERN_SUCCESS;}
 assert(r.batch==77);reply.batch=p.wrongBatch?78:77;
 if(r.action==NativeAdrenoBatchAppend){assert(r.offset==p.uploaded.size());++p.appends;p.uploaded.insert(p.uploaded.end(),r.buffers,r.buffers+r.buffer_count);return KERN_SUCCESS;}
 assert(r.action==NativeAdrenoBatchCommit && !r.buffer_count && r.offset==p.expected && p.uploaded.size()==p.expected);++p.commits;
 if(p.failOnCall && p.commits>=p.failOnCall)return kIOReturnError;
 if(!p.ready){++p.busyReplies;return kIOReturnBusy;}
 reply.fence=p.nextFence;++p.published;return KERN_SUCCESS;
}
"""
footer=r"""
static std::vector<NativeAdrenoAbiSubmitBuffer> ibs(unsigned n){std::vector<NativeAdrenoAbiSubmitBuffer> b(n);for(unsigned i=0;i<n;++i)b[i]={uint64_t(i+1),uint64_t(0x1000+i*16),16};return b;}
static VkResult execute(Peer &p,unsigned count,tu_darwin_submit_rec &rec,uint32_t &fence){active=&p;p.expected=count;auto b=ibs(count);return darwin_submit_whole(&p.queue,b.data(),count,&fence,&rec);}
int main(){
#ifdef CONTROL
 Peer p;p.device.dw.inflight_count=1;p.device.dw.inflight[0]=12;p.failOnCall=21;
 tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,64,rec,fence)==VK_ERROR_DEVICE_LOST);
 assert(p.waits==20 && p.sleeps==0 && p.published==0 && p.device.dw.inflight_count==1);
 puts("CONTROL reproduced: completed oldest local fence polled 20 times without any pause while another VM owns the ring");return 0;
#else
 unsigned cases=0;
 for(unsigned count:{64u,140u,1024u}){
  Peer p;p.device.dw.inflight_count=2;p.device.dw.inflight[0]=12;p.device.dw.inflight[1]=13;
  tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,count,rec,fence)==VK_SUCCESS);
  assert(fence==90 && p.published==1 && p.waits==2 && p.waited==std::vector<uint32_t>({12,13}) && p.sleeps==3 && p.yields==0 && p.device.dw.inflight_count==0);
  assert(rec.busy==5);
  if(count>128)assert(p.begins==1 && p.appends==(count+127)/128 && p.commits==6 && p.uploaded.size()==count);
  else assert(p.submitCalls==6 && p.begins==0);
  ++cases;
 }
 for(unsigned count:{64u,140u}){
  Peer p;tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,count,rec,fence)==VK_SUCCESS && p.waits==0 && p.sleeps==3 && p.published==1);++cases;
 }
 {
  Peer p;p.device.dw.inflight_count=1;p.device.dw.inflight[0]=12;p.waitFailure=true;
  tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,140,rec,fence)==VK_ERROR_DEVICE_LOST && !p.published && p.device.dw.inflight_count==1 && !p.sleeps);++cases;
 }
 {
  Peer p;p.device.dw.inflight_count=1;p.device.dw.inflight[0]=12;
  p.onWait=[&]{pthread_mutex_lock(&p.device.dw.lock);p.device.dw.inflight[0]=13;p.device.dw.inflight_count=1;pthread_mutex_unlock(&p.device.dw.lock);p.onWait=nullptr;};
  tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,64,rec,fence)==VK_SUCCESS && p.waited==std::vector<uint32_t>({12,13}) && p.published==1);++cases;
 }
 {
  Peer p;p.device.dw.inflight_count=1;p.device.dw.inflight[0]=12;
  p.onWait=[&]{pthread_mutex_lock(&p.device.dw.lock);p.device.dw.inflight[1]=13;p.device.dw.inflight_count=2;pthread_mutex_unlock(&p.device.dw.lock);p.onWait=nullptr;};
  tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,140,rec,fence)==VK_SUCCESS && p.waited==std::vector<uint32_t>({12,13}) && p.published==1);++cases;
 }
 {
  Peer p;p.failOnCall=1;tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,64,rec,fence)==VK_ERROR_DEVICE_LOST && !p.waits && !p.sleeps && !p.published);++cases;
 }
 {
  Peer p;p.ready=true;p.wrongBatch=true;tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,140,rec,fence)==VK_ERROR_DEVICE_LOST && !p.commits && !p.published);++cases;
 }
 {
  Peer p;tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,0,rec,fence)==VK_ERROR_DEVICE_LOST && !p.submitCalls && !p.begins);++cases;
 }
 {
  Peer p;p.device.dw.params.flags=0;tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,140,rec,fence)==VK_ERROR_DEVICE_LOST && !p.begins);++cases;
 }
 {
  Peer p;p.device.dw.inflight_count=2;p.device.dw.inflight[0]=UINT32_MAX;p.device.dw.inflight[1]=1;tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,64,rec,fence)==VK_SUCCESS && p.waited==std::vector<uint32_t>({UINT32_MAX,1}));++cases;
 }
 {
  Peer p,q;p.device.dw.inflight_count=q.device.dw.inflight_count=1;p.device.dw.inflight[0]=12;q.device.dw.inflight[0]=44;tu_darwin_submit_rec rec{};uint32_t fence=0;assert(execute(p,64,rec,fence)==VK_SUCCESS && q.device.dw.inflight_count==1 && q.device.dw.inflight[0]==44);++cases;
 }
 printf("PASS %u actual Submit/SubmitBatch retry cases: no local completed-fence spin; pauses outside locks; one atomic 64/140/1024-IB publication; wait errors, changed history, cross-device and wrapped fence identity retained\n",cases);
#endif
}
"""
def functions(text):
 start=text.index('static VkResult\ndarwin_wait_submit_room(') if 'static VkResult\ndarwin_wait_submit_room(' in text else text.index('/* One kernel submission of up to NativeAdrenoAbiMaxBuffers indirect buffers;')
 end=text.index('static VkResult\ndarwin_queue_submit(',start)
 return text[start:end]
results=[]
for name,path,control in [('actual',source,False)]+([('control',a.control,True)] if a.control else []):
 original=path.read_text();fragment=functions(original);cpp=a.out/(name+'.cpp');binary=a.out/name
 cpp.write_text(header+fragment+footer)
 cmd=shlex.split(os.environ.get('CXX','clang++'))+['-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(native),str(cpp),'-o',str(binary)]
 if control:cmd.insert(-2,'-DCONTROL=1')
 with (a.out/(name+'-build.log')).open('x') as log:subprocess.run(cmd,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=120)
 with (a.out/(name+'-run.log')).open('x') as log:subprocess.run([str(binary)],check=True,stdout=log,stderr=subprocess.STDOUT,timeout=15)
 results.append(dict(name=name,source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),functions_sha256=hashlib.sha256(fragment.encode()).hexdigest(),command=cmd,output=(a.out/(name+'-run.log')).read_text()))
report=dict(passed=True,actual_Submit_and_SubmitBatch_functions=True,controlled_other_VM_and_IOKit=True,ASan=True,UBSan=True,old_completed_fence_spin_reproduced=bool(a.control),exact_kernel_sync_not_removed=True,physical_frame_improvement_verified=False,phone_operations=False,results=results)
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
