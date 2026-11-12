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
/* N0's actual FDT, not pandora's formula order. All selected physical
 * channels use adc5_gen4_chans_pmic scale8/prescale0 in the OS4 module. */
static const uint16_t channel_specifier[]={
 0x2747,0x014b,0x274d,0x014d,0x0144,0x0146,0x0147,0x014c
};
int np_gen4_adc_channel_supported(unsigned channel)
{return channel<sizeof(channel_specifier)/sizeof(channel_specifier[0]);}
static int negative(int error){return error>0?-error:error;}
static int owned(NpGen4Adc *a){return a->io.owns_adc(a->io.context)==1;}
static int read_reg(NpGen4Adc *a,uint16_t ppid,uint8_t reg,uint8_t *bytes,unsigned count)
{if(!owned(a))return -EACCES;return negative(a->io.read(a->io.context,ppid,reg,bytes,count));}
static int high_impedance(NpGen4Adc *a,unsigned channel)
{
 /* N0 pinctrl default states: QTM GPIO2, charger GPIO12 on bus1,
  * CPU GPIO12 and wireless GPIO9 on bus0. The Native adapter has already
  * acquired/restored the selected EN_CTL lease around this conversion. */
 const uint16_t ppid=channel==1?0x0189:channel==2?0x2793:
  channel==3?0x0193:channel==7?0x0190:0;
 if(!ppid)return 0;
 uint8_t enable=0;int error=read_reg(a,ppid,0x46,&enable,1);
 return error?error:(enable&0x80)?-EACCES:0;
}
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
static int own_request(NpGen4Adc *a,uint8_t *request,int unchanged_before_allowed)
{
 uint8_t config[7];int error=read_reg(a,SDAM0,CONFIG,config,sizeof(config));
 if(error)return error;
 if(unchanged_before_allowed&&!config[1]&&!memcmp(config,a->before_config,sizeof(config))){
  *request=0;return 0;
 }
 for(unsigned i=0;i<sizeof(config);++i)
  if(i!=1&&config[i]!=a->requested_config[i])return -EIO;
 if(config[1]!=0&&config[1]!=a->requested_config[1])return -EIO;
 *request=config[1];return 0;
}
static int cleanup_once(NpGen4Adc *a)
{
 if(a->cleanup_started)return -EIO;
 if(!owned(a))return -EACCES;
 a->cleanup_started=1;
 const uint8_t one=1,zero=0;
 int first_error=0,error=write_reg(a,CLEAR,&one,1);
 if(error){
  a->uncertain=1;
  if(!a->io.dropped_cleanup_write||!a->io.dropped_cleanup_write(a->io.context,CLEAR))return error;
  first_error=error;
  /* EOC_CLEAR is W1C and cannot submit another conversion. Observe first;
   * only a still-set bit requires the one bounded repeat of this clear.
   * Conversion CONFIG and its REQUEST notification never take this path. */
  uint8_t eoc=0;error=read_reg(a,SDAM0,EOC,&eoc,1);
  if(!error&&(eoc&1))error=write_reg(a,CLEAR,&one,1);
  if(error)return first_error;
 }
 if(!error)error=write_reg(a,PERIPHERAL,&zero,1);
 if(error){
  a->uncertain=1;
  if(!a->io.dropped_cleanup_write||!a->io.dropped_cleanup_write(a->io.context,PERIPHERAL))
   return first_error?first_error:error;
  if(!first_error)first_error=error;
  /* 044 observed a dropped clear of this conversion's request byte. Read
   * the entire owned config before touching it again. A clear already
   * applied needs no repeat; a still-set own byte permits only one clear.
   * Notify the remaining cleanup once, only after the byte is observed0. */
  uint8_t request=0;error=own_request(a,&request,0);if(error)return first_error;
  if(request){
   error=write_reg(a,PERIPHERAL,&zero,1);
   if(error&&(!a->io.dropped_cleanup_write||
      !a->io.dropped_cleanup_write(a->io.context,PERIPHERAL)))return first_error;
   error=own_request(a,&request,0);if(error||request)return first_error;
  }
 }
 if(!error)error=write_reg(a,REQUEST,&one,1);
 if(!error)error=idle(a);
 if(error)return first_error?first_error:error;
 uint8_t eoc=0;error=read_reg(a,SDAM0,EOC,&eoc,1);
 if(error||(eoc&1))return first_error?first_error:error?error:-EIO;
 uint8_t request=0;error=own_request(a,&request,0);
 if(error||request)return first_error?first_error:error?error:-EIO;
 a->touched=a->uncertain=a->cleanup_started=a->async_phase=0;
 return first_error;
}
int np_gen4_adc_init(NpGen4Adc *a,const NpGen4AdcOps *io)
{
 if(!a||!io||!io->owns_adc||!io->read||!io->write||!io->pause_us)return -EINVAL;
 memset(a,0,sizeof(*a));a->io=*io;return 0;
}
int np_gen4_adc_read(NpGen4Adc *a,unsigned channel,uint16_t *out)
{
 if(!a||!out||!np_gen4_adc_channel_supported(channel))return -EINVAL;
 if(a->touched||a->uncertain)return -EBUSY;
 int error=idle(a);if(error)return error;
 uint8_t eoc=0;error=read_reg(a,SDAM0,EOC,&eoc,1);
 if(error)return error;
 if(eoc&1)return -EBUSY; /* Existing slot0 event was not this request. */
 error=high_impedance(a,channel);if(error)return error;
 error=read_reg(a,SDAM0,CONFIG,a->before_config,7);if(error)return error;
 uint8_t *config=a->requested_config;memcpy(config,a->before_config,7);
 config[0]=(uint8_t)(channel_specifier[channel]>>8);config[1]=0x80;config[2]=1;
 /* Actual gen4 default decimation1, ratiometric1; preserve reserved bits. */
 config[3]=(uint8_t)((config[3]&~0x3cu)|0x14u);
 config[4]=(uint8_t)((config[4]&~7u)|0x80u);
 config[5]=(uint8_t)channel_specifier[channel];config[6]=(uint8_t)((config[6]&~15u)|2u);
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
 /* A dropped conversion notification may leave EOC0 with our request byte
  * still set. It is not quiescent: that notification can arrive later.
  * Verify the exact owned config, then finish only this request's existing
  * cleanup. An unchanged pre-write config with request0 needs no write. */
 uint8_t request=0;error=own_request(a,&request,!(eoc&1));
 if(error)return fail(a,error);
 if((eoc&1)||request){
  error=cleanup_once(a);if(error&&(a->touched||a->uncertain))return fail(a,error);
 }
 a->touched=a->uncertain=a->cleanup_started=a->async_phase=0;return 0;
}

