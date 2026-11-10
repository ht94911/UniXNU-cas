/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Only ADC SDAM/GPIO traffic on the actual N0 arbiter layout. */
#include "NezhaAdcSpmiV8.h"
#include <errno.h>
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
 s->apids=count;s->ready=1;return 0;
}
int np_adc_spmi_v8_owns(void *v)
{
 NpAdcSpmiV8 *s=v;unsigned owner=7;
 return apid(s,0x90,&owner)>=0&&owner==0;
}
int np_adc_spmi_v8_read(void *v,uint16_t ppid,uint8_t reg,uint8_t *out,unsigned bytes)
{
 NpAdcSpmiV8 *s=v;
 if(!out||!read_scope(ppid,reg,bytes))return -EINVAL;
 unsigned owner=7;int channel=apid(s,ppid,&owner);if(channel<0)return channel;
 const uint64_t base=OBSERVER+0x20u*(unsigned)channel;
 int e=wr(s,base,(1u<<27)|((uint32_t)reg<<4)|(bytes-1));if(!e)e=wait(s,base);if(e)return e;
 uint32_t words[2]={};e=rd(s,base+0x18,&words[0]);
 if(!e&&bytes>4)e=rd(s,base+0x1c,&words[1]);
 if(e)return e;
 for(unsigned i=0;i<bytes;++i)out[i]=(uint8_t)(words[i/4]>>(8*(i%4)));
 return 0;
}
int np_adc_spmi_v8_write(void *v,uint16_t ppid,uint8_t reg,const uint8_t *data,unsigned bytes)
{
 NpAdcSpmiV8 *s=v;if(!write_scope(ppid,reg,data,bytes))return -EINVAL;
 unsigned owner=7;int channel=apid(s,ppid,&owner);if(channel<0)return channel;
 if(owner!=0)return -EACCES;
 const uint64_t base=WRITER+0x200u*(unsigned)channel;
 uint32_t words[2]={};for(unsigned i=0;i<bytes;++i)words[i/4]|=(uint32_t)data[i]<<(8*(i%4));
 int e=wr(s,base+0x10,words[0]);if(!e&&bytes>4)e=wr(s,base+0x14,words[1]);
 if(!e)e=wr(s,base,((uint32_t)reg<<4)|(bytes-1));
 return e?e:wait(s,base);
}
