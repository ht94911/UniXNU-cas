/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_BUCK_H
#define NEZHA_BUCK_H
#include "NezhaPd.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NP_BUCK_ICL 0x2008u
#define NP_BUCK_FCC 0x2005u
enum np_buck_state { NP_BUCK_IDLE,NP_BUCK_SET_ICL,NP_BUCK_CHECK_ICL,
 NP_BUCK_SET_FCC,NP_BUCK_CHECK_FCC,NP_BUCK_RUNNING,NP_BUCK_RESTORE_FCC_READ,
 NP_BUCK_RESTORE_FCC_WRITE,NP_BUCK_RESTORE_FCC_CHECK,NP_BUCK_RESTORE_ICL_READ,
 NP_BUCK_RESTORE_ICL_WRITE,NP_BUCK_RESTORE_ICL_CHECK,NP_BUCK_FINISHED,NP_BUCK_UNCERTAIN,
 NP_BUCK_FCC_INITIALIZE,NP_BUCK_FCC_BASELINE,NP_BUCK_FCC_OBSERVE,NP_BUCK_FCC_REDUCE };
enum np_buck_backend { NP_BUCK_READBACK,NP_BUCK_OS4_SETTER };
enum np_buck_confirmation { NP_BUCK_CONFIRM_NONE,NP_BUCK_CONFIRM_BASELINE,
 NP_BUCK_CONFIRM_CURRENT,NP_BUCK_CONFIRM_REDUCTION_ACK };
enum np_buck_block { NP_BUCK_READY,NP_BUCK_NOT_ARMED,NP_BUCK_STALE,
 NP_BUCK_SOURCE,NP_BUCK_TEMPERATURE,NP_BUCK_VOLTAGE,NP_BUCK_PUMP_ACTIVE,
 NP_BUCK_BASELINE,NP_BUCK_CAPS,NP_BUCK_FAULT,NP_BUCK_EFFECT_UNKNOWN,NP_BUCK_FEEDBACK };
typedef struct {
 uint64_t epoch,attachment,now_ms,sampled_ms;
 uint32_t valid,online,real_type,boost_enabled,gate_enabled,input_suspended;
 uint32_t charge_enabled,pmic_charging,pump_known_off,term_mv,icl_ma,fcc_ma;
 int32_t battery_mv,temperature_centi_c;
 uint32_t vbus_mv,fault,full;
 const NpPdCaps *pd;
 uint64_t gauge_ms;
 int32_t battery_ma;
} NpBuckFacts;
typedef struct {
 uint64_t id,epoch,deadline_ms;
 uint32_t property,value;
 uint8_t write;
} NpBuckCommand;
typedef struct {
 uint64_t epoch,attachment,next_id,request_id,lease_until;
 uint32_t state,blocked,baseline_icl,baseline_fcc,target_icl,target_fcc;
 uint32_t restore_value,last_value,owned,failures,changes,restore_icl,restore_fcc;
 uint8_t armed,pending,stop,failed_write,unverified_effect,wire_accepted;
 NpBuckCommand command;
 uint64_t feedback_after_ms,feedback_last_ms;
 int64_t feedback_sum;
 uint32_t backend,confirmation,feedback_samples;
 int32_t feedback_min,feedback_max,feedback_baseline_ma,feedback_target_ma;
} NpBuck;
void np_buck_init(NpBuck *);
/* Normal charging only, within FDT limits. READBACK caps both limits at1500mA.
 * OS4_SETTER keeps the real ICL, initializes500mA, and observes one1000mA FCC
 * step. It reports current response and reduction ACK, never register readback.
 * Both require pump-off/source/temperature evidence; no voltage, enable,
 * protection, authentication, pump or calibration writes. */
int np_buck_arm(NpBuck *,uint64_t now_ms,uint32_t lease_ms);
void np_buck_stop(NpBuck *);
int np_buck_next(NpBuck *,const NpBuckFacts *,NpBuckCommand *);
/* A write ACK alone never advances to the next increase. The selected backend
 * requires its actual readback or fresh current response. No timeout replay. */
int np_buck_complete(NpBuck *,uint64_t id,int status,uint32_t read_value);
#ifdef __cplusplus
}
#endif
#endif
