/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Shared resource binding follows the validated nezha audio/baseband
 * native adapters. Reuse ArmSmmu4K for context and route programming. */
#include "NezhaCameraNativeSmmu.h"
#include "ArmSmmu4KMmio.h"
#include "Sm8850ResourcesNative.h"

#include <IOKit/IOLib.h>

extern "C" IOReturn armcpu_spi_attach(uint32_t, bool, void (*)(void *),
    void *, uint32_t *, uint64_t *);
extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

namespace {
constexpr unsigned CameraOwner = 0x4e43;
constexpr unsigned CameraResource = 11;
constexpr size_t SmmuBytes = 0x100000;
constexpr uint32_t FaultMask = 0xc00001feu;
constexpr uint32_t FaultStall = 1u << 30;
constexpr uint32_t FaultInterrupt = 1u << 6;
constexpr uint32_t CameraActlr = 1;
}

struct NezhaCameraNativeSmmu {
    Sm8850Resources *common{};
    uint64_t lease{};
    Sm8850SmmuAssignment assignment{};
    ArmSmmu4KMmio map{};
    ArmSmmu4KIo io{};
    ArmSmmu4KContextConfig context{};
    size_t bank_base{};
    unsigned physical_bits{};
    uint32_t fault{}, syndrome{}, irq_readback{}, actlr{}, handlers{};
    uint64_t fault_address{}, irq_route{};
    bool locked{}, irq_attached{}, has_context{}, programming_started{}, actlr_touched{}, fault_irq_masked{};
};

static NezhaCameraNativeSmmu domain;
static bool domain_used;

static void record_fault(NezhaCameraNativeSmmu &state)
{
    const size_t bank = state.bank_base;
    uint32_t fsr = 0;
    if (state.io.read32(state.io.cookie, bank + 0x58, &fsr) || !(fsr & FaultMask)) {
        return;
    }
    if (!__atomic_load_n(&state.fault, __ATOMIC_ACQUIRE)) {
        (void)state.io.read32(state.io.cookie, bank + 0x68, &state.syndrome);
        (void)state.io.read64(state.io.cookie, bank + 0x60, &state.fault_address);
        __atomic_store_n(&state.fault, fsr, __ATOMIC_RELEASE);
    }
    // Keep translation and the route active until the worker drains DMA.
    // Mask only this bank's fault interrupt, preserving SCTLR.M.
    uint32_t sctlr = 0;
    if (!state.io.read32(state.io.cookie, bank, &sctlr) && (sctlr & FaultInterrupt)) {
        state.fault_irq_masked = true;
        (void)state.io.write32(state.io.cookie, bank, sctlr & ~FaultInterrupt);
        (void)state.io.barrier(state.io.cookie);
    }
}

static void fault_irq(void *cookie)
{
    auto &state = *static_cast<NezhaCameraNativeSmmu *>(cookie);
    __atomic_add_fetch(&state.handlers, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&state.irq_attached, __ATOMIC_SEQ_CST)) {
        record_fault(state);
    }
    __atomic_sub_fetch(&state.handlers, 1u, __ATOMIC_SEQ_CST);
}

static bool retire_irq(NezhaCameraNativeSmmu &state)
{
    __atomic_store_n(&state.irq_attached, false, __ATOMIC_SEQ_CST);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        if (__atomic_load_n(&state.handlers, __ATOMIC_SEQ_CST) == 0) return true;
        IOSleep(1);
    }
    return __atomic_load_n(&state.handlers, __ATOMIC_SEQ_CST) == 0;
}

