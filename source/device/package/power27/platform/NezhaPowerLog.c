/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Same native VFS open/read/write/close ABI as the selected BootDiagExport.
 * File IO stays on a separate worker; it never stalls the charging controller. */
#include "NezhaPowerLog.h"
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <mach/mach_time.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <string.h>
extern void IOSleep(unsigned int);
enum { LOG_QUEUE=32,LOG_LINE=1024,LOG_LIMIT=NP_POWER_LOG_LIMIT_BYTES,LOG_FOOTER_RESERVE=128 };
static struct {
 IOLock *lock;
 char line[LOG_QUEUE][LOG_LINE];uint16_t lengths[LOG_QUEUE];
 unsigned read,write,count;uint32_t stopping,started,contended;
 NpPowerLogStatus status;
} log_state;
static uint32_t crc_update(uint32_t crc,const char *bytes,unsigned count)
{
 for(unsigned i=0;i<count;++i){crc^=(uint8_t)bytes[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&-(crc&1u));}
 return crc;
}
static int append(const char *line,unsigned bytes,int create)
{
 vfs_context_t ctx=vfs_context_kernel();vnode_t file=NULLVP;
 int flags=FWRITE|O_NOFOLLOW|(create?(O_CREAT|O_EXCL):O_APPEND);
 int error=vnode_open(log_state.status.path,flags,0600,0,&file,ctx);
 if(error)return error;
 struct vnode_attr va;VATTR_INIT(&va);VATTR_WANTED(&va,va_data_size);
 VATTR_WANTED(&va,va_uid);VATTR_WANTED(&va,va_nlink);
 error=vnode_getattr(file,&va,ctx);
 if(!error&&(vnode_vtype(file)!=VREG||!VATTR_IS_SUPPORTED(&va,va_data_size)||
    !VATTR_IS_SUPPORTED(&va,va_uid)||!VATTR_IS_SUPPORTED(&va,va_nlink)||va.va_uid||va.va_nlink!=1))error=EACCES;
 if(!error&&(va.va_data_size>LOG_LIMIT||bytes>LOG_LIMIT-va.va_data_size))error=EFBIG;
 if(!error) {
  int residual=(int)bytes;
  error=vn_rdwr(UIO_WRITE,file,(caddr_t)line,(int)bytes,0,UIO_SYSSPACE,IO_APPEND|IO_SYNC|IO_NOAUTH,
    vfs_context_ucred(ctx),&residual,vfs_context_proc(ctx));
  if(!error&&residual)error=EIO;
 }
 int close_error=vnode_close(file,FWRITE,ctx);return error?error:close_error;
}
static void log_worker(void *unused,wait_result_t result)
{
 (void)unused;(void)result;
 const char header[]="NEZHA_POWER_LOG_V1 scoped telemetry; no authentication data\n";
 int error=append(header,sizeof(header)-1,1);
 uint32_t crc=crc_update(0xffffffffu,header,sizeof(header)-1);
 IOLockLock(log_state.lock);log_state.status.phase=error?3:2;log_state.status.error=(uint32_t)error;
 log_state.status.bytes=error?0:(uint32_t)(sizeof(header)-1);IOLockUnlock(log_state.lock);
 printf("NEZHA POWER LOG START: phase=%u error=%d path=%s\n",log_state.status.phase,error,log_state.status.path);
 while(!error) {
  char line[LOG_LINE];unsigned bytes=0;int stop;
  IOLockLock(log_state.lock);stop=log_state.stopping;
  if(log_state.count){bytes=log_state.lengths[log_state.read];memcpy(line,log_state.line[log_state.read],bytes);
   log_state.read=(log_state.read+1)%LOG_QUEUE;--log_state.count;}
  IOLockUnlock(log_state.lock);
  if(!bytes){if(stop)break;IOSleep(100);continue;}
  /* User replies need not arrive in three minutes. A bounded file still
   * needs room for its integrity footer when the telemetry budget ends. */
  if(log_state.status.bytes>LOG_LIMIT-LOG_FOOTER_RESERVE||
     bytes>LOG_LIMIT-LOG_FOOTER_RESERVE-log_state.status.bytes) {
   IOLockLock(log_state.lock);log_state.status.capped=1;log_state.stopping=1;
   log_state.status.dropped+=1+log_state.count;log_state.count=0;IOLockUnlock(log_state.lock);
   break;
  }
  error=append(line,bytes,0);
  IOLockLock(log_state.lock);
  if(!error){++log_state.status.written;log_state.status.bytes+=bytes;crc=crc_update(crc,line,bytes);}
  else{log_state.status.phase=3;log_state.status.error=(uint32_t)error;}
  IOLockUnlock(log_state.lock);
 }
 if(!error) {
  char end[128];int length=snprintf(end,sizeof(end),"END_NEZHA_POWER_LOG_V1 records=%u dropped=%u crc32=%08x capped=%u\n",
    log_state.status.written,log_state.status.dropped+__atomic_load_n(&log_state.contended,__ATOMIC_RELAXED),~crc,
    log_state.status.capped);
  error=length>0&&(size_t)length<sizeof(end)?append(end,(unsigned)length,0):EIO;
  IOLockLock(log_state.lock);log_state.status.phase=error?3:4;log_state.status.error=(uint32_t)error;
  if(!error)log_state.status.bytes+=(uint32_t)length;
  IOLockUnlock(log_state.lock);
 }
 printf("NEZHA POWER LOG: phase=%u error=%u lines=%u dropped=%u capped=%u path=%s\n",
  log_state.status.phase,log_state.status.error,log_state.status.written,
  log_state.status.dropped+__atomic_load_n(&log_state.contended,__ATOMIC_RELAXED),log_state.status.capped,log_state.status.path);
 thread_terminate(current_thread());
}
int np_power_log_start(void)
{
 if(__atomic_exchange_n(&log_state.started,1u,__ATOMIC_ACQ_REL))return -1;
 log_state.lock=IOLockAlloc();if(!log_state.lock)return -1;
 /* Boot cleanup can unlink /var/tmp entries after this worker starts. Use
  * the existing Data log directory so the same named file survives startup. */
 snprintf(log_state.status.path,sizeof(log_state.status.path),"/private/var/log/nezha-power-%llx.log",
  (unsigned long long)mach_continuous_time());log_state.status.phase=1;
 thread_t thread;
 if(kernel_thread_start(log_worker,0,&thread)!=KERN_SUCCESS){log_state.status.phase=3;log_state.status.error=EAGAIN;return -1;}
 thread_deallocate(thread);return 0;
}
int np_power_log_push(const char *line,size_t bytes)
{
 if(!line||!bytes||bytes>=LOG_LINE||!log_state.lock)return -1;
 if(!IOLockTryLock(log_state.lock)){(void)__atomic_add_fetch(&log_state.contended,1u,__ATOMIC_RELAXED);return -1;}
 int result=0;
 if(log_state.stopping||(log_state.status.phase!=1&&log_state.status.phase!=2)||log_state.count==LOG_QUEUE){++log_state.status.dropped;result=-1;}
 else{memcpy(log_state.line[log_state.write],line,bytes);log_state.lengths[log_state.write]=(uint16_t)bytes;
  log_state.write=(log_state.write+1)%LOG_QUEUE;++log_state.count;}
 IOLockUnlock(log_state.lock);return result;
}
void np_power_log_stop(void)
{if(log_state.lock){IOLockLock(log_state.lock);log_state.stopping=1;IOLockUnlock(log_state.lock);}}
int np_power_log_status(NpPowerLogStatus *out)
{
 if(!out)return 0;
 memset(out,0,sizeof(*out));
 if(!log_state.lock)return 1;
 if(!IOLockTryLock(log_state.lock))return 0;
 *out=log_state.status;out->dropped+=__atomic_load_n(&log_state.contended,__ATOMIC_RELAXED);
 IOLockUnlock(log_state.lock);return 1;
}
