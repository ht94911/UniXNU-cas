/* SPDX-License-Identifier: MIT */
/* Reuse the CF/CM/CV objects and real renderer fixture from the provider
 * test. The backend itself uses the real dispatch timer/serial queue. */
#define main camera_provider_fixture_main
#include "camera-fig-provider.c"
#undef main
#include "NezhaCameraDeviceBackend.h"
#include "upstream/hakios/hakios.camera.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <unistd.h>

static atomic_uint native_reads,native_closes,native_opens,native_controls;
static atomic_bool bad_frame,has_frame;
static dispatch_semaphore_t got_frame;
int backend_test_access(const char *path,int mode)
{
 assert(!strcmp(path,"/dev/nezha-camera") && (mode==R_OK || mode==F_OK));
 if(mode==R_OK){errno=EACCES;return -1;}
 return 0;
}
int backend_test_open(const char *path,int flags,...)
{ assert(!strcmp(path,"/dev/nezha-camera") && (flags&O_NONBLOCK));atomic_fetch_add(&native_opens,1);return 42; }
int backend_test_close(int fd)
{ assert(fd==42);atomic_fetch_add(&native_closes,1);return 0; }
int backend_test_ioctl(int fd,unsigned long command,...)
{ assert(fd==42 && command==0x80184e31UL);atomic_fetch_add(&native_controls,1);return 0; }
ssize_t backend_test_read(int fd,void *bytes,size_t count)
{
 assert(fd==42 && count==sizeof(struct hakios_camera_frame)+15728640);
 atomic_fetch_add(&native_reads,1);
 if (!atomic_exchange(&has_frame,false)) { errno=EAGAIN;return -1; }
 struct hakios_camera_frame h={0};
 memcpy(h.magic,HAKIOS_CAMERA_MAGIC,8);h.header_bytes=sizeof(h);h.width=4096;h.height=3072;
 h.stride=5120;h.format=1;h.bayer=3;h.black=64;h.sequence=1;h.time=77;
 memcpy(bytes,&h,sizeof(h));memset((uint8_t *)bytes+sizeof(h),255,15728640);
 return atomic_load(&bad_frame) ? (ssize_t)count-1:(ssize_t)count;
}
static void wait_for_close(unsigned target)
{
 for (unsigned i=0;i<200 && atomic_load(&native_closes)<target;++i) usleep(1000);
 assert(atomic_load(&native_closes)==target);
}
int main(void)
{
 assert(nezha_camera_device_backend_bind()==0);
 NezhaFigObject device=NULL;
 assert(nezha_camera_device_create(NULL,NULL,&device)==0 && device);
 const struct NezhaFigDeviceVTable *dt=device->class;
 CFArrayRef streams=NULL;
 assert(dt->base->copy_property(device,CFSTR("StreamArray"),NULL,&streams)==0);
 NezhaFigObject stream=(NezhaFigObject)CFArrayGetValueAtIndex(streams,0);
 const struct NezhaFigStreamVTable *st=stream->class;
 NezhaFigObject other=NULL;
 assert(nezha_camera_device_create(NULL,NULL,&other)==-12783 && !other);
 struct __CVPixelBufferPool *pool=new(sizeof(*pool),Pool);
 got_frame=dispatch_semaphore_create(0);
 NezhaCameraPixelHandler callback=^(CVPixelBufferRef pixel,CMTime time) {
  assert_pixel(pixel,time);
  assert(st->stream->stop(stream)==0); /* end waits reader queue, not a synchronous render callback */
  dispatch_semaphore_signal(got_frame);
 };
 CFDictionaryRef configs=nest("PrimaryScaler","BufferPool",pool);
 CFDictionaryRef handlers=nest("PrimaryScaler","PixelBufferHandler",callback);
 CFDictionaryRef enabled=one("PrimaryScaler",kCFBooleanTrue);
 assert(st->base->set_property(stream,CFSTR("VideoOutputConfigurations"),configs)==0);
 assert(st->base->set_property(stream,CFSTR("VideoOutputHandlers"),handlers)==0);
 assert(st->base->set_property(stream,CFSTR("VideoOutputsEnabled"),enabled)==0);
 assert(st->stream->start(stream)==0);
 assert(dt->base->invalidate(device)==0);
 CFRelease(device);device=NULL;
 assert(atomic_load(&native_opens)==1 && atomic_load(&native_closes)==0);
 atomic_store(&has_frame,true);
 assert(dispatch_semaphore_wait(got_frame,dispatch_time(DISPATCH_TIME_NOW,2000000000))==0);
 wait_for_close(1);
 assert(atomic_load(&native_opens)==1 && !live_pixels);
 /* Start can race the release of the just-delivered raw; retry the
  * user operation once after serial completion, no native error replay. */
 int status=st->stream->start(stream);
 if (status) { usleep(10000);status=st->stream->start(stream); }
 assert(status==0);
 atomic_store(&bad_frame,true);atomic_store(&has_frame,true);
 wait_for_close(2);
 assert(st->stream->stop(stream)==0);
 CFRelease(configs);CFRelease(handlers);CFRelease(enabled);CFRelease(pool);
 CFRelease(streams);dispatch_release(got_frame);
 assert(!live_objects && !live_pixels);
 assert(atomic_load(&native_reads)>=2 && atomic_load(&native_opens)==2 && !atomic_load(&native_controls));
 puts("Camera native reader: timer-to-render delivery, reentrant Stop, exclusive stream, bad native frame closure and finalization passed");
}
