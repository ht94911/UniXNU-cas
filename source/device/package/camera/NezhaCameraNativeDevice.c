/* SPDX-License-Identifier: MIT */
#include "NezhaCameraNativeDevice.h"
#include "NezhaCameraNativeCapture.h"
#include "userland/upstream/hakios/hakios.camera.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>

/* HakiOS f128f1d camdev_read supplies the header + CPU RAW and bounded
 * reader sleep sequence. Hardware, DMA/cache ownership and Stop use this
 * port's continuous controller. A reading user buffer is never DMA-backed.
 * One staging frame replaces older unconsumed frames without waiting for
 * a client while the hardware owns its two RAW buffers. */
static LCK_GRP_DECLARE(camera_group,"nezha-camera");
static LCK_MTX_DECLARE(camera_mutex,&camera_group);
enum { FrameBytes=4096*3072*5/4, MobileUid=501, MobileGid=501 };
static uint8_t *staging;
static struct hakios_camera_frame header;
static struct NezhaCameraNativeCaptureRequest request;
static struct NezhaCameraNativeCaptureResult result;
static bool made,opened,worker_active,reading,frame_ready;
static int failure;
static uint64_t last_read;
extern void thread_terminate_self(void);
extern volatile uint32_t native_source_storage_boot_state;
extern volatile unsigned nezha27_usb_reset_owner_ready;
extern volatile int nezha_touch_stage;

static uint64_t elapsed_ms(uint64_t start)
{
    uint64_t ns=0;
    absolutetime_to_nanoseconds(mach_absolute_time()-start,&ns);
    return ns/1000000;
}

static bool continuing(void *context)
{
    (void)context;
    lck_mtx_lock(&camera_mutex);
    const bool on=opened && !failure && (!header.sequence || elapsed_ms(last_read)<3000);
    lck_mtx_unlock(&camera_mutex);
    return on;
}

static bool checkpoint(unsigned step,unsigned a,unsigned b,unsigned c)
{
    (void)step;(void)a;(void)b;(void)c;
    return continuing(NULL);
}

static bool delivered(void *context,const struct NezhaCameraRaw10Layout *layout,
    const uint8_t *frame,size_t bytes,uint64_t ticks,uint32_t sequence)
{
    (void)context;
    if (!frame || bytes!=FrameBytes || !ticks || layout->width!=4096 ||
        layout->height!=3072 || layout->stride!=5120) return false;
    lck_mtx_lock(&camera_mutex);
    if (opened && !reading) {
        memcpy(staging,frame,bytes);
        bzero(&header,sizeof(header));
        memcpy(header.magic,HAKIOS_CAMERA_MAGIC,8);
        header.header_bytes=sizeof(header);
        header.width=4096;header.height=3072;header.stride=5120;
        header.format=1;header.bayer=3;header.black=64;
        header.sequence=sequence;header.time=ticks;header.focus=HAKIOS_CAMERA_NO_FOCUS;
        frame_ready=true;
    }
    lck_mtx_unlock(&camera_mutex);
    wakeup(&frame_ready);
    return true;
}

static void worker(void *argument,wait_result_t wait_result)
{
    (void)argument;(void)wait_result;
    thread_set_thread_name(current_thread(),"nezha-camera-frames");
    /* Publishing the node precedes camera-daemon discovery. The first
     * open waits for the same storage/USB/touch prerequisites as C2. */
    bool eligible=false;
    for (unsigned attempt=0;attempt<2400 && continuing(NULL);++attempt) {
        if (native_source_storage_boot_state==4 && nezha27_usb_reset_owner_ready && nezha_touch_stage==7) {
            eligible=true;break;
        }
        if (native_source_storage_boot_state==5 || native_source_storage_boot_state==6) break;
        IOSleep(50);
    }
    const bool okay=!continuing(NULL) || (eligible && nezha_camera_native_capture(&request,&result));
    lck_mtx_lock(&camera_mutex);
    if (!okay) failure=EIO;
    else if (opened) failure=EAGAIN; /* idle client must close before reopening */
    worker_active=false;
    lck_mtx_unlock(&camera_mutex);
    wakeup(&worker_active);wakeup(&frame_ready);
    thread_terminate_self();
}

