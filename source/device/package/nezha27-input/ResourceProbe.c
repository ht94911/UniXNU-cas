// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse sheng BootDiagExport's native VFS directory-read method,limited to
// selected original OS resources. No file/volume writes,no user-data export.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
extern void IOSleep(unsigned int);
// Same native open/read/close path as upstream BootDiagExport, fixed original
// system files only. Check metadata and all bytes up to 64 KiB; export a digest
// and format header, never file contents. No write direction or open flags.
static void read_one(const char *path,vfs_context_t ctx) {
    vnode_t file=NULLVP;int error=vnode_open(path,FREAD,0,0,&file,ctx);
    if(error){printf("NEZHA RESOURCE OPEN: error=%d path=%s\n",error,path);return;}
    struct vnode_attr va;memset(&va,0,sizeof(va));VATTR_INIT(&va);VATTR_WANTED(&va,va_data_size);
    error=vnode_getattr(file,&va,ctx);
    printf("NEZHA RESOURCE ATTR: error=%d supported=%d size=%llu path=%s\n",error,
        VATTR_IS_SUPPORTED(&va,va_data_size)!=0,(unsigned long long)va.va_data_size,path);
    if(error || !VATTR_IS_SUPPORTED(&va,va_data_size) || va.va_data_size>65536)goto close;
    unsigned char bytes[512],header[8]={0};uint64_t at=0,hash=UINT64_C(14695981039346656037);
    while(at<va.va_data_size) {
        int want=(int)((va.va_data_size-at)<sizeof(bytes)?(va.va_data_size-at):sizeof(bytes));
        int residual=want;error=vn_rdwr(UIO_READ,file,(caddr_t)bytes,want,(off_t)at,UIO_SYSSPACE,0,
            vfs_context_ucred(ctx),&residual,vfs_context_proc(ctx));
        if(error || residual<0 || residual>want){printf("NEZHA RESOURCE READ: error=%d residual=%d at=%llu path=%s\n",error,residual,(unsigned long long)at,path);break;}
        unsigned got=(unsigned)(want-residual);if(!got){printf("NEZHA RESOURCE READ: short EOF at=%llu path=%s\n",(unsigned long long)at,path);break;}
        if(!at)memcpy(header,bytes,got<sizeof(header)?got:sizeof(header));
        for(unsigned i=0;i<got;++i){hash^=bytes[i];hash*=UINT64_C(1099511628211);}
        at+=got;
    }
    printf("NEZHA RESOURCE CONTENT: error=%d bytes=%llu fnv1a64=%llx header=%02x%02x%02x%02x%02x%02x%02x%02x path=%s\n",error,
        (unsigned long long)at,(unsigned long long)hash,header[0],header[1],header[2],header[3],header[4],header[5],header[6],header[7],path);
close:
    error=vnode_close(file,FREAD,ctx);if(error)printf("NEZHA RESOURCE CLOSE: error=%d path=%s\n",error,path);
}
static void lookup_one(const char *path,vfs_context_t ctx) {
    vnode_t file=NULLVP;int error=vnode_lookup(path,0,&file,ctx);
    printf("NEZHA RESOURCE LOOKUP: error=%d path=%s\n",error,path);
    if(file)vnode_put(file);
}
static void list_one(const char *path,vfs_context_t ctx) {
    vnode_t dir=NULLVP;int error=vnode_lookup(path,0,&dir,ctx);
    if(error){printf("NEZHA RESOURCE DIR: lookup=%d path=%s\n",error,path);return;}
    if(vnode_vtype(dir)!=VDIR){printf("NEZHA RESOURCE DIR: not-directory path=%s\n",path);vnode_put(dir);return;}
    char *buffer=IOMallocData(8192);
    if(!buffer){vnode_put(dir);return;}
    off_t offset=0;unsigned records=0;int eof=0;
    for(unsigned pass=0;pass<8 && !eof && records<128;++pass) {
        uio_t uio=uio_create(1,offset,UIO_SYSSPACE,UIO_READ);if(!uio)break;
        error=uio_addiov(uio,CAST_USER_ADDR_T(buffer),8192);
        int count=0;if(!error)error=VNOP_READDIR(dir,uio,0,&eof,&count,ctx);
        size_t got=8192-(size_t)uio_resid(uio);off_t next=uio_offset(uio);uio_free(uio);
        printf("NEZHA RESOURCE READDIR: error=%d bytes=%lu count=%d eof=%d offset=%lld next=%lld path=%s\n",
            error,(unsigned long)got,count,eof,(long long)offset,(long long)next,path);
        if(error || got>8192 || !got)break;
        for(size_t at=0;at<got && records<128;) {
            const size_t name=offsetof(struct dirent,d_name);
            if(got-at<name){printf("NEZHA RESOURCE DIRENT: short header\n");goto done;}
            const struct dirent *d=(const struct dirent *)(buffer+at);
            if(d->d_reclen<name+1 || d->d_reclen>got-at || d->d_namlen>=d->d_reclen-name || d->d_name[d->d_namlen]){
                printf("NEZHA RESOURCE DIRENT: malformed at=%lu reclen=%u namlen=%u\n",(unsigned long)at,d->d_reclen,d->d_namlen);goto done;
            }
            if(d->d_ino)printf("NEZHA RESOURCE ENTRY: type=%u name=%.*s\n",d->d_type,(int)d->d_namlen,d->d_name);
            ++records;at+=d->d_reclen;
        }
        if(next==offset && !eof){printf("NEZHA RESOURCE DIR: no cookie progress\n");break;}
        offset=next;
    }
done:
    IOFreeData(buffer,8192);vnode_put(dir);
}
static void resource_thread(void *argument,wait_result_t result) {
    (void)argument;(void)result;
    IOSleep(120000);
    vfs_context_t ctx=vfs_context_current();
    const char *dirs[]={
        "/System/Library/DuetActivityScheduler/Scheduler/DuetActivitySchedulerDaemon.bundle",
        "/System/Library/DuetActivityScheduler/Scheduler/DuetActivitySchedulerDaemon.bundle/DuetActivityScheduler.momd",
        "/System/Library/PrivateFrameworks/SafariShared.framework"
    };
    for(unsigned i=0;i<sizeof(dirs)/sizeof(dirs[0]);++i)list_one(dirs[i],ctx);
    lookup_one("/System/Library/DuetActivityScheduler/Scheduler/DuetActivitySchedulerDaemon.bundle/DuetActivityScheduler.momd/VersionInfo.plist",ctx);
    lookup_one("/System/Library/DuetActivityScheduler/Scheduler/DuetActivitySchedulerDaemon.bundle/DuetActivityScheduler.momd/DuetActivityScheduler-v3.mom",ctx);
    const char *files[]={
        "/System/Library/DuetActivityScheduler/Scheduler/DuetActivitySchedulerDaemon.bundle/Info.plist",
        "/System/Library/DuetActivityScheduler/Scheduler/DuetActivitySchedulerDaemon.bundle/DuetActivityScheduler.momd/VersionInfo.plist",
        "/System/Library/DuetActivityScheduler/Scheduler/DuetActivitySchedulerDaemon.bundle/DuetActivityScheduler.momd/DuetActivityScheduler-v3.mom",
        "/System/Library/PrivateFrameworks/SafariShared.framework/SitesAllowedToAutoplay.plist"
    };
    for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);++i)read_one(files[i],ctx);
    printf("NEZHA RESOURCE PROBE: complete;read-only original system paths,no export or persistence\n");
    thread_terminate(current_thread());
}
void nezha_resource_probe_start(void) {
    unsigned option=0;PE_parse_boot_argn("resource-diag",&option,sizeof(option));if(option!=1)return;
    static bool started;if(started)return;started=true;
    thread_t thread;int result=kernel_thread_start(resource_thread,NULL,&thread);
    if(result==KERN_SUCCESS)thread_deallocate(thread);
    printf("NEZHA RESOURCE PROBE: thread=%d deferred system-resource reads\n",result);
}
