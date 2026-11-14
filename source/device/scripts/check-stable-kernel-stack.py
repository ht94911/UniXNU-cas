#!/usr/bin/env python3
"""Exercise the actual sheng-derived stack sampler, without a phone or XNU.

The guard is from UniXNU-sheng c498750f98cea8893546f84ef0dfee6e3ed38a0e.
This host test covers state and memory bounds, not arm64e pointer authentication.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'package/BootUiTraceProbe.c').read_text()
match = re.search(r'^static void bt_kernel_stack\([^\n]+\) \{\n.*?^\}', source, re.M | re.S)
assert match
function = match[0]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TH_WAIT 1
#define TH_RUN 4
#define TH_TERMINATE 16
#define TH_TERMINATE2 32
#define BT_KFRAMES 12
struct saved {struct {struct {uint64_t fp,lr;} ss;} machine;};
typedef struct saved *thread_kernel_state_t;
struct thread {uintptr_t kernel_stack;unsigned state;thread_kernel_state_t saved;int sched_locked;};
typedef struct thread *thread_t;
static uintptr_t kernel_stack_size;
static unsigned reads;
static thread_kernel_state_t thread_get_kernel_state(thread_t t) {
 assert(t->sched_locked);
 assert((t->state&(TH_WAIT|TH_RUN|TH_TERMINATE|TH_TERMINATE2))==TH_WAIT);
 ++reads;return t->saved;
}
'''
tests = r'''
static struct {uint64_t frames[64];struct saved state;} stack __attribute__((aligned(16)));
static void empty(const uint64_t *p) {for(unsigned i=0;i<BT_KFRAMES;i++)assert(p[i]==0);}
int main(void) {
 kernel_stack_size=sizeof(stack);
 struct thread t={.kernel_stack=(uintptr_t)&stack,.state=TH_WAIT,.saved=&stack.state,.sched_locked=1};
 stack.state.machine.ss.lr=0x1234;
 stack.state.machine.ss.fp=(uintptr_t)&stack.frames[8];
 stack.frames[8]=(uintptr_t)&stack.frames[16];stack.frames[9]=0x5678;
 stack.frames[16]=0;stack.frames[17]=0x9abc;
 uint64_t out[BT_KFRAMES];
 bt_kernel_stack(&t,out);assert(out[0]==0x1234 && out[1]==0x5678 && out[2]==0x9abc && !out[3]);
 for(unsigned state=0;state<256;state++) {
  t.state=state;
  if((state&(TH_WAIT|TH_RUN|TH_TERMINATE|TH_TERMINATE2))==TH_WAIT)continue;
  t.saved=(void *)(uintptr_t)1;unsigned before=reads;
  memset(out,0xff,sizeof(out));bt_kernel_stack(&t,out);empty(out);assert(reads==before);
 }
 t.state=TH_WAIT;
 uintptr_t bad[]={0,(uintptr_t)&stack-16,(uintptr_t)&stack+sizeof(stack)-8,(uintptr_t)&stack+sizeof(stack),UINTPTR_MAX-7};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
  t.saved=(void *)bad[i];bt_kernel_stack(&t,out);empty(out);
 }
 t.saved=&stack.state;t.kernel_stack=0;bt_kernel_stack(&t,out);empty(out);t.kernel_stack=(uintptr_t)&stack;
 for(unsigned size=0;size<sizeof(struct saved);size++) {kernel_stack_size=size;bt_kernel_stack(&t,out);empty(out);}
 kernel_stack_size=sizeof(stack);
 uintptr_t frames[]={0,(uintptr_t)&stack-16,(uintptr_t)&stack+4,(uintptr_t)&stack+sizeof(stack),UINTPTR_MAX-15};
 for(unsigned i=0;i<sizeof(frames)/sizeof(frames[0]);i++) {
  stack.state.machine.ss.fp=frames[i];bt_kernel_stack(&t,out);assert(out[0]==0x1234);
  for(unsigned j=1;j<BT_KFRAMES;j++)assert(out[j]==0);
 }
 stack.state.machine.ss.fp=(uintptr_t)&stack.frames[8];stack.frames[8]=(uintptr_t)&stack.frames[8];
 bt_kernel_stack(&t,out);assert(out[1]==0x5678 && !out[2]);
 puts("PASS: real sampler,256 thread states,saved-state bounds,short/null stack,FP wrap/alignment/self-loop and stable three-frame trace");
}
'''
resource = root.parent/'private/host-runtime/extracted/usr/lib/llvm-18/lib/clang/18'
resource_flags = []
if resource.is_dir():
    builtin = subprocess.check_output(['clang', '-print-resource-dir'], text=True).strip()
    resource_flags = ['-resource-dir', str(resource), '-isystem', builtin+'/include']
with tempfile.TemporaryDirectory(prefix='nezha-stable-stack-') as temp:
    work = Path(temp)
    (work/'test.c').write_text(prefix + function + '\n' + tests)
    command = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', *resource_flags,
               str(work/'test.c'), '-o', str(work/'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(work/'test')], check=True)
print(json.dumps({'source_sha256':hashlib.sha256(source.encode()).hexdigest(),
                  'function_sha256':hashlib.sha256(function.encode()).hexdigest(),
                  'source_extracted':True,'ASan_UBSan':True,'states':256,
                  'arm64e_PAC_not_covered_by_host':True,'phone_operation':False}, indent=2))
