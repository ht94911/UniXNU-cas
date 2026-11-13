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
