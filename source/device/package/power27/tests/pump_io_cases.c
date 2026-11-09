/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaGaugeBus.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
static unsigned writes,waited,fail_readback,stuck,denied,permit_usbovp,reg16_reserved_zero;
static uint8_t regfile[256];
static int acquire(void *p){(void)p;return denied?EACCES:0;}
static void release(void *p,int stopped){(void)p;assert(stopped);}
static void pause(void *p,uint32_t us){(void)p;waited+=us;}
int np_gpi_open(NpGpi *g,uint64_t pa,uint32_t n,uint32_t ee,uint32_t i,uint32_t se,uint32_t proto)
{assert(pa==0xa00000&&n==0x60000&&ee==0x10000&&i==1&&se==5&&proto==3);g->open=g->owned=true;return 0;}
int np_gpi_close(NpGpi *g){g->open=g->owned=false;return 0;}
int np_gpi_i2c(NpGpi *g,const PgpiI2cClock *c,uint8_t addr,const uint8_t *w,unsigned nw,uint8_t *r,unsigned nr)
{
 (void)c;assert(g->open&&addr==0x6e&&nw>=1);
 if(nr){assert(nw==1&&w[0]+nr<=256);for(unsigned n=0;n<nr;n++)assert(w[0]+n<0x23||w[0]+n>0x26);memcpy(r,regfile+w[0],nr);return 0;}
 assert(nw==2);assert(w[0]==0x15||w[0]==0x16||w[0]==0x0b||(permit_usbovp&&w[0]==3));++writes;
 if(w[0]==3)assert(!((regfile[3]^w[1])&0xf0));
 if(!fail_readback) {
  regfile[w[0]]=w[1];
  /* SC8585's unsupported IBAT/TSBAT scan bits read as zero on this device. */
  if(w[0]==0x16&&reg16_reserved_zero)regfile[0x16]&=(uint8_t)~6u;
 }
 if(w[0]==0x0b&&!(w[1]&0x80)&&!stuck)regfile[0x0a]&=(uint8_t)~2u;
 return 0;
}
static NpGaugeBus setup(void){NpGaugeBus b={0};b.acquire=acquire;b.release=release;b.pause_us=pause;b.gpii=1;b.clock=(PgpiI2cClock){2,12,11,22,0};b.pump_registers_owned=1;
 memset(regfile,0,sizeof(regfile));writes=waited=fail_readback=stuck=denied=permit_usbovp=reg16_reserved_zero=0;
 regfile[0x6e]=0x85;regfile[0x15]=1;regfile[0x16]=0xff;
 regfile[0x17]=0;regfile[0x18]=100;regfile[0x19]=3;regfile[0x1a]=32;
 regfile[0x21]=13;regfile[0x22]=72;regfile[0x27]=0;regfile[0x28]=60;return b;}