ArmSmmu4KStatus nezha_camera_native_smmu_prepare(NezhaCameraNativeSmmu **out,
    unsigned slot, unsigned bank, unsigned asid, unsigned *physical_bits)
{
    if (!out || *out || !physical_bits || domain_used || !asid || asid > 0xffff ||
        bank >= 22 || slot >= 128 || !sm8850_usb_domain_ready()) {
        return ArmSmmu4KArgument;
    }
    auto *common = sm8850_native_resources();
    if (!common || sm8850_resource_acquire(common, CameraResource, CameraOwner, &domain.lease)) {
        return ArmSmmu4KBusy;
    }
    domain_used = true;
    *out = &domain;
    domain.common = common;
    domain.assignment = {bank, asid, {slot, 0, 0, 0}, {NezhaCameraIfeSid, 0, 0, 0}, 1};
    domain.map = {reinterpret_cast<volatile unsigned char *>(ml_io_map(0x15000000, SmmuBytes)), SmmuBytes};
    if (ArmSmmu4KBindMmio(&domain.map, &domain.io) != ArmSmmu4KSuccess) {
        return ArmSmmu4KIoError;
    }
    if (sm8850_resource_enter(common, CameraResource, CameraOwner, domain.lease)) {
        return ArmSmmu4KBusy;
    }
    uint32_t id1 = 0, id2 = 0;
    ArmSmmu4KStatus result = ArmSmmu4KStateMismatch;
    if (sm8850_smmu_check_assignment(common, &domain.assignment) == Sm8850Okay &&
        !domain.io.read32(domain.io.cookie, 0x24, &id1) &&
        !domain.io.read32(domain.io.cookie, 0x28, &id2)) {
        domain.bank_base = (size_t(2u << ((id1 >> 28) & 7u)) + bank) * 4096;
        const unsigned code = (id2 >> 4) & 15u;
        domain.physical_bits = code == 1 ? 36u : code == 2 ? 40u : 0u;
        if (domain.physical_bits && !domain.io.read32(domain.io.cookie, domain.bank_base + 4, &domain.actlr) &&
            (domain.actlr & ~0x103u) == 0) {
            *physical_bits = domain.physical_bits;
            result = ArmSmmu4KSuccess;
        }
    }
    sm8850_resource_leave(common);
    return result;
}

ArmSmmu4KStatus nezha_camera_native_smmu_bind(NezhaCameraNativeSmmu *state,
    const NezhaCameraNativeMemory *memory)
{
    if (state != &domain || !state->lease || state->locked || state->has_context) {
        return ArmSmmu4KArgument;
    }
    const auto *config = nezha_camera_native_memory_config(memory);
    const auto *plan = nezha_camera_native_memory_plan(memory);
    if (!config || !plan || config->output_bits != state->physical_bits) return ArmSmmu4KArgument;
    state->context = {};
    state->context.lower = config;
    state->context.lower_tables = &plan->tables;
    state->context.asid = state->assignment.asid;
    state->has_context = true;
    return ArmSmmu4KSuccess;
}

static bool prepare_hardware(NezhaCameraNativeSmmu *state)
{
    uint32_t actlr = 0;
    if (sm8850_smmu_check_assignment(state->common, &state->assignment) != Sm8850Okay ||
        state->io.read32(state->io.cookie, state->bank_base + 4, &actlr) || actlr != state->actlr) {
        return false;
    }
    // Bank 0..21 follows the board DT's level-high SPI 0x61+bank.
    if (armcpu_spi_attach(129u + state->assignment.bank, false, fault_irq, state,
                          &state->irq_readback, &state->irq_route) != kIOReturnSuccess ||
        (state->irq_readback & 7u) != 3u) {
        return false;
    }
    // The stock qcom,actlr entry for exact SID 0x1c00 is 1. Like
    // qsmmuv500_init_cb, install it on the owned disabled bank and flush
    // that bank before binding tables. Restore the saved value at exit.
    if (state->actlr != CameraActlr) {
        state->actlr_touched = true;
        ArmSmmu4KProgramResult tlb{};
        if (state->io.write32(state->io.cookie, state->bank_base + 4, CameraActlr) ||
            state->io.barrier(state->io.cookie) ||
            state->io.read32(state->io.cookie, state->bank_base + 4, &actlr) || actlr != CameraActlr ||
            ArmSmmu4KSyncContextTlb(&state->io, state->bank_base, state->assignment.asid, 1, 1000, &tlb)) {
            return false;
        }
    }
    state->programming_started = true;
    __atomic_store_n(&state->irq_attached, true, __ATOMIC_SEQ_CST);
    return true;
}

static bool lock(NezhaCameraNativeSmmu *state)
{
    if (state != &domain || state->locked ||
        sm8850_resource_enter(state->common, CameraResource, CameraOwner, state->lease)) return false;
    state->locked = true;
    return true;
}

static void unlock(NezhaCameraNativeSmmu *state)
{
    if (state == &domain && state->locked) {
        state->locked = false;
        sm8850_resource_leave(state->common);
    }
}

static bool restore_actlr(NezhaCameraNativeSmmu &state)
{
    if (!state.actlr_touched) return true;
    auto &io = state.io;
    const size_t bank = state.bank_base;
    uint32_t actlr, sctlr;
    ArmSmmu4KProgramResult tlb{};
    const bool restored = state.locked && !io.read32(io.cookie, bank, &sctlr) && !(sctlr & 1u) &&
        !io.read32(io.cookie, bank + 4, &actlr) && (actlr == CameraActlr || actlr == state.actlr) &&
        !io.write32(io.cookie, bank + 4, state.actlr) && !io.barrier(io.cookie) &&
        !io.read32(io.cookie, bank + 4, &actlr) && actlr == state.actlr &&
        !ArmSmmu4KSyncContextTlb(&io, bank, state.assignment.asid, 1, 1000, &tlb);
    if (restored) state.actlr_touched = false;
    return restored;
}

