#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the selected original LS signature and exec/dyld scope on host."""
import argparse
import os
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();out=a.out.resolve();out.mkdir(exist_ok=True,parents=True)
patch=(root/'patches/xnu-12377/zzzzzz-nezha-camera-lsd-registration.patch').read_text()
exec_patch,mac_patch=patch.split('--- a/security/mac_base.c',1)
body='\n'.join(line[1:] for line in exec_patch.splitlines()
    if line.startswith('+') and not line.startswith('+++'))
selected=body[body.index('\t/* Camera074'):]
source=r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define CS_CDHASH_LEN 20
#ifndef CAST_USER_ADDR_T
#define CAST_USER_ADDR_T(x) ((uintptr_t)(x))
#endif
#define UIO_SYSSPACE 0
#define TRUE true
struct component {const char *cn_nameptr;size_t cn_namelen;};
struct nameidata {struct component ni_cnd;};
struct image_params {struct nameidata *ip_ndp;void *ip_vp;unsigned ip_arch_offset,ip_argspace,ip_envc;};
static unsigned camera_mode,appended;static int failed;
static char environment[128];
static int exec_add_user_string(struct image_params *p,uintptr_t text,int seg,bool nc)
{(void)p;assert(!seg && nc);if(failed)return EFAULT;++appended;strcpy(environment,(const char *)text);return 0;}
static int append(struct image_params *imgp)
{int error=0;const unsigned new_ptr_size=8;
'''+selected+r'''
return 0;
bad:return error;
}
int main(void)
{
 struct nameidata name={{"lsd",3}};struct image_params image={&name,NULL,0,32,2};
 camera_mode=5;assert(!append(&image) && !appended && image.ip_envc==2);
 camera_mode=6;assert(!append(&image) && appended==1 && image.ip_envc==3 && image.ip_argspace==24);
 assert(!strcmp(environment,"DYLD_INSERT_LIBRARIES=/System/Library/MediaCapture/libNezhaCameraLS27.dylib"));
 name.ni_cnd=(struct component){"lsdX",4};assert(!append(&image) && appended==1);
 image.ip_ndp=NULL;assert(!append(&image) && appended==1);image.ip_ndp=&name;
 name.ni_cnd=(struct component){"lsd",3};
 image.ip_argspace=4;assert(append(&image)==E2BIG && image.ip_envc==3);
 failed=1;image.ip_argspace=32;assert(append(&image)==EFAULT && image.ip_envc==3);
 puts("Camera075 LS exec: mode, name, argument count and failure paths passed");
}
'''
(out/'environment.c').write_text(source)
subprocess.run([os.environ.get('CC','gcc'),'-std=c11','-Wall','-Wextra','-Werror','-g',
    '-fsanitize=address,undefined',str(out/'environment.c'),'-o',str(out/'environment')],check=True)
subprocess.run([str(out/'environment')],check=True)
# The existing owner predicate retains its mode/parent/CS_VALID conditions.
old=(root/'patches/xnu-12377/zzzzzz-nezha-camera-dyld-policy.patch').read_text()
assert 'proc_original_ppid(p) != 1' in old and 'CS_VALID' in old and 'mode != 6' in old
assert 'com.apple.lsd' in mac_patch and 'com.apple.cameracaptured' in mac_patch
assert '0xea, 0x57, 0xfe, 0x6d, 0xbb, 0x8d, 0x64, 0x09' in mac_patch
print('Original dyld policy mode6/launchd-parent/valid-signature gates retained')
