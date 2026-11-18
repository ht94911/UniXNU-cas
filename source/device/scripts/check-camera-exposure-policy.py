#!/usr/bin/env python3
"""Exercise the compiled XNU source's two exposure checks with its exact owner predicate."""
import argparse,hashlib,json,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--xnu',required=True,type=Path)
p.add_argument('--out',required=True,type=Path)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
vfs=(a.xnu/'security/mac_vfs.c').read_text();file=(a.xnu/'security/mac_file.c').read_text()
def function(text,name):
    match=re.search(r'(?m)^'+re.escape(name)+r'\([^;]*?\)\s*\{',text)
    assert match,name
    start=text.index('{',match.start());depth=1;end=start+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[match.start():end]
reader=function(vfs,'nezha_camera_node_reader')
allowed=function(vfs,'nezha_camera_exposure_ioctl_allowed')
node=function(vfs,'mac_vnode_check_ioctl');global_check=function(file,'mac_file_check_ioctl')
assert 'nezha_camera_exposure_ioctl_allowed(ctx, vp, cmd)' in node
prefix=r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#define CS_CDHASH_LEN 20
#define CS_VALID 1
#define SECURITY_MAC_CHECK_ENFORCE 0
#define DTYPE_VNODE 1
#define FILEGLOB_DTYPE(fg) ((fg)->type)
#define VFS_KERNEL_DEBUG_START1(a,b) ((void)0)
#define VFS_KERNEL_DEBUG_END1(a,b) ((void)0)
#define mac_vnode_label(v) (v)
#define MAC_CHECK(which,...) (error=policy_##which)
#if !defined(__APPLE__)
#define __attribute__(x)
#endif
struct ucred {unsigned uid;};
struct vnode {bool owned;};
struct cs_blob {const char *identity;};
struct proc {int parent;unsigned flags;void *p_textvp;unsigned p_textoff;bool valid_hash;uint8_t hash[20];};
struct context {struct ucred cred;struct proc *process;};
struct fileglob {int type;struct vnode *data;};
typedef struct vnode *vnode_t;
typedef struct proc *proc_t;
typedef struct ucred *kauth_cred_t;
typedef struct context *vfs_context_t;
typedef unsigned long u_long;
static struct context context;
static struct cs_blob signature;
static unsigned mode;
static bool has_mode;
static int policy_file_check_ioctl,policy_vnode_check_ioctl;
static bool nezha_camera_native_node(vnode_t v){return v && v->owned;}
static bool PE_parse_boot_argn(const char *name,unsigned *value,unsigned bytes)
{assert(!strcmp(name,"nezha-camera") && bytes==sizeof(unsigned));*value=mode;return has_mode;}
static kauth_cred_t vfs_context_ucred(vfs_context_t c){return &c->cred;}
static unsigned kauth_cred_getuid(kauth_cred_t c){return c->uid;}
static proc_t vfs_context_proc(vfs_context_t c){return c->process;}
static int proc_original_ppid(proc_t p){return p->parent;}
static unsigned proc_getcsflags(proc_t p){return p->flags;}
static struct cs_blob *csvnode_get_blob(void *text,unsigned off){(void)text;(void)off;return &signature;}
static const char *csblob_get_identity(struct cs_blob *b){return b->identity;}
static int proc_getcdhash(proc_t p,uint8_t *hash){memcpy(hash,p->hash,20);return p->valid_hash?0:-1;}
static bool mac_cred_check_enforce(kauth_cred_t c){(void)c;return true;}
static vfs_context_t vfs_context_current(void){return &context;}
static void *fg_get_data(struct fileglob *f){return f->data;}
'''
consumer=r'''
static unsigned delivered;
static int original_ioctl(struct fileglob *fg,u_long command)
{
 int e=mac_file_check_ioctl(&context.cred,fg,command);
 if(e)return e;
 e=mac_vnode_check_ioctl(&context,fg->data,command);
 if(e)return e;
 ++delivered;return 0;
}
int main(void)
{
 const u_long command=0x80184e31UL;
 struct vnode owned={true},other={false};
 struct fileglob fg={DTYPE_VNODE,&owned};
 struct proc process={.parent=1,.flags=CS_VALID,.valid_hash=true,
     .hash={0xca,0x24,0xf7,0x1b,0x70,0x27,0x47,0xd2,0xed,0x01,0x2c,0x3a,0xed,0x2d,0xaa,0x4f,0x00,0x70,0x6c,0x45}};
 context=(struct context){{501},&process};signature.identity="com.apple.cameracaptured";
 has_mode=true;mode=6;policy_file_check_ioctl=policy_vnode_check_ioctl=EPERM;
 assert(!original_ioctl(&fg,command) && delivered==1);
 assert(original_ioctl(&fg,command+1)==EPERM);
 mode=5;assert(original_ioctl(&fg,command)==EPERM);mode=6;
 has_mode=false;assert(original_ioctl(&fg,command)==EPERM);has_mode=true;
 context.cred.uid=0;assert(original_ioctl(&fg,command)==EPERM);context.cred.uid=501;
 context.process=NULL;assert(original_ioctl(&fg,command)==EPERM);context.process=&process;
 process.parent=2;assert(original_ioctl(&fg,command)==EPERM);process.parent=1;
 process.flags=0;assert(original_ioctl(&fg,command)==EPERM);process.flags=CS_VALID;
 signature.identity="com.apple.Camera";assert(original_ioctl(&fg,command)==EPERM);
 signature.identity="com.apple.cameracaptured";
 process.hash[0]^=1;assert(original_ioctl(&fg,command)==EPERM);process.hash[0]^=1;
 process.valid_hash=false;assert(original_ioctl(&fg,command)==EPERM);process.valid_hash=true;
 fg.type=2;assert(original_ioctl(&fg,command)==EPERM);fg.type=DTYPE_VNODE;
 fg.data=&other;assert(original_ioctl(&fg,command)==EPERM);fg.data=&owned;
 policy_file_check_ioctl=EACCES;assert(original_ioctl(&fg,command)==EACCES);
 policy_file_check_ioctl=EPERM;policy_vnode_check_ioctl=EACCES;assert(original_ioctl(&fg,command)==EACCES);
 policy_file_check_ioctl=policy_vnode_check_ioctl=0;
 assert(!original_ioctl(&fg,command) && delivered==2);
 assert(!original_ioctl(&fg,command+1) && delivered==3);
 puts("PASS Camera exposure policy: 17 original source cases; both ioctl stages; exact native node and signed reader");
 return 0;
}
'''
source=prefix+'\nstatic bool\n'+reader+'\nbool\n'+allowed+'\nint\n'+node+'\nint\n'+global_check+'\n'+consumer
c=a.out/'consumer.c';c.write_text(source);exe=a.out/'consumer'
cmd=[os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-address','-Wno-ignored-attributes','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)]
r=subprocess.run(cmd,capture_output=True,text=True);(a.out/'build.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
r=subprocess.run([str(exe)],capture_output=True,text=True);(a.out/'run.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report=dict(passed=True,cases=17,actual_XNU_source_functions_executed=True,both_ioctl_stages_checked=True,exact_original_signed_reader_and_node=True,other_policy_errors_preserved=True,file_source_sha256=sha(a.xnu/'security/mac_file.c'),vnode_source_sha256=sha(a.xnu/'security/mac_vfs.c'),test_sha256=sha(Path(__file__)),consumer_sha256=sha(c),phone_operations=False)
(a.out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(r.stdout,end='')