static int device_open(dev_t dev,int flags,int type,struct proc *process)
{
    (void)dev;(void)flags;(void)type;(void)process;
    lck_mtx_lock(&camera_mutex);
    if (!made || opened || worker_active || reading || failure==EIO) {
        const int error=failure==EIO ? EIO:EBUSY;
        lck_mtx_unlock(&camera_mutex);return error;
    }
    opened=worker_active=true;frame_ready=false;failure=0;
    bzero(&header,sizeof(header));last_read=mach_absolute_time();
    lck_mtx_unlock(&camera_mutex);
    thread_t thread=THREAD_NULL;
    const kern_return_t started=kernel_thread_start(worker,NULL,&thread);
    if (started!=KERN_SUCCESS) {
        lck_mtx_lock(&camera_mutex);opened=worker_active=false;lck_mtx_unlock(&camera_mutex);
        return EIO;
    }
    thread_deallocate(thread);
    return 0;
}

static int device_close(dev_t dev,int flags,int type,struct proc *process)
{
    (void)dev;(void)flags;(void)type;(void)process;
    lck_mtx_lock(&camera_mutex);opened=false;frame_ready=false;lck_mtx_unlock(&camera_mutex);
    nezha_camera_native_capture_request_stop();wakeup(&frame_ready);
    const uint64_t start=mach_absolute_time();
    lck_mtx_lock(&camera_mutex);
    while (worker_active && elapsed_ms(start)<5000) {
        struct timespec wait={0,10000000};
        (void)msleep(&worker_active,&camera_mutex,PZERO,"nezha-camera-stop",&wait);
    }
    const int error=worker_active ? EBUSY:0;
    lck_mtx_unlock(&camera_mutex);return error;
}

static int device_read(dev_t dev,struct uio *uio,int flags)
{
    (void)dev;
    if (uio_resid(uio)!=(user_ssize_t)(sizeof(header)+FrameBytes)) return EINVAL;
    const uint64_t start=mach_absolute_time();
    lck_mtx_lock(&camera_mutex);last_read=start;
    while (!frame_ready) {
        if (!opened || failure || !worker_active) {
            const int error=failure ? failure:ENXIO;
            lck_mtx_unlock(&camera_mutex);return error;
        }
        if (flags & IO_NDELAY) { lck_mtx_unlock(&camera_mutex);return EAGAIN; }
        if (elapsed_ms(start)>=15000) { lck_mtx_unlock(&camera_mutex);return EAGAIN; }
        struct timespec wait={0,100000000};
        const int error=msleep(&frame_ready,&camera_mutex,PZERO|PCATCH,"nezha-camera-frame",&wait);
        if (error && error!=EWOULDBLOCK) { lck_mtx_unlock(&camera_mutex);return error; }
    }
    if (reading) { lck_mtx_unlock(&camera_mutex);return EBUSY; }
    reading=true;frame_ready=false;
    const struct hakios_camera_frame complete=header;
    lck_mtx_unlock(&camera_mutex);
    int error=uiomove((caddr_t)&complete,(int)sizeof(complete),uio);
    if (!error) error=uiomove((caddr_t)staging,FrameBytes,uio);
    lck_mtx_lock(&camera_mutex);reading=false;last_read=mach_absolute_time();lck_mtx_unlock(&camera_mutex);
    return error;
}

static const struct cdevsw device_methods={
    .d_open=device_open,.d_close=device_close,.d_read=device_read,.d_write=eno_rdwrt,
    .d_ioctl=eno_ioctl,.d_stop=eno_stop,.d_reset=eno_reset,.d_select=eno_select,
    .d_mmap=eno_mmap,.d_strategy=eno_strat,.d_type=0
};

bool nezha_camera_native_device_init(const uint8_t *profile,size_t profile_bytes,
    const uint8_t *database,size_t database_bytes)
{
    if (made || !profile || profile_bytes!=NezhaCameraProfileCapacity || !database || database_bytes!=0x20000)
        return false;
    struct NezhaCameraProfile parsed;
    if (!nezha_camera_profile_open(profile,profile_bytes,&parsed) ||
        parsed.width!=4096 || parsed.height!=3072) return false;
    staging=(uint8_t *)IOMallocData(FrameBytes);
    if (!staging) return false;
    static const struct NezhaCameraFrameConsumer consumer={NULL,continuing,delivered};
    request=(struct NezhaCameraNativeCaptureRequest){.profile=profile,.profile_bytes=profile_bytes,
        .isolated_sensor_votes=true,.slot=80,.bank=8,.asid=0xfc00,.cci_queue_words=NezhaCameraCciCanoeQueue0Words,
        .command_database=database,.database_bytes=database_bytes,.checkpoint=checkpoint,.consumer=&consumer};
    const int major=cdevsw_add(-1,&device_methods);
    made=major>=0 && devfs_make_node(makedev(major,0),DEVFS_CHAR,MobileUid,MobileGid,0600,"nezha-camera")!=NULL;
    if (!made) { IOFreeData(staging,FrameBytes);staging=NULL; }
    return made;
}
