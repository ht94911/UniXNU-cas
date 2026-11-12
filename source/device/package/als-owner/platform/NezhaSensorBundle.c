/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaSensorBundle.h"
#include <string.h>
static int octal(const uint8_t *p,unsigned bytes,uint32_t *result) {
    uint32_t n=0;unsigned i=0;while (i<bytes && p[i]==' ') ++i;
    for (;i<bytes && p[i] && p[i]!=' ';++i) {if (p[i]<'0' || p[i]>'7' || n>UINT32_MAX/8) return -1;n=n*8+p[i]-'0';}
    for (;i<bytes;++i) if (p[i] && p[i]!=' ') return -1;
    *result=n;return 0;
}
static int clean_path(const char *path) {
    if (!path || !*path || *path=='/') return 0;
    for (const char *p=path;*p;++p) {
        if ((unsigned char)*p<32 || (unsigned char)*p>126 || *p=='\\') return 0;
        if ((p==path || p[-1]=='/') && (*p=='/' || (*p=='.' && (!p[1] || p[1]=='/' || (p[1]=='.' && (!p[2] || p[2]=='/')))))) return 0;
    }
    return 1;
}
int nezha_sensor_bundle_prepare(NezhaSensorBundle *b,const uint8_t *data,uint32_t bytes) {
    if (!b || b->count || !data || bytes<1024 || bytes>NEZHA_SENSOR_BUNDLE_CAPACITY || (bytes&511)) return -1;
    uint32_t at=0;
    while (at<=bytes-512 && data[at]) {
        const uint8_t *h=data+at;uint32_t n=0,checksum=0,sum=0;
        if (memcmp(h+257,"ustar",5) || !memchr(h,0,100) || !memchr(h+345,0,155) ||
            octal(h+124,12,&n) || octal(h+148,8,&checksum) || b->count==NEZHA_SENSOR_BUNDLE_ENTRIES) return -1;
        for (unsigned i=0;i<512;++i) sum+=i>=148 && i<156 ? ' ':h[i];
        if (sum!=checksum || n>bytes-at-512 || (h[156] && h[156]!='0' && h[156]!='5')) return -1;
        NezhaSensorBundleEntry *e=&b->entry[b->count];size_t prefix=strlen((const char *)h+345),name=strlen((const char *)h);
        if (prefix+name+(prefix ? 1:0)>=sizeof e->path) return -1;
        if (prefix) {memcpy(e->path,h+345,prefix);e->path[prefix++]='/';}
        memcpy(e->path+prefix,h,name+1);size_t length=strlen(e->path);
        if (h[156]=='5' && length && e->path[length-1]=='/') e->path[length-1]=0;
        if (!clean_path(e->path)) return -1;
        for (unsigned i=0;i<b->count;++i) if (!strcmp(e->path,b->entry[i].path)) return -1;
        e->data=h+512;e->bytes=n;e->directory=h[156]=='5';++b->count;
        at+=512+((n+511)&~511u);
    }
    if (!b->count || at>bytes-1024) return -1;
    for (;at<bytes;++at) if (data[at]) return -1;
    return 0;
}
static const char *relative(const char *path) {
    while (*path=='/') ++path;
    return path;
}
int nezha_sensor_bundle_file(void *v,const char *path,const uint8_t **data,uint32_t *bytes) {
    NezhaSensorBundle *b=v;if (!b || !path || !data || !bytes) return 0x0e;
    path=relative(path);if (!clean_path(path)) return 0x0e;
    for (unsigned i=0;i<b->count;++i) if (!b->entry[i].directory && !strcmp(path,b->entry[i].path)) {
        *data=b->entry[i].data;*bytes=b->entry[i].bytes;return 0;
    }
    return 39;
}
int nezha_sensor_bundle_property(void *v,const char *name,const char **value) {
    if (!name || !*name || strchr(name,'/') || strlen(name)>128 || !value) return 0x0e;
    char path[256]="properties/";memcpy(path+11,name,strlen(name)+1);
    const uint8_t *data=0;uint32_t bytes=0;int result=nezha_sensor_bundle_file(v,path,&data,&bytes);
    if (result) return result;
    if (!bytes || bytes>4096 || data[bytes-1] || memchr(data,0,bytes-1)) return 0x0e;
    *value=(const char *)data;return 0;
}
int nezha_sensor_bundle_directory(void *v,const char *path,uint32_t index,const char **name,int *end) {
    NezhaSensorBundle *b=v;if (!b || !path || !name || !end) return 0x0e;
    path=relative(path);size_t n=strlen(path);if (n && !clean_path(path)) return 0x0e;
    uint32_t count=0;int exists=0;
    for (unsigned i=0;i<b->count;++i) {
        const char *p=b->entry[i].path;
        if (b->entry[i].directory && !strcmp(p,path)) exists=1;
        if (strncmp(path,p,n) || (n && p[n]!='/')) continue;
        p+=n+(n ? 1:0);if (!*p || strchr(p,'/')) continue;
        exists=1;if (count++==index) {*name=p;*end=0;return 0;}
    }
    if (!exists) return 39;
    *name=0;*end=1;return 0;
}
