/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Independent evaluation of the matching OS4 APDO/mode selection. Values
 * describe an advertised capability, not a measured power or charge grant. */
#include "NezhaAdapterPolicy.h"
#include <string.h>
int np_adapter_stock_support100w(unsigned configured,unsigned boost,unsigned sku)
{
 if(configured>3||boost>1||sku>1)return -1;
 return boost||configured==3||(configured==1&&!sku)||(configured==2&&sku);
}
int np_adapter_select(const NpPdCaps *caps,uint64_t epoch,unsigned type,
                       int support_100w,NpAdapterChoice *out)
{
 if(!out)return -1;
 memset(out,0,sizeof(*out));
 if(!caps||!caps->valid||caps->invalid_mask||!epoch||caps->epoch!=epoch||
    (type!=11&&type!=12)||support_100w>1||support_100w<-1||
    (type==11&&support_100w<0))return -1;
 uint32_t maximum[2]={0},current[2]={0},index[2]={0};
 unsigned found=0;
 /* The vendor PD class first compacts variable-voltage rows. Preserve
  * their original catalog indices while retaining its order/tie behavior. */
 for(unsigned i=0;i<NP_PD_ENTRIES;++i){
  const NpPdEntry *p=&caps->entry[i];
  if(!(caps->present_mask&(1u<<i))||p->type!=NP_PD_APDO||p->min_mv==p->max_mv)continue;
  for(unsigned mode=0;mode<2;++mode){
   const unsigned ratio=mode?4:2;
   const unsigned ceiling=mode?15600:12000;
   if((mode&&type==11&&!support_100w)||p->min_mv>(mode?12000u:6000u)||
      p->max_mv<(mode?18000u:9000u))continue;
   uint32_t battery=p->max_ma*ratio;
   if(battery>ceiling)battery=ceiling;
   if(battery>=maximum[mode]&&p->max_ma>=current[mode]){
    maximum[mode]=battery;current[mode]=p->max_ma;index[mode]=i;found|=1u<<mode;
   }
  }
 }
 if(!found)return -1;
 const unsigned selected=maximum[1]>maximum[0]?1:0;
 const NpPdEntry *p=&caps->entry[index[selected]];
 out->epoch=epoch;out->sampled_ms=caps->at_ms;
 out->ratio=selected?4:2;out->pump_mode=selected?0:1;out->pdo_index=index[selected];
 out->minimum_mv=p->min_mv;out->maximum_mv=p->max_mv;out->adapter_ma=current[selected];
 out->single_battery_ma=selected?15600:11000;out->maximum_battery_ma=maximum[selected];
 out->initial_headroom_mv=selected?600:300;out->valid=1;return 0;
}
