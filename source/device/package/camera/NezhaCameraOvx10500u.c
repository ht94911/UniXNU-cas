/* SPDX-License-Identifier: MIT */
#include "NezhaCameraOvx10500u.h"
#include "NezhaCameraControl.h"

/* Main sensor route from nezha's stock DT. All four stock OVX10500U
 * module variants probe slave 0x20 (8-bit) with a word at 0x300b. */
enum {
    OvxMaster = 1,
    OvxSlave = 0x10,
    OvxIdRegister = 0x300b,
    OvxStream = 0x0100,
};

bool nezha_camera_ovx10500u_control_plan(const struct NezhaCameraOvxControls *controls,
    struct NezhaCameraOvxControlPlan *plan)
{
    if (plan == NULL) return false;
    *plan = (struct NezhaCameraOvxControlPlan){0};
    if (controls == NULL || controls->exposure_lines == 0 || controls->frame_lines == 0 ||
        controls->exposure_lines > 0xffffffu - 10u || controls->frame_lines > 0xffffffu ||
        controls->gain_q8 < 256 || controls->gain_q8 > 255u * 256u) return false;

    /* Stock mode1 RealToRegisterGain quantizes at 1/128 up to 2x and
     * doubles that step at each following power of two. Integer Q8 keeps
     * the same representable settings in a kernel using general regs. */
    const uint16_t gain = nezha_camera_control_gain_q8(controls->gain_q8);
    const uint32_t required = controls->exposure_lines + 10;
    const uint32_t frame = controls->frame_lines > required ? controls->frame_lines : required;
    const uint32_t exposure = controls->exposure_lines;
    const uint16_t regs[11] = {0x3840,0x380e,0x380f,0x3500,0x3501,0x3502,
        0x3508,0x3509,0x350a,0x350b,0x350c};
    const uint8_t values[11] = {(uint8_t)(frame >> 16),(uint8_t)(frame >> 8),(uint8_t)frame,
        (uint8_t)(exposure >> 16),(uint8_t)(exposure >> 8),(uint8_t)exposure,
        (uint8_t)(gain >> 8),(uint8_t)gain,1,0,0};
    plan->exposure_lines = exposure;
    plan->frame_lines = frame;
    plan->analog_gain_q8 = gain;
    plan->digital_gain_q10 = 1024;
    plan->writes[0].reg = 0x3208;
    plan->writes[0].value = 0;
    for (unsigned i = 0; i < 11; ++i) {
        plan->writes[i + 1].reg = regs[i];
        plan->writes[i + 1].value = values[i];
    }
    plan->writes[12].reg = plan->writes[13].reg = 0x3208;
    plan->writes[12].value = 0x10;
    plan->writes[13].value = 0xa0;
    return true;
}

