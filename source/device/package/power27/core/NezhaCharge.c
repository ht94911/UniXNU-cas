/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host transaction/ownership layer around STOCK charger policy. This does not
 * replace MCA's battery-vendor curves, FFC, authentication or thermal voters.
 * Those must supply verified limits and hardware operations at integration. */
#include "NezhaCharge.h"
#include <string.h>

int np_pps_encode(uint32_t mv, uint32_t ma, uint32_t *wire)
{
    /* Actual OS4 qcom_adsp_pd_protocol floors mV/20 and mA/50. Stock
     * vbat*ratio+delta is not necessarily a multiple of20mV. The caller's
     * policy and contract check must use the same encoded operating point. */
    if (!wire || mv / 20 == 0 || ma / 50 == 0 || mv / 20 > 65535 || ma / 50 > 65535) return -1;
    *wire = (mv / 20 << 16) | (ma / 50); return 0;
}
void np_charge_init(NpChargePolicy *p) { memset(p, 0, sizeof(*p)); }
static int qualified(const NpChargeFacts *f)
{
    return f->epoch && f->profile && f->wired && f->sink && f->external && f->sample_valid &&
        f->now_ms >= f->sample_ms && f->now_ms - f->sample_ms <= 1000 && f->protocol_verified &&
        f->battery_authenticated && f->adapter_authenticated && f->cable_authenticated &&
        f->protections_verified && f->autonomous_fault_shutdown && f->limits_valid &&
        f->remote_ready && !f->suspend && !f->full && !f->fault;
}
static int target(const NpChargeFacts *f,uint32_t *mv,uint32_t *ma)
{
    uint32_t wire;
    if (np_pps_encode(f->requested_mv, f->requested_ma, &wire)) return 0;
    *mv=(wire>>16)*20;*ma=(wire&65535)*50;
    if (!f->pps_max_ma || !f->cable_max_ma || !f->input_limit_ma || !f->thermal_input_ma ||
        !f->battery_limit_ma || !f->board_power_mw || f->board_power_mw > 90000 ||
        !f->requested_battery_ma || f->requested_battery_ma > f->battery_limit_ma) return 0;
    if (f->requested_mv < f->pps_min_mv || f->requested_mv > f->pps_max_mv ||
        *mv < f->pps_min_mv || *mv > f->pps_max_mv ||
        f->requested_ma > f->pps_max_ma || f->requested_ma > f->cable_max_ma ||
        f->requested_ma > f->input_limit_ma || f->requested_ma > f->thermal_input_ma ||
        (f->usb_data_budget_ma && f->requested_ma > f->usb_data_budget_ma)) return 0;
    return (uint64_t)f->requested_mv * f->requested_ma <= (uint64_t)f->board_power_mw * 1000;
}
static int emit(NpChargePolicy *p, const NpChargeFacts *f, NpChargeCommand *c, uint8_t op)
{
    if (p->request_id == UINT64_MAX || f->now_ms > UINT64_MAX - 1000) {
        p->state = NP_POLICY_QUARANTINE; return -1;
    }
    memset(c, 0, sizeof(*c));
    c->operation = op; c->epoch = p->epoch; c->profile = p->profile;
    c->request_id = ++p->request_id; c->mv = p->mv; c->ma = p->ma; c->battery_ma = p->battery_ma;
    p->action = op; p->waiting = 1; p->issued_ms = f->now_ms; p->deadline_ms = f->now_ms + 1000;
    return 1;
}
int np_charge_step(NpChargePolicy *p, const NpChargeFacts *f, NpChargeCommand *c)
{
    if (!p || !f || !c) return -1;
    memset(c, 0, sizeof(*c));
    if (p->state == NP_POLICY_QUARANTINE) return -1;
    if (p->epoch && p->epoch != f->epoch) {
        /* Remote reset invalidates ACKs and our control lease. Host cannot
         * claim to have disabled unreachable hardware. Its autonomous
         * watchdog/protections are a mandatory entry condition above. */
        p->waiting = 0; p->last_error = -2;
        p->state = p->owned ? NP_POLICY_QUARANTINE : NP_POLICY_FIRMWARE;
        p->epoch = f->epoch;
        return p->owned ? -1 : 0;
    }
    uint32_t target_mv=0,target_ma=0;
    const int target_valid=target(f,&target_mv,&target_ma);
    /* Invalidate an in-flight enable/request immediately on a new protection
     * condition; its late ACK must never re-enable the pump. The adapter must
     * serialize hardware operations in request-ID order. */
    if (p->owned && p->state < NP_POLICY_STOP &&
        (!qualified(f) || f->profile != p->profile || !target_valid ||
         target_mv != p->mv || target_ma != p->ma || f->requested_battery_ma != p->battery_ma)) {
        p->waiting = 0; p->state = NP_POLICY_STOP;
    }
    if (p->waiting) {
        if (f->now_ms < p->deadline_ms) return 0;
        p->waiting = 0; p->last_error = -3;
        if (p->action == NP_ACTION_DISABLE_PUMP || p->action == NP_ACTION_RELEASE_OWN_VOTES) {
            p->state = NP_POLICY_QUARANTINE; return -1;
        }
        p->state = NP_POLICY_STOP;
    }
    switch (p->state) {
    case NP_POLICY_FIRMWARE:
        if (!qualified(f) || !target_valid || !f->pump_off_confirmed) return 0;
        p->epoch = f->epoch; p->profile = f->profile;
        p->mv = target_mv; p->ma = target_ma; p->battery_ma = f->requested_battery_ma;
        p->owned = 1; p->state = NP_POLICY_PREPARE;
        return emit(p, f, c, NP_ACTION_CONFIGURE_DISABLED_PUMP);
    case NP_POLICY_REQUEST:
        return emit(p, f, c, NP_ACTION_REQUEST_PPS);
    case NP_POLICY_VERIFY:
        return emit(p, f, c, NP_ACTION_VERIFY_CONTRACT);
    case NP_POLICY_ENABLE:
        /* A successful request is not proof of a negotiated contract. */
        if (!f->contract_valid || f->actual_mv != p->mv || f->actual_ma > p->ma || !f->actual_ma) {
            p->state = NP_POLICY_STOP; return emit(p, f, c, NP_ACTION_DISABLE_PUMP);
        }
        return emit(p, f, c, NP_ACTION_ENABLE_PUMP);
    case NP_POLICY_RUN:
        if (!f->contract_valid || !f->actual_ma || f->actual_ma > p->ma || f->actual_mv != p->mv ||
            !target_valid || target_mv != p->mv || target_ma != p->ma) {
            p->state = NP_POLICY_STOP; return emit(p, f, c, NP_ACTION_DISABLE_PUMP);
        }
        return 0;
    case NP_POLICY_STOP:
        if (!f->remote_ready) { p->state = NP_POLICY_QUARANTINE; return -1; }
        return emit(p, f, c, NP_ACTION_DISABLE_PUMP);
    case NP_POLICY_FALLBACK:
        if (!f->pump_off_confirmed) { p->state = NP_POLICY_QUARANTINE; return -1; }
        return emit(p, f, c, NP_ACTION_RELEASE_OWN_VOTES);
    default:
        p->state = NP_POLICY_QUARANTINE; return -1;
    }
}
int np_charge_ack(NpChargePolicy *p, uint64_t epoch, uint64_t id, int status, uint64_t now)
{
    if (!p || !p->waiting || epoch != p->epoch || id != p->request_id || now < p->issued_ms || now >= p->deadline_ms) return 0;
    p->waiting = 0; p->deadline_ms = 0; p->last_error = status;
    if (status) {
        p->state = (p->action == NP_ACTION_DISABLE_PUMP || p->action == NP_ACTION_RELEASE_OWN_VOTES) ?
            NP_POLICY_QUARANTINE : NP_POLICY_STOP;
        return -1;
    }
    switch (p->action) {
    case NP_ACTION_CONFIGURE_DISABLED_PUMP: p->state = NP_POLICY_REQUEST; break;
    case NP_ACTION_REQUEST_PPS: p->state = NP_POLICY_VERIFY; break;
    case NP_ACTION_VERIFY_CONTRACT: p->state = NP_POLICY_ENABLE; break;
    case NP_ACTION_ENABLE_PUMP: p->state = NP_POLICY_RUN; break;
    case NP_ACTION_DISABLE_PUMP: p->state = NP_POLICY_FALLBACK; break;
    case NP_ACTION_RELEASE_OWN_VOTES: p->state = NP_POLICY_FIRMWARE; p->owned = 0; break;
    default: p->state = NP_POLICY_QUARANTINE; return -1;
    }
    return 1;
}
static uint32_t lower(uint32_t a,uint32_t b){return a<b?a:b;}
int np_charge_adapter_step(NpChargePolicy *p,const NpChargeFacts *facts,
                           const NpPdCaps *caps,const NpCccv *curve,uint32_t battery_mv,
                           unsigned real_type,int support_100w,
                           NpAdapterChoice *choice,NpChargeCommand *command)
{
 if(!p||!facts||!choice||!command)return -1;
 NpChargeFacts f=*facts;
 const int selected=np_adapter_select(caps,f.epoch,real_type,support_100w,choice);
 if(selected||!curve||!curve->valid||(choice->ratio==4&&!curve->cycles_valid)||curve->epoch!=f.epoch||!f.attachment||curve->attachment!=f.attachment||
    f.now_ms<choice->sampled_ms||f.now_ms-choice->sampled_ms>3000||
    f.now_ms<curve->sampled_ms||f.now_ms-curve->sampled_ms>3000||
    battery_mv<3000||battery_mv>=4490||curve->current_max_ma<=0||
    !f.cable_max_ma||!f.input_limit_ma||!f.thermal_input_ma||!f.battery_limit_ma||
    !f.board_power_mw||f.board_power_mw>90000||
    (p->owned&&(p->adapter_ratio!=choice->ratio||p->adapter_attachment!=f.attachment))){
  f.limits_valid=0;return np_charge_step(p,&f,command);
 }
 const uint32_t initial=(battery_mv*choice->ratio+choice->initial_headroom_mv)/20*20;
 const uint32_t mv=p->owned?p->mv:initial;
 if(mv<3000||mv>22000){f.limits_valid=0;return np_charge_step(p,&f,command);}
 const uint32_t ma=lower(lower(choice->adapter_ma,f.cable_max_ma),
                         lower(f.input_limit_ma,f.thermal_input_ma));
 uint32_t request=lower(ma,(uint32_t)((uint64_t)f.board_power_mw*1000/mv));
 request-=request%50;
 if(f.usb_data_budget_ma)request=lower(request,f.usb_data_budget_ma/50*50);
 if(f.requested_ma)request=lower(request,f.requested_ma/50*50);
 uint32_t battery=lower((uint32_t)curve->current_max_ma,choice->single_battery_ma);
 battery=lower(battery,f.battery_limit_ma);
 /* The explicitly supplied test budget remains an upper bound as the
  * stock curve tapers. It is not replaced with the curve's high maximum. */
 if(f.requested_battery_ma)battery=lower(battery,f.requested_battery_ma);
 /* The same battery budget also bounds the adapter request through the
  * selected pump ratio. Clamp before the50mA wire quantization; a4A budget
  * permits1A for4:1 and2A for2:1, regardless of a larger APDO offer. */
 request=lower(request,battery/choice->ratio/50u*50u);
 battery=lower(battery,request*choice->ratio);
 if(battery<=1500){f.limits_valid=0;return np_charge_step(p,&f,command);}
 f.pps_min_mv=choice->minimum_mv;f.pps_max_mv=choice->maximum_mv;
 f.pps_max_ma=choice->adapter_ma;f.requested_mv=mv;f.requested_ma=request;
 f.requested_battery_ma=battery;
 const int result=np_charge_step(p,&f,command);
 if(result>0&&command->operation==NP_ACTION_CONFIGURE_DISABLED_PUMP){
  p->adapter_ratio=choice->ratio;p->adapter_attachment=f.attachment;
 }
 return result;
}
