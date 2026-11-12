/* SPDX-License-Identifier: MIT */
#include "NezhaCameraOvx10500u.h"

/* Same-device u0er ed8978ac0087 w33 capture, cross-checked with the
 * stock main sensor's CCI master in the Android device tree. */
enum {
    OvxMaster = 1,
    OvxSlave = 0x10,
    OvxIdHigh = 0x300a,
    OvxIdLow = 0x300b,
    OvxStream = 0x0100,
};

enum NezhaCameraCciStatus nezha_camera_ovx10500u_read_identity(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraOvxIdentity *identity)
{
    if (identity == NULL) {
        return NezhaCciInvalid;
    }
    *identity = (struct NezhaCameraOvxIdentity){0};
    const uint16_t address[] = {OvxIdHigh, OvxIdLow};
    for (unsigned i = 0; i < 2; ++i) {
        enum NezhaCameraCciStatus status = nezha_camera_cci_read_byte(
            cci, OvxMaster, OvxSlave, address[i], &identity->bytes[i], &identity->transfer);
        if (status != NezhaCciOk) {
            return status;
        }
        ++identity->bytes_read;
    }
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_ovx10500u_apply_mode(
    const struct NezhaCameraRegisterIo *cci, const struct NezhaCameraOvxModeWrite *writes,
    size_t count, size_t *applied, struct NezhaCameraCciObservation *transfer)
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
        if (writes[i].reg == OvxStream) {
            return NezhaCciInvalid;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        enum NezhaCameraCciStatus status = nezha_camera_cci_write_byte(
            cci, OvxMaster, OvxSlave, writes[i].reg, writes[i].value, transfer);
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
