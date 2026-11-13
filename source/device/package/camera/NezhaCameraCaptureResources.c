/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCaptureResources.h"

static bool valid(const struct NezhaCameraCaptureResourceIo *io,
    const struct NezhaCameraCaptureResources *state)
{
    return io && io->gcc && io->camcc && io->crm && io->crmc && io->crm_common &&
        io->crm_common->read && io->ife_rsc && io->cpas && io->phy &&
        io->rpmh && io->rpmh->capture_resources && state;
}

static bool crm_ready(const struct NezhaCameraRegisterIo *io)
{
    /* Do not enable or reconfigure the firmware's global CRM. Read the
     * v3 capability fields used by the stock probe before any vote. */
    const uint32_t version = io->read(io->context, 0);
    const uint32_t config = io->read(io->context, 4);
    const uint32_t enable = io->read(io->context, 0xc);
    return version != 0 && version != UINT32_MAX && config != UINT32_MAX &&
        enable != 0 && enable != UINT32_MAX && ((config >> 18) & 0x1fu) >= 1 &&
        ((config >> 6) & 15u) >= 8 && ((config >> 23) & 7u) >= 1;
}

static bool clocks_start(const struct NezhaCameraCaptureResourceIo *io,
    struct NezhaCameraCaptureResources *state)
{
    static const enum NezhaCameraCrmClock clocks[] = {
        NezhaCameraCrmCphyRx, NezhaCameraCrmCsid, NezhaCameraCrmTfe0,
    };
    static const uint32_t minimum[] = {266666667, 266666667, 360280000};
    const struct NezhaCameraCrmParent parent = {(void *)io->camcc, nezha_camera_crm_canoe_parent_rate};
    state->step = NezhaResourcesCrmClock;
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i) {
        state->detail = (unsigned)clocks[i];
        struct NezhaCameraCrmPlan plan;
        state->status = nezha_camera_crm_load_plan(io->crmc, &parent, clocks[i], &plan);
        if (state->status != NezhaCameraCrmOk) {
            return false;
        }
        /* The next possible firmware bin is allowed up to the LOW_SVS
         * floor already taken for the 300 MHz fast AHB. */
        unsigned n = 0;
        while (n < plan.count && plan.rate[n] < minimum[i]) {
            ++n;
        }
        if (n == plan.count || plan.rate[n] > (i == 2 ? 480000000u : 400000000u)) {
            state->status = NezhaCameraCrmNoRate;
            return false;
        }
        state->status = nezha_camera_crm_acquire(io->crm, &plan, minimum[i], &state->clocks[i]);
        if (state->status != NezhaCameraCrmOk) {
            return false;
        }
    }
    state->detail = NezhaCameraCrmCamnoc;
    state->status = nezha_camera_crm_acquire_camnoc(io->crm, 200000000, &state->clocks[3]);
    return state->status == NezhaCameraCrmOk;
}

bool nezha_camera_capture_resources_start(const struct NezhaCameraCaptureResourceIo *io,
    const struct NezhaCameraIfeBandwidth *bandwidth, const struct NezhaCameraCsiphy24Profile *profile,
    struct NezhaCameraCaptureResources *state)
{
    if (!valid(io, state) || !bandwidth || !profile || state->step != NezhaResourcesIdle) {
        return false;
    }
    state->step = NezhaResourcesArc;
    /* Stock Canoe: both 300 MHz fast AHB and 80 MHz slow AHB need
     * LOW_SVS on MMCX/MXC. These selected roots do not require MX. */
    for (unsigned i = 0; i < 2; ++i) {
        state->detail = i;
        if (!nezha_camera_rpmh_acquire_arc(io->rpmh, (enum NezhaCameraArcRail)i, 64)) {
            state->status = io->rpmh->status;
            return false;
        }
    }
    state->step = NezhaResourcesPhyRails;
    for (unsigned i = 0; i < 2; ++i) {
        state->detail = i;
        if (!nezha_camera_rpmh_acquire_phy(io->rpmh, (enum NezhaCameraPhyRail)i)) {
            state->status = io->rpmh->status;
            return false;
        }
    }
    state->detail = 0;
    state->step = NezhaResourcesInterface;
    state->status = nezha_camera_interfaces_start(io->gcc, io->camcc, &state->interfaces);
    if (state->status) return false;
    state->step = NezhaResourcesTop;
    state->status = nezha_camera_camcc_acquire_top(io->camcc, &state->top);
    if (state->status) return false;
    state->step = NezhaResourcesPll0;
    state->status = nezha_camera_camcc_acquire_pll0(io->camcc, &state->pll0);
    if (state->status) return false;
    state->step = NezhaResourcesCrmIdentity;
    if (!crm_ready(io->crm_common)) {
        state->status = NezhaCameraCrmReadback;
        return false;
    }
    if (!clocks_start(io, state)) return false;
    state->detail = 0;
    state->step = NezhaResourcesCciClock;
    state->status = nezha_camera_camcc_select_cci0_rate(io->camcc, &state->top, 37500000);
    if (!state->status) state->status = nezha_camera_camcc_enable_cci0(io->camcc, &state->top);
    if (state->status) return false;
    state->step = NezhaResourcesCaptureClock;
    state->status = nezha_camera_camcc_acquire_capture(io->camcc, &state->top, &state->pll0, &state->capture);
    if (state->status) return false;
    state->step = NezhaResourcesIfeRsc;
    state->status = nezha_camera_ife_rsc_start(io->ife_rsc, bandwidth, &state->ife);
    if (state->status) return false;
    state->step = NezhaResourcesCpas;
    state->status = nezha_camera_cpas_start(io->cpas, &state->cpas);
    if (state->status) return false;
    state->step = NezhaResourcesPhy;
    if (!nezha_camera_csiphy24_start(io->phy, profile, &state->phy)) {
        state->status = 1;
        return false;
    }
    state->step = NezhaResourcesReady;
    return true;
}

