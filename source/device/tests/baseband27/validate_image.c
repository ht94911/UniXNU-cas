/* SPDX-License-Identifier: BSD-2-Clause */
#include "../../package/baseband27/platform/NbFirmware.h"
#include "../../package/baseband27/platform/NbPas.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int read_image(void *ctx,uint64_t off,void *out,size_t n){FILE *f=ctx;return fseeko(f,(off_t)off,SEEK_SET)||fread(out,1,n,f)!=n?-1:0;}
static int hash(void *v,QcomMdtRead read,void *ctx,uint64_t off,uint64_t bytes,uint8_t out[32]){
    (void)v;EVP_MD_CTX *h=EVP_MD_CTX_new();if(!h)return -1;int ok=EVP_DigestInit_ex(h,EVP_sha256(),NULL);uint8_t buf[65536];
    while(ok&&bytes){size_t n=bytes>sizeof buf?sizeof buf:(size_t)bytes;ok=!read(ctx,off,buf,n)&&EVP_DigestUpdate(h,buf,n);off+=n;bytes-=n;}
    unsigned size=32;if(ok)ok=EVP_DigestFinal_ex(h,out,&size);EVP_MD_CTX_free(h);return ok?0:-1;
}
static int sync_only(void *v,void *p,uint64_t a,uint64_t n){(void)v;(void)p;(void)a;(void)n;return 0;}
static int no_secure_call(void *v,uint32_t c,uint32_t id,uint64_t a,uint64_t b){(void)v;(void)c;(void)id;(void)a;(void)b;abort();}
int main(int argc,char **argv){
    if(argc!=3||strlen(argv[2])!=64)return 2;
    uint8_t expected[32];for(unsigned i=0;i<32;i++){unsigned x;if(sscanf(argv[2]+i*2,"%2x",&x)!=1)return 2;expected[i]=(uint8_t)x;}
    FILE *f=fopen(argv[1],"rb");if(!f)return 2;if(fseeko(f,0,SEEK_END)){fclose(f);return 2;}uint64_t bytes=(uint64_t)ftello(f);
    NbArchive a;uint8_t *index=malloc(NB_ARCHIVE_ENTRY*NB_ARCHIVE_FILES),*headers=malloc(65536);int result=1;
    void *metadata=NULL;if(posix_memalign(&metadata,4096,65536)||!index||!headers)goto done;
    if(nb_archive_open(&a,read_image,f,bytes,expected,hash,NULL,index,NB_ARCHIVE_ENTRY*NB_ARCHIVE_FILES))goto done;
    const char *names[]={"ipa_fws","modem","modem_dtb"};const uint32_t ids[]={15,4,0x26};
    const NbRange ranges[]={{0x99590000,0xa000},{0x8aa00000,0xe100000},{0x99500000,0x80000}};
    NbPasOps ops={NULL,no_secure_call,sync_only};
    for(unsigned i=0;i<3;i++){
        NbFirmware source;NbPas loader;void *mapped=malloc((size_t)ranges[i].bytes);if(!mapped)goto done;
        int r=nb_firmware_open(&source,&a,names[i],headers,65536);
        if(!r)r=nb_pas_begin(&loader,ids[i],ranges[i],mapped,source.bytes,nb_firmware_read,&source,metadata,0xb0000000,65536,&ops,0,120000);
        free(mapped);if(r){fprintf(stderr,"input %u layout refused: %d\n",i,r);goto done;}
    }
    result=0;puts("PASS local image: digest, index and all three input layouts; no secure call executed");
done:
    free(metadata);free(index);free(headers);fclose(f);return result;
}
