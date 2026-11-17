/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioInputPack.h"
#include <string.h>
static uint32_t u32(const uint8_t *p) {uint32_t n;memcpy(&n,p,4);return n;}
static uint64_t u64(const uint8_t *p) {uint64_t n;memcpy(&n,p,8);return n;}
static int name_length(const char *s) {
    for (unsigned i=0;i<48;++i) {
        unsigned char c=(unsigned char)s[i];
        if (!c) return i ? (int)i:-1;
        if (!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
              (c>='0' && c<='9') || c=='_' || c=='-' || c=='.')) return -1;
    }
    return -1;
}
int na_input_pack(NaInputPack *out,const void *data,uint64_t bytes) {
    if (!out || !data || bytes<4096 || bytes>0x8000000 || (bytes&4095)) return NA_ARGUMENT;
    *out=(NaInputPack){0};const uint8_t *p=data;
    uint32_t count=u32(p+8);
    if (memcmp(p,"PANDFW01",8) || !count || count>63 || u32(p+12)) return NA_WIRE;
    uint64_t previous=4096;
    for (unsigned i=0;i<count;++i) {
        const uint8_t *e=p+16+64*i;int n=name_length((const char *)e);
        if (n<0 || !strcmp((const char *)e,".") || !strcmp((const char *)e,"..")) return NA_WIRE;
        for (unsigned k=(unsigned)n+1;k<48;++k) if (e[k]) return NA_WIRE;
        uint64_t offset=u64(e+48),size=u64(e+56);
        if ((offset&4095) || offset<previous || offset>bytes || !size || size>bytes-offset) return NA_WIRE;
        previous=(offset+size+4095)&~UINT64_C(4095);
        for (unsigned j=0;j<i;++j) if (!strcmp((const char *)e,(const char *)(p+16+64*j))) return NA_WIRE;
    }
    for (unsigned i=16+64*count;i<4096;++i) if (p[i]) return NA_WIRE;
    *out=(NaInputPack){p,bytes,count};return NA_OK;
}
int na_input_find(const NaInputPack *pack,const char *name,NaFirmwarePiece *piece) {
    if (!pack || !pack->data || !pack->count || !name || !piece || name_length(name)<0) return NA_ARGUMENT;
    *piece=(NaFirmwarePiece){0};
    for (unsigned i=0;i<pack->count;++i) {
        const uint8_t *e=pack->data+16+64*i;
        if (!strcmp((const char *)e,name)) {*piece=(NaFirmwarePiece){pack->data+u64(e+48),u64(e+56)};return NA_OK;}
    }
    return NA_MISSING;
}
int na_input_firmware(const NaInputPack *pack,unsigned kind,NaFirmwarePiece *pieces,
                      uint32_t capacity,uint32_t *count,NaFirmwarePiece *mdt) {
    if (kind>1 || !pieces || capacity<QCOM_MDT_MAX_SEGMENTS || !count || !mdt) return NA_ARGUMENT;
    *count=0;memset(pieces,0,QCOM_MDT_MAX_SEGMENTS*sizeof *pieces);
    int r=na_input_find(pack,kind ? "adsp_dtb.mdt":"adsp.mdt",mdt);if (r) return r;
    QcomMdtSplit split;
    if (mdt->bytes>65536 || qcom_mdt_split(&split,mdt->data,(size_t)mdt->bytes)) return NA_WIRE;
    for (unsigned i=0;i<split.count;++i) if (split.piece[i].size) {
        if (i>99) return NA_WIRE;
        char name[48]={0};const char *base=kind ? "adsp_dtb.b":"adsp.b";
        size_t n=strlen(base);memcpy(name,base,n);name[n]=(char)('0'+i/10);name[n+1]=(char)('0'+i%10);
        r=na_input_find(pack,name,&pieces[i]);if (r) return r;
        if (pieces[i].bytes!=split.piece[i].size) return NA_WIRE;
    }
    *count=split.count;return NA_OK;
}
