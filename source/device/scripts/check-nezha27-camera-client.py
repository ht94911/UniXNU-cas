#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the actual Camera insertion and signature scope branches on the host."""
import argparse
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
out=p.parse_args().out.resolve();out.mkdir(parents=True,exist_ok=True)
patch=(root/'patches/xnu-12377/zzzzzz-nezha-camera-client-observer.patch').read_text()
exec_patch,mac_patch=patch.split('--- a/security/mac_base.c',1)
body='\n'.join(x[1:] for x in exec_patch.splitlines() if x.startswith('+') and not x.startswith('+++'))
owner=(root/'patches/xnu-12377/zzzzzz-nezha-camera-dyld-policy.patch').read_text()
added='\n'.join(x[1:] for x in owner.splitlines() if x.startswith('+') and not x.startswith('+++'))
predicate=added[added.index('static bool\nnezha_camera_dyld_owner'):]
predicate=predicate[:predicate.index('\n}\n')+3]
ls=(root/'patches/xnu-12377/zzzzzz-nezha-camera-lsd-registration.patch').read_text().split('--- a/security/mac_base.c',1)[1]
old='\n'.join(x[1:] for x in ls.splitlines() if x.startswith('-') and not x.startswith('---'))
new='\n'.join(x[1:] for x in ls.splitlines() if x.startswith('+') and not x.startswith('+++'))
assert old in predicate;predicate=predicate.replace(old,new)
lines=mac_patch.splitlines()
old='\n'.join(x[1:] for x in lines if x.startswith((' ','-')) and not x.startswith('---')).split('\n}\n',1)[0]+'\n}'
new='\n'.join(x[1:] for x in lines if x.startswith((' ','+')) and not x.startswith('+++')).split('\n}\n',1)[0]+'\n}'
assert old in predicate;predicate=predicate.replace(old,new)
source=r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define CS_CDHASH_LEN 20
#define CS_VALID 1
#define CAST_USER_ADDR_T(x) ((uintptr_t)(x))
#define UIO_SYSSPACE 0
#define TRUE true
struct component {const char *cn_nameptr;size_t cn_namelen;};
struct nameidata {struct component ni_cnd;};
struct image_params {struct nameidata *ip_ndp;unsigned ip_argspace,ip_envc;};
struct cs_blob {const char *identity;};
struct proc {void *p_textvp;unsigned p_textoff;int parent;unsigned flags;uint8_t hash[20];};
typedef struct proc *proc_t;
static struct cs_blob blob;
static unsigned camera_mode,appended;static int failed;
static char environment[160];
static bool PE_parse_boot_argn(const char *name,void *out,size_t bytes)
{assert(!strcmp(name,"nezha-camera") && bytes==sizeof(camera_mode));*(unsigned *)out=camera_mode;return true;}
static int proc_original_ppid(proc_t p){return p->parent;}
static unsigned proc_getcsflags(proc_t p){return p->flags;}
static struct cs_blob *csvnode_get_blob(void *vp,unsigned off){(void)vp;(void)off;return &blob;}
static const char *csblob_get_identity(struct cs_blob *b){assert(b==&blob);return b->identity;}
static int proc_getcdhash(proc_t p,uint8_t *out){memcpy(out,p->hash,20);return 0;}
static int exec_add_user_string(struct image_params *p,uintptr_t text,int seg,bool nc)
{(void)p;assert(!seg && nc);if(failed)return EFAULT;++appended;strcpy(environment,(const char *)text);return 0;}
static int append(struct image_params *imgp)
{int error=0;const unsigned new_ptr_size=8;
'''+body+r'''
return 0;
bad:return error;
}
'''+predicate+r'''
int main(void)
{
 struct nameidata name={{"Camera",6}};struct image_params image={&name,32,2};
 camera_mode=5;assert(!append(&image) && !appended && image.ip_envc==2);
 camera_mode=6;assert(!append(&image) && appended==1 && image.ip_envc==3 && image.ip_argspace==24);
 assert(!strcmp(environment,"DYLD_INSERT_LIBRARIES=/System/Library/MediaCapture/libNezhaCameraDiscovery27.dylib"));
 name.ni_cnd=(struct component){"CameraX",7};assert(!append(&image) && appended==1);
 image.ip_ndp=NULL;assert(!append(&image) && appended==1);image.ip_ndp=&name;
 name.ni_cnd=(struct component){"Camera",6};image.ip_argspace=4;
 assert(append(&image)==E2BIG && image.ip_envc==3);
 failed=1;image.ip_argspace=32;assert(append(&image)==EFAULT && image.ip_envc==3);
 struct proc process={.parent=1,.flags=CS_VALID};blob.identity="com.apple.camera";
 const uint8_t camera[20]={0xb5,0x7d,0x77,0xb3,0xd3,0xed,0x97,0x1e,0x9d,0x25,0xeb,0x60,0x28,0xd8,0x70,0x9d,0xba,0x4f,0x0f,0x57};
 memcpy(process.hash,camera,20);assert(nezha_camera_dyld_owner(&process));
 for(unsigned i=0;i<20;++i){process.hash[i]^=1;assert(!nezha_camera_dyld_owner(&process));process.hash[i]^=1;}
 camera_mode=5;assert(!nezha_camera_dyld_owner(&process));camera_mode=6;
 process.parent=2;assert(!nezha_camera_dyld_owner(&process));process.parent=1;
 process.flags=0;assert(!nezha_camera_dyld_owner(&process));process.flags=CS_VALID;
 blob.identity="com.apple.cameraX";assert(!nezha_camera_dyld_owner(&process));
 const uint8_t lsd[20]={0xea,0x57,0xfe,0x6d,0xbb,0x8d,0x64,0x09,0x30,0x3e,0x21,0x67,0x59,0x80,0xe2,0xf7,0x91,0x65,0x80,0x14};
 blob.identity="com.apple.lsd";memcpy(process.hash,lsd,20);assert(nezha_camera_dyld_owner(&process));
 const uint8_t daemon[20]={0xca,0x24,0xf7,0x1b,0x70,0x27,0x47,0xd2,0xed,0x01,0x2c,0x3a,0xed,0x2d,0xaa,0x4f,0x00,0x70,0x6c,0x45};
 blob.identity="com.apple.cameracaptured";memcpy(process.hash,daemon,20);assert(nezha_camera_dyld_owner(&process));
 puts("Camera079 client insertion: native mode/name/env contract and signature/parent/20hash-byte scope passed; original two daemon owners retained");
}
'''
(out/'client.c').write_text(source)
subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',
 str(out/'client.c'),'-o',str(out/'test-client')],check=True)
subprocess.run([str(out/'test-client')],check=True)
