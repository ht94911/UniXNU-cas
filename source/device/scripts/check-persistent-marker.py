"""Exercise the actual bounded marker function with modeled file syscalls."""
from pathlib import Path
import subprocess,hashlib,json
import argparse
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--device',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
a=ap.parse_args();D=a.device;O=a.output;O.mkdir(parents=True,exist_ok=True)
p=D/'package/BootWatchdogPolicy.c';s=p.read_text();a=s.index('static int nezha_persist_check(');b=s.index('\nint sheng_boot_watchdog_policy',a);body=s[a:b]
pre=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef int proc_t;typedef long user_ssize_t;
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x100
#define O_EXCL 0x200
#define O_NOFOLLOW 0x1000
#define S_ISREG(x) (((x)&0170000)==0100000)
struct open_args{uint64_t path;int flags,mode;};
struct write_args{int fd;uint64_t cbuf;uint64_t nbyte;};
struct read_args{int fd;uint64_t cbuf;uint64_t nbyte;};
struct fsync_args{int fd;};struct close_args{int fd;};
struct fstat64_args{int fd;uint64_t ub;};
struct stat64{unsigned st_uid,st_gid,st_mode;long st_size;};
static unsigned persist=2,exists,opened,reads,writes,syncs,closed,foreign_owner,short_read,sync_fail,race,symlinked;
static uint64_t ticks=12345;static uint8_t user[4096],saved[64];
static int PE_parse_boot_argn(const char *n,void *v,int b){assert(!strcmp(n,"sheng-data-persist")&&b==4);*(unsigned *)v=persist;return 1;}
static uint64_t mach_absolute_time(void){return ++ticks;}
static int copyout(const void *src,uint64_t dst,size_t n){assert(dst>=0x10000&&dst-0x10000+n<=sizeof(user));memcpy(user+dst-0x10000,src,n);return 0;}
static int copyin(uint64_t src,void *dst,size_t n){assert(src>=0x10000&&src-0x10000+n<=sizeof(user));memcpy(dst,user+src-0x10000,n);return 0;}
static int open(proc_t p,struct open_args *a,int32_t *ret){
 assert(p==1 && (a->flags&O_NOFOLLOW));assert(!strcmp((char *)user+a->path-0x10000,"/private/var/db/nezha27-persistence-check"));
 if(symlinked)return ELOOP;
 if(a->flags&O_CREAT){assert(a->flags&O_EXCL);if(race||exists)return EEXIST;assert(a->mode==0600);exists=1;}
 else if(!exists)return ENOENT;
 ++opened;*ret=7;return 0;
}
static int write(proc_t p,struct write_args *a,user_ssize_t *n){assert(p==1&&a->fd==7&&a->nbyte==64);++writes;copyin(a->cbuf,saved,64);*n=64;return 0;}
static int read(proc_t p,struct read_args *a,user_ssize_t *n){assert(p==1&&a->fd==7&&a->nbyte==64);++reads;copyout(saved,a->cbuf,64);*n=short_read?63:64;return 0;}
static int fsync(proc_t p,struct fsync_args *a,int32_t *n){assert(p==1&&a->fd==7);++syncs;*n=0;return sync_fail?EIO:0;}
static int sys_close(proc_t p,struct close_args *a,int32_t *n){assert(p==1&&a->fd==7);++closed;*n=0;return 0;}
static int sys_fstat64(proc_t p,struct fstat64_args *a,int32_t *n){assert(p==1&&a->fd==7);struct stat64 s={foreign_owner?1:0,0,0100600,64};copyout(&s,a->ub,sizeof(s));*n=0;return 0;}
'''
test=r'''
int main(void){
 assert(nezha_persist_check(1,0x10000)==0);assert(writes==1&&reads==0&&syncs==1&&closed==1&&exists);
 uint8_t first[64];memcpy(first,saved,64);ticks=45678;
 assert(nezha_persist_check(1,0x10000)==0);assert(writes==1&&reads==1&&syncs==1&&closed==2&&!memcmp(saved,first,64));
 foreign_owner=1;assert(nezha_persist_check(1,0x10000)==EINVAL);foreign_owner=0;
 short_read=1;assert(nezha_persist_check(1,0x10000)==EINVAL);short_read=0;
 symlinked=1;assert(nezha_persist_check(1,0x10000)==ELOOP);symlinked=0;
 exists=0;race=1;assert(nezha_persist_check(1,0x10000)==EEXIST);race=0;
 sync_fail=1;assert(nezha_persist_check(1,0x10000)==EIO);sync_fail=0;
 unsigned before=opened;persist=0;assert(nezha_persist_check(1,0x10000)==0&&opened==before);
 assert(opened==closed);puts("PASS actual filesystem probe: durable create, retained nonce, no overwrite, foreign file/symlink/race/short-read/sync errors and RAM no-op");
}
'''
(O/'actual-marker.c').write_text(pre+body+test)
subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-sign-compare','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(O/'actual-marker.c'),'-o',str(O/'marker')],check=True)
out=subprocess.check_output([str(O/'marker')],text=True);print(out)
(O/'report.json').write_text(json.dumps(dict(passed=True,source=str(p),source_sha256=hashlib.sha256(p.read_bytes()).hexdigest(),actual_function_sha256=hashlib.sha256(body.encode()).hexdigest(),modeled_file_syscalls=True,phone_operations=False,output=out),indent=2)+'\n')
