/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaPowerControl.h"
#include "NezhaMca.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
enum { NORMAL,NO_VOLTAGE_RESPONSE,NO_CURRENT_RESPONSE,HOT_RUNNING,WRONG_ROLE,
       NO_9V_CAP,OVP_REFUSED,OVP_UNCERTAIN,VOLTAGE_TIMEOUT,CURRENT_REDUCE_TIMEOUT,
       TRANSIENT_BASELINE,PERSISTENT_BASELINE };
static unsigned scenario,writes,local_writes,fcc_raises,voltage_raises;
static uint32_t actual_fcc,actual_mv,actual_role;static uint8_t ovp;
static uint32_t properties[]={0x2008,0x2005,0x2007,0x2003,0x200d,0x200e,
 0x201b,0x2000c,0x21002,0x21014,0x21018,0x2101c,0x2100d};
static void reply(NpPowerControl *c,NpAuxCommand *cmd,uint64_t now)
{
 uint8_t bytes[NP_PD_BYTES]={0};size_t n=cmd->reply_bytes;int status=0;
 if(cmd->control==2) {
  assert(cmd->property==NP_BUCK_USB_OVP&&np_control_register_allowed(c,cmd,now));
  assert(actual_mv<=5500&&(cmd->value>>8)==ovp);++local_writes;
  if(scenario==OVP_REFUSED&&cmd->value==(15u<<8))status=EACCES;
  else {
   c->buck.wire_accepted=1;
   if(scenario==OVP_UNCERTAIN&&cmd->value==(15u<<8))status=EIO;
   else ovp=(uint8_t)cmd->value&15;
  }
  bytes[0]=ovp;
 } else if(cmd->control) {
  assert(cmd->control==1&&np_control_write_allowed(c,cmd,now));++writes;
  c->buck.wire_accepted=1;
  if(cmd->property==NP_BUCK_FCC) {
   assert(cmd->value==100||cmd->value==500||cmd->value==1500);
   if(cmd->value==1500){assert(actual_mv==9000&&ovp==0&&actual_fcc==500);++fcc_raises;}
   if(scenario==CURRENT_REDUCE_TIMEOUT&&cmd->value==100)status=ETIMEDOUT;
   else if(!(scenario==NO_CURRENT_RESPONSE&&cmd->value==1500))actual_fcc=cmd->value;
  } else {
   assert(cmd->property==NP_BUCK_FIXED_PD&&(cmd->value==5000||cmd->value==9000));
   if(cmd->value==9000){assert(ovp==0&&actual_fcc==500&&actual_role==1);++voltage_raises;}
   if(!(scenario==NO_VOLTAGE_RESPONSE&&cmd->value==9000))actual_mv=cmd->value;
   if(scenario==VOLTAGE_TIMEOUT&&cmd->value==9000)status=ETIMEDOUT;
  }
 } else {
  assert(cmd->index<NP_META_COUNT&&properties[cmd->index]==cmd->property);
  uint32_t v=0;
  switch(cmd->index) {
  case NP_META_ICL:v=3000;break;case NP_META_FCC:v=0;break;
  case NP_META_TERM:v=4400;break;case NP_META_ENABLED:v=1;break;
  case NP_META_TYPE:v=10;break;case NP_META_DATA_ROLE:v=actual_role;break;
  case NP_META_PDOS:
   np_put32(bytes,5000);np_put32(bytes+4,5000);np_put32(bytes+8,3000);
   if(scenario!=NO_9V_CAP){np_put32(bytes+16,9000);np_put32(bytes+20,9000);np_put32(bytes+24,3000);}
   break;
  default:break;
  }
  if(cmd->index!=NP_META_PDOS){if(n==1)bytes[0]=(uint8_t)v;else np_put32(bytes,v);}
 }
 (void)np_control_complete(c,cmd,status? -status:0,n?bytes:NULL,n,now);
}
static void run(unsigned which)
{
 scenario=which;writes=local_writes=fcc_raises=voltage_raises=0;
 actual_fcc=500;actual_mv=5000;actual_role=which==WRONG_ROLE?2:1;ovp=15;
 NpPowerControl c;np_control_init(&c,4);np_control_link(&c,1,1);c.allow_control=1;
 unsigned hot=0;
 uint64_t prior_gauge=0;unsigned baseline_samples=0;
 for(uint64_t now=1000;now<350000;now+=100) {
  NpSnapshot s={0};s.generation=1;s.valid=0x7fff;s.external=s.charging=1;
  s.gauge.present=1;s.gauge.soc=80;s.gauge.voltage_mv=4300;
  s.gauge.temperature_centi_c=hot?3600:3000;s.gauge.current_ma=(int32_t)actual_fcc;
  s.gauge.end_ms=s.charger.end_ms=(now/2000)*2000;s.charger.usb_uv=(int32_t)actual_mv*1000;s.at_ms=now;
  if((which==TRANSIENT_BASELINE||which==PERSISTENT_BASELINE)&&
     c.buck.state==NP_BUCK_FCC_BASELINE) {
   if(s.gauge.end_ms>c.buck.feedback_after_ms&&s.gauge.end_ms!=prior_gauge){
    prior_gauge=s.gauge.end_ms;++baseline_samples;
   }
   const int32_t actual019[]={495,417,312,476};
   if(baseline_samples&&(which==PERSISTENT_BASELINE||baseline_samples<=4))
    s.gauge.current_ma=actual019[(baseline_samples-1)%4];
  }
  NpPump p={0};p.chip=0x85;p.controls_valid=p.switching_valid=1;
  p.valid=18;p.bus_mv=(int32_t)actual_mv;p.die_centi_c=3000;p.control[2]=ovp;
  np_control_sample(&c,&s,&p,0,now);
  if((which==HOT_RUNNING||which==CURRENT_REDUCE_TIMEOUT)&&c.buck.state==NP_BUCK_RUNNING)hot=1;
  NpAuxCommand cmd;int got=np_control_next(&c,now,&cmd);
  if(got>0)reply(&c,&cmd,now);
  if(c.buck.state==NP_BUCK_UNCERTAIN)break;
  if(c.buck.state==NP_BUCK_FINISHED&&!c.buck.armed&&writes)break;
 }
 if(which==WRONG_ROLE||which==NO_9V_CAP)assert(!writes&&!local_writes&&!voltage_raises);
 else if(which==OVP_UNCERTAIN)assert(c.buck.state==NP_BUCK_UNCERTAIN&&!voltage_raises&&c.buck.owned);
 else if(which==CURRENT_REDUCE_TIMEOUT) {
  assert(c.buck.state==NP_BUCK_UNCERTAIN&&actual_mv==5000&&ovp==15&&c.buck.protection_restored);
 } else {
  assert(c.buck.state==NP_BUCK_FINISHED&&!c.buck.owned&&!c.buck.pending&&!c.buck.unverified_effect);
  assert(actual_mv==5000&&ovp==15);
  if(which!=PERSISTENT_BASELINE)assert(c.buck.protection_restored);
  if(which==NORMAL||which==TRANSIENT_BASELINE)assert(actual_fcc==500&&writes==5&&local_writes==2&&fcc_raises==1&&voltage_raises==1);
  if(which==NO_VOLTAGE_RESPONSE||which==VOLTAGE_TIMEOUT)assert(!fcc_raises&&actual_fcc==100);
  if(which==NO_CURRENT_RESPONSE||which==HOT_RUNNING||which==OVP_REFUSED)assert(actual_fcc==100);
  if(which==PERSISTENT_BASELINE)assert(actual_fcc==100&&!voltage_raises&&!fcc_raises&&!local_writes&&c.buck.changes==2);
 }
 printf("PASS fixed PD scenario%u writes%u local%u state%u\n",which,writes,local_writes,c.buck.state);
}
int main(void)
{
 for(unsigned i=0;i<=PERSISTENT_BASELINE;++i)run(i);
 puts("PASS fixed9V requires OVP/current evidence; no repeated increase;5V before OVP restore");
 return 0;
}
