/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Public fixtures contain synthetic bytes only. Optional private stdin is
 * kept in memory and returned through the pipe; it is never saved here. */
#include "qbt_firmware.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPACITY 1024u
#define STOCK_BYTES 741u
#define STOCK_BAUD_OFFSET 93u
static void put16(uint8_t *p,uint16_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put_tlv(uint8_t *p,unsigned type,size_t bytes) {
    p[0]=(uint8_t)type;p[1]=(uint8_t)bytes;p[2]=(uint8_t)(bytes>>8);p[3]=(uint8_t)(bytes>>16);
}
static size_t tag(uint8_t *p,size_t at,uint16_t id,uint16_t bytes) {
    memset(p+at,0x6d,12);put16(p+at,id);put16(p+at+2,bytes);
    for(size_t i=0;i<bytes;i++)p[at+12+i]=(uint8_t)(id+i*17u+0xa4u);
    return at+12+bytes;
}
static size_t type2(uint8_t *p) {
    /* Captured PERI shape: 741 bytes, six-byte tag17 payload at92. These
     * constructed payload/header values are not the original firmware. */
    size_t at=tag(p,4,2,6);at=tag(p,at,0x201,46);
    assert(at==80);at=tag(p,at,17,6);p[STOCK_BAUD_OFFSET]=0xfd;
    at=tag(p,at,27,5);assert(at==115);at=tag(p,at,0x202,614);
    assert(at==STOCK_BYTES);put_tlv(p,2,at-4);return at;
}
static size_t type4(uint8_t *p) {
    size_t at=4+type2(p+4);
    /* A separate FM set with a tag17-shaped record must stay untouched. */
    put_tlv(p+at,3,18);size_t fm=at+4;at=tag(p,fm,17,6);p[fm+13]=0xfd;
    put_tlv(p,4,at-4);return at;
}
static void unchanged_error(uint8_t *data,size_t size,int error) {
    uint8_t before[CAPACITY];memcpy(before,data,sizeof(before));
    assert(qbt_firmware_prepare_peri_nvm(data,size,3200000)==error);
    assert(!memcmp(before,data,sizeof(before)));
}
static void prepared_copy(unsigned container) {
    uint8_t original[CAPACITY],snapshot[CAPACITY],copy[CAPACITY],before[CAPACITY];
    memset(original,0xa7,sizeof(original));size_t size=container==2?type2(original):type4(original);
    memcpy(snapshot,original,sizeof(original));memcpy(copy,original,sizeof(copy));memcpy(before,copy,sizeof(copy));
    assert(!qbt_firmware_validate(QBT_FW_NVM,original,size));
    assert(!qbt_firmware_prepare_peri_nvm(copy,size,3200000));
    size_t baud=STOCK_BAUD_OFFSET+(container==4?4:0);
    for(size_t i=0;i<sizeof(copy);i++)assert(copy[i]==(i==baud?0x11:before[i]));
    assert(!memcmp(original,snapshot,sizeof(original)));
    assert(!qbt_firmware_validate(QBT_FW_NVM,copy,size));
    memcpy(before,copy,sizeof(copy));assert(!qbt_firmware_prepare_peri_nvm(copy,size,3200000));
    assert(!memcmp(copy,before,sizeof(copy))); /* already prepared: zero changes */
    assert(qbt_firmware_prepare_peri_nvm(copy,size,115200)==-EOPNOTSUPP);
    assert(!memcmp(copy,before,sizeof(copy)));
}
static void malformed_type2(void) {
    uint8_t data[CAPACITY];memset(data,0x87,sizeof(data));size_t size=type2(data);
    for(size_t n=0;n<size;n++)unchanged_error(data,n,-EINVAL);
    for(unsigned fault=0;fault<7;fault++) {
        memset(data,0x87,sizeof(data));size=type2(data);
        switch(fault) {
        case 0:put_tlv(data,2,size-3);break;
        case 1:put16(data+115+2,615);break; /* malformed tail after tag17 */
        case 2:put16(data+80+2,2);break;
        case 3:put16(data+4+2,5);break;
        case 4:put16(data+98+2,0);break;
        case 5:put16(data+80,18);break; /* mandatory first-set HCI tag missing */
        case 6:data[0]=1;break;
        }
        assert(qbt_firmware_validate(QBT_FW_NVM,data,size)==-EINVAL);
        unchanged_error(data,size,-EINVAL);
    }
    memset(data,0x87,sizeof(data));type2(data);unchanged_error(data,4u*1024*1024+1u,-EINVAL);
    put_tlv(data,2,0);unchanged_error(data,4,-EINVAL);
    assert(qbt_firmware_prepare_peri_nvm(NULL,STOCK_BYTES,3200000)==-EINVAL);
}
static void malformed_type4(void) {
    uint8_t data[CAPACITY];memset(data,0x87,sizeof(data));size_t size=type4(data);
    for(size_t n=0;n<size;n++)unchanged_error(data,n,-EINVAL);
    for(unsigned fault=0;fault<5;fault++) {
        memset(data,0x87,sizeof(data));size=type4(data);
        switch(fault) {
        case 0:put_tlv(data+4+STOCK_BYTES,3,19);break;
        case 1:data[4]=3;break;
        case 2:put_tlv(data+4,2,STOCK_BYTES-5);break;
        case 3:put_tlv(data+size,2,11);memset(data+size+4,0x87,11);size+=15;put_tlv(data,4,size-4);break;
        case 4:data[size++]=0x87;put_tlv(data,4,size-4);break;
        }
        assert(qbt_firmware_validate(QBT_FW_NVM,data,size)==-EINVAL);
        unchanged_error(data,size,-EINVAL);
    }
    put_tlv(data,4,0);unchanged_error(data,4,-EINVAL);
}
static void later_type2_set(void) {
    uint8_t original[CAPACITY],before[CAPACITY],copy[CAPACITY];memset(original,0x87,sizeof(original));
    size_t size=type4(original),next=size;put_tlv(original+next,2,13);size=tag(original,next+4,0x203,1);
    put_tlv(original,4,size-4);memcpy(before,original,sizeof(before));memcpy(copy,original,sizeof(copy));
    assert(!qbt_firmware_validate(QBT_FW_NVM,original,size));
    assert(!qbt_firmware_prepare_peri_nvm(copy,size,3200000));
    for(size_t i=0;i<sizeof(copy);i++)assert(copy[i]==(i==STOCK_BAUD_OFFSET+4?0x11:before[i]));
    assert(!memcmp(original,before,sizeof(original)));
    /* The same walker handles a later BT tag17 while preserving FM bytes. */
    size=type4(original);next=size;put_tlv(original+next,2,18);size=tag(original,next+4,17,6);
    original[next+17]=0xfd;put_tlv(original,4,size-4);
    memcpy(before,original,sizeof(before));memcpy(copy,original,sizeof(copy));
    assert(!qbt_firmware_prepare_peri_nvm(copy,size,3200000));
    for(size_t i=0;i<sizeof(copy);i++)assert(copy[i]==((i==STOCK_BAUD_OFFSET+4 || i==next+17)?0x11:before[i]));
    assert(!memcmp(original,before,sizeof(original)));
}
static int prepare_stdin(void) {
    const size_t capacity=4u*1024*1024;
    uint8_t *original=malloc(capacity+1);assert(original);
    size_t size=fread(original,1,capacity+1,stdin);assert(size<=capacity && !ferror(stdin));
    assert(size==STOCK_BYTES && original[STOCK_BAUD_OFFSET]==0xfd);
    uint8_t *copy=malloc(size);assert(copy);memcpy(copy,original,size);
    assert(!qbt_firmware_prepare_peri_nvm(copy,size,3200000));
    for(size_t i=0;i<size;i++)assert(copy[i]==(i==STOCK_BAUD_OFFSET?0x11:original[i]));
    assert(fwrite(copy,1,size,stdout)==size);free(copy);free(original);return 0;
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--prepare-stdin"))return prepare_stdin();
    assert(argc==1);prepared_copy(2);prepared_copy(4);malformed_type2();malformed_type4();later_type2_set();
    puts("PASS: synthetic type2/type4 PERI NVM preparation, full validation before writes, exact tag17 baud edits, immutable originals and unchanged FM/vendor data; no controller/device operations.");
    return 0;
}
