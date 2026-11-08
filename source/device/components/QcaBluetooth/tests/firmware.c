/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "qbt_firmware.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put16(uint8_t *p,unsigned n){p[0]=n;p[1]=n>>8;}
static void put32(uint8_t *p,unsigned n){put16(p,n);put16(p+2,n>>16);}
static void boundaries(void){
    uint8_t elf[100]={0};memcpy(elf,"\x7f" "ELF",4);elf[4]=elf[5]=elf[6]=1;
    put16(elf+16,2);put32(elf+28,52);put16(elf+40,52);put16(elf+42,32);put16(elf+44,1);
    put32(elf+52,1);put32(elf+56,84);put32(elf+68,16);put32(elf+72,16);
    assert(!qbt_firmware_validate(QBT_FW_ELF_PATCH,elf,sizeof(elf)));
    assert(qbt_firmware_validate(QBT_FW_ELF_PATCH,elf,51));
    put32(elf+56,101);assert(qbt_firmware_validate(QBT_FW_ELF_PATCH,elf,sizeof(elf)));put32(elf+56,84);
    put32(elf+72,15);assert(qbt_firmware_validate(QBT_FW_ELF_PATCH,elf,sizeof(elf)));put32(elf+72,16);
    put16(elf+44,257);assert(qbt_firmware_validate(QBT_FW_ELF_PATCH,elf,sizeof(elf)));
    uint8_t nvm[19]={2,15,0,0};put16(nvm+4,17);put16(nvm+6,3);
    assert(!qbt_firmware_validate(QBT_FW_NVM,nvm,sizeof(nvm)));
    put16(nvm+6,4);assert(qbt_firmware_validate(QBT_FW_NVM,nvm,sizeof(nvm)));put16(nvm+6,3);
    put16(nvm+4,2);assert(qbt_firmware_validate(QBT_FW_NVM,nvm,sizeof(nvm)));
    put16(nvm+4,99);assert(qbt_firmware_validate(QBT_FW_NVM,nvm,sizeof(nvm)));
    uint8_t multi[23]={4,19,0,0};memcpy(multi+4,nvm,sizeof(nvm));
    assert(qbt_firmware_validate(QBT_FW_NVM,multi,sizeof(multi)));put16(multi+8,17);
    assert(!qbt_firmware_validate(QBT_FW_NVM,multi,sizeof(multi)));
    multi[4]=3;assert(qbt_firmware_validate(QBT_FW_NVM,multi,sizeof(multi)));
}
int main(int argc,char **argv){
    boundaries();
    for(int i=1;i<argc;i++){
        FILE *f=fopen(argv[i],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0 && n<=4*1024*1024);rewind(f);
        uint8_t *bytes=malloc((size_t)n);assert(bytes && fread(bytes,1,(size_t)n,f)==(size_t)n);assert(!fclose(f));
        enum qbt_firmware_kind kind=bytes[0]==0x7f?QBT_FW_ELF_PATCH:bytes[0]==1?QBT_FW_TLV_PATCH:QBT_FW_NVM;
        int result=qbt_firmware_validate(kind,bytes,(size_t)n);
        if(result)fprintf(stderr,"Firmware format rejected: %s error=%d\n",argv[i],result);
        assert(!result);free(bytes);
    }
    printf("PASS: malformed ELF/TLV/NVM boundaries and %d actual firmware inputs; signatures/radio separate\n",argc-1);
    return 0;
}
