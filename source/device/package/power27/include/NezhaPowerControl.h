/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_POWER_CONTROL_H
#define NEZHA_POWER_CONTROL_H
#include "NezhaPower.h"
#include "NezhaCharge.h"
#include "NezhaBuck.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { NP_META_ICL,NP_META_FCC,NP_META_TERM,NP_META_ENABLED,NP_META_BOOST,
 NP_META_GATE,NP_META_SUSPEND,NP_META_TYPE,NP_META_VERIFY,NP_META_PDOS,
 NP_META_TYPEC,NP_META_PPS,NP_META_DATA_ROLE,NP_META_COUNT };
typedef struct { uint32_t value;int32_t error;uint64_t at_ms,retry_ms;uint8_t valid; } NpMeta;
typedef struct {
 uint64_t id,attachment;uint32_t property,value;uint16_t reply_bytes;
 uint8_t write,control,index;
} NpAuxCommand;
typedef struct {
 NpBuck buck;NpPdCaps caps;NpMeta meta[NP_META_COUNT];
 NpSnapshot sample;NpPump pump;NpAuxCommand pending_command;
 uint64_t epoch,attachment,next_id,pump_at,last_armed_attachment;
 uint32_t mode,cursor,pump_error,changes,queries,errors;
 uint8_t pending,stopping,allow_control;
} NpPowerControl;
typedef struct {
 uint64_t id,at_ms,generation,attachment;
 int32_t soc,battery_mv,battery_ma,temperature,remaining,external,charging,vbus_uv,buck_ua;
 uint32_t valid,pump_valid,pump_error,pump_mode,pump_enable,pump_ibus_ma;
 uint32_t real_type,icl_ma,fcc_ma,meta_valid,state,blocked,owned;
 uint32_t baseline_icl,baseline_fcc,target_icl,target_fcc,control_errors;
 uint32_t backend,confirmation;
 int32_t feedback_baseline_ma,feedback_target_ma;
 uint32_t confirmed_vbus_mv,usb_ovp_baseline,usb_ovp_raw,protection_restored;
} NpPowerHistoryRecord;
void np_control_init(NpPowerControl *,unsigned mode);
void np_control_link(NpPowerControl *,uint64_t epoch,int up);
void np_control_source_event(NpPowerControl *);
void np_control_sample(NpPowerControl *,const NpSnapshot *,const NpPump *,int,uint64_t);
int np_control_next(NpPowerControl *,uint64_t,NpAuxCommand *);
int np_control_complete(NpPowerControl *,const NpAuxCommand *,int,const uint8_t *,size_t,uint64_t);
int np_control_write_allowed(NpPowerControl *,const NpAuxCommand *,uint64_t);
int np_control_register_allowed(NpPowerControl *,const NpAuxCommand *,uint64_t);
void np_control_stop(NpPowerControl *);
void np_control_facts(const NpPowerControl *,uint64_t,NpBuckFacts *);
#ifdef __cplusplus
}
#endif
#endif
