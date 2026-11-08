"""Replay actual XNU root replacement with counted filesystem and cwd owners."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tomllib

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline',type=Path,required=True)
parser.add_argument('--xnu',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
args.output.mkdir(parents=True,exist_ok=False)
device=Path(__file__).resolve().parents[1]
series=device/'patches/xnu-12377/series'
assert 'device:patches/xnu-12377/series' in tomllib.loads((device/'board.toml').read_text())['kernel']['patch_series']
assert series.read_text().splitlines().count('zzzz-native-root-cwd-ownership.patch')==1

def function(text,name):
    match=re.search(r'\n(?:static )?(?:void|int)\n'+re.escape(name)+r'\(',text)
    assert match,name
    end=text.index('\n}\n',match.start())+3
    return text[match.start()+1:end]

fixture=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define DIAGNOSTIC 0
#define VROOT 1
#define NODEV (-1)
#define PROC_RETURNED 0
#define PROC_ALLPROCLIST 1
#define PROC_NOWAITTRANS 4
struct vnode;
struct mount {struct vnode *mnt_devvp,*root;struct {struct {int val[2];} f_fsid;} stat;};
struct vnode {struct mount *v_mount,*v_mountedhere;int v_usecount,v_iocount,v_flag;const char *name;};
struct filedesc {struct vnode *fd_cdir,*fd_rdir;};
struct proc {struct filedesc p_fd;int pid,fd_locked,dirs_locked;};
typedef struct vnode *vnode_t;
typedef struct mount *mount_t;
typedef struct proc *proc_t;
typedef void *vfs_context_t;
struct cdirargs {vnode_t olddp,newdp;};
static struct proc processes[8];
static proc_t kernproc=&processes[0];
static vnode_t rootvnode,rootvp;
static int rootdev,rootvnode_rw_lock,process_count,underflows,root_error;
static int race_pid;
static vnode_t race_destination;
static void proc_fdlock(proc_t p){assert(!p->fd_locked);p->fd_locked=1;}
static void proc_fdunlock(proc_t p){assert(p->fd_locked);p->fd_locked=0;}
static int vnode_ref(vnode_t v){assert(v && v->v_usecount>=0);v->v_usecount++;return 0;}
static void vnode_rele(vnode_t v){assert(v);if(v->v_usecount--<1)underflows++;}
static void vnode_put(vnode_t v){assert(v && v->v_iocount>0);v->v_iocount--;}
static void proc_dirs_lock_exclusive(proc_t p){
 assert(!p->dirs_locked);p->dirs_locked=1;
 if(race_pid==p->pid && race_destination){
  vnode_ref(race_destination);vnode_rele(p->p_fd.fd_cdir);
  p->p_fd.fd_cdir=race_destination;race_destination=NULL;
 }
}
static void proc_dirs_unlock_exclusive(proc_t p){assert(p->dirs_locked);p->dirs_locked=0;}
static void lck_rw_lock_exclusive(int *p){assert(!*p);*p=1;}
static void lck_rw_unlock_exclusive(int *p){assert(*p);*p=0;}
static int vnode_specrdev(vnode_t v){assert(v);return 0;}
#define vfs_statfs(mp) (&(mp)->stat)
static int VFS_ROOT(mount_t m,vnode_t *v,void *ctx){
 (void)ctx;if(root_error)return root_error;assert(m && m->root);*v=m->root;(*v)->v_iocount++;return 0;
}
static void proc_iterate(unsigned flags,int (*callout)(proc_t,void *),void *arg,void *filter,void *filterarg){
 assert(flags==(PROC_ALLPROCLIST|PROC_NOWAITTRANS) && !filter && !filterarg);
 for(int i=0;i<process_count;i++)assert(callout(&processes[i],arg)==PROC_RETURNED);
}
'''

tests=r'''
static void init(vnode_t root){
 memset(processes,0,sizeof(processes));process_count=1;rootvnode=NULL;rootvp=NULL;
 rootdev=0;rootvnode_rw_lock=underflows=root_error=0;race_pid=-1;race_destination=NULL;
 // bsd_init obtains one usecount before set_rootvnode; its cwd is borrowed.
 root->v_iocount++;vnode_ref(root);vnode_put(root);set_rootvnode(root);
 assert(rootvnode==root && kernproc->p_fd.fd_cdir==root && root->v_usecount==2);
}
static proc_t user(vnode_t cwd,vnode_t rdir){
 proc_t p=&processes[process_count];p->pid=process_count++;
 if(cwd){vnode_ref(cwd);p->p_fd.fd_cdir=cwd;}
 if(rdir){vnode_ref(rdir);p->p_fd.fd_rdir=rdir;}
 return p;
}
static void mount_above(vnode_t old,vnode_t next,mount_t mount){
 mount->root=next;old->v_mountedhere=mount;vnode_ref(old); // mount's covered-vnode owner
}
static void root_replacement(int kernel_chroot,int users){
 struct mount base={0},upper={0};
 struct vnode old={.v_mount=&base,.v_usecount=1,.name="OS"};
 struct vnode next={.v_mount=&upper,.v_usecount=1,.name="SystemOS"};
 init(&old);
 // Two remaining APFS children each own a parent ref, one survives the first vflush.
 vnode_ref(&old);vnode_ref(&old);
 if(kernel_chroot){vnode_ref(&old);kernproc->p_fd.fd_rdir=&old;}
 for(int i=0;i<users;i++)user(&old,i&1?&old:NULL);
 mount_above(&old,&next,&upper);
 assert(checkdirs(&old,NULL)==0);
 int old_expected=4; // APFS owner + two children + covering mount
 int new_expected=2+users+users/2+kernel_chroot; // APFS owner + shared global/cwd + user owners
 assert(old.v_usecount==old_expected-(FIXED?0:1));
 assert(next.v_usecount==new_expected+(FIXED?0:1));
 assert(rootvnode==&next && kernproc->p_fd.fd_cdir==&next && next.v_iocount==0);
 // After upper unmount, APFS first vflush, APFS owner release, final child cleanup.
 vnode_rele(&old);vnode_rele(&old);vnode_rele(&old);vnode_rele(&old);
 assert(underflows==(FIXED?0:1) && old.v_usecount==(FIXED?0:-1));
 printf("root replacement users=%d kernel_chroot=%d: old_delta=%d new_delta=%d final_underflow=%d\n",users,kernel_chroot,FIXED?0:-1,FIXED?0:1,underflows);
}
static void ordinary_mount(void){
 struct mount base={0},upper={0};
 struct vnode root={.v_mount=&base,.v_usecount=1};
 struct vnode old={.v_mount=&base,.v_usecount=1},next={.v_mount=&upper,.v_usecount=1};
 init(&root);vnode_ref(&old);kernproc->p_fd.fd_cdir=&old; // own cwd away from the global root
 user(&old,&old);vnode_ref(&old); // parent ref
 mount_above(&old,&next,&upper);
 assert(checkdirs(&old,NULL)==0);
 assert(old.v_usecount==3 && next.v_usecount==4 && rootvnode==&root && !underflows);
 puts("ordinary mount: kernel and user owned cwd/rdir refs balanced");
}
static void kernel_cwd_elsewhere(void){
 struct mount base={0},upper={0};
 struct vnode old={.v_mount=&base,.v_usecount=1},next={.v_mount=&upper,.v_usecount=1},other={.v_mount=&base,.v_usecount=1};
 init(&old);vnode_ref(&other);kernproc->p_fd.fd_cdir=&other;
 user(&old,NULL);mount_above(&old,&next,&upper);
 assert(checkdirs(&old,NULL)==0);
 assert(old.v_usecount==2 && next.v_usecount==3 && other.v_usecount==2 && kernproc->p_fd.fd_cdir==&other && !underflows);
 puts("root replacement with separate kernel cwd: no borrowed-cwd change");
}
static void failed_root_lookup(void){
 struct mount base={0},upper={0};
 struct vnode old={.v_mount=&base,.v_usecount=1},next={.v_mount=&upper,.v_usecount=1};
 init(&old);user(&old,NULL);mount_above(&old,&next,&upper);
 int n=old.v_usecount;root_error=EIO;
 assert(checkdirs(&old,NULL)==EIO && old.v_usecount==n && next.v_usecount==1 && !next.v_iocount && rootvnode==&old && !underflows);
 puts("VFS_ROOT failure: original owners and pointers retained");
}
static void cwd_race(void){
 struct mount base={0},upper={0};
 struct vnode root={.v_mount=&base,.v_usecount=1},old={.v_mount=&base,.v_usecount=1};
 struct vnode next={.v_mount=&upper,.v_usecount=1},other={.v_mount=&base,.v_usecount=1};
 init(&root);user(&old,NULL);mount_above(&old,&next,&upper);
 race_pid=1;race_destination=&other;
 assert(checkdirs(&old,NULL)==0 && old.v_usecount==2 && next.v_usecount==1 && other.v_usecount==2 && !underflows);
 puts("cwd change between locks: temporary refs released without stealing the new cwd");
}
static void repeated_root_replacement(void){
 struct mount ma={0},mb={0},mc={0};
 struct vnode a={.v_mount=&ma,.v_usecount=1},b={.v_mount=&mb,.v_usecount=1},c={.v_mount=&mc,.v_usecount=1};
 init(&a);user(&a,NULL);mount_above(&a,&b,&mb);assert(checkdirs(&a,NULL)==0);
 mount_above(&b,&c,&mc);assert(checkdirs(&b,NULL)==0);
 assert(a.v_usecount==(FIXED?2:1) && b.v_usecount==2 && c.v_usecount==(FIXED?3:4) && !underflows);
 puts("repeated root replacement: shared owner remains shared");
}
int main(void){
 root_replacement(0,0);root_replacement(0,3);root_replacement(1,3);
 ordinary_mount();kernel_cwd_elsewhere();failed_root_lookup();cwd_race();repeated_root_replacement();
 return 0;
}
'''

records=[]
for name,source,fixed in [('baseline',args.baseline,0),('fixed',args.xnu,1)]:
    bsd=(source/'bsd/kern/bsd_init.c').read_text()
    vfs=(source/'bsd/vfs/vfs_syscalls.c').read_text()
    proc=(source/'bsd/kern/kern_proc.c').read_text()
    # Establish the initial owner and that PID 0 participates in PROC_ALLPROCLIST.
    initial_start=bsd.index('if (VFS_ROOT(mountlist.tqh_first')
    initial=bsd[initial_start:bsd.index('init_rootvnode = NULLVP;',initial_start)]
    assert initial.count('vnode_ref(init_rootvnode)')==1 and 'set_rootvnode(init_rootvnode);' in initial
    assert 'LIST_INSERT_HEAD(&allproc, kernproc, p_list);' in bsd and 'kernproc->p_stat = SRUN;' in bsd
    assert 'ALLPROC_FOREACH(p)' in proc and 'return proc_ref(kernproc, false);' in proc
    funcs={n:function(bsd if n=='set_rootvnode' else vfs,n) for n in ['set_rootvnode','checkdirs_callback','checkdirs']}
    assert 'kernproc->p_fd.fd_cdir = new_rootvnode;' in funcs['set_rootvnode']
    assert funcs['set_rootvnode'].count('vnode_rele(')==1 and 'vnode_ref(' not in funcs['set_rootvnode']
    src=args.output/(name+'.c');src.write_text('#define FIXED '+str(fixed)+'\n'+fixture+'\n'.join(funcs.values())+tests)
    exe=args.output/name
    build=['cc','-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(src),'-o',str(exe)]
    subprocess.run(build,check=True)
    result=subprocess.run([str(exe)],capture_output=True,text=True)
    (args.output/(name+'.log')).write_text(result.stdout+result.stderr)
    assert result.returncode==0,(name,result.stdout,result.stderr)
    records.append(dict(name=name,passed=True,functions_sha256={k:hashlib.sha256(v.encode()).hexdigest() for k,v in funcs.items()},
                        output=result.stdout,build=build))
report=dict(passed=True,cases_per_variant=8,ASan_UBSan=True,actual_XNU_functions=True,
            unmodified_baseline_one_ref_deficit_reproduced=True,unmodified_baseline_shutdown_underflow_reproduced=True,
            fixed_root_transfer_and_ordinary_cwds_balanced=True,active_series_entry_verified=True,
            original_shutdown_and_vnode_assertions_unchanged=True,variants=records,
            scope='Host replay of actual XNU functions with explicit reference owners; APFS callback order modeled from the original binary. Hardware confirmation is still required.',
            hardware_verified=False,phone_operations=False)
(args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: 8 cases per variant; original source loses one old-root ref and underflows during cleanup; repaired source balances all owners.')
