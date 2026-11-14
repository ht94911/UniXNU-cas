#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the actual vendor installer and scoped exec environment addition."""
import argparse
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True);out=a.out.resolve()
patch=(root/'patches/xnu-12377/zzzzzz-nezha-camera-service-environment.patch').read_text()
body='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
# Extract the complete added branch; outside context increments are the
# existing environment reader, not part of the new condition.
body=body[body.index('\t/* HakiOS'):]
body+='\n\timgp->ip_envc++;\n\t}\n'
source=r'''#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define CAST_USER_ADDR_T(x) ((uintptr_t)(x))
#define UIO_SYSSPACE 0
#define TRUE true
struct component {const char *cn_nameptr;size_t cn_namelen;};
struct nameidata {struct component ni_cnd;};
struct image_params {struct nameidata *ip_ndp;unsigned ip_argspace,ip_envc;};
static unsigned mode,appended;static int copy_failed;static char value[128];
static bool PE_parse_boot_argn(const char *name,void *out,size_t bytes)
{assert(!strcmp(name,"nezha-camera") && bytes==sizeof(mode));*(unsigned *)out=mode;return true;}
static int exec_add_user_string(struct image_params *p,uintptr_t text,int seg,bool nc)
{(void)p;assert(!seg && nc);if(copy_failed)return EFAULT;++appended;strcpy(value,(const char *)text);return 0;}
static int apply(struct image_params *imgp)
{int error=0;const unsigned new_ptr_size=8;
'''+body+r'''
return 0;
bad:return error;
}
int main(void)
{
 struct nameidata name={.ni_cnd={"cameracaptured",14}};
 struct image_params image={&name,32,2};
 mode=5;assert(!apply(&image) && !appended && image.ip_argspace==32 && image.ip_envc==2);
 mode=6;assert(!apply(&image) && appended==1 && image.ip_argspace==24 && image.ip_envc==3);
 assert(!strcmp(value,"DYLD_INSERT_LIBRARIES=/private/var/ngpu/addons/libNezhaCamera27.dylib"));
 name.ni_cnd.cn_nameptr="mediaserverd";name.ni_cnd.cn_namelen=12;
 assert(!apply(&image) && appended==1);
 name.ni_cnd.cn_nameptr="cameracapturedX";name.ni_cnd.cn_namelen=15;
 assert(!apply(&image) && appended==1);
 image.ip_ndp=NULL;assert(!apply(&image) && appended==1);
 image.ip_ndp=&name;name.ni_cnd=(struct component){"cameracaptured",14};
 image.ip_argspace=4;assert(apply(&image)==E2BIG && image.ip_envc==3);
 image.ip_argspace=32;copy_failed=1;assert(apply(&image)==EFAULT && image.ip_envc==3);
 puts("Camera exec environment: exact daemon/mode6 only, existing env count, copy failure and argument-space checks passed");
}
'''
(out/'environment.c').write_text(source)
subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',str(out/'environment.c'),'-o',str(out/'test-environment')],check=True)
subprocess.run([str(out/'test-environment')],check=True)
subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-fblocks','-g','-fsanitize=address,undefined','-I'+str(root/'tests/camera-platform/vendor-fixture'),'-I'+str(root/'tests/camera-platform'),'-I'+str(root/'package/camera/userland'),'-c',str(root/'tests/camera-system-vendor.c'),'-o',str(out/'vendor.o')],check=True)
subprocess.run(['gcc','-fsanitize=address,undefined',str(out/'vendor.o'),'-o',str(out/'test-vendor')],check=True)
subprocess.run([str(out/'test-vendor')],check=True)