static void sc8585_scan_readback(void)
{
 NpPump p;NpGaugeBus b=setup();reg16_reserved_zero=1;regfile[0x16]=0xf9;
 for(unsigned round=0;round<3;++round) {
  const unsigned before=writes;
  int error=np_pump_bus_read(&b,&p);
  if(error)fprintf(stderr,"SC8585 first ADC read: error=%d reg15=%02x reg16=%02x\n",error,regfile[0x15],regfile[0x16]);
  assert(!error&&writes==before+2&&b.pump_adc_owned);
  assert(p.chip==0x85&&p.bus_mv==5000&&p.battery_mv==4250&&p.die_centi_c==3000);
  assert(regfile[0x16]==0x70); /* Preserve the three unrelated scan bits. */
  assert(!np_pump_bus_restore(&b)&&!b.pump_adc_owned&&writes==before+4);
  assert(regfile[0x15]==1&&regfile[0x16]==0xf9);
 }
 puts("PASS SC8585 first and repeated ADC acquisition with unsupported scan bits reading zero");
}
static void adc_readback_after_failure(void)
{
 NpPump p;NpGaugeBus b=setup();fail_readback=1;
 assert(np_pump_bus_read(&b,&p)==EIO&&b.pump_adc_owned);
 const unsigned before=writes;
 int error=np_pump_bus_read(&b,&p);
 if(error!=EIO)fprintf(stderr,"Unconfirmed ADC configuration accepted on next read: error=%d valid=%u\n",error,p.controls_valid);
 assert(error==EIO&&!p.controls_valid&&writes==before);
 /* A later hardware read can confirm the original effect without rewriting it. */
 regfile[0x15]=0x88;regfile[0x16]=0x76;fail_readback=0;
 assert(!np_pump_bus_read(&b,&p)&&p.controls_valid&&p.bus_mv==5000&&writes==before);
 puts("PASS failed ADC readback remains invalid until actual configuration is observed; no write replay");
}
int main(int argc,char **argv){
 if(argc==2) {
  if(!strcmp(argv[1],"sc8585-scan")){sc8585_scan_readback();return 0;}
  if(!strcmp(argv[1],"adc-unconfirmed")){adc_readback_after_failure();return 0;}
  return 2;
 }
 NpPump p;NpGaugeBus b=setup();
 assert(!np_pump_bus_read(&b,&p)&&writes==2&&waited==20000&&p.chip==0x85);
 assert(p.controls_valid&&p.switching_valid&&!p.switching&&!p.enable&&p.bus_ma==187&&p.bus_mv==5000&&p.battery_mv==4250&&p.die_centi_c==3000);
 assert(!(p.valid&8)&&regfile[0x15]==0x88&&regfile[0x16]==0x76);
 assert(!np_pump_bus_read(&b,&p)&&writes==2);
 assert(!np_pump_bus_restore(&b)&&!b.pump_adc_owned&&regfile[0x15]==1&&regfile[0x16]==0xff);
 b=setup();regfile[0x0b]=0x80;regfile[0x0a]=2;
 assert(!np_pump_bus_read(&b,&p)&&p.initial_enable&&!p.enable&&!p.switching&&writes==3);
 b=setup();b.pump_initialized=1;regfile[0x0b]=0x80;regfile[0x0a]=2;
 assert(!np_pump_bus_read(&b,&p)&&p.enable&&p.switching&&!writes&&!b.pump_adc_owned);
 assert(!np_pump_bus_disable(&b)&&!regfile[0x0b]&&!(regfile[0x0a]&2));
 b=setup();regfile[0x6e]=0x41;assert(np_pump_bus_read(&b,&p)==ENODEV&&!writes);
 b=setup();denied=1;assert(np_pump_bus_read(&b,&p)==EACCES&&!writes);
 b=setup();fail_readback=1;assert(np_pump_bus_read(&b,&p)==EIO&&b.pump_adc_owned);
 b=setup();regfile[0x0b]=0x80;regfile[0x0a]=2;stuck=1;assert(np_pump_bus_disable(&b)==ETIMEDOUT&&waited==50000);
 b=setup();regfile[0x0b]=0x80;regfile[0x0a]=2;stuck=1;assert(np_pump_bus_read(&b,&p)==ETIMEDOUT&&b.pump_init_failed);
 unsigned old_writes=writes;assert(np_pump_bus_read(&b,&p)==EIO&&writes==old_writes);
 b=setup();regfile[3]=0x4f;assert(!np_pump_bus_read(&b,&p));permit_usbovp=1;
 uint8_t observed=255,attempted=0;old_writes=writes;
 assert(!np_pump_bus_usb_ovp(&b,15,0,&observed,&attempted)&&observed==0&&attempted==1);
 assert(writes==old_writes+1&&regfile[3]==0x40&&b.pump_usb_ovp_owned);
 regfile[0x19]=5;regfile[0x1a]=160;old_writes=writes; /* 9V */
 assert(np_pump_bus_usb_ovp(&b,0,15,&observed,&attempted)==EBUSY&&!attempted&&writes==old_writes);
 assert(b.pump_usb_ovp_owned&&np_pump_bus_restore(&b)==EBUSY);
 regfile[0x19]=3;regfile[0x1a]=32; /* 5V */
 assert(!np_pump_bus_usb_ovp(&b,0,15,&observed,&attempted)&&observed==15&&attempted==1);
 assert(regfile[3]==0x4f&&!b.pump_usb_ovp_owned);
 old_writes=writes;assert(np_pump_bus_usb_ovp(&b,0,15,&observed,&attempted)==EACCES&&!attempted&&writes==old_writes);
 b=setup();regfile[3]=0x4f;assert(!np_pump_bus_read(&b,&p));permit_usbovp=1;
 regfile[0x0b]=0x80;old_writes=writes;
 assert(np_pump_bus_usb_ovp(&b,15,0,&observed,&attempted)==EBUSY&&!attempted&&writes==old_writes);
 b=setup();regfile[3]=0x4f;assert(!np_pump_bus_read(&b,&p));permit_usbovp=1;fail_readback=1;
 assert(np_pump_bus_usb_ovp(&b,15,0,&observed,&attempted)==EIO&&attempted&&b.pump_usb_ovp_failed&&b.pump_usb_ovp_owned);
 old_writes=writes;assert(np_pump_bus_usb_ovp(&b,15,0,&observed,&attempted)==EACCES&&!attempted&&writes==old_writes);
 sc8585_scan_readback();adc_readback_after_failure();
 puts("PASS exact SC8585 reads/ADC ownership/restore/disabled confirmation; reserved channels avoided; errors retained");return 0;}
