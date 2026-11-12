/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* QAIC layouts: Qualcomm apps_std_skel.c/apps_remotectl_skel.c d247519. */
#include "NezhaSensorFiles.h"
#include <string.h>
#define BAD 0x0e
#define UNSUPPORTED 0x14
#define NO_HANDLE 0x2c
#define FULL 0x2d
#define READONLY 0x31
#define MISSING 39
enum {ModuleStd=1,ModuleRegistry};
static uint32_t g32(const void *v) {const uint8_t *p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t g64(const void *v) {const uint8_t *p=v;return g32(p)|(uint64_t)g32(p+4)<<32;}
static void p32(void *v,uint32_t x) {uint8_t *p=v;for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(x>>(8*i));}
static void p64(void *v,uint64_t x) {uint8_t *p=v;p32(p,(uint32_t)x);p32(p+4,(uint32_t)(x>>32));}
static const char *string(QcomRpcArg a,uint32_t n) {
    if (!a.data || !n || n>a.bytes || n>256 || ((const char *)a.data)[n-1] || memchr(a.data,0,n-1)) return 0;
    return a.data;
}
static int shape(uint32_t scalar,unsigned in,unsigned out,QcomRpcArg *a,uint32_t count,unsigned prim,unsigned result) {
    return (scalar&0x00ffffff)==(in<<16|out<<8) && count==in+out && a &&
           a[0].data && a[0].bytes>=prim && (!out || (a[in].data && a[in].bytes>=result));
}
int nezha_sensor_files_prepare(NezhaSensorFiles *s,const NezhaSensorFileOps *ops) {
    if (!s || s->ops.file || !ops || !ops->file || !ops->property || !ops->directory) return BAD;
    memset(s,0,sizeof *s);s->ops=*ops;s->next_module=0x100;return 0;
}
static void observed(NezhaSensorFiles *s,const char *kind,const char *name,uint32_t method,int result) {
    ++s->requests;if (s->ops.observe) s->ops.observe(s->ops.context,kind,name,method,result);
}
static int module_kind(const char *name) {
    /* Plain module and generated file URI use the same actual skeleton.
     * Full string matching prevents loading an unrelated AP module. */
    if (!strcmp(name,"apps_std") || !strcmp(name,"libapps_std_skel.so") ||
        !strcmp(name,"file:///libapps_std_skel.so?apps_std_skel_invoke&_modver=1.0")) return ModuleStd;
    if (!strcmp(name,"sns_registry") || !strcmp(name,"libsns_registry_skel.so") ||
        !strcmp(name,"file:///libsns_registry_skel.so?sns_registry_skel_invoke&_modver=1.0")) return ModuleRegistry;
    return 0;
}
static int remotectl(NezhaSensorFiles *s,uint32_t sc,QcomRpcArg *a,uint32_t count) {
    unsigned method=sc>>24;if (method==0) {
        if (!shape(sc,2,2,a,count,8,8)) return BAD;
        uint32_t error_bytes=g32((uint8_t *)a[0].data+4);
        const char *name=string(a[1],g32(a[0].data));
        if (!name || error_bytes>a[3].bytes || (error_bytes && !a[3].data)) return BAD;
        if (error_bytes) memset(a[3].data,0,error_bytes);
        int kind=module_kind(name),result=0;uint32_t handle=0;
        if (!kind) result=MISSING;
        else {
            unsigned i=0;while (i<NEZHA_SENSOR_MODULE_HANDLES && s->modules[i].used) ++i;
            if (i==NEZHA_SENSOR_MODULE_HANDLES || s->next_module==UINT32_MAX) result=FULL;
            else {handle=s->next_module++;s->modules[i].handle=handle;s->modules[i].kind=(uint32_t)kind;s->modules[i].used=1;}
        }
        p32(a[2].data,handle);p32((uint8_t *)a[2].data+4,(uint32_t)result);
        observed(s,"module",name,method,result);return 0; /* loader error is a rout value */
    }
    if (method==1) {
        if (!shape(sc,1,2,a,count,8,4) || g32((uint8_t *)a[0].data+4)>a[2].bytes) return BAD;
        uint32_t handle=g32(a[0].data);int found=0;
        for (unsigned i=0;i<NEZHA_SENSOR_MODULE_HANDLES;++i) if (s->modules[i].used && s->modules[i].handle==handle) {s->modules[i].used=0;found=1;break;}
        p32(a[1].data,found ? 0:NO_HANDLE);return 0;
    }
    return UNSUPPORTED;
}
static int property(NezhaSensorFiles *s,uint32_t sc,QcomRpcArg *a,uint32_t count) {
    if (sc!=0x00020100 || !shape(sc,2,1,a,count,8,1)) return BAD;
    const char *name=string(a[1],g32(a[0].data)),*value=0;uint32_t capacity=g32((uint8_t *)a[0].data+4);
    if (!name || capacity<=91 || capacity>a[2].bytes) return BAD;
    memset(a[2].data,0,capacity);int result=s->ops.property(s->ops.context,name,&value);
    if (!result && value) {size_t n=strlen(value);if (n>=capacity) n=capacity-1;memcpy(a[2].data,value,n);}
    observed(s,"property",name,0,result);return result;
}
static int file_open(NezhaSensorFiles *s,const char *path,const char *mode,uint32_t *handle) {
    if (strcmp(mode,"r") && strcmp(mode,"rb")) return READONLY;
    unsigned i=0;while (i<NEZHA_SENSOR_FILE_HANDLES && s->files[i].used) ++i;
    if (i==NEZHA_SENSOR_FILE_HANDLES) return FULL;
    const uint8_t *data=0;uint32_t bytes=0;int result=s->ops.file(s->ops.context,path,&data,&bytes);
    if (result) return result;
    if (bytes && !data) return BAD;
    s->files[i].data=data;s->files[i].bytes=bytes;s->files[i].used=1;s->files[i].position=0;s->files[i].eof=0;
    *handle=i+1;return 0;
}
static int std(NezhaSensorFiles *s,uint32_t sc,QcomRpcArg *a,uint32_t count) {
    uint32_t method=sc>>24;
    if (method==0) {
        if (!shape(sc,3,1,a,count,8,4)) return BAD;
        const char *path=string(a[1],g32(a[0].data)),*mode=string(a[2],g32((uint8_t *)a[0].data+4));
        if (!path || !mode) return BAD;
        uint32_t handle=0;int r=file_open(s,path,mode,&handle);
        if (!r) p32(a[3].data,handle);
        observed(s,"file",path,method,r);return r;
    }
    if (method==16) {
        if (!shape(sc,2,2,a,count,8,4)) return BAD;
        const char *name=string(a[1],g32(a[0].data)),*value=0;uint32_t cap=g32((uint8_t *)a[0].data+4);
        if (!name || cap>a[3].bytes || (cap && !a[3].data)) return BAD;
        int r=s->ops.property(s->ops.context,name,&value);if (r) return r;
        uint32_t n=value ? (uint32_t)strlen(value)+1:1;p32(a[2].data,n);
        if (cap) {memset(a[3].data,0,cap);if (value) memcpy(a[3].data,value,n<=cap ? n-1:cap-1);}return 0;
    }
    if (method==26) {
        if (!shape(sc,2,1,a,count,4,8)) return BAD;
        const char *path=string(a[1],g32(a[0].data));if (!path) return BAD;
        const char *name=0;int end=0,r=s->ops.directory(s->ops.context,path,0,&name,&end);
        if (r) {observed(s,"directory",path,method,r);return r;}
        unsigned i=0;while (i<8 && s->directories[i].used) ++i;if (i==8) return FULL;
        memcpy(s->directories[i].path,path,strlen(path)+1);s->directories[i].used=1;s->directories[i].position=0;
        p64(a[2].data,i+1);observed(s,"directory",path,method,0);return 0;
    }
    if (method==27 || method==28) {
        if (!shape(sc,1,method==28 ? 1:0,a,count,8,264)) return BAD;
        uint64_t id=g64(a[0].data);if (!id || id>8 || !s->directories[id-1].used) return NO_HANDLE;
        if (method==27) {s->directories[id-1].used=0;return 0;}
        const char *name=0;int end=0;unsigned index=s->directories[id-1].position;
        int r=s->ops.directory(s->ops.context,s->directories[id-1].path,index,&name,&end);if (r) return r;
        memset(a[1].data,0,264);p32((uint8_t *)a[1].data+260,end ? 1:0);
        if (!end && name) {size_t n=strlen(name);if (n>254) return BAD;p32(a[1].data,index+1);memcpy((uint8_t *)a[1].data+4,name,n);++s->directories[id-1].position;}
        return 0;
    }
    if (method==31) {
        if (!shape(sc,2,1,a,count,8,96) || g32(a[0].data)!=31) return UNSUPPORTED;
        const char *path=string(a[1],g32((uint8_t *)a[0].data+4));if (!path) return BAD;
        const uint8_t *data=0;uint32_t bytes=0;int r=s->ops.file(s->ops.context,path,&data,&bytes);int directory=0;
        if (r) {const char *name=0;int end=0;r=s->ops.directory(s->ops.context,path,0,&name,&end);directory=!r;}
        if (r) {observed(s,"stat",path,31,r);return r;}
        memset(a[2].data,0,96);p64(a[2].data,96);p32((uint8_t *)a[2].data+24,directory ? 0040555:0100444);
        p32((uint8_t *)a[2].data+28,1);p64((uint8_t *)a[2].data+40,bytes);observed(s,"stat",path,31,0);return 0;
    }
    if (method==5 || method==17 || method==18 || method==24 || method==29 || method==30) return READONLY;
    if (!a || !count || !a[0].data || a[0].bytes<4) return BAD;
    uint32_t id=g32(a[0].data);if (!id || id>NEZHA_SENSOR_FILE_HANDLES || !s->files[id-1].used) return NO_HANDLE;
    unsigned i=id-1;
    if (method==3) {if (!shape(sc,1,0,a,count,4,0)) return BAD;s->files[i].used=0;return 0;}
    if (method==4) {
        if (!shape(sc,1,2,a,count,8,8)) return BAD;
        uint32_t cap=g32((uint8_t *)a[0].data+4);
        if (cap>a[2].bytes || (cap && !a[2].data)) return BAD;
        uint32_t left=s->files[i].bytes-s->files[i].position,n=cap<left ? cap:left;
        if (n) memcpy(a[2].data,s->files[i].data+s->files[i].position,n);
        s->files[i].position+=n;s->files[i].eof=n<cap;
        p32(a[1].data,n);p32((uint8_t *)a[1].data+4,s->files[i].eof);return 0;
    }
    if (method==9) {
        if (!shape(sc,1,0,a,count,12,0)) return BAD;
        int32_t offset=(int32_t)g32((uint8_t *)a[0].data+4);uint32_t whence=g32((uint8_t *)a[0].data+8);
        int64_t position=(whence==0 ? 0:whence==1 ? s->files[i].position:s->files[i].bytes)+(int64_t)offset;
        if (whence>2 || position<0 || position>s->files[i].bytes) return BAD;
        s->files[i].position=(uint32_t)position;s->files[i].eof=0;return 0;
    }
    if (method==8 || method==12 || method==13) {
        if (!shape(sc,1,1,a,count,4,4)) return BAD;
        p32(a[1].data,method==8 ? s->files[i].position:method==12 ? s->files[i].eof:0);return 0;
    }
    if (method==10) {if (!shape(sc,1,1,a,count,4,8)) return BAD;p64(a[1].data,s->files[i].bytes);return 0;}
    if (method==2 || method==11 || method==14 || method==23) {
        if (!shape(sc,1,0,a,count,4,0)) return BAD;
        if (method==11) s->files[i].position=0;
        s->files[i].eof=0;return 0;
    }
    return UNSUPPORTED;
}
int32_t nezha_sensor_files_invoke(void *v,uint32_t handle,uint32_t scalar,QcomRpcArg *args,uint32_t count) {
    NezhaSensorFiles *s=v;if (!s || !s->ops.file || (count && !args)) return BAD;
    if (handle==0) return remotectl(s,scalar,args,count);
    for (unsigned i=0;i<NEZHA_SENSOR_MODULE_HANDLES;++i) if (s->modules[i].used && s->modules[i].handle==handle)
        return s->modules[i].kind==ModuleStd ? std(s,scalar,args,count):property(s,scalar,args,count);
    observed(s,"unknown-handle","",scalar>>24,NO_HANDLE);return NO_HANDLE;
}
