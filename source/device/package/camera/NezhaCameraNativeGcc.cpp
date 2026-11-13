/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaCameraNativeGcc.h"
#include "Sm8850ResourcesNative.h"

namespace {
bool camera_gate(uint32_t offset)
{
    return offset == 0x26004 || offset == 0x2603c || offset == 0xa0004 ||
        offset == 0x26014 || offset == 0x26028;
}

uint32_t read(void *cookie, uint32_t offset)
{
    if (!camera_gate(offset) || sm8850_apps_config_lock()) {
        return UINT32_MAX;
    }
    auto *common = static_cast<Sm8850Resources *>(cookie);
    uint32_t word;
    const int error = common->io.read32(common->io.context, Sm8850Gcc, offset, &word);
    sm8850_apps_config_unlock();
    return error == 0 ? word : UINT32_MAX;
}

bool update(void *cookie, uint32_t offset, bool enabled)
{
    if (!camera_gate(offset) || sm8850_apps_config_lock()) {
        return false;
    }
    auto *common = static_cast<Sm8850Resources *>(cookie);
    const int result = sm8850_gcc_update(common, offset, 1, 1, enabled ? 1u : 0u);
    sm8850_apps_config_unlock();
    return result == Sm8850Okay;
}
}

bool nezha_camera_native_gcc(NezhaCameraGccIo *io)
{
    if (!io) {
        return false;
    }
    auto *common = sm8850_native_resources();
    if (!common) {
        return false;
    }
    *io = {common, read, update};
    return true;
}
