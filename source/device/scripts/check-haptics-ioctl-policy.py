#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the actual fileglob exception predicate with host policy/vnode fixtures."""
import argparse,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
source=Path(__file__).resolve().parents[1]/'patches/xnu-12377/nezha-haptics-device-access.patch'
text=source.read_text();segment=text[text.index('--- a/bsd/kern/sys_generic.c'):]
added='\n'.join(line[1:] for line in segment.splitlines() if line.startswith('+') and not line.startswith('+++'))
match=re.search(r'if \(error == EPERM.*?error = 0;',added,re.S);assert match
body=match.group()
program=a.out/'policy.c'
program.write_text(r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "NezhaHapticsProtocol.h"
#define DTYPE_VNODE 1
struct vnode {int ours;};typedef struct vnode *vnode_t;
struct context {int allowed;};typedef struct context *vfs_context_t;
struct fileproc {int f_type;vnode_t data;};
static int queries;
static void *fp_get_data(struct fileproc *fp) {return fp->data;}
static int actual_fixture(vfs_context_t ctx,vnode_t vp,unsigned long command) {
 ++queries;return ctx->allowed && vp && vp->ours && (command==NZ_HAPTICS_GET_STATUS || command==NZ_HAPTICS_FIX_HOST_CLOCK);
}
static int (*nezha_haptics_ioctl)(vfs_context_t,vnode_t,unsigned long)=actual_fixture;
static int gate(int error,struct fileproc *fp,unsigned long com,struct context context) {
''' + body + r'''
 return error;
}
int main(void) {
 unsigned cases=0;
 for(unsigned allowed=0;allowed<2;++allowed)for(unsigned ours=0;ours<2;++ours)
 for(unsigned type=0;type<2;++type)for(unsigned exact=0;exact<3;++exact)
 for(unsigned policy=0;policy<3;++policy) {
  struct vnode vp={ours};struct fileproc fp={type?DTYPE_VNODE:2,&vp};struct context context={allowed};
  unsigned long command=exact==2?NZ_HAPTICS_FIX_HOST_CLOCK:exact==1?NZ_HAPTICS_GET_STATUS:NZ_HAPTICS_GET_STATUS^1u;
  int original=policy==0?0:policy==1?EPERM:EACCES;
  int expected=original==EPERM && type && allowed && ours && exact?0:original;
  assert(gate(original,&fp,command,context)==expected);++cases;
 }
 struct vnode vp={1};struct fileproc fp={DTYPE_VNODE,&vp};struct context context={1};
 nezha_haptics_ioctl=NULL;assert(gate(EPERM,&fp,NZ_HAPTICS_GET_STATUS,context)==EPERM);++cases;
 printf("PASS actual haptics fileglob ioctl predicate: %u cases, exact status/node/process, absent hook and original policy fallthrough\n",cases);
}
''')
device=Path(__file__).resolve().parents[1];cc=os.environ.get('CC','clang');binary=a.out/'policy'
cmd=[cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I',str(device/'package/haptics27'),str(program),'-o',str(binary)]
subprocess.run(cmd,check=True);result=subprocess.run([str(binary)],capture_output=True,text=True,check=True);(a.out/'execution.log').write_text(result.stdout+result.stderr);print(result.stdout,end='')
