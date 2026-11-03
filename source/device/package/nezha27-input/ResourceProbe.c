// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse sheng BootDiagExport's native VFS directory-read method,limited to
// selected original OS resources. No file/volume writes,no user-data export.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
extern void IOSleep(unsigned int);
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
