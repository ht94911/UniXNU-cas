/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Only ADC SDAM/GPIO traffic on the actual N0 arbiter layout. */
#include "NezhaAdcSpmiV8.h"
#include <sys/errno.h>
#include <string.h>
#define CORE 0x0c400000ull
#define MAP 0x0c403000ull
#define OWNER 0x0c42a000ull
#define OBSERVER 0x0c4c0000ull
#define WRITER 0x0c900000ull
static int error(int value){return value>0?-value:value;}
static int rd(NpAdcSpmiV8 *s,uint64_t pa,uint32_t *v)
{return error(s->io.read32(s->io.context,pa,v));}
static int wr(NpAdcSpmiV8 *s,uint64_t pa,uint32_t v)
{return error(s->io.write32(s->io.context,pa,v));}
static void transfer_begin(NpAdcSpmiV8 *s,uint16_t ppid,uint8_t reg,uint8_t op,unsigned bytes)
{
 if(!s)return;
 memset(&s->last_transfer,0,sizeof(s->last_transfer));
 s->last_transfer.ppid=ppid;s->last_transfer.reg=reg;
 s->last_transfer.op=op;s->last_transfer.bytes=(uint8_t)bytes;
}
static int transfer_end(NpAdcSpmiV8 *s,int result)
{if(s)s->last_transfer.error=result;return result;}
static int apid(NpAdcSpmiV8 *s,uint16_t ppid,unsigned *owner)
{
 if(!s||!s->ready)return -EACCES;
 int found=-1;unsigned selected=7;
 for(uint32_t i=0;i<s->apids;++i){
  uint32_t value=0,who=0;int e=rd(s,MAP+4*i,&value);if(e)return e;
  if(!value||(value&0x1fff)!=ppid)continue;
  e=rd(s,OWNER+4*i,&who);if(e)return e;
  if(found<0||(who&7)==0){found=(int)i;selected=who&7;}
 }
 if(found>=0)*owner=selected;
 return found<0?-ENODEV:found;
}
static int wait(NpAdcSpmiV8 *s,uint64_t base)
{
 for(unsigned i=0;i<1000;++i){
  uint32_t status=0;int e=rd(s,base+8,&status);if(e)return e;
  s->last_transfer.status=status;s->last_transfer.status_valid=1;
  if(status&1)return status&4?-EACCES:status&10?-EIO:0;
  s->io.pause_us(s->io.context,1);
 }
 return -ETIMEDOUT;
}
static int read_scope(uint16_t ppid,uint8_t reg,unsigned bytes)
{
 if(ppid==0x189)return reg==0x46&&bytes==1;
 if(ppid!=0x90&&ppid!=0x91)return 0;
 return (bytes==1&&(reg==0x45||reg==0x47||reg==0x4a||reg==0xe5))||
  (reg==0x4f&&bytes==7)||(reg==0x5c&&bytes==2);
}
static int write_scope(uint16_t ppid,uint8_t reg,const uint8_t *data,unsigned bytes)
{
 if(ppid!=0x90||!data)return 0;
 if(reg==0x4f&&bytes==7)return data[1]==0x80&&data[2]==1&&
  (data[3]&0x3c)==0x14&&(data[4]&0x87)==0x80&&(data[6]&15)==2&&
  ((data[0]==0x27&&data[5]==0x47)||(data[0]==1&&data[5]==0x4b));
 return bytes==1&&((reg==0xe5&&data[0]==1)||(reg==0x4b&&data[0]==1)||(reg==0x50&&data[0]==0));
}
int np_adc_spmi_v8_init(NpAdcSpmiV8 *s,const NpAdcSpmiOps *io)
{
 if(!s||!io||!io->read32||!io->write32||!io->pause_us)return -EINVAL;
 memset(s,0,sizeof(*s));s->io=*io;
 uint32_t version=0,count=0;int e=rd(s,CORE,&version);if(!e)e=rd(s,CORE+4,&count);
 if(e)return e;
 count&=0x1fff;
 if(version<0x80000000u||version>=0x90000000u||!count||count>8192)return -ENODEV;
 s->apids=count;s->ready=1;s->qtm_owner=7;return 0;
}
int np_adc_spmi_v8_owns(void *v)
{
 NpAdcSpmiV8 *s=v;unsigned owner=7;
 return apid(s,0x90,&owner)>=0&&owner==0;
}
int np_adc_spmi_v8_read(void *v,uint16_t ppid,uint8_t reg,uint8_t *out,unsigned bytes)
{
 NpAdcSpmiV8 *s=v;
 transfer_begin(s,ppid,reg,NP_ADC_SPMI_READ,bytes);
 if(!out||!read_scope(ppid,reg,bytes))return transfer_end(s,-EINVAL);
 if(s)s->last_transfer.valid=1;
 unsigned owner=7;int channel=apid(s,ppid,&owner);if(channel<0)return transfer_end(s,channel);
 const uint64_t base=OBSERVER+0x20u*(unsigned)channel;
 int e=wr(s,base,(1u<<27)|((uint32_t)reg<<4)|(bytes-1));if(!e)e=wait(s,base);if(e)return transfer_end(s,e);
 uint32_t words[2]={};e=rd(s,base+0x18,&words[0]);
 if(!e&&bytes>4)e=rd(s,base+0x1c,&words[1]);
 if(e)return transfer_end(s,e);
 for(unsigned i=0;i<bytes;++i)out[i]=(uint8_t)(words[i/4]>>(8*(i%4)));
 return transfer_end(s,0);
}
int np_adc_spmi_v8_write(void *v,uint16_t ppid,uint8_t reg,const uint8_t *data,unsigned bytes)
{
 NpAdcSpmiV8 *s=v;transfer_begin(s,ppid,reg,NP_ADC_SPMI_WRITE,bytes);
 if(!write_scope(ppid,reg,data,bytes))return transfer_end(s,-EINVAL);
 if(s)s->last_transfer.valid=1;
 unsigned owner=7;int channel=apid(s,ppid,&owner);if(channel<0)return transfer_end(s,channel);
 if(owner!=0)return transfer_end(s,-EACCES);
 const uint64_t base=WRITER+0x200u*(unsigned)channel;
 uint32_t words[2]={};for(unsigned i=0;i<bytes;++i)words[i/4]|=(uint32_t)data[i]<<(8*(i%4));
 int e=wr(s,base+0x10,words[0]);if(!e&&bytes>4)e=wr(s,base+0x14,words[1]);
 if(!e)e=wr(s,base,((uint32_t)reg<<4)|(bytes-1));
 return transfer_end(s,e?e:wait(s,base));
}
/* The N0 qtm-therm pinctrl selects bias-high-impedance: EN_CTL46 bit7=0.
 * Borrow only that bit under EE0, preserving/restoring every other bit. */
