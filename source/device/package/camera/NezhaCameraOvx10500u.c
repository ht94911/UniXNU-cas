/* SPDX-License-Identifier: MIT */
#include "NezhaCameraOvx10500u.h"

/* Main sensor route from nezha's stock DT. All four stock OVX10500U
 * module variants probe slave 0x20 (8-bit) with a word at 0x300b. */
enum {
    OvxMaster = 1,
    OvxSlave = 0x10,
    OvxIdRegister = 0x300b,
    OvxStream = 0x0100,
};

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
             burst_queue_words < 16 || burst_queue_words > 128))) {
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
