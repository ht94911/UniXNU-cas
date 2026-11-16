/* SPDX-License-Identifier: BSD-2-Clause */
#include "../../package/baseband27/platform/NbPower.h"
#include "../../package/baseband27/platform/NbScm.h"
#include "../../package/baseband27/platform/NbPas.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t regs[5][0x100000/4],ram[256];static unsigned locked,writes,trigger,auto_ack=1;
static int lock(void *p){(void)p;assert(!locked);locked=1;return 0;}
static void unlock(void *p){(void)p;assert(locked);locked=0;}
static int read32(void *p,unsigned d,uint32_t at,uint32_t *v){(void)p;assert(d<5&&at<0x100000);*v=regs[d][at/4];return 0;}
static int read64(void *p,unsigned d,uint32_t at,uint64_t *v){uint32_t lo,hi;read32(p,d,at,&lo);read32(p,d,at+4,&hi);*v=lo|(uint64_t)hi<<32;return 0;}
static int write32(void *p,unsigned d,uint32_t at,uint32_t v){
    (void)p;assert(locked);writes++;
    if(d==1&&at==0xd08)regs[d][0xd04/4]&=~v;else regs[d][at/4]=v;
    if(d==1&&at==0xd00+0x2a0+0x24&&(v&(1u<<24))){
        trigger++;if(auto_ack){regs[d][0xd04/4]|=2;regs[d][(0xd00+0x2a0+0x40)/4]=0x10100;}
    }
    if(d==4&&at==0xc)ram[0x100/4]=0;
    return 0;
}
static int barrier(void *p){(void)p;return 0;}
static void event(void *p,uint32_t id){(void)p;(void)id;}
static void w16(uint8_t *p,uint32_t v){p[0]=v;p[1]=v>>8;}
static void w32(uint8_t *p,uint32_t v){w16(p,v);w16(p+2,v>>16);}
static void init(Sm8850Resources *c,NbPower *power){
    memset(regs,0,sizeof regs);memset(c,0,sizeof *c);writes=trigger=0;auto_ack=1;
    Sm8850ResourceIo io={0,lock,unlock,read32,write32,read64,barrier};assert(!sm8850_resources_init(c,&io));
    regs[1][0]=0x40300;regs[1][3]=0x800c8104;regs[1][(0xd00+0x2a0+0x28)/4]=1;regs[1][0xd00/4]=5;
    static uint8_t db[0x20000];memset(db,0,sizeof db);w32(db+4,QCOM_CMD_DB_MAGIC);w16(db+8,1);w16(db+10,0);w16(db+12,1024);w16(db+14,NB_POWER_RESOURCES);
    const char *names[]={"xo.lvl","cx.lvl","mss.lvl","IP0","MC0","SH0","SH1","SN0","SN3","CE0","CN0"};
    for(unsigned i=0;i<NB_POWER_RESOURCES;i++){
        uint8_t *e=db+144+24*i,*aux=db+144+1024+32*i;memcpy(e,names[i],strlen(names[i]));w32(e+16,0x10000+4*i);w16(e+20,8);w16(e+22,32*i);
        if(i<3){w16(aux,0);w16(aux+2,320);w16(aux+4,384);w16(aux+6,512);}else{w32(aux,1000);w16(aux+4,16);aux[6]=1;}
    }
    memset(ram,0,sizeof ram);ram[0]=0x4d41494c;ram[1]=1;ram[9]=ram[10]=ram[11]=ram[5]=ram[12]=0xffff;ram[13]=0x100;ram[14]=0x100;
    assert(!sm8850_ipcc_register(c,0,event,0));assert(!nb_power_init(power,c,ram,db,sizeof db));
}
static uint64_t clock_ms;
static int drive(NbPower *p,unsigned kind,int on){
    int result=1;for(unsigned i=0;i<1000&&result==1;i++)result=nb_power_set(p,kind,on,clock_ms++);return result;
}
static void smmu_count_test(void){
    Sm8850Resources common;NbPower p;init(&common,&p);
    regs[0][0x20/4]=1275166324;regs[0][0x24/4]=1610612817;regs[0][0x28/4]=21009;regs[0][0]=0x406;
    for(unsigned i=0;i<116;i++)regs[0][(0xc00+4*i)/4]=0x20000;
    Sm8850SmmuAssignment a={8,0xfd00,{13,14},{0x4a0,0x4a2},2};
    lock(0);assert(!sm8850_smmu_check_assignment(&common,&a));
    a.count=5;assert(sm8850_smmu_check_assignment(&common,&a)==Sm8850Invalid);a.count=2;
    regs[0][(0x800+20*4)/4]=0x800004a0;regs[0][(0xc00+20*4)/4]=3;
    assert(sm8850_smmu_check_assignment(&common,&a)==Sm8850Conflict);unlock(0);
    puts("PASS shared SMMU: explicit two-stream assignment and collision rejection");
}
static void power_test(void){
    Sm8850Resources common;NbPower p;init(&common,&p);clock_ms=0;
    assert(!drive(&p,NB_POWER_IPA,1));uint32_t ipa[NB_POWER_RESOURCES];memcpy(ipa,p.sent,sizeof ipa);
    assert(!common.owner[NB_RES_TCS]&&regs[1][0xd00/4]==5);
    assert(!drive(&p,NB_POWER_MSS,1)&&p.load_state);assert(!drive(&p,NB_POWER_MSS,0));
    assert(!memcmp(ipa,p.sent,sizeof ipa)); // dropping temporary votes preserves IPA's votes
    unsigned sent=trigger;assert(!drive(&p,NB_POWER_IPA,1)&&trigger==sent);
    init(&common,&p);auto_ack=0;clock_ms=0;assert(drive(&p,NB_POWER_IPA,1)==NB_TIMEOUT);
    assert(common.owner[NB_RES_TCS]==NB_OWNER&&p.lease);sent=trigger;
    assert(nb_power_set(&p,NB_POWER_IPA,1,2000)==NB_TIMEOUT&&trigger==sent);
    puts("PASS power: shared leases, acknowledged votes, retained timeout, IPA/MSS aggregation");
}
struct scm_res{uint64_t a0,a1,a2,a3;};
#define SCM_FN(s,c) ((UINT64_C(1)<<30)|(UINT64_C(2)<<24)|((s)<<8)|(c))
#define SCM_ARGS(n) (n)
#define SCM_RW 2u
#define TRUE 1
static unsigned secure_calls,cleans,secure_fail;static uint64_t functions[4],last_info,last_args[4];
static struct scm_res pas_reply;static int use_pas_reply;
static void *scm_page;static int sync_error;
static void scm_smc(uint64_t fn,uint64_t info,uint64_t a,uint64_t b,uint64_t c,uint64_t d,struct scm_res *r){
    assert(secure_calls<4);functions[secure_calls++]=fn;last_info=info;last_args[0]=a;last_args[1]=b;last_args[2]=c;last_args[3]=d;
    *r=use_pas_reply?pas_reply:(struct scm_res){0,secure_fail?1:0,123,0};
}
static int nb_native_low_sync(void *cpu,uint64_t pa,uint64_t bytes){
    if(cpu!=scm_page||pa!=0x90000000||bytes!=4096)return NB_OWNERSHIP;
    if(sync_error)return sync_error;
    cleans++;return NB_OK;
}
#include "../../package/baseband27/integration/NbScmOwner.inc"
static void scm_test(void){
    static uint8_t page[4096] __attribute__((aligned(4096)));int state=0;uint64_t handle=0;
    scm_page=page;sync_error=NB_OWNERSHIP;
    assert(nezha_bb_scm_assign(page,0x90000000,&state,&handle,(NbRange){0x85000000,0x1000},1,1)==NB_OWNERSHIP);
    assert(!secure_calls&&!cleans&&!state&&!handle);sync_error=0;
    assert(nezha_bb_scm_assign(page,0x90001000,&state,&handle,(NbRange){0x85000000,0x1000},1,1)==NB_OWNERSHIP&&!secure_calls);
    assert(nezha_bb_scm_pas(99,4,0,0)==NB_ARGUMENT&&!secure_calls);
    assert(nezha_bb_scm_pas(1,4,0x100000000ULL,0)==NB_ARGUMENT&&!secure_calls);
    assert(!nezha_bb_scm_pas(5,4,0,0)&&secure_calls==1&&functions[0]==SCM_FN(2,5));secure_calls=0;
    uint64_t raw[4]={0};assert(nezha_bb_scm_query(6,4,raw)==NB_ARGUMENT&&!secure_calls);
    assert(nezha_bb_scm_query(7,15,raw)==NB_ARGUMENT&&!secure_calls);
    assert(nezha_bb_scm_query(9,0x26,0)==NB_ARGUMENT&&!secure_calls);
    use_pas_reply=1;pas_reply=(struct scm_res){0,1,0x8aa00000,0x98b00000};
    assert(!nezha_bb_scm_query(7,4,raw) && raw[1]==1 && raw[2]==0x8aa00000 && raw[3]==0x98b00000);
    assert(last_info==SCM_ARGS(1) && last_args[0]==4 && !last_args[1] && !last_args[2] && !last_args[3]);
    pas_reply.a0=0xffcfffb8;assert(nezha_bb_scm_query(9,0x26,raw)==NB_IO && raw[0]==0xffcfffb8);
    secure_calls=0;pas_reply=(struct scm_res){0,(uint64_t)-22,0,0};
    assert(nezha_bb_scm_pas(6,4,0,0)==NB_PAS_EINVAL && secure_calls==1);
    secure_calls=0;pas_reply=(struct scm_res){1,(uint64_t)-22,0,0};
    assert(nezha_bb_scm_pas(6,4,0,0)==NB_IO && secure_calls==1);
    secure_calls=0;use_pas_reply=0;
    assert(!nezha_bb_scm_assign(page,0x90000000,&state,&handle,(NbRange){0x85000000,0x1000},1,1));
    assert(state==1&&handle==123&&secure_calls==2&&cleans==1);
    assert(functions[0]==SCM_FN(0xc,0x1e)&&functions[1]==SCM_FN(0xc,0x16));
    assert(last_args[0]==0x90000040&&last_args[1]==16&&last_args[2]==0x90000000&&last_args[3]==0x90000100);
    assert(page[0]==3&&page[128]==15&&page[132]==6&&page[152]==3&&page[156]==6&&page[272]==48);
    secure_calls=0;assert(!nezha_bb_scm_assign(page,0x90000000,&state,&handle,(NbRange){0x85000000,0x1000},0,1)&&secure_calls==1);
    assert(page[0]==15&&page[4]==3&&page[128]==3&&page[256]==8);
    state=0;secure_calls=0;secure_fail=1;assert(nezha_bb_scm_assign(page,0x90000000,&state,&handle,(NbRange){0x85000000,0x1000},1,0)<0);
    assert(state<0&&secure_calls==1);assert(nezha_bb_scm_assign(page,0x90000000,&state,&handle,(NbRange){0x85000000,0x1000},1,0)<0&&secure_calls==1);
    puts("PASS SCM owner wrappers: validated page, existing calls, retained failure");
}
int main(void){power_test();smmu_count_test();scm_test();return 0;}