static int qtm_channel(NpAdcSpmiV8 *s)
{
 unsigned owner=7;int channel=apid(s,0x189,&owner);s->qtm_owner=owner;
 return channel<0?channel:owner? -EACCES:channel;
}
static int qtm_wait(NpAdcSpmiV8 *s,uint64_t base)
{
 for(unsigned i=0;i<1000;++i){
  uint32_t status=0;int e=rd(s,base+8,&status);if(e)return e;
  s->last_transfer.status=status;s->last_transfer.status_valid=1;
  if(status&1){s->qtm_pending=0;return status&4?-EACCES:status&10?-EIO:0;}
  s->io.pause_us(s->io.context,1);
 }
 return -ETIMEDOUT;
}
static int qtm_write(NpAdcSpmiV8 *s,uint8_t value)
{
 transfer_begin(s,0x189,0x46,NP_ADC_SPMI_WRITE,1);
 int channel=qtm_channel(s);if(channel<0)return transfer_end(s,channel);
 if(value!=s->qtm_before&&value!=s->qtm_expected)return transfer_end(s,-EINVAL);
 s->last_transfer.valid=1;
 uint64_t base=WRITER+0x200u*(unsigned)channel;
 int e=wr(s,base+0x10,value);if(e)return transfer_end(s,e);
 e=wr(s,base,0x46u<<4);if(e)return transfer_end(s,e);
 s->qtm_pending=1;return transfer_end(s,qtm_wait(s,base));
}
int np_adc_spmi_v8_qtm_prepare(NpAdcSpmiV8 *s)
{
 if(!s||!s->ready)return -EINVAL;
 if(s->qtm_owned||s->qtm_pending)return -EBUSY;
 s->qtm_phase=1;int channel=qtm_channel(s);if(channel<0)return channel;
 s->qtm_phase=2;uint8_t value=0;int e=np_adc_spmi_v8_read(s,0x189,0x46,&value,1);
 if(e)return e;
 s->qtm_before=s->qtm_last=value;s->qtm_read_valid=1;
 s->qtm_expected=(uint8_t)(value&~0x80u);s->qtm_restore_attempted=0;
 if(!(value&0x80u)){s->qtm_phase=5;return 0;}
 s->qtm_owned=1;s->qtm_phase=3;e=qtm_write(s,s->qtm_expected);if(e)return e;
 s->qtm_phase=4;e=np_adc_spmi_v8_read(s,0x189,0x46,&value,1);if(e)return e;
 s->qtm_last=value;
 if(value!=s->qtm_expected)return -EIO;
 s->qtm_phase=5;return 0;
}
int np_adc_spmi_v8_qtm_restore(NpAdcSpmiV8 *s)
{
 if(!s)return -EINVAL;
 if(!s->qtm_owned)return 0;
 int channel=qtm_channel(s);if(channel<0)return channel;
 if(s->qtm_pending){
  transfer_begin(s,0x189,0x46,NP_ADC_SPMI_PENDING_STATUS,1);
  s->last_transfer.valid=1;
  int e=qtm_wait(s,WRITER+0x200u*(unsigned)channel);
  (void)transfer_end(s,e);
  if(s->qtm_pending)return e;
 }
 uint8_t value=0;int e=np_adc_spmi_v8_read(s,0x189,0x46,&value,1);if(e)return e;
 s->qtm_last=value;
 if(value==s->qtm_before){s->qtm_owned=0;s->qtm_phase=7;return 0;}
 if(value!=s->qtm_expected||s->qtm_restore_attempted)return -EIO;
 s->qtm_restore_attempted=1;s->qtm_phase=6;e=qtm_write(s,s->qtm_before);if(e)return e;
 e=np_adc_spmi_v8_read(s,0x189,0x46,&value,1);if(e)return e;
 s->qtm_last=value;
 if(value!=s->qtm_before)return -EIO;
 s->qtm_owned=0;s->qtm_phase=7;return 0;
}
