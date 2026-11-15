/* SPDX-License-Identifier: MIT */
#include "NezhaCameraDeviceBackend.h"
#include "NezhaCameraFactory.h"
#include "upstream/hakios/hakios.camera.h"
#include "../NezhaCameraControl.h"
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { RawBytes=4096*3072*5/4, FrameBytes=sizeof(struct hakios_camera_frame)+RawBytes };
enum { Unavailable=-12783 };
static struct {
    dispatch_source_t timer;
    dispatch_queue_t queue;
    uint8_t *frame;
    NezhaFigObject stream;
    int fd;
    unsigned busy;
    uintptr_t reservation;
    uint64_t last_sequence,last_time;
    int stopped_error;
    bool bound;
} reader={.fd=-1};

static bool ready(void *context)
{
    static unsigned reported;
    (void)context;
    /* HakiOS's vendor discovery tests presence. access(R_OK) uses real
     * credentials, while the later open is the actual capture permission
     * check. Registering a device must not begin a hardware reservation. */
    const int exists=access("/dev/nezha-camera",F_OK);
    const int presence_error=exists ? errno:0;
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4) {
        const int readable=access("/dev/nezha-camera",R_OK);
        const int read_error=readable ? errno:0;
        struct stat info={0};
        const int metadata=stat("/dev/nezha-camera",&info);
        const int metadata_error=metadata ? errno:0;
        fprintf(stderr,"nezha-camera: native node exists=%u errno=%d readable=%u errno=%d stat=%d uid=%u euid=%u owner=%u mode=%o\n",
            (unsigned)(exists==0),presence_error,(unsigned)(readable==0),read_error,metadata_error,
            (unsigned)getuid(),(unsigned)geteuid(),(unsigned)info.st_uid,(unsigned)info.st_mode);
    }
    return exists==0;
}

static void released(void *context)
{
    (void)context;
    __atomic_store_n(&reader.busy,0u,__ATOMIC_RELEASE);
}

static void frame_ready(void *context)
{
    const char *failure_step="read";
    int status=0;
    if ((uintptr_t)context!=reader.reservation || reader.fd<0 || !reader.stream ||
        __atomic_exchange_n(&reader.busy,1u,__ATOMIC_ACQ_REL)) return;
    const ssize_t bytes=read(reader.fd,reader.frame,FrameBytes);
    const int read_error=bytes<0 ? errno:0;
    if (bytes<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) { released(NULL);return; }
    struct hakios_camera_frame header;
    if (bytes!=FrameBytes) goto failed;
    failure_step="header";
    memcpy(&header,reader.frame,sizeof(header));
    if (memcmp(header.magic,HAKIOS_CAMERA_MAGIC,8) || header.header_bytes!=sizeof(header) ||
        header.width!=4096 || header.height!=3072 || header.stride!=5120 || header.format!=1 ||
        header.bayer!=3 || header.black!=64 || header.sequence<=reader.last_sequence ||
        header.time<=reader.last_time) goto failed;
    reader.last_sequence=header.sequence;reader.last_time=header.time;
    const struct NezhaCameraPreviewFrame geometry={4096,3072,5120,3,64,1023};
    failure_step="metadata";
    const char *names[]={"FrameSequence","ExposureLines","LineNanoseconds","AnalogGainQ8",
        "GainUnity","GainMaximum","FrameLengthLines","MinimumFrameLengthLines",
        "MaximumFrameLengthLines","ExposureMargin"};
    const uint32_t controls[]={header.exposure_lines,header.line_ns,header.gain,header.gain_unity,
        header.gain_max,header.frame_lines,header.frame_lines_min,header.frame_lines_max,header.margin};
    CFMutableDictionaryRef metadata=CFDictionaryCreateMutable(NULL,0,

        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if (!metadata) goto failed;
    const bool has_frame_controls=header.exposure_lines && header.line_ns && header.gain &&
        header.gain_unity && header.gain_max && header.frame_lines && header.frame_lines_min &&
        header.frame_lines_max && header.margin;
    for (unsigned i=0;i<(has_frame_controls ? 10u:1u);++i) {
        CFStringRef key=CFStringCreateWithCString(NULL,names[i],kCFStringEncodingUTF8);
        double value=i ? (double)controls[i-1]:(double)header.sequence;
        CFNumberRef number=CFNumberCreate(NULL,kCFNumberDoubleType,&value);
        if (!key || !number) {
            if (key) CFRelease(key);
            if (number) CFRelease(number);
            CFRelease(metadata);goto failed;
        }
        CFDictionarySetValue(metadata,key,number);
        CFRelease(key);CFRelease(number);
    }
    failure_step="enqueue";
    status=nezha_camera_fig_stream_enqueue(reader.stream,&geometry,reader.frame+sizeof(header),

        RawBytes,header.time,metadata,released,NULL);
    CFRelease(metadata);
    if (!status) return;
failed:
    {
        static unsigned reports;
        if (__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<12)
            fprintf(stderr,"nezha-camera: reader failure step=%s bytes=%lld expected=%u errno=%d status=%d sequence=%llu\n",
                failure_step,(long long)bytes,(unsigned)FrameBytes,read_error,status,
                (unsigned long long)reader.last_sequence);
    }
    /* One bad native frame ends this reservation. No template frame or
     * automatic reopen may conceal a native driver or copied-frame error. */
    dispatch_source_cancel(reader.timer);
    if (close(reader.fd)) reader.stopped_error=Unavailable;
    reader.fd=-1;released(NULL);
}

static int begin(void *context)
{
    static unsigned reported;
    (void)context;
    __block int result=0;
    dispatch_sync(reader.queue,^{
        if (reader.fd>=0) return;
        if (!reader.stream || reader.timer || reader.stopped_error ||
            __atomic_load_n(&reader.busy,__ATOMIC_ACQUIRE)) {
            result=Unavailable;return;
        }
        reader.fd=open("/dev/nezha-camera",O_RDONLY|O_NONBLOCK|O_CLOEXEC);
        if (reader.fd<0) {
            const int error=errno;
            if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4)
                fprintf(stderr,"nezha-camera: native open status=%d errno=%d\n",Unavailable,error);
            result=Unavailable;return;
        }
        if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4)
            fprintf(stderr,"nezha-camera: native open status=0 errno=0\n");
        reader.timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,reader.queue);
        if (!reader.timer) { close(reader.fd);reader.fd=-1;result=Unavailable;return; }
        ++reader.reservation;
        reader.last_sequence=reader.last_time=0;
        dispatch_set_context(reader.timer,(void *)reader.reservation);
        dispatch_source_set_event_handler_f(reader.timer,frame_ready);
        dispatch_source_set_timer(reader.timer,DISPATCH_TIME_NOW,16*1000000,2*1000000);
        dispatch_resume(reader.timer);
    });
    return result;
}

