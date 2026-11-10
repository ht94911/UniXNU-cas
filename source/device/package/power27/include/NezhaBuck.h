/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_BUCK_H
#define NEZHA_BUCK_H
#include "NezhaPd.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NP_BUCK_ICL 0x2008u
#define NP_BUCK_FCC 0x2005u
#define NP_BUCK_FIXED_PD 0x2101bu
#define NP_BUCK_TERM_VOLT 0x2007u
#define NP_BUCK_TERM_CURRENT 0x2006u
#define NP_BUCK_ENABLE 0x2003u
#define NP_BUCK_COMMAND_MS 1500u
/* A local SC8585 operation, never an MCA property. Value encodes
 * expected low nibble in bits8..11 and requested low nibble in bits0..3. */
#define NP_BUCK_USB_OVP 0xff000003u
enum np_buck_state { NP_BUCK_IDLE,NP_BUCK_SET_ICL,NP_BUCK_CHECK_ICL,
 NP_BUCK_SET_FCC,NP_BUCK_CHECK_FCC,NP_BUCK_RUNNING,NP_BUCK_RESTORE_FCC_READ,
 NP_BUCK_RESTORE_FCC_WRITE,NP_BUCK_RESTORE_FCC_CHECK,NP_BUCK_RESTORE_ICL_READ,
 NP_BUCK_RESTORE_ICL_WRITE,NP_BUCK_RESTORE_ICL_CHECK,NP_BUCK_FINISHED,NP_BUCK_UNCERTAIN,
 NP_BUCK_FCC_INITIALIZE,NP_BUCK_FCC_BASELINE,NP_BUCK_FCC_OBSERVE,NP_BUCK_FCC_REDUCE,
 NP_BUCK_PD_PROTECT,NP_BUCK_PD_REQUEST_9V,NP_BUCK_PD_OBSERVE_9V,
 NP_BUCK_PD_REQUEST_5V,NP_BUCK_PD_OBSERVE_5V,NP_BUCK_PD_RESTORE_PROTECTION,
 NP_BUCK_POLICY_BASELINE,NP_BUCK_POLICY_SYNC,NP_BUCK_POLICY_CHECK,
 NP_BUCK_POLICY_FCC_CHECK,NP_BUCK_POLICY_MONITOR,NP_BUCK_POLICY_DISABLE,
 NP_BUCK_POLICY_DISABLED_CHECK,NP_BUCK_POLICY_STOP_OBSERVE,
 NP_BUCK_POLICY_RESTORE_READ,NP_BUCK_POLICY_RESTORE_WRITE,NP_BUCK_POLICY_RESTORE_CHECK };
enum np_buck_backend { NP_BUCK_READBACK,NP_BUCK_OS4_SETTER,NP_BUCK_OS4_FIXED_PD,
 NP_BUCK_OS4_STOCK_POLICY };
enum np_buck_confirmation { NP_BUCK_CONFIRM_NONE,NP_BUCK_CONFIRM_BASELINE,
 NP_BUCK_CONFIRM_CURRENT,NP_BUCK_CONFIRM_REDUCTION_ACK };
enum np_buck_block { NP_BUCK_READY,NP_BUCK_NOT_ARMED,NP_BUCK_STALE,
 NP_BUCK_SOURCE,NP_BUCK_TEMPERATURE,NP_BUCK_VOLTAGE,NP_BUCK_PUMP_ACTIVE,
 NP_BUCK_BASELINE,NP_BUCK_CAPS,NP_BUCK_FAULT,NP_BUCK_EFFECT_UNKNOWN,NP_BUCK_FEEDBACK,
 NP_BUCK_PROTECTION,NP_BUCK_ROLE };
typedef struct {
 uint64_t epoch,attachment,now_ms,sampled_ms;
 uint32_t valid,online,real_type,boost_enabled,gate_enabled,input_suspended;
 uint32_t charge_enabled,pmic_charging,pump_known_off,term_mv,icl_ma,fcc_ma;
 int32_t battery_mv,temperature_centi_c;
 uint32_t vbus_mv,fault,full;
 const NpPdCaps *pd;
 uint64_t gauge_ms;
 int32_t battery_ma;
 uint64_t charger_ms;
 uint32_t data_role,pd_state,pump_protection_valid,usb_ovp_raw;
 int32_t pump_bus_mv,pump_die_centi_c;
 uint32_t policy_valid,policy_enabled,policy_fcc,policy_fv,policy_iterm;
} NpBuckFacts;
typedef struct {
 uint64_t id,epoch,deadline_ms;
 uint32_t property,value;
 uint8_t write;
} NpBuckCommand;
typedef struct {
 /* Stock mode uses ICL0, FV1 and enable3. Reserved slot2 has no supported
  * ITERM value; it must not be reported as a zero configuration. */
 uint32_t baseline[4],actual[4],target[4],index,seen;
 uint64_t stop_after_ms,stop_last_ms,confirm_until_ms;
 uint32_t stop_samples;
 uint8_t configured,check_stopping;
} NpBuckStock;
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
 uint64_t voltage_after_ms,voltage_last_ms;
 uint32_t voltage_samples,confirmed_vbus_mv,usb_ovp_baseline;
 uint8_t protection_restored,cleanup_failed;
 NpBuckStock stock;
} NpBuck;
void np_buck_init(NpBuck *);
/* Normal charging only, within FDT limits. READBACK caps both limits at1500mA.
 * OS4_SETTER keeps the real ICL, initializes500mA, and observes one1000mA FCC
 * step. It reports current response and reduction ACK, never register readback.
 * Both require pump-off/source/temperature evidence; no voltage, enable,
 * protection, authentication, pump or calibration writes. */
/* OS4_FIXED_PD additionally requests only fixed5/9V with a matching PDO and
 * independently confirmed sink-ready power state and UFP/DFP data role. A sole
 * SC8585 owner reads back the USB-OVP field before9V;
 * FCC stays<=1500mA, the pump remains off, and voltage/current are measured.
 * Exit reduces FCC, confirms5V, then restores the owned OVP nibble. */
int np_buck_arm(NpBuck *,uint64_t now_ms,uint32_t lease_ms);
void np_buck_stop(NpBuck *);
int np_buck_next(NpBuck *,const NpBuckFacts *,NpBuckCommand *);
/* A write ACK alone never advances to the next increase. The selected backend
 * requires its actual readback or fresh current response. No timeout replay. */
int np_buck_complete(NpBuck *,uint64_t id,int status,uint32_t read_value);
/* Shared sole-owner command creation; the stock policy uses the same broker,
 * deadlines and accepted-write tracking as the existing test backends. */
int np_buck_issue(NpBuck *,const NpBuckFacts *,NpBuckCommand *,unsigned,uint32_t,uint32_t);
int np_buck_stock_next(NpBuck *,const NpBuckFacts *,NpBuckCommand *);
int np_buck_stock_complete(NpBuck *,int,uint32_t);
/* A pending limit readback keeps its original write deadline. Only reads are
 * issued in this window, including when a stop arrives during confirmation. */
int np_buck_stock_confirming(const NpBuck *);
int np_buck_stock_write_allowed(const NpBuck *,const NpBuckFacts *,uint32_t,uint32_t);
#ifdef __cplusplus
}
#endif
#endif
