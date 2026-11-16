/* SPDX-License-Identifier: MIT */
#include "NezhaCameraNativeDevice.h"
#include "NezhaCameraNativeCapture.h"
#include "NezhaCameraControl.h"
#include "userland/upstream/hakios/hakios.camera.h"
#include <kern/thread.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

static const struct cdevsw *methods;
static void (*queued_worker)(void *,wait_result_t);
static unsigned runs,stops;
static uint64_t ticks;
static bool stream_failure,copy_failure;
static unsigned controls_taken;
static bool control_requested;
static bool expected_AF_readonly;
extern bool nezha_camera_native_node(vnode_t);
int vnode_ischr(vnode_t n) {return n->character;}
int vnode_tag(vnode_t n) {return n->tag;}
dev_t vnode_specrdev(vnode_t n) {return n->device;}
volatile uint32_t native_source_storage_boot_state=4;
volatile unsigned nezha27_usb_reset_owner_ready=1;
volatile int nezha_touch_stage=7;
void IOSleep(unsigned ms) { assert(ms==50); }
void *IOMallocData(size_t bytes) { return calloc(1,bytes); }
void IOFreeData(void *p,size_t bytes) { (void)bytes;free(p); }
uint64_t mach_absolute_time(void) { return ticks+=1000000; }
void absolutetime_to_nanoseconds(uint64_t t,uint64_t *out) { *out=t; }
thread_t current_thread(void) { return NULL; }
void thread_set_thread_name(thread_t t,const char *name) { (void)t;assert(name); }
int kernel_thread_start(void (*fn)(void *,wait_result_t),void *p,thread_t *out)
{ assert(!p && !queued_worker);queued_worker=fn;*out=(void *)1;return 0; }
void thread_deallocate(thread_t t) { assert(t==(void *)1); }
void thread_terminate_self(void) { }
int cdevsw_add(int wanted,const struct cdevsw *table) { assert(wanted==-1);methods=table;return 1; }
void *devfs_make_node(dev_t d,int type,uid_t uid,gid_t gid,int mode,const char *path,...)
{ (void)d;assert(type==1 && uid==501 && gid==501 && mode==0600 && !strcmp(path,"nezha-camera"));return (void *)1; }
void wakeup(void *p) { assert(p); }
int msleep(void *channel,lck_mtx_t *lock,int flags,const char *name,const struct timespec *wait)
{
 (void)channel;(void)flags;(void)name;(void)wait;
 pthread_mutex_unlock(lock);
 if (queued_worker) {void (*fn)(void *,wait_result_t)=queued_worker;queued_worker=NULL;fn(NULL,0);}
 pthread_mutex_lock(lock);return EWOULDBLOCK;
}
user_ssize_t uio_resid(struct uio *uio) { return (user_ssize_t)(uio->capacity-uio->offset); }
int uiomove(caddr_t source,int bytes,struct uio *uio)
{
 if (copy_failure) return EFAULT;
 assert(bytes>0 && (size_t)bytes<=uio->capacity-uio->offset);
 memcpy(uio->bytes+uio->offset,source,(size_t)bytes);uio->offset+=(size_t)bytes;return 0;
}
bool nezha_camera_profile_open(const uint8_t *bytes,size_t capacity,struct NezhaCameraProfile *p)
{ assert(bytes && capacity==NezhaCameraProfileCapacity);p->vendor=3;p->width=4096;p->height=3072;return true; }
void nezha_camera_native_capture_request_stop(void) { ++stops; }
bool nezha_camera_native_capture(const struct NezhaCameraNativeCaptureRequest *request,struct NezhaCameraNativeCaptureResult *result)
{
 ++runs;assert(request->consumer && request->consumer->continuing(NULL));
 assert(request->AF_readonly_probe==expected_AF_readonly);
 assert(request->controls && request->controls->take && request->controls->complete && request->controls->readback);
 struct NezhaCameraOvxControls control;
 uint32_t sampled_after=1;
 if(request->controls->take(NULL,&control)) {
  assert(control_requested && control.exposure_lines==320 && control.frame_lines==4000 && control.gain_q8==544);
  ++controls_taken;
  struct NezhaCameraOvxControlResult applied={.applied_writes=14};
  request->controls->complete(NULL,NezhaCciOk,&applied,1);
  struct NezhaCameraOvxControlReadback readback={.exposure_lines=320,.frame_lines=4000,.analog_gain_q8=544,.digital_gain_q10=1024,.bytes_read=11};
  request->controls->readback(NULL,&readback,1);
 } else {
  struct NezhaCameraOvxControlReadback readback={.exposure_lines=160,.frame_lines=2246,
   .analog_gain_q8=256,.digital_gain_q10=1024,.bytes_read=11};
  request->controls->readback(NULL,&readback,sampled_after);
 }
 assert(!request->controls->take(NULL,&control));
 struct NezhaCameraRaw10Layout layout={4096,3072,5120,5120,15728640};
 uint8_t *raw=malloc(layout.frame_bytes);assert(raw);memset(raw,0x5a,layout.frame_bytes);
 assert(request->consumer->deliver(NULL,&layout,raw,layout.frame_bytes,75,1));
 assert(request->consumer->deliver(NULL,&layout,raw,layout.frame_bytes,76,2));
 assert(request->consumer->deliver(NULL,&layout,raw,layout.frame_bytes,77,3));free(raw);
 result->cleaned=true;return !stream_failure;
}
int main(int argc,char **argv)
{
 uint8_t profile=0,database=0;
 expected_AF_readonly=argc==2 && !strcmp(argv[1],"AF-readonly");
 assert(nezha_camera_native_device_init(&profile,NezhaCameraProfileCapacity,&database,0x20000,expected_AF_readonly));
 assert(methods && !runs);
 struct vnode own={1,VT_DEVFS,makedev(1,0)},other={1,VT_DEVFS,makedev(2,0)};
 assert(nezha_camera_native_node(&own) && !nezha_camera_native_node(&other) && !nezha_camera_native_node(NULL));
 own.character=0;assert(!nezha_camera_native_node(&own));own.character=1;
 own.tag=0;assert(!nezha_camera_native_node(&own));own.tag=VT_DEVFS;
 struct hakios_camera_command control={.command=HAKIOS_CAMERA_EXPOSURE,.a=320,.b=544,.c=4000};
 assert(methods->d_ioctl(0,NEZHA_CAMERA_EXPOSURE_IOCTL,(caddr_t)&control,0,NULL)==ENXIO);
 assert(methods->d_open(0,0,0,NULL)==0);
 assert(methods->d_open(0,0,0,NULL)==EBUSY);
 assert(methods->d_ioctl(0,NEZHA_CAMERA_EXPOSURE_IOCTL+1,(caddr_t)&control,0,NULL)==ENOTTY);
 control.bytes=1;assert(methods->d_ioctl(0,NEZHA_CAMERA_EXPOSURE_IOCTL,(caddr_t)&control,0,NULL)==EINVAL);
 control.bytes=0;control.a=0;assert(methods->d_ioctl(0,NEZHA_CAMERA_EXPOSURE_IOCTL,(caddr_t)&control,0,NULL)==EINVAL);
 control.a=320;
 assert(methods->d_ioctl(0,NEZHA_CAMERA_EXPOSURE_IOCTL,(caddr_t)&control,0,NULL)==0);
 control_requested=true;
 assert(controls_taken==0); // ioctl queues; no CCI or worker execution
 assert(methods->d_ioctl(0,NEZHA_CAMERA_EXPOSURE_IOCTL,(caddr_t)&control,0,NULL)==EBUSY);
 uint8_t *buffer=malloc(sizeof(struct hakios_camera_frame)+15728640);assert(buffer);
 struct uio uio={buffer,sizeof(struct hakios_camera_frame)+15728640,0};
 assert(methods->d_read(0,&uio,IO_NDELAY)==EAGAIN && runs==0);
 assert(methods->d_read(0,&uio,0)==0 && !uio_resid(&uio) && runs==1);
 struct hakios_camera_frame *h=(void *)buffer;
 assert(!memcmp(h->magic,"RWFRAME1",8) && h->sequence==3 && h->time==77 && h->width==4096 && h->stride==5120);
 assert(h->exposure_lines==320 && h->gain==544 && h->line_ns==14841 && h->frame_lines==4000 &&
  h->gain_unity==256 && h->frame_lines_min==2246 && h->margin==10 && h->reserved[4]==1);
 for (unsigned i=0;i<15728640;++i) assert(buffer[sizeof(*h)+i]==0x5a);
 assert(methods->d_close(0,0,0,NULL)==0 && stops==1 && controls_taken==1);
 assert(methods->d_open(0,0,0,NULL)==0);
 copy_failure=true;uio.offset=0;
 assert(methods->d_read(0,&uio,0)==EFAULT);copy_failure=false;
 assert(methods->d_close(0,0,0,NULL)==0);
 assert(methods->d_open(0,0,0,NULL)==0);
 const unsigned prior_runs=runs;
 assert(methods->d_close(0,0,0,NULL)==0 && runs==prior_runs); /* close before worker acquired hardware */
 stream_failure=true;
 assert(methods->d_open(0,0,0,NULL)==0);uio.offset=0;
 assert(methods->d_read(0,&uio,0)==0); /* queued completed frame remains valid before terminal fault */
 assert(methods->d_close(0,0,0,NULL)==0);
 assert(methods->d_open(0,0,0,NULL)==EIO);
 free(buffer);
 puts("Native camera device: full header/RAW read, exclusive opens, bounded worker close, copy fault and no failed-session restart passed");
}
