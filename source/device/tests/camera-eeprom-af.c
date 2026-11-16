/* SPDX-License-Identifier: MIT */
#include "NezhaCameraEepromAf.h"
#include <string.h>
#define main camera_cci_baseline_main
#include "camera-cci.c"
#undef main

struct Eeprom {
    struct Model cci;
    uint8_t data[NezhaCameraEepromAfPrefixBytes];
    unsigned at, requests, fail_at;
    enum NezhaCameraCciStatus failure;
};

static uint32_t eeprom_read(void *context,uint32_t offset)
{ return read32(&((struct Eeprom *)context)->cci,offset); }
static void eeprom_write(void *context,uint32_t offset,uint32_t value)
{
    struct Eeprom *s=context;
    if(offset==0x008){
        assert(value==8 && s->at<NezhaCameraEepromAfPrefixBytes);
        const unsigned remaining=NezhaCameraEepromAfPrefixBytes-s->at;
        const unsigned n=remaining<3 ? remaining:3;
        s->cci.expected_slave=0x511;
        s->cci.expected_write=0x29u | (uint32_t)(s->at>>8)<<8 | (uint32_t)(s->at&255)<<16;
        s->cci.expected_read=0xau | n<<4;
        s->cci.fifo_word=0x21;
        for(unsigned i=0;i<n;++i)s->cci.fifo_word|=(uint32_t)s->data[s->at+i]<<(8*(i+1));
        s->cci.data_words=1;s->cci.data_result=M1ReadDone|M1DataReport;
        if(s->requests==s->fail_at)s->cci.data_result=s->failure==NezhaCciNack ? M1DataNack:0;
        ++s->requests;s->at+=n;
    }
    write32(&s->cci,offset,value);
}
static void eeprom_wait(void *context,unsigned ms)
{ wait_ms(&((struct Eeprom *)context)->cci,ms); }

static struct Eeprom sample(void)
{
    struct Eeprom s={.cci=initial(),.fail_at=99};
    s.data[0x27]=1;s.data[0x2d]=0xe;s.data[0x2e]=0x38;s.data[0x31]=2;
    unsigned sum=0;for(unsigned i=0x28;i<=0x42;++i)sum+=s.data[i];s.data[0x43]=(uint8_t)(sum%255u+1u);
    return s;
}

int main(void)
{
    struct Eeprom s=sample();
    const struct NezhaCameraRegisterIo io={&s,eeprom_read,eeprom_write,eeprom_wait};
    struct NezhaCameraEepromAfRead result;
    assert(nezha_camera_eeprom_read_af(&io,&result)==NezhaCciOk);
    assert(result.bytes_read==68 && s.requests==23 && result.AF_checksum_valid);
    assert(result.macro_dac==3640 && result.infinity_dac==512 && !memcmp(result.bytes,s.data,68));
    for(unsigned type=0;type<2;++type)for(unsigned index=0;index<23;++index){
        s=sample();s.fail_at=index;s.failure=type ? NezhaCciTimeout:NezhaCciNack;
        assert(nezha_camera_eeprom_read_af(&io,&result)==s.failure);
        assert(result.bytes_read==index*3 && s.requests==index+1);
        assert(!result.AF_checksum_valid && result.transfer.needs_recovery);
    }
    s=sample();s.data[0x2d]^=1;
    assert(nezha_camera_eeprom_read_af(&io,&result)==NezhaCciFifo && !result.AF_checksum_valid);
    s=sample();s.data[0x27]=0;
    assert(nezha_camera_eeprom_read_af(&io,&result)==NezhaCciFifo && !result.AF_checksum_valid);
    s=sample();s.data[0x43]^=1;
    assert(nezha_camera_eeprom_read_af(&io,&result)==NezhaCciFifo && !result.AF_checksum_valid);
    s=sample();s.cci.queued[0]=1;
    assert(nezha_camera_eeprom_read_af(&io,&result)==NezhaCciBusy && !s.cci.writes && !result.bytes_read);
    assert(nezha_camera_eeprom_read_af(NULL,&result)==NezhaCciInvalid);
    assert(nezha_camera_eeprom_read_af(&io,NULL)==NezhaCciInvalid);
    puts("EEPROM AF read68bytes,46 first-error exits, checksum and original CCI route passed");
    return 0;
}
