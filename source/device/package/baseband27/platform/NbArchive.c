/* SPDX-License-Identifier: BSD-2-Clause */
#include "NbArchive.h"
#include <string.h>
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t u64(const uint8_t *p) { return u32(p)|(uint64_t)u32(p+4)<<32; }
static int fits(uint64_t at,uint64_t n,uint64_t size) { return at<=size && n<=size-at; }
static int zeros(const uint8_t *p,size_t n) { for(size_t i=0;i<n;i++) if(p[i]) return 0; return 1; }
static int valid_name(const uint8_t *p) {
    size_t end=0,start=0;
    while(end<NB_ARCHIVE_NAME && p[end]) {
        if(p[end]<32 || p[end]>126 || p[end]=='\\') return 0;
        end++;
    }
    if(!end || end==NB_ARCHIVE_NAME || !zeros(p+end,NB_ARCHIVE_NAME-end)) return 0;
    for(size_t i=0;i<=end;i++) if(i==end || p[i]=='/') {
        size_t n=i-start;
        if(!n || (n==1 && p[start]=='.') || (n==2 && p[start]=='.' && p[start+1]=='.')) return 0;
        start=i+1;
    }
    return 1;
}
int nb_archive_open(NbArchive *a,QcomMdtRead read,void *ctx,uint64_t bytes,const uint8_t expected[32],
                    NbArchiveHash hash,void *hashctx,void *index,size_t cap) {
    uint8_t h[NB_ARCHIVE_HEADER],digest[32];
    if(!a || !read || !expected || !hash || !index || bytes<NB_ARCHIVE_BLOCK || bytes%NB_ARCHIVE_BLOCK) return NB_ARGUMENT;
    *a=(NbArchive){0};
    if(hash(hashctx,read,ctx,0,bytes,digest) || memcmp(digest,expected,32)) return NB_WIRE;
    if(read(ctx,0,h,sizeof h)) return NB_IO;
    uint32_t count=u32(h+12);uint64_t idx=u64(h+32),data=u64(h+40);
    if(memcmp(h,"NBARCH01",8) || u32(h+8)!=1 || !count || count>NB_ARCHIVE_FILES ||
       u32(h+16)!=NB_ARCHIVE_ENTRY || u32(h+20)!=NB_ARCHIVE_BLOCK || u64(h+24)!=bytes ||
       idx!=(uint64_t)count*NB_ARCHIVE_ENTRY || idx>cap || !fits(NB_ARCHIVE_HEADER,idx,bytes) ||
       data!=((NB_ARCHIVE_HEADER+idx+4095)&~UINT64_C(4095)) || data>bytes || !zeros(h+48,16)) return NB_WIRE;
    if(read(ctx,NB_ARCHIVE_HEADER,index,(size_t)idx)) return NB_IO;
    uint8_t *entries=index;uint64_t end=data;
    for(uint32_t i=0;i<count;i++) {
        uint8_t *e=entries+i*NB_ARCHIVE_ENTRY;uint64_t off=u64(e+192),size=u64(e+200);
        if(!valid_name(e) || (i && strcmp((char *)e-NB_ARCHIVE_ENTRY,(char *)e)>=0) ||
           off<end || off%NB_ARCHIVE_BLOCK || !fits(off,size,bytes) || !zeros(e+240,16)) return NB_WIRE;
        end=off+size;
    }
    *a=(NbArchive){read,ctx,hash,hashctx,index,count,bytes};return NB_OK;
}
static int entry(NbArchive *a,const char *name) {
    if(!a || !a->read || !name) return -1;
    uint32_t lo=0,hi=a->count;
    while(lo<hi) {uint32_t m=lo+(hi-lo)/2;int c=strcmp(name,(char *)a->index+m*NB_ARCHIVE_ENTRY);if(!c)return (int)m;if(c<0)hi=m;else lo=m+1;}
    return -1;
}
int nb_archive_find(NbArchive *a,const char *name,NbArchiveFile *f) {
    if(!f)return NB_ARGUMENT;
    *f=(NbArchiveFile){0};int i=entry(a,name);if(i<0)return NB_IO;
    const uint8_t *e=a->index+(unsigned)i*NB_ARCHIVE_ENTRY;
    *f=(NbArchiveFile){a,u64(e+192),u64(e+200)};return NB_OK;
}
int nb_archive_read(void *v,uint64_t off,void *to,size_t n) {
    NbArchiveFile *f=v;if(!f || !f->archive || (!to && n) || !fits(off,n,f->bytes))return NB_ARGUMENT;
    return f->archive->read(f->archive->context,f->offset+off,to,n) ? NB_IO:NB_OK;
}
int nb_archive_verify(NbArchive *a,const char *name) {
    int i=entry(a,name);if(i<0)return NB_IO;uint8_t digest[32];const uint8_t *e=a->index+(unsigned)i*NB_ARCHIVE_ENTRY;
    return a->hash(a->hash_context,a->read,a->context,u64(e+192),u64(e+200),digest) || memcmp(digest,e+208,32) ? NB_WIRE:NB_OK;
}
