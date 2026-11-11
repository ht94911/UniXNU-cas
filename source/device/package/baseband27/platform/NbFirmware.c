/* SPDX-License-Identifier: BSD-2-Clause */
#include "NbFirmware.h"
#include <string.h>
static int piece_read(void *v,unsigned i,uint64_t off,void *dst,size_t n) {
    NbFirmware *f=v;
    if(i>=f->split.count || off>UINT64_MAX-f->piece_offset[i]) return NB_ARGUMENT;
    return nb_archive_read(&f->piece[i],f->piece_offset[i]+off,dst,n);
}
int nb_firmware_read(void *v,uint64_t off,void *dst,size_t n) {
    NbFirmware *f=v;if(!f)return NB_ARGUMENT;
    if(f->joined)return nb_archive_read(&f->piece[0],off,dst,n);
    return qcom_mdt_split_read(&f->split,piece_read,f,off,dst,n);
}
int nb_firmware_open(NbFirmware *f,NbArchive *a,const char *base,void *headers,size_t cap) {
    char name[NB_ARCHIVE_NAME];size_t n;
    if(!f || !a || !base || !headers || cap<4096 || (n=strlen(base))>sizeof name-5)return NB_ARGUMENT;
    *f=(NbFirmware){0};memcpy(name,base,n);memcpy(name+n,".mbn",5);
    if(!nb_archive_find(a,name,&f->piece[0])) {f->joined=1;f->bytes=f->piece[0].bytes;return NB_OK;}
    NbArchiveFile mdt;memcpy(name+n,".mdt",5);
    if(nb_archive_find(a,name,&mdt))return NB_MISSING;
    size_t take=mdt.bytes<cap?(size_t)mdt.bytes:cap;
    if(nb_archive_read(&mdt,0,headers,take) || qcom_mdt_split(&f->split,headers,take))return NB_WIRE;
    if(qcom_mdt_parse(&f->image,f->split.size,nb_archive_read,&mdt))return NB_WIRE;
    for(unsigned i=0;i<f->split.count;i++) {
        uint64_t off=f->split.piece[i].offset,size=f->split.piece[i].size;
        if(!size)continue;
        /* Reject ambiguous piece ranges before handing them to the virtual reader. */
        for(unsigned j=0;j<i;j++) {
            uint64_t p=f->split.piece[j].offset,s=f->split.piece[j].size;
            if(s && off<p+s && p<off+size)return NB_WIRE;
        }
        name[n]='.';name[n+1]='b';name[n+2]=(char)('0'+i/10);name[n+3]=(char)('0'+i%10);name[n+4]=0;
        if(!nb_archive_find(a,name,&f->piece[i])) {
            if(f->piece[i].bytes!=size)return NB_WIRE;
        } else {
            /* Metadata-only .mdt puts its hash immediately after the headers. */
            if((int)i==f->image.hash && mdt.bytes==f->split.piece[0].size+size)off=f->split.piece[0].size;
            if(off>mdt.bytes || size>mdt.bytes-off)return NB_MISSING;
            f->piece[i]=mdt;f->piece_offset[i]=off;
        }
    }
    f->bytes=f->split.size;return NB_OK;
}
