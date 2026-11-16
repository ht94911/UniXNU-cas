/* SPDX-License-Identifier: BSD-2-Clause */
#include "../../package/baseband27/platform/NbArchive.h"
#include "../../package/baseband27/platform/NbFirmware.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
static uint8_t *image;static size_t bytes;
static int raw(void *v,uint64_t off,void *to,size_t n) {
    (void)v;if(off>bytes || n>bytes-off)return -1;memcpy(to,image+off,n);return 0;
}
static int hash(void *v,QcomMdtRead read,void *ctx,uint64_t off,uint64_t n,uint8_t out[32]) {
    (void)v;EVP_MD_CTX *md=EVP_MD_CTX_new();assert(md);assert(EVP_DigestInit_ex(md,EVP_sha256(),0));
    uint8_t buf[1024];while(n) {size_t take=n>sizeof buf ? sizeof buf:(size_t)n;if(read(ctx,off,buf,take)){EVP_MD_CTX_free(md);return -1;}
        assert(EVP_DigestUpdate(md,buf,take));off+=take;n-=take;}
    unsigned len=32;assert(EVP_DigestFinal_ex(md,out,&len) && len==32);EVP_MD_CTX_free(md);return 0;
}
static void put64(uint8_t *p,uint64_t n) {for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(8*i));}
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>0);
    bytes=(size_t)size;rewind(f);image=malloc(bytes);assert(image && fread(image,1,bytes,f)==bytes);fclose(f);
    uint8_t digest[32],index[256*32],cross[8300],headers[4096];NbArchive a;NbArchiveFile af;
    assert(!hash(0,raw,0,0,bytes,digest));
    assert(!nb_archive_open(&a,raw,0,bytes,digest,hash,0,index,sizeof index));
    NbFirmware fw;
    assert(!nb_firmware_open(&fw,&a,"fw32",headers,sizeof headers) && !fw.image.elf64);
    assert(!nb_firmware_read(&fw,8192,cross,17));for(unsigned i=0;i<17;i++)assert(cross[i]==0x72);
    assert(!nb_firmware_read(&fw,4096,cross,32));for(unsigned i=0;i<32;i++)assert(cross[i]==0x35);
    assert(!nb_firmware_open(&fw,&a,"fw64",headers,sizeof headers) && fw.image.elf64);
    assert(!nb_firmware_read(&fw,8192,cross,17));for(unsigned i=0;i<17;i++)assert(cross[i]==0x72);
    assert(nb_firmware_open(&fw,&a,"missing",headers,sizeof headers)==NB_MISSING);
    assert(!nb_archive_find(&a,"tree/sample.bin",&af) && af.bytes==9000);
    assert(!nb_archive_verify(&a,"tree/sample.bin"));
    assert(!nb_archive_read(&af,4088,cross,4100));for(unsigned i=0;i<4100;i++)assert(cross[i]==(uint8_t)((4088+i)%251));
    assert(nb_archive_read(&af,8999,cross,2)<0);assert(nb_archive_find(&a,"../sample.bin",&af)<0);
    image[bytes-1]^=1;assert(nb_archive_open(&a,raw,0,bytes,digest,hash,0,index,sizeof index)<0);image[bytes-1]^=1;
    uint8_t header[64+512];memcpy(header,image,sizeof header);
    /* Re-sign each malformed fixture: these test structural validation, not only its digest gate. */
    image[64]='.';image[65]='.';image[66]='/';assert(!hash(0,raw,0,0,bytes,digest));
    assert(nb_archive_open(&a,raw,0,bytes,digest,hash,0,index,sizeof index)<0);memcpy(image,header,sizeof header);
    put64(image+64+192,0);assert(!hash(0,raw,0,0,bytes,digest));
    assert(nb_archive_open(&a,raw,0,bytes,digest,hash,0,index,sizeof index)<0);memcpy(image,header,sizeof header);
    image[48]=1;assert(!hash(0,raw,0,0,bytes,digest));
    assert(nb_archive_open(&a,raw,0,bytes,digest,hash,0,index,sizeof index)<0);memcpy(image,header,sizeof header);
    assert(!hash(0,raw,0,0,bytes,digest));assert(nb_archive_open(&a,raw,0,bytes,digest,hash,0,index,1)<0);
    free(image);puts("PASS archive: digest, index, bounds, file reads");return 0;
}