const ArmSmmu4KIo *nezha_camera_native_smmu_io(const NezhaCameraNativeSmmu *state)
{
    return state == &domain && state->has_context ? &state->io : nullptr;
}

bool nezha_camera_native_smmu_capture_enter(void *cookie)
{
    auto *state = static_cast<NezhaCameraNativeSmmu *>(cookie);
    if (state != &domain || !state->has_context || state->programming_started || !lock(state)) return false;
    // Install ACTLR under the same lock as the context and SID programming.
    // Software preparation and the sensor table load do not reserve a bank.
    if (!prepare_hardware(state)) {
        // ACTLR can have changed even if its readback or TLBSYNC failed.
        // Roll it back before releasing the lock.
        const bool restored = restore_actlr(*state);
        unlock(state);
        if (!restored) (void)sm8850_resource_quarantine(state->common, CameraResource, CameraOwner, state->lease);
        return false;
    }
    return true;
}

void nezha_camera_native_smmu_capture_leave(void *cookie)
{
    unlock(static_cast<NezhaCameraNativeSmmu *>(cookie));
}

const ArmSmmu4KContextConfig *nezha_camera_native_smmu_context(const NezhaCameraNativeSmmu *state)
{
    return state == &domain && state->has_context ? &state->context : nullptr;
}

uint32_t nezha_camera_native_smmu_fault(const NezhaCameraNativeSmmu *state)
{
    return state == &domain ? __atomic_load_n(&state->fault, __ATOMIC_ACQUIRE) : UINT32_MAX;
}

void nezha_camera_native_smmu_fault_info(const NezhaCameraNativeSmmu *state,
    NezhaCameraNativeSmmuFault *info)
{
    *info = {};
    if (state == &domain) {
        info->status = nezha_camera_native_smmu_fault(state);
        if (info->status) {
            info->address = state->fault_address;
            info->syndrome = state->syndrome;
        }
    }
}

bool nezha_camera_native_smmu_prepare_detach(NezhaCameraNativeSmmu *state)
{
    if (state != &domain || !state->has_context || state->locked) return false;
    if (!state->programming_started) return !state->actlr_touched;
    if (!retire_irq(*state) || !lock(state)) return false;
    const size_t bank = state->bank_base;
    auto &io = state->io;
    uint32_t fsr = 0, sctlr = 0, after = 0;
    bool okay = !io.read32(io.cookie, bank + 0x58, &fsr) && !io.read32(io.cookie, bank, &sctlr);
    if (okay && (fsr & FaultMask)) {
        if (!__atomic_load_n(&state->fault, __ATOMIC_ACQUIRE)) {
            (void)io.read32(io.cookie, bank + 0x68, &state->syndrome);
            (void)io.read64(io.cookie, bank + 0x60, &state->fault_address);
            __atomic_store_n(&state->fault, fsr, __ATOMIC_RELEASE);
        }
        // This capture uses non-stalling translation. A stalled
        // transaction is unexpected and must keep the owned context live.
        okay = !(fsr & FaultStall) && !io.write32(io.cookie, bank + 0x58, fsr) &&
            !io.barrier(io.cookie) && !io.read32(io.cookie, bank + 0x58, &after) && !(after & FaultMask);
    }
    if (okay && state->fault_irq_masked && (sctlr & 1u) && !(sctlr & FaultInterrupt)) {
        okay = !io.write32(io.cookie, bank, sctlr | FaultInterrupt) && !io.barrier(io.cookie) &&
            !io.read32(io.cookie, bank, &after) && after == (sctlr | FaultInterrupt);
    }
    if (!okay) unlock(state);
    return okay;
}

bool nezha_camera_native_smmu_release(NezhaCameraNativeSmmu **out, bool dma_detached)
{
    if (!out || *out != &domain || !domain.lease) {
        return false;
    }
    const bool restored = dma_detached && restore_actlr(domain);
    unlock(&domain);
    if (!restored || __atomic_load_n(&domain.irq_attached, __ATOMIC_SEQ_CST)) {
        (void)sm8850_resource_quarantine(domain.common, CameraResource, CameraOwner, domain.lease);
        return false;
    }
    if (sm8850_resource_release(domain.common, CameraResource, CameraOwner, domain.lease, 1)) {
        return false;
    }
    domain.lease = 0;
    domain.has_context = false;
    *out = nullptr;
    return true;
}
