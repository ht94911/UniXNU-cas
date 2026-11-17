/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "qbt_firmware.h"
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif
static uint16_t u16(const uint8_t *p) {return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
static uint32_t u32(const uint8_t *p) {return (uint32_t)u16(p)|(uint32_t)u16(p+2)<<16;}
static uint64_t u64(const uint8_t *p) {return (uint64_t)u32(p)|(uint64_t)u32(p+4)<<32;}
static int range(size_t size,uint64_t offset,uint64_t count) {return offset<=size && count<=size-offset;}
static int nvm_tags(const uint8_t *p,size_t size,int require_hci,uint8_t *copy) {
    size_t at=0;int hci=0;
    while (at<size) {
        if (size-at<12) return -EINVAL;
        uint16_t id=u16(p+at),length=u16(p+at+2);
        if (length>size-at-12) return -EINVAL;
        if ((id==2 && length!=6) || (id==17 && length<3) || (id==27 && length<1)) return -EINVAL;
        if(id==17) {
            hci=1;
            if(copy)copy[at+13]=0x11;
        }
        at+=12+length;
    }
    return require_hci && !hci?-EINVAL:0;
}
static int nvm_sets(const uint8_t *p,size_t size,uint8_t *copy) {
    uint32_t head=u32(p);unsigned type=head&255;size_t bytes=head>>8;
    if (type==2) return nvm_tags(p+4,bytes,1,copy?copy+4:NULL);
    if (type!=4) return -EINVAL;
    size_t at=4;unsigned first=1;
    while (at<size) {
        if (size-at<4) return -EINVAL;
        head=u32(p+at);type=head&255;bytes=head>>8;
        if (!bytes || bytes>size-at-4 || (first && type!=2)) return -EINVAL;
        if (type==2) {int e=nvm_tags(p+at+4,bytes,first,copy?copy+at+4:NULL);if(e)return e;}
        /* Preserve vendor/FM sets in the original verified input. */
        at+=4+bytes;first=0;
    }
    return first?-EINVAL:0;
}
int qbt_firmware_validate(enum qbt_firmware_kind kind,const uint8_t *p,size_t size) {
    if (!p || !size || size>4u*1024*1024) return -EINVAL;
    if (kind==QBT_FW_ELF_PATCH) {
        if (size<52 || p[0]!=0x7f || p[1]!='E' || p[2]!='L' || p[3]!='F' || p[5]!=1 || p[6]!=1) return -EINVAL;
        uint64_t phoff;uint16_t ehsize,phsize,count;
        if (p[4]==1) {phoff=u32(p+28);ehsize=u16(p+40);phsize=u16(p+42);count=u16(p+44);if(ehsize!=52 || phsize!=32)return -EINVAL;}
        else if (p[4]==2) {if(size<64)return -EINVAL;phoff=u64(p+32);ehsize=u16(p+52);phsize=u16(p+54);count=u16(p+56);if(ehsize!=64 || phsize!=56)return -EINVAL;}
        else return -EINVAL;
        if (!count || count>256 || phoff<ehsize || !range(size,phoff,(uint64_t)phsize*count)) return -EINVAL;
        unsigned loads=0;
        for (unsigned i=0;i<count;i++) {
            const uint8_t *ph=p+(size_t)phoff+i*phsize;
            uint64_t offset=p[4]==1?u32(ph+4):u64(ph+8);
            uint64_t files=p[4]==1?u32(ph+16):u64(ph+32);
            uint64_t memory=p[4]==1?u32(ph+20):u64(ph+40);
            if (!range(size,offset,files)) return -EINVAL;
            if (u32(ph)==1) {if(files>memory)return -EINVAL;loads++;}
        }
        return loads?0:-EINVAL;
    }
    if (size<4) return -EINVAL;
    uint32_t head=u32(p);unsigned type=head&255;size_t bytes=head>>8;
    if (bytes!=size-4) return -EINVAL;
    if (kind==QBT_FW_TLV_PATCH) {
        if (type!=1 || bytes<24 || p[14]>3) return -EINVAL;
        /* Linux downloads the original complete TLV. Header fields describe
         * its patch contents; signed tails need not equal the code length. */
        return 0;
    }
    if (kind!=QBT_FW_NVM) return -EINVAL;
    return nvm_sets(p,size,NULL);
}
int qbt_firmware_prepare_peri_nvm(uint8_t *copy,size_t size,uint32_t baud) {
    if (baud!=3200000) return -EOPNOTSUPP;
    int e=qbt_firmware_validate(QBT_FW_NVM,copy,size);
    if(e)return e;
    /* N0 HAL SHA d6d6c883ecb3f904fa3907d85f67b47396ee7e9d25231e7016754742d4519844:
     * ReadTlvInfo130aac gets the actual max-baud code and130ad4 stores it at
     * tag+13 (12-byte header plus payload byte1). The complete read-only
     * validation above succeeds before this second walk changes any byte. */
    return nvm_sets(copy,size,copy);
}
