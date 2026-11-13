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
 static const uint16_t ppids[]={0x90,0x91,0x189,0x190,0x193,0x2793};
 unsigned slot=0;
 while(slot<sizeof(ppids)/sizeof(ppids[0])&&ppids[slot]!=ppid)++slot;
 if(slot==sizeof(ppids)/sizeof(ppids[0]))return -ENODEV;
 uint32_t first=0,last=s->apids;uint16_t local_ppid=ppid;
 /* N0 charger GPIO is the only selected peripheral used on bus1.
  * v8's shared map uses global APIDs; its PPIDs are local to each bus.
  * Its global owner entry must identify EE0 before any GPIO lease write. */
 if(ppid==0x2793){
  uint32_t count=0;int e=rd(s,CORE+8,&count);if(e)return e;
  count&=0x1fff;
  if(!count||s->apids+count>8192)return -ENODEV;
  first=s->apids;last=first+count;local_ppid=0x0793;
 }
 const uint16_t cached=s->cached_apids[slot];
 if(cached&0x8000u){
  const unsigned channel=cached&0x1fffu;
  if(channel>=first&&channel<last){
   uint32_t value=0,who=0;int e=rd(s,MAP+4*channel,&value);if(e)return e;
   if(value&&(value&0x1fff)==local_ppid){
    e=rd(s,OWNER+4*channel,&who);if(e)return e;
    if((who&7)==0){*owner=0;return (int)channel;}
   }
  }
  /* A changed map/EE must not authorize a cached writer. Reapply the same
   * original selection, which prefers a currently owned EE0 mapping. */
  s->cached_apids[slot]=0;
 }
 int found=-1;unsigned selected=7;
 for(uint32_t i=first;i<last;++i){
  uint32_t value=0,who=0;int e=rd(s,MAP+4*i,&value);if(e)return e;
  if(!value||(value&0x1fff)!=local_ppid)continue;
  e=rd(s,OWNER+4*i,&who);if(e)return e;
  if(found<0||(who&7)==0){found=(int)i;selected=who&7;}
 }
 if(found>=0){*owner=selected;s->cached_apids[slot]=(uint16_t)(0x8000u|(unsigned)found);}
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
 if(ppid==0x189||ppid==0x190||ppid==0x193||ppid==0x2793)
  return reg==0x46&&bytes==1;
 if(ppid!=0x90&&ppid!=0x91)return 0;
 return (bytes==1&&(reg==0x45||reg==0x47||reg==0x4a||reg==0xe5))||
  (reg==0x4f&&bytes==7)||(reg==0x5c&&bytes==2);
}
static int write_scope(uint16_t ppid,uint8_t reg,const uint8_t *data,unsigned bytes)
{
 if(ppid!=0x90||!data)return 0;
 if(reg==0x4f&&bytes==7)return data[1]==0x80&&data[2]==1&&
  (data[3]&0x3c)==0x14&&(data[4]&0x87)==0x80&&(data[6]&15)==2&&
  ((data[0]==0x27&&(data[5]==0x47||data[5]==0x4d))||
   (data[0]==1&&(data[5]==0x4b||data[5]==0x4d||data[5]==0x44||
    data[5]==0x46||data[5]==0x47||data[5]==0x4c)));
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
static int read_transaction(void *v,uint16_t ppid,uint8_t reg,uint8_t *out,unsigned bytes)
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
int np_adc_spmi_v8_read(void *v,uint16_t ppid,uint8_t reg,uint8_t *out,unsigned bytes)
{
 int locked=nezha_spmi_bus_acquire();
 if(locked){transfer_begin(v,ppid,reg,NP_ADC_SPMI_READ,bytes);return transfer_end(v,error(locked));}
 int result=read_transaction(v,ppid,reg,out,bytes);
 nezha_spmi_bus_release();return result;
}
static int write_transaction(void *v,uint16_t ppid,uint8_t reg,const uint8_t *data,unsigned bytes)
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
int np_adc_spmi_v8_write(void *v,uint16_t ppid,uint8_t reg,const uint8_t *data,unsigned bytes)
{
 int locked=nezha_spmi_bus_acquire();
 if(locked){transfer_begin(v,ppid,reg,NP_ADC_SPMI_WRITE,bytes);return transfer_end(v,error(locked));}
 int result=write_transaction(v,ppid,reg,data,bytes);
 nezha_spmi_bus_release();return result;
}
/* The four N0 ADC pinctrl states select bias-high-impedance. Reuse the
 * proven QTM EN_CTL46 bit7 lease; only one GPIO can be borrowed at a time. */
uint16_t np_adc_spmi_v8_gpio_ppid(const NpAdcSpmiV8 *s)
{
 static const uint16_t pins[]={0x189,0x190,0x193,0x2793};
 return s&&s->gpio_selector<sizeof(pins)/sizeof(pins[0])?pins[s->gpio_selector]:0;
}
static int qtm_channel(NpAdcSpmiV8 *s)
{
 const uint16_t ppid=np_adc_spmi_v8_gpio_ppid(s);if(!ppid)return -EINVAL;
 unsigned owner=7;int channel=apid(s,ppid,&owner);s->qtm_owner=owner;
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
static int qtm_write_transaction(NpAdcSpmiV8 *s,uint8_t value)
{
 transfer_begin(s,np_adc_spmi_v8_gpio_ppid(s),0x46,NP_ADC_SPMI_WRITE,1);
 int channel=qtm_channel(s);if(channel<0)return transfer_end(s,channel);
 if(value!=s->qtm_before&&value!=s->qtm_expected)return transfer_end(s,-EINVAL);
 s->last_transfer.valid=1;
 uint64_t base=WRITER+0x200u*(unsigned)channel;
 int e=wr(s,base+0x10,value);if(e)return transfer_end(s,e);
 e=wr(s,base,0x46u<<4);if(e)return transfer_end(s,e);
 s->qtm_pending=1;return transfer_end(s,qtm_wait(s,base));
}
static int qtm_write(NpAdcSpmiV8 *s,uint8_t value)
{
 int locked=nezha_spmi_bus_acquire();
 if(locked){transfer_begin(s,np_adc_spmi_v8_gpio_ppid(s),0x46,NP_ADC_SPMI_WRITE,1);return transfer_end(s,error(locked));}
 int result=qtm_write_transaction(s,value);
 nezha_spmi_bus_release();return result;
}
int np_adc_spmi_v8_gpio_prepare(NpAdcSpmiV8 *s,uint16_t ppid)
{
 if(!s||!s->ready)return -EINVAL;
 if(s->qtm_owned||s->qtm_pending)return -EBUSY;
 switch(ppid){case 0x189:s->gpio_selector=0;break;case 0x190:s->gpio_selector=1;break;
  case 0x193:s->gpio_selector=2;break;case 0x2793:s->gpio_selector=3;break;default:return -EINVAL;}
 s->qtm_read_valid=0;s->qtm_owner=7;
 s->qtm_phase=1;unsigned owner=7;int channel=apid(s,ppid,&owner);s->qtm_owner=owner;
 if(channel<0)return channel;
 s->qtm_phase=2;uint8_t value=0;int e=np_adc_spmi_v8_read(s,ppid,0x46,&value,1);
 if(e)return e;
 s->qtm_before=s->qtm_last=value;s->qtm_read_valid=1;
 s->qtm_expected=(uint8_t)(value&~0x80u);s->qtm_restore_attempted=NP_ADC_QTM_RESTORE_NONE;
 if(!(value&0x80u)){s->qtm_phase=5;return 0;}
 if(owner!=0)return -EACCES;
 s->qtm_owned=1;s->qtm_phase=3;e=qtm_write(s,s->qtm_expected);if(e)return e;
 s->qtm_phase=4;e=np_adc_spmi_v8_read(s,ppid,0x46,&value,1);if(e)return e;
 s->qtm_last=value;
 if(value!=s->qtm_expected)return -EIO;
 s->qtm_phase=5;return 0;
}
int np_adc_spmi_v8_qtm_prepare(NpAdcSpmiV8 *s)
{return np_adc_spmi_v8_gpio_prepare(s,0x189);}
int np_adc_spmi_v8_qtm_restore(NpAdcSpmiV8 *s)
{
 if(!s)return -EINVAL;
 if(!s->qtm_owned)return 0;
 const int recovery=s->qtm_restore_attempted==NP_ADC_QTM_RESTORE_DROPPED;
 int channel=qtm_channel(s);
 if(channel<0){if(recovery)s->qtm_restore_attempted=NP_ADC_QTM_RESTORE_REJECTED;return channel;}
 if(s->qtm_pending){
  int locked=nezha_spmi_bus_acquire();if(locked)return error(locked);
  transfer_begin(s,np_adc_spmi_v8_gpio_ppid(s),0x46,NP_ADC_SPMI_PENDING_STATUS,1);
  s->last_transfer.valid=1;
  int e=qtm_wait(s,WRITER+0x200u*(unsigned)channel);
  (void)transfer_end(s,e);
  nezha_spmi_bus_release();
  if(s->qtm_pending)return e;
 }
 uint8_t value=0;int e=np_adc_spmi_v8_read(s,np_adc_spmi_v8_gpio_ppid(s),0x46,&value,1);
 if(e){if(recovery)s->qtm_restore_attempted=NP_ADC_QTM_RESTORE_REJECTED;return e;}
 s->qtm_last=value;
 if(value==s->qtm_before){s->qtm_owned=0;s->qtm_phase=7;return 0;}
 if(value!=s->qtm_expected){
  if(s->qtm_restore_attempted!=NP_ADC_QTM_RESTORE_RETRY_REJECTED)
   s->qtm_restore_attempted=NP_ADC_QTM_RESTORE_REJECTED;
  return -EIO;
 }
 /* Attempts fence writes only. Even a failed attempt can later be proven
  * restored by the existing pending wait and exact original-value read. */
 if(s->qtm_restore_attempted&&!recovery)return -EIO;
 s->qtm_restore_attempted=recovery?NP_ADC_QTM_RESTORE_RETRY:NP_ADC_QTM_RESTORE_FIRST;
 s->qtm_phase=6;e=qtm_write(s,s->qtm_before);
 if(e){
  const NpAdcSpmiTransfer *t=&s->last_transfer;
  /* Return this first error before any recovery I/O so Native can retain it.
   * This permission belongs only to this lease's completed dropped restore. */
  const int dropped=!recovery&&!s->qtm_pending&&t->valid&&t->status_valid&&
   t->ppid==np_adc_spmi_v8_gpio_ppid(s)&&t->reg==0x46&&t->op==NP_ADC_SPMI_WRITE&&t->bytes==1&&t->error==-EIO&&t->status==9;
  s->qtm_restore_attempted=dropped?NP_ADC_QTM_RESTORE_DROPPED:
   recovery?NP_ADC_QTM_RESTORE_RETRY_REJECTED:NP_ADC_QTM_RESTORE_REJECTED;
  return e;
 }
 e=np_adc_spmi_v8_read(s,np_adc_spmi_v8_gpio_ppid(s),0x46,&value,1);
 if(e){s->qtm_restore_attempted=recovery?NP_ADC_QTM_RESTORE_RETRY_REJECTED:NP_ADC_QTM_RESTORE_REJECTED;return e;}
 s->qtm_last=value;
 if(value!=s->qtm_before){s->qtm_restore_attempted=recovery?NP_ADC_QTM_RESTORE_RETRY_REJECTED:NP_ADC_QTM_RESTORE_REJECTED;return -EIO;}
 s->qtm_owned=0;s->qtm_phase=7;return 0;
}