static int end(void *context)
{
    (void)context;
    dispatch_sync(reader.queue,^{
        static unsigned reports;
        if (__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<12)
            fprintf(stderr,"nezha-camera: reader end fd=%d timer=%u sequence=%llu error=%d\n",
                reader.fd,(unsigned)(reader.timer!=NULL),(unsigned long long)reader.last_sequence,reader.stopped_error);
        if (reader.timer) {
            dispatch_source_cancel(reader.timer);
            dispatch_release(reader.timer);reader.timer=NULL;
        }
        if (reader.fd>=0 && close(reader.fd)) reader.stopped_error=Unavailable;
        reader.fd=-1;
    });
    return reader.stopped_error;
}

static int exposure(void *context,const struct hakios_camera_command *command)
{
    (void)context;
    if (!command || command->command!=HAKIOS_CAMERA_EXPOSURE || command->bytes || command->d)
        return Unavailable;
    __block int result=Unavailable;
    dispatch_sync(reader.queue,^{
        if (reader.fd<0 || reader.stopped_error || !reader.timer) return;
        if (!ioctl(reader.fd,NEZHA_CAMERA_EXPOSURE_IOCTL,command)) result=0;
        else if (errno==EBUSY) result=-12781;
    });
    return result;
}

static int bind_stream(void *context,NezhaFigObject stream)
{
    (void)context;
    if (!stream) (void)end(NULL);
    __block int result=0;
    dispatch_sync(reader.queue,^{
        if (stream && reader.stream && reader.stream!=stream) { result=Unavailable;return; }
        reader.stream=stream;
    });
    return result;
}

int nezha_camera_device_backend_bind(void)
{
    if (reader.bound) return 0;
    reader.frame=malloc(FrameBytes);
    if (!reader.frame) return Unavailable;
    reader.queue=dispatch_queue_create("org.unixnu.nezha.camera.reader",DISPATCH_QUEUE_SERIAL);
    if (!reader.queue) { free(reader.frame);reader.frame=NULL;return Unavailable; }
    const struct NezhaCameraFigBackend backend={.begin=begin,.end=end,.bind_stream=bind_stream,.exposure=exposure};
    const int status=nezha_camera_factory_bind(&backend,ready);
    if (status) { dispatch_release(reader.queue);reader.queue=NULL;free(reader.frame);reader.frame=NULL;return status; }
    reader.bound=true;
    return 0;
}
