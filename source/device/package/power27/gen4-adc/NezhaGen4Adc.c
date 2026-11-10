/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual OS4 gen4 SDAM0 immediate conversion contract. Transport binding and
 * live ownership are separate; this file is not yet in the frozen029 image. */
#include "NezhaGen4Adc.h"
#include <sys/errno.h>
#include <string.h>
#define SDAM0 0x090u
#define HS 0x45u
#define EOC 0x4au
#define CLEAR 0x4bu
#define CONFIG 0x4fu
#define PERIPHERAL 0x50u
#define DATA 0x5cu
#define REQUEST 0xe5u
static int negative(int error){return error>0?-error:error;}
static int owned(NpGen4Adc *a){return a->io.owns_adc(a->io.context)==1;}
static int read_reg(NpGen4Adc *a,uint16_t ppid,uint8_t reg,uint8_t *bytes,unsigned count)
{if(!owned(a))return -EACCES;return negative(a->io.read(a->io.context,ppid,reg,bytes,count));}
static int write_reg(NpGen4Adc *a,uint8_t reg,const uint8_t *bytes,unsigned count)
{
 if(!owned(a))return -EACCES;
 a->touched=1;
 return negative(a->io.write(a->io.context,SDAM0,reg,bytes,count));
}
static int idle(NpGen4Adc *a)
{
 for(unsigned i=0;i<150;++i){
  uint8_t hs=0,request=0;int error=read_reg(a,SDAM0,HS,&hs,1);
  if(!error)error=read_reg(a,SDAM0,REQUEST,&request,1);
  if(error)return error;
  if(hs==1&&request==0)return 0;
  a->io.pause_us(a->io.context,100);
 }
 return -ETIMEDOUT;
}
static int fail(NpGen4Adc *a,int error)
{if(a->touched)a->uncertain=1;return error;}
static int cleanup_once(NpGen4Adc *a)
{
 if(a->cleanup_started)return -EIO;
 if(!owned(a))return -EACCES;
 a->cleanup_started=1;
 const uint8_t one=1,zero=0;
 int error=write_reg(a,CLEAR,&one,1);
 if(!error)error=write_reg(a,PERIPHERAL,&zero,1);
 if(!error)error=write_reg(a,REQUEST,&one,1);
 if(!error)error=idle(a);
 if(error)return error;
 uint8_t eoc=0;error=read_reg(a,SDAM0,EOC,&eoc,1);
 return error?error:(eoc&1)?-EIO:0;
}
int np_gen4_adc_init(NpGen4Adc *a,const NpGen4AdcOps *io)
{
 if(!a||!io||!io->owns_adc||!io->read||!io->write||!io->pause_us)return -EINVAL;
 memset(a,0,sizeof(*a));a->io=*io;return 0;
}
int np_gen4_adc_read(NpGen4Adc *a,unsigned channel,uint16_t *out)
{
 if(!a||!out||channel>1)return -EINVAL;
 if(a->touched||a->uncertain)return -EBUSY;
 int error=idle(a);if(error)return error;
 uint8_t eoc=0;error=read_reg(a,SDAM0,EOC,&eoc,1);
 if(error)return error;
 if(eoc&1)return -EBUSY; /* Existing slot0 event was not this request. */
 if(channel==1){
  uint8_t enable=0;error=read_reg(a,0x189,0x46,&enable,1);
  if(error)return error;
  if(enable&0x80)return -EACCES; /* Stock qtm GPIO2 must already be high-Z. */
 }
 error=read_reg(a,SDAM0,CONFIG,a->before_config,7);if(error)return error;
 uint8_t *config=a->requested_config;memcpy(config,a->before_config,7);
 config[0]=channel?0x01:0x27;config[1]=0x80;config[2]=1;
 /* Actual gen4 default decimation1, ratiometric1; preserve reserved bits. */
 config[3]=(uint8_t)((config[3]&~0x3cu)|0x14u);
 config[4]=(uint8_t)((config[4]&~7u)|0x80u);
 config[5]=channel?0x4b:0x47;config[6]=(uint8_t)((config[6]&~15u)|2u);
 error=write_reg(a,CONFIG,config,7);if(error)return fail(a,error);
 const uint8_t one=1;error=write_reg(a,REQUEST,&one,1);
 if(error)return fail(a,error);
 unsigned tries;
 for(tries=0;tries<500;++tries){
  error=read_reg(a,SDAM0,EOC,&eoc,1);if(error)return fail(a,error);
  if(eoc&1)break;
  a->io.pause_us(a->io.context,1000);
 }
 if(tries==500)return fail(a,-ETIMEDOUT);
 uint8_t raw[2]={};error=read_reg(a,SDAM0,DATA,raw,2);
 if(error)return fail(a,error);
 a->last_raw=(uint16_t)(raw[0]|raw[1]<<8);
 error=cleanup_once(a);if(error)return fail(a,error);
 a->touched=a->uncertain=a->cleanup_started=0;
 ++a->conversions;*out=a->last_raw;return 0;
}
int np_gen4_adc_quiesce(NpGen4Adc *a)
{
 if(!a)return -EINVAL;
 if(!a->touched&&!a->uncertain)return 0;
 int error=idle(a);if(error)return fail(a,error);
 uint8_t eoc=0;error=read_reg(a,SDAM0,EOC,&eoc,1);
 if(error)return fail(a,error);
 if(eoc&1){
  error=cleanup_once(a);if(error)return fail(a,error);
 }
 a->touched=a->uncertain=a->cleanup_started=0;return 0;
}
