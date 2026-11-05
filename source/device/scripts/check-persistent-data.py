"""Exercise the actual pinned command path and existing USB protocol, on the host only."""
from pathlib import Path
import json,hashlib,subprocess,sys
import argparse
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--ufs',type=Path,required=True);ap.add_argument('--device',type=Path,required=True);ap.add_argument('--usb',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
a=ap.parse_args();U=a.ufs;D=a.device;USB=a.usb;O=a.output;O.mkdir(parents=True,exist_ok=True)
def replace(p,a,b):
 s=p.read_text();assert s.count(a)==1,(str(p),a[:90]);p.write_text(s.replace(a,b))
p=U/'tests/RootCommandsBridge.cpp'
if 'static UfsExtentMap gExtentMap' not in p.read_text():
 replace(p,'#include "../RootCommands.h"','''constexpr IOReturn kIOReturnNotPermitted=0xe00002e2;
#include "../RootExtents.h"
static UfsExtentMap gExtentMap={};
static bool IOLockTryLock(void *) {return true;}
#include "../RootCommands.h"''')
 replace(p,'  gStoreOpen=gLogOpen=false; gStoreFirst=gStoreBlocks=gLogFirst=gLogBlocks=0;',
 '''  gStoreOpen=gLogOpen=false; gStoreFirst=gStoreBlocks=gLogFirst=gLogBlocks=0;
  gExtentMap={};gDataExtentMap={};gProtectedExtentMap={};gDataWritable=false;gHostPanic=false;''')
