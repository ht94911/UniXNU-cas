/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaPowerControl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static NpPowerControl running(void)
{
 NpPowerControl c;np_control_init(&c,9);np_control_link(&c,1,1);
 NpGauge g={0};g.valid=0x3ff;g.present=1;g.soc=95;g.voltage_mv=4464;
 g.current_ma=1664;g.temperature_centi_c=3615;g.remaining_mah=5401;
 g.full_mah=5700;g.cycles=331;g.raw[2]=3090;g.generation=7;
 g.begin_ms=9950;g.end_ms=10000;
 NpCharger b={0};b.epoch=1;b.generation=7;b.begin_ms=9950;b.end_ms=10000;
 b.external=1;b.usb_uv=18056000;b.usb_ua=156000;
 b.valid=NP_BIT(NP_EXTERNAL)|NP_BIT(NP_CHARGING)|NP_BIT(NP_CHARGE_DONE)|
  NP_BIT(NP_USB_VOLTAGE)|NP_BIT(NP_USB_CURRENT);
 b.state_raw=7;b.state_valid=1;b.input_current_scope=NP_INPUT_CURRENT_MAIN_BUCK;
 NpSnapshot s;np_snapshot(&s,&g,&b,10000,7);
 NpPump p={0};p.chip=0x85;p.mode=0;p.controls_valid=p.switching_valid=1;
 p.protection.valid=1;p.enable=p.switching=1;
 NpGaugeIdentity id={0};id.valid=3;id.cell_code='F';memcpy(id.device_name,"BM6N",5);
 np_control_profile(&c,&id,1);np_control_sample(&c,&s,&p,0,10000);
 for(unsigned i=0;i<NP_META_COUNT;++i){c.meta[i].valid=1;c.meta[i].at_ms=10000;}
 c.meta[NP_META_ENABLED].value=1;c.meta[NP_META_TERM].value=4400;
 c.meta[NP_META_ICL].value=4800;c.meta[NP_META_TYPE].value=11;
 c.buck.owned=64;c.buck.state=NP_BUCK_PUMP_RUN;
 return c;
}
static void stopped(NpPowerControl c,uint64_t now)
{
 NpSnapshot s;np_control_snapshot(&c,now,&s);assert(!s.charging);
}
int main(void)
{
 NpPowerControl c=running();NpSnapshot s;
 np_control_snapshot(&c,10001,&s);
 assert(s.charging&&(s.valid&NP_BIT(NP_CHARGING))&&!s.charger.charging);
 assert(s.charger.state_raw==7&&s.charger.state_valid==1&&s.gauge.current_ma==1664);
 assert(!s.usb_power_valid); /* Main buck current remains a separate branch. */
 c.sample.charger.state_raw=6;np_control_snapshot(&c,10001,&s);assert(s.charging);
 c=running();c.pump.mode=1;np_control_snapshot(&c,10001,&s);assert(s.charging);
 c=running();c.sample.charger.state_valid=0;stopped(c,10001);
 c=running();c.sample.charger.state_raw=9;stopped(c,10001);
 c=running();c.pump.enable=0;stopped(c,10001);
 c=running();c.pump.switching=0;stopped(c,10001);
 c=running();c.pump.protection.faults=NP_PUMP_WD_TIMEOUT;stopped(c,10001);
 c=running();c.pump_error=5;stopped(c,10001);
 c=running();c.pump_at=6999;stopped(c,10001);
 c=running();c.sample.gauge.current_ma=-1;stopped(c,10001);
 c=running();c.sample.gauge.full_flag=1;stopped(c,10001);
 c=running();c.sample.charger.state_raw=5;c.sample.charger.done=1;stopped(c,10001);
 c=running();c.sample.charger.external=0;stopped(c,10001);
 c=running();c.sample.charger.epoch=2;stopped(c,10001);
 c=running();c.sample.charger.generation=6;stopped(c,10001);
 c=running();c.sample.charger.fault=NP_FAULT_TRANSPORT;stopped(c,10001);
 c=running();c.meta[NP_META_ENABLED].value=0;stopped(c,10001);
 c=running();c.meta[NP_META_SUSPEND].value=1;stopped(c,10001);
 c=running();c.meta[NP_META_GATE].value=1;stopped(c,10001);
 c=running();c.adc_error=5;stopped(c,10001);
 c=running();c.jeita.vote.enabled=0;stopped(c,10001);
 c=running();c.jeita.vote.full=1;stopped(c,10001);
 c=running();c.buck.owned=0;stopped(c,10001);
 c=running();c.buck.state=NP_BUCK_PUMP_PREPARE_EXIT;stopped(c,10001);
 c=running();c.mode=8;stopped(c,10001);
 c=running();stopped(c,15001);
 c=running();c.mode=8;c.sample.charger.charging=1;
 np_control_snapshot(&c,10001,&s);assert(s.charging&&s.charger.charging);
 c=running();c.sample.charger.charging=1;np_control_link(&c,1,0);
 np_control_snapshot(&c,10001,&s);
 assert(!(s.valid&NP_BIT(NP_CHARGING))&&!s.external&&!s.charging);
 puts("PASS 29 power-source cases: current pump charging, raw buck unchanged and original invalidation");
 return 0;
}