/* Cooperative continuation of the same SDAM0 request/clear sequence. The
 * continuous sampler must not occupy the MCA worker for the500ms EOC wait.
 * Each poll observes once; the next worker pass continues the same request. */
int np_gen4_adc_begin(NpGen4Adc *a,unsigned channel,uint64_t now)
{
 if(!a||!np_gen4_adc_channel_supported(channel))return -EINVAL;
 if(a->touched||a->uncertain||a->async_phase)return -EBUSY;
 uint8_t hs=0,request=0,eoc=0;
 int error=read_reg(a,SDAM0,HS,&hs,1);
 if(!error)error=read_reg(a,SDAM0,REQUEST,&request,1);
 if(!error)error=read_reg(a,SDAM0,EOC,&eoc,1);
 if(error)return error;
 if(hs!=1||request||(eoc&1))return -EBUSY;
 error=high_impedance(a,channel);if(error)return error;
 error=read_reg(a,SDAM0,CONFIG,a->before_config,7);if(error)return error;
 uint8_t *config=a->requested_config;memcpy(config,a->before_config,7);
 config[0]=(uint8_t)(channel_specifier[channel]>>8);config[1]=0x80;config[2]=1;
 config[3]=(uint8_t)((config[3]&~0x3cu)|0x14u);
 config[4]=(uint8_t)((config[4]&~7u)|0x80u);
 config[5]=(uint8_t)channel_specifier[channel];config[6]=(uint8_t)((config[6]&~15u)|2u);
 error=write_reg(a,CONFIG,config,7);if(error)return fail(a,error);
 const uint8_t one=1;error=write_reg(a,REQUEST,&one,1);
 if(error)return fail(a,error);
 a->async_phase=1;a->async_deadline_ms=now+500;return 0;
}
int np_gen4_adc_poll(NpGen4Adc *a,uint64_t now,uint16_t *out)
{
 if(!a||!out)return -EINVAL;
 if(a->uncertain)return -EIO;
 if(!a->async_phase)return -ENOENT;
 if(now>=a->async_deadline_ms)return fail(a,-ETIMEDOUT);
 uint8_t eoc=0;int error=read_reg(a,SDAM0,EOC,&eoc,1);
 if(error)return fail(a,error);
 if(!(eoc&1))return 1;
 uint8_t raw[2]={};error=read_reg(a,SDAM0,DATA,raw,2);
 if(error)return fail(a,error);
 a->last_raw=(uint16_t)(raw[0]|raw[1]<<8);
 /* The existing, separately bounded15ms cleanup is short enough for the
  * MCA worker. Only its former500ms EOC wait needed continuation. */
 error=cleanup_once(a);if(error)return fail(a,error);
 a->touched=a->uncertain=a->cleanup_started=a->async_phase=0;
 ++a->conversions;*out=a->last_raw;return 0;
}