(U/'tests/PinnedData.cpp').write_text(r'''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual RootCommands and extent translators; only the controller and DMA are modeled.
#include "RootCommandsBridge.cpp"
#include <array>
#include <vector>
#include <algorithm>
static std::array<uint8_t,kDmaBytes> device;
static std::array<uint32_t,64> regs;
static uint64_t now;
static unsigned mmioWrites,doorbells;
static int fault;
struct Sent {uint32_t lba,blocks;uint8_t opcode,fua;};
static std::vector<Sent> sent;
static uint64_t io(unsigned op,uint64_t address,uint64_t value,void *bytes) {
 if(op==0)return regs.at(address/4);
 if(op==1) {
  ++mmioWrites;
  if(address==0x20)regs[address/4]&=~uint32_t(value);else regs.at(address/4)=uint32_t(value);
  if(address==0x58) {
   ++doorbells;
   if(fault==1){regs[0x58/4]=0;return 0;}
   if(fault==2){regs[0x20/4]=0x20000;return 0;}
   uint8_t *cmd=device.data()+kUcdOffset,*rsp=cmd+kRspOffset;
   auto *c=cmd+16;
   sent.push_back({loadBe32(c+2),uint32_t(c[7])*256+c[8],c[0],uint8_t(c[1]&8)});
   assert(cmd[0]==1 && cmd[2]==0);
   if(c[0]==0x2a)assert(c[1]==8 && cmd[1]==0x20);
   else assert(c[0]==0x28 && cmd[1]==0x40);
   memset(rsp,0,512);rsp[0]=fault==3?0x3f:0x21;regs[0x58/4]=0;
   *reinterpret_cast<uint32_t *>(device.data()+8)=0;
  }
 }
 if(op==2 || op==3) {
  assert(address>=gHost.phys && address-gHost.phys+value<=device.size());
  auto at=device.data()+address-gHost.phys;
  if(op==2)memcpy(at,bytes,value);else memcpy(bytes,at,value);
 }
 if(op==4)return now;
 if(op==5)now+=value;
 return 0;
}
static UfsExtent re[8],de[8],pe[8];static uint16_t ri[8],di[8],pi[8];
static void reset() {
 device.fill(0);regs.fill(0);now=0;mmioWrites=doorbells=0;fault=0;sent.clear();
 ufs_test_reset(io,0x8cb5e9000);
 gExtentMap.ext=re;gExtentMap.byPart=ri;
 gDataExtentMap.ext=de;gDataExtentMap.byPart=di;
 gProtectedExtentMap.ext=pe;gProtectedExtentMap.byPart=pi;
 const uint64_t root[]={0,40,8,8,60,8},data[]={0,80,2,2,120,6},protect[]={0,40,8,8,60,8,16,200,16};
 assert(!ufsExtentLoad(gExtentMap,root,6,16));assert(!ufsExtentBind(gExtentMap,1000,100000));
 assert(!ufsExtentLoad(gDataExtentMap,data,6,8));assert(!ufsExtentLoad(gProtectedExtentMap,protect,9,32));
 assert(!ufsBindDataWindow(gDataExtentMap,gProtectedExtentMap,gExtentMap));
}
static IOReturn writePages(uint64_t rel,uint32_t n,const uint8_t *bytes) {
 return ufsDataTransfer(gDataExtentMap,gExtentMap,gProtectedExtentMap,gStoreBlocks,rel,n,bytes,
  [](uint64_t l,uint32_t n,const uint8_t *p){return hostWrite4k(0,l,n,p,true);});
}
int main() {
 std::array<uint8_t,5*4096> data;data.fill(0x5a);
 reset();auto initial=memory[0];assert(hostWrite4k(0,1080,1,data.data(),true)!=0);assert(!doorbells && memory[0]==initial);
 assert(!hostStoreOpen(1080,9));assert(!hostLogOpen(4000,2));assert(!doorbells);
 assert(hostStoreOpen(1080,8));assert(!hostStoreOpen(1080,8));
 assert(writePages(1,4,data.data())==0);assert(sent.size()==2);
 assert(sent[0].lba==1081 && sent[0].blocks==1 && sent[1].lba==1120 && sent[1].blocks==3);
 assert(sent[0].fua==8 && sent[1].fua==8);auto count=doorbells;
 for(uint64_t l:{1040ULL,1060ULL,1200ULL,1079ULL,1082ULL,1126ULL,0ULL,0xffffffffULL,~0ULL})
  assert(hostWrite4k(0,l,1,data.data(),true)!=0);
 assert(hostWrite4k(1,1080,1,data.data(),true)!=0);
 assert(hostWrite4k(0,1080,1,data.data(),false)!=0);
 assert(hostWrite4k(0,1081,2,data.data(),true)!=0);
 assert(writePages(8,1,data.data())!=0 && writePages(7,2,data.data())!=0 && writePages(~0ULL,1,data.data())!=0);
 assert(doorbells==count);assert(hostSyncCache()==0 && doorbells==count);
 gDataWritable=false;assert(writePages(0,1,data.data())!=0 && doorbells==count);
 reset();de[0].part=40;assert(ufsBindDataWindow(gDataExtentMap,gProtectedExtentMap,gExtentMap));
 reset();{const uint64_t overlap[]={0,200,2,2,120,6};assert(!ufsExtentLoad(gDataExtentMap,overlap,6,8));}
 assert(ufsBindDataWindow(gDataExtentMap,gProtectedExtentMap,gExtentMap));
 reset();pe[0].part=20;assert(ufsBindDataWindow(gDataExtentMap,gProtectedExtentMap,gExtentMap));
 reset();de[1].part=100000;assert(ufsBindDataWindow(gDataExtentMap,gProtectedExtentMap,gExtentMap));
 for(int f:{1,2,3}) {
  reset();assert(hostStoreOpen(1080,8));fault=f;assert(writePages(0,1,data.data())!=0 && gHost.poisoned);
  std::array<uint8_t,kDmaBytes> frozen;memcpy(frozen.data(),memory,sizeof(memory));count=mmioWrites;
  assert(writePages(1,1,data.data())!=0);assert(!memcmp(frozen.data(),memory,sizeof(memory)) && mmioWrites==count);
 }
 reset();assert(hostStoreOpen(1080,8));regs[0x58/4]=1;
 std::array<uint8_t,kDmaBytes> frozen;memcpy(frozen.data(),memory,sizeof(memory));
 assert(writePages(0,1,data.data())!=0 && gHost.poisoned && !doorbells && !memcmp(frozen.data(),memory,sizeof(memory)));
 puts("PASS actual pinned UFS: fragmented FUA, six-file exclusion, readonly, boundaries, no tail/log fallback, response/timeout quarantine and no DMA reuse");
}
''')
flags=['g++','-std=c++17','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wall','-Wextra','-Wno-unused-function']
subprocess.run(flags+[str(U/'tests/PinnedData.cpp'),'-o',str(O/'pinned-data')],check=True)
ufs_output=subprocess.check_output([str(O/'pinned-data')],text=True)
subprocess.run(flags+[str(U/'tests/RootCommands.cpp'),'-o',str(O/'root-commands')],check=True)
read_output=subprocess.check_output([str(O/'root-commands')],text=True)
print(ufs_output+read_output)