bool nezha_camera_capture_resources_drain(const struct NezhaCameraCaptureResourceIo *io,
    struct NezhaCameraCaptureResources *state, bool producers_stopped)
{
    if (!valid(io, state) || !producers_stopped) {
        return false;
    }
    state->cleanup_step = NezhaResourcesCpas;
    return !state->cpas.acquired || state->cpas.drained ||
        nezha_camera_cpas_drain(io->cpas, &state->cpas, true) == NezhaCameraCpasOk;
}

bool nezha_camera_capture_resources_stop(const struct NezhaCameraCaptureResourceIo *io,
    const struct NezhaCameraIfeBandwidth *clear, const struct NezhaCameraCsiphy24Profile *profile,
    struct NezhaCameraCaptureResources *state, bool sensor_quiet, bool dma_detached)
{
    if (!valid(io, state) || !sensor_quiet || !dma_detached ||
        (state->cpas.acquired && !state->cpas.drained)) {
        return false;
    }
    state->cleanup_step = NezhaResourcesPhy;
    if (state->phy.reset_owned && !nezha_camera_csiphy24_stop(io->phy, profile, &state->phy)) return false;
    state->cleanup_step = NezhaResourcesCpas;
    if (state->cpas.acquired && nezha_camera_cpas_restore(io->cpas, &state->cpas) != NezhaCameraCpasOk) return false;
    state->cleanup_step = NezhaResourcesIfeRsc;
    if (state->ife.owned && nezha_camera_ife_rsc_stop(io->ife_rsc, clear, &state->ife, true) != NezhaCameraIfeRscOk) return false;
    state->cleanup_step = NezhaResourcesCaptureClock;
    if (state->step >= NezhaResourcesCaptureClock &&
        nezha_camera_camcc_release_capture(io->camcc, &state->capture, true) != NezhaCameraCamccOk) return false;
    state->cleanup_step = NezhaResourcesCrmClock;
    for (unsigned i = 4; i != 0; --i) {
        struct NezhaCameraCrmLease *lease = &state->clocks[i - 1];
        if (lease->requested && nezha_camera_crm_release(io->crm, lease, true) != NezhaCameraCrmOk) return false;
    }
    state->cleanup_step = NezhaResourcesCciClock;
    if (state->step >= NezhaResourcesCciClock &&
        nezha_camera_camcc_release_cci0(io->camcc, &state->top) != NezhaCameraCamccOk) return false;
    state->cleanup_step = NezhaResourcesPll0;
    if (state->step >= NezhaResourcesPll0 && nezha_camera_camcc_release_pll0(io->camcc, &state->pll0, true) != NezhaCameraCamccOk) return false;
    state->cleanup_step = NezhaResourcesTop;
    if (state->step >= NezhaResourcesTop && nezha_camera_camcc_release(io->camcc, &state->top) != NezhaCameraCamccOk) return false;
    state->cleanup_step = NezhaResourcesInterface;
    if (state->step >= NezhaResourcesInterface &&
        nezha_camera_interfaces_stop(io->gcc, io->camcc, &state->interfaces, true) != NezhaCameraCamccOk) return false;
    state->cleanup_step = NezhaResourcesPhyRails;
    for (unsigned i = 2; i != 0; --i) {
        if (!nezha_camera_rpmh_release_phy(io->rpmh, (enum NezhaCameraPhyRail)(i - 1))) return false;
    }
    state->cleanup_step = NezhaResourcesArc;
    for (unsigned i = 2; i != 0; --i) {
        if (!nezha_camera_rpmh_release_arc(io->rpmh, (enum NezhaCameraArcRail)(i - 1))) return false;
    }
    state->cleanup_step = NezhaResourcesIdle;
    return true;
}
