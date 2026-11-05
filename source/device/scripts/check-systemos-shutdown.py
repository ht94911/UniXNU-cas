"""Exercise XNU's actual system-mount setter and two-pass unmount callback."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--xnu',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
kpi=(a.xnu/'bsd/vfs/kpi_vfs.c').read_text()
vfs=(a.xnu/'bsd/vfs/vfs_subr.c').read_text()
shutdown=(a.xnu/'bsd/kern/kern_shutdown.c').read_text()
header=(a.xnu/'bsd/sys/mount_internal.h').read_text()
public=(a.xnu/'bsd/sys/mount.h').read_text()
setter=kpi[kpi.index('void\nvfs_setmntsystem('):kpi.index('\nvoid\nvfs_setmntsystemdata(')]
start=vfs.index('struct unmount_info {')
end=vfs.index('\n/*',vfs.index('\nstatic int\nunmount_callback(',start))
callback=vfs[start:end]
assert 'mp->mnt_kern_flag |= MNTK_SYSTEM;' in setter
assert 'MNT_ROOTFS' in callback and 'MNTK_SYSTEM' in callback and 'dounmount(mp, MNT_FORCE, 1' in callback
first=shutdown.index('proc_shutdown(TRUE);')
non=shutdown.index('vfs_unmountall(TRUE);',first)
dext=shutdown.index('kIOSystemShutdownNotificationTerminateDEXTs',non)
second=shutdown.index('proc_shutdown(FALSE);',dext)
system=shutdown.index('vfs_unmountall(FALSE);',second)
assert first<non<dext<second<system
assert shutdown.index('if (only_non_dext) {\n\t\treturn;')<shutdown.index('task_suspend_internal(proc_task(initproc));')
def constant(source,name):
    m=re.search(r'^#define\s+'+name+r'\s+(0x[0-9A-Fa-f]+|[0-9]+)\b',source,re.M)
    assert m,name
    return int(m.group(1),0)
values={n:constant(header if n.startswith('MNTK') else public,n) for n in ['MNTK_SYSTEM','MNT_RDONLY','MNT_UNION','MNT_ROOTFS','MNT_FORCE']}
fixture=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
struct mount {uint64_t mnt_flag;uint32_t mnt_kern_flag;struct {char f_mntonname[64];} mnt_vfsstat;};
typedef struct mount *mount_t;
static int locked,drops,refs,unmounts,forced_error;
static void mount_lock(mount_t p){assert(p && !locked);locked=1;}
static void mount_unlock(mount_t p){assert(p && locked);locked=0;}
static void mount_iterdrop(mount_t p){assert(p && !locked);drops++;}
static void mount_ref(mount_t p,int flags){assert(p && !flags && !locked);refs++;}
static void *vfs_context_current(void){return NULL;}
static int dounmount(mount_t p,int force,int kernel,void *ctx){assert(p && force==MNT_FORCE && kernel==1 && !ctx && !locked);unmounts++;return forced_error;}
static size_t strlcpy(char *d,const char *s,size_t n){size_t l=strlen(s);if(n){size_t k=l<n-1?l:n-1;memcpy(d,s,k);d[k]=0;}return l;}
#define MAXPATHLEN 1024
#define ZV_NAMEI 0
#define Z_WAITOK 1
#define Z_NOFAIL 2
#define VFS_RETURNED 0
#define zalloc_flags(zone,flags) malloc(MAXPATHLEN)
#define zfree(zone,ptr) free(ptr)
'''
fixture='\n'.join('#define '+n+' '+hex(v) for n,v in values.items())+'\n'+fixture+setter+'\n'+callback+r'''
int main(void){
 struct mount system={.mnt_flag=MNT_RDONLY|MNT_UNION,.mnt_vfsstat={"SystemOS"}};
 struct mount data={.mnt_vfsstat={"Data"}};
 struct mount root={.mnt_flag=MNT_RDONLY|MNT_ROOTFS,.mnt_vfsstat={"Root"}};
 struct unmount_info first={.u_only_non_system=1},last={0};
 unmount_callback(&system,&first);assert(unmounts==1 && first.u_count==1);
 uint64_t old_flags=system.mnt_flag;vfs_setmntsystem(&system);
 assert(!locked && system.mnt_flag==old_flags && system.mnt_kern_flag==MNTK_SYSTEM);
 unmount_callback(&system,&first);assert(unmounts==1);
 unmount_callback(&data,&first);assert(unmounts==2 && data.mnt_flag==0 && data.mnt_kern_flag==0);
 unmount_callback(&root,&first);assert(unmounts==2);
 unmount_callback(&system,&last);assert(unmounts==3 && last.u_count==1);
 unmount_callback(&root,&last);assert(unmounts==4);
 forced_error=EBUSY;unmount_callback(&data,&last);assert(unmounts==5 && last.u_busy==1 && last.u_errs==1);
 assert(drops==7 && refs==5 && !locked);
 puts("PASS: untagged SystemOS removed early; tagged SystemOS deferred; Data unchanged; late unmount and errors retained");
 return 0;
}
'''
src=a.output/'actual-unmount-callback.c';src.write_text(fixture)
exe=a.output/'actual-unmount-callback'
subprocess.run(['cc','-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(src),'-o',str(exe)],check=True)
result=subprocess.run([str(exe)],capture_output=True,text=True,check=True)
(a.output/'run.log').write_text(result.stdout+result.stderr)
sha=lambda s:hashlib.sha256(s.encode()).hexdigest()
report=dict(passed=True,actual_XNU_setter_sha256=sha(setter),actual_XNU_unmount_callback_sha256=sha(callback),
    actual_shutdown_source_sha256=sha(shutdown),actual_mount_constants=values,ASan_UBSan=True,
    early_unmarked_system_unmount_reproduced=True,marked_system_deferred_until_second_pass=True,
    Data_policy_unchanged=True,late_unmount_and_failure_accounting_retained=True,
    original_process_and_DEXT_shutdown_order_retained=True,hardware_verified=False,phone_operations=False)
(a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(result.stdout.strip())