# Keep the production USB module unchanged. Extend its existing test fixture
# to the new board policy; all previous 33 cases still run.
sys.path.insert(0,str(USB/'tests'))
from source_helpers import function
checker=(USB/'tests/check_reboot_drain27.py').read_text()
ns={};exec(compile(checker,str(USB/'tests/check_reboot_drain27.py'),'exec'),{'__name__':'fixture','__file__':str(USB/'tests/check_reboot_drain27.py')},ns)
cut=ns['cut'];cut.__globals__['function']=function;cut.__globals__['re']=__import__('re')
native=(USB/'kernel/QcomXnuUsbDebug.c').read_text();board=(D/'package/Nezha27UsbControl.c').read_text()
names=[('qcom_usb_debug_control_reset_available','int'),('qcom_usb_debug_control_use_restart_owner','int'),('control_mix','static uint64_t'),('control_rotate_session_owned','static void'),('qcom_usb_debug_control_start','int'),('append_text','static char *'),('append_decimal','static char *'),('control_reply_reboot','static size_t'),('control_handle_frame','static void'),('qcom_usb_debug_control_take_reboot','int'),('control_reboot_valid_owned','static int'),('qcom_usb_debug_control_arm_reboot','int'),('qcom_usb_debug_control_consume_reboot','int'),('qcom_usb_debug_control_reboot_failed','void'),('qcom_usb_debug_wait_drained','int')]
bnames=[('ram_only','static int'),('persistent_selected','static int'),('storage_ready','static int'),('restart_owner','static int'),('nezha27_usb_control_prepare','void'),('reboot_worker','static void'),('nezha27_usb_control_service','void')]
code='\n'.join([cut(native,n,r) for n,r in names]+[cut(board,n,r) for n,r in bnames])
(O/'reboot27-production.inc').write_text(code)
fixture=(USB/'tests/test_reboot_drain27.c').read_text()
fixture=fixture.replace('static uint64_t native_data_persist_writes;', '''static uint64_t native_data_persist_writes,native_data_persist_errors;
static unsigned normal_shutdown_started,persistent_real_shutdown;
static int store_ready=1,store_sync=1,store_gpt=0;
static int native_ufs_store_ready(void){return store_ready;}
static int native_ufs_store_sync(void){return store_sync;}
static unsigned native_ufs_store_gpt_check(void){return store_gpt;}
static int restart_owner(unsigned);''')
fixture=fixture.replace('shutdown_calls++;return -99;', 'shutdown_calls++;if(persistent_real_shutdown){assert(normal_shutdown_started);return restart_owner(1);}return -99;')
needle=' nezha27_usb_control_prepare();assert(qcom_usb_debug_control_start());'
addition='''
 if(!strncmp(test,"persist-",8)){
  persist=2;boot_gate=1;native_data_persist_state=2;native_data_persist_writes=17;persistent_real_shutdown=1;
  if(!strcmp(test,"persist-fresh"))native_data_persist_state=3;
  if(!strcmp(test,"persist-error"))native_data_persist_errors=1;
  if(!strcmp(test,"persist-missing"))store_ready=0;
  if(!strcmp(test,"persist-bad-gpt"))store_gpt=3;
  if(!strcmp(test,"persist-sync-fail"))store_sync=0;
  if(!strcmp(test,"persist-bad-state"))native_data_persist_state=7;
 }
'''+needle+'''
 if(!strncmp(test,"persist-",8)){
  assert(qcom_usb_debug_control_reset_available());request(usb_control_context.state.session);
  assert(control_reboot_requested);
  if(!setjmp(reset_exit))nezha27_usb_control_service();
  int ok=!strcmp(test,"persist-resume")||!strcmp(test,"persist-fresh");
  assert(reset_calls==(unsigned)ok && shutdown_calls==(unsigned)ok);
  if(ok){assert(normal_shutdown_started && control_reboot_armed==2);}
  else{assert(!normal_shutdown_started);assert_resumed();}
  goto done;
 }
'''
assert fixture.count(needle)==1;fixture=fixture.replace(needle,addition)
(O/'test_reboot_persist.c').write_text(fixture)
cmd=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(USB/'include'),'-I'+str(USB),'-I'+str(O),str(O/'test_reboot_persist.c'),str(USB/'kernel/QcomUsbScreenshot.c'),'-o',str(O/'reboot-persist')]
subprocess.run(cmd,check=True)
cases=['success','continuous-log','guard-busy','buffer-busy','drain-fail','guard-timeout','old-session','old-generation','owner-changed','disconnect','cancel','poll-fail','generation-during-drain','owner-during-drain','session-during-drain','cancel-during-drain','create-fail','wrong-cpu','no-control','no-mode','persistent-data','RAM-writes','owner-after-arm','queue-after-arm','log-after-arm','generation-after-arm','consume-once','not-armed','not-drained','failed-without-request','normal-shutdown-return','nonreboot-continuous','generic-drain','persist-resume','persist-fresh','persist-error','persist-missing','persist-bad-gpt','persist-sync-fail','persist-bad-state']
results={case:subprocess.check_output([str(O/'reboot-persist'),case],text=True).strip() for case in cases}
report=dict(phone_operations=False,ufs_actual_commands=ufs_output,root_command_regression=read_output,usb_actual_protocol_and_board_cases=results,sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [U/'RootCommands.h',U/'PinnedDataWindow.h',U/'NativeUfs.cpp',D/'package/Nezha27UsbControl.c',USB/'kernel/QcomXnuUsbDebug.c']})
(O/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS',len(results),'actual USB board/protocol cases; physical PSCI and controller modeled')