enum NezhaCameraCciStatus nezha_camera_ovx10500u_apply_controls(
    const struct NezhaCameraRegisterIo *cci, const struct NezhaCameraOvxControls *controls,
    struct NezhaCameraOvxControlResult *result)
{
    if (!result) return NezhaCciInvalid;
    *result = (struct NezhaCameraOvxControlResult){0};
    if (!cci || !cci->read || !cci->write || !cci->wait_ms ||
        !nezha_camera_ovx10500u_control_plan(controls, &result->plan)) return NezhaCciInvalid;
    for (unsigned i = 0; i < 14; ++i) {
        const struct NezhaCameraOvxModeWrite *write = &result->plan.writes[i];
        const enum NezhaCameraCciStatus status = nezha_camera_cci_write_byte(
            cci, OvxMaster, OvxSlave, write->reg, write->value, &result->transfer);
        if (i == 0 && (status == NezhaCciOk || result->transfer.needs_recovery))
            result->group_may_be_held = true;
        if (status != NezhaCciOk) return status;
        ++result->applied_writes;
    }
    result->group_may_be_held = false;
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_ovx10500u_read_identity(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraOvxIdentity *identity)
{
    if (identity == NULL) {
        return NezhaCciInvalid;
    }
    *identity = (struct NezhaCameraOvxIdentity){0};
    const enum NezhaCameraCciStatus status = nezha_camera_cci_read(
        cci, OvxMaster, OvxSlave, OvxIdRegister, identity->bytes, 2, &identity->transfer);
    if (status == NezhaCciOk) {
        identity->bytes_read = 2;
    }
    return status;
}

enum NezhaCameraCciStatus nezha_camera_ovx10500u_read_controls(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraOvxControlReadback *result)
{
    if (!result) return NezhaCciInvalid;
    *result = (struct NezhaCameraOvxControlReadback){0};
    static const uint16_t address[5] = {0x3840,0x380e,0x3500,0x3508,0x350a};
    static const unsigned count[5] = {1,2,3,2,3};
    for (unsigned i=0;i<5;++i) {
        enum NezhaCameraCciStatus status = nezha_camera_cci_read(cci,OvxMaster,OvxSlave,
            address[i],result->registers+result->bytes_read,count[i],&result->transfer);
        if (status!=NezhaCciOk) return status;
        result->bytes_read+=count[i];
    }
    const uint8_t *r=result->registers;
    result->frame_lines=(uint32_t)r[0]<<16|(uint32_t)r[1]<<8|r[2];
    result->exposure_lines=(uint32_t)r[3]<<16|(uint32_t)r[4]<<8|r[5];
    result->analog_gain_q8=(uint16_t)((uint16_t)r[6]<<8|r[7]);
    result->digital_gain_q10=((uint32_t)r[8]<<16|(uint32_t)r[9]<<8|r[10])>>6;
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_ovx10500u_read_revision(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraOvxRevision *revision)
{
    if (revision == NULL) return NezhaCciInvalid;
    *revision = (struct NezhaCameraOvxRevision){0};
    enum NezhaCameraCciStatus status = nezha_camera_cci_read_byte(cci, OvxMaster, 0x51, 0x0001,
        &revision->vendor, &revision->transfer);
    if (status != NezhaCciOk) return status;
    revision->vendor_read = true;
    if (revision->vendor != 3 && revision->vendor != 7) return NezhaCciInvalid;

    /* Stock selector: start the identified sensor, read its version,
     * then return it to standby before loading any mode table. */
    revision->stream_may_be_on = true;
    status = nezha_camera_ovx10500u_set_stream(cci, true, &revision->transfer);
    if (status != NezhaCciOk) return status;
    cci->wait_ms(cci->context, 3);
    status = nezha_camera_cci_read_byte(cci, OvxMaster, OvxSlave, 0x900e,
        &revision->version, &revision->transfer);
    if (status != NezhaCciOk) {
        if (!revision->transfer.needs_recovery) {
            struct NezhaCameraCciObservation stop;
            revision->stream_may_be_on = nezha_camera_ovx10500u_set_stream(cci, false, &stop) != NezhaCciOk;
            if (revision->stream_may_be_on) revision->transfer = stop;
        }
        return status;
    }
    revision->version_read = true;
    status = nezha_camera_ovx10500u_set_stream(cci, false, &revision->transfer);
    revision->stream_may_be_on = status != NezhaCciOk;
    return status;
}

enum NezhaCameraCciStatus nezha_camera_ovx10500u_apply_mode(
    const struct NezhaCameraRegisterIo *cci, const struct NezhaCameraOvxModeWrite *writes,
    size_t count, unsigned burst_queue_words, size_t *applied, struct NezhaCameraCciObservation *transfer)
{
    if (applied == NULL || transfer == NULL) {
        return NezhaCciInvalid;
    }
    *applied = 0;
    *transfer = (struct NezhaCameraCciObservation){0};
    if (cci == NULL || cci->wait_ms == NULL || writes == NULL || count == 0) {
        return NezhaCciInvalid;
    }
    for (size_t i = 0; i < count; ++i) {
        if (writes[i].reg == OvxStream || (writes[i].burst == NULL) != (writes[i].burst_bytes == 0) ||
            (writes[i].burst != NULL && (writes[i].burst_bytes > UINT16_MAX ||
             burst_queue_words < 16 || burst_queue_words > NezhaCameraCciCanoeQueue0Words))) {
            return NezhaCciInvalid;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const struct NezhaCameraOvxModeWrite *entry = &writes[i];
        const enum NezhaCameraCciStatus status = entry->burst == NULL ?
            nezha_camera_cci_write_byte(cci, OvxMaster, OvxSlave, entry->reg, entry->value, transfer) :
            nezha_camera_cci_write_burst(cci, OvxMaster, OvxSlave, entry->reg, entry->burst,
                                        entry->burst_bytes, burst_queue_words, transfer);
        if (status != NezhaCciOk) {
            return status;
        }
        ++*applied;
        if (writes[i].delay_ms != 0) {
            cci->wait_ms(cci->context, writes[i].delay_ms);
        }
    }
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_ovx10500u_set_stream(
    const struct NezhaCameraRegisterIo *cci, bool running,
    struct NezhaCameraCciObservation *transfer)
{
    return nezha_camera_cci_write_byte(cci, OvxMaster, OvxSlave, OvxStream,
                                       running ? 1 : 0, transfer);
}
