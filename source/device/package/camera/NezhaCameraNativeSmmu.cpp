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
}

struct NezhaCameraNativeSmmu {
    Sm8850Resources *common{};
    uint64_t lease{};
    Sm8850SmmuAssignment assignment{};
    ArmSmmu4KMmio map{};
    ArmSmmu4KIo io{};
    ArmSmmu4KContextConfig context{};
    const NezhaCameraNativeMemory *memory{};
    size_t bank_base{};
    unsigned physical_bits{};
    uint32_t fault{}, syndrome{}, irq_readback{}, actlr{};
    uint64_t fault_address{}, irq_route{};
    bool locked{}, irq_attached{}, has_context{};
};

static NezhaCameraNativeSmmu domain;
static bool domain_used;

static void fault_irq(void *cookie)
{
    auto *state = static_cast<NezhaCameraNativeSmmu *>(cookie);
    if (!__atomic_load_n(&state->irq_attached, __ATOMIC_ACQUIRE)) {
        return;
    }
    const size_t bank = state->bank_base;
    uint32_t fsr = 0;
    if (state->io.read32(state->io.cookie, bank + 0x58, &fsr) || !(fsr & FaultMask)) {
        return;
    }
    if (!__atomic_load_n(&state->fault, __ATOMIC_ACQUIRE)) {
        (void)state->io.read32(state->io.cookie, bank + 0x68, &state->syndrome);
        (void)state->io.read64(state->io.cookie, bank + 0x60, &state->fault_address);
        __atomic_store_n(&state->fault, fsr, __ATOMIC_RELEASE);
    }
    // Keep translation and the route active until the worker drains DMA.
    // Mask only this bank's fault interrupt, preserving SCTLR.M.
    uint32_t sctlr = 0;
    if (!state->io.read32(state->io.cookie, bank, &sctlr)) {
        (void)state->io.write32(state->io.cookie, bank, sctlr & ~0x40u);
        (void)state->io.barrier(state->io.cookie);
    }
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
        // Stock qcom,actlr for SID 0x1c00 is 1. Require the inherited
        // compatible state; don't overwrite a bank's implementation bits.
        if (domain.physical_bits && !domain.io.read32(domain.io.cookie, domain.bank_base + 4, &domain.actlr) &&
            (domain.actlr & 0x103u) == 1u) {
            *physical_bits = domain.physical_bits;
            result = ArmSmmu4KSuccess;
        }
    }
    sm8850_resource_leave(common);
    return result;
}

ArmSmmu4KStatus nezha_camera_native_smmu_enter(NezhaCameraNativeSmmu *state,
    const NezhaCameraNativeMemory *memory)
{
    if (state != &domain || !state->lease || state->locked) {
        return ArmSmmu4KArgument;
    }
    const auto *config = nezha_camera_native_memory_config(memory);
    const auto *plan = nezha_camera_native_memory_plan(memory);
    if (!config || !plan || config->output_bits != state->physical_bits ||
        sm8850_resource_enter(state->common, CameraResource, CameraOwner, state->lease)) {
        return ArmSmmu4KBusy;
    }
    state->locked = true;
    if (state->has_context) {
        if (state->context.lower != config || state->context.lower_tables != &plan->tables) {
            nezha_camera_native_smmu_leave(state);
            return ArmSmmu4KStateMismatch;
        }
        return ArmSmmu4KSuccess;
    }
    uint32_t actlr = 0;
    if (sm8850_smmu_check_assignment(state->common, &state->assignment) != Sm8850Okay ||
        state->io.read32(state->io.cookie, state->bank_base + 4, &actlr) || actlr != state->actlr) {
        nezha_camera_native_smmu_leave(state);
        return ArmSmmu4KBusy;
    }
    // Bank 0..21 follows the board DT's level-high SPI 0x61+bank.
    __atomic_store_n(&state->irq_attached, true, __ATOMIC_RELEASE);
    if (armcpu_spi_attach(129u + state->assignment.bank, false, fault_irq, state,
                          &state->irq_readback, &state->irq_route) != kIOReturnSuccess ||
        (state->irq_readback & 7u) != 3u) {
        nezha_camera_native_smmu_leave(state);
        return ArmSmmu4KIoError;
    }
    state->context = {};
    state->context.lower = config;
    state->context.lower_tables = &plan->tables;
    state->context.asid = state->assignment.asid;
    state->memory = memory;
    state->has_context = true;
    return ArmSmmu4KSuccess;
}

void nezha_camera_native_smmu_leave(NezhaCameraNativeSmmu *state)
{
    if (state == &domain && state->locked) {
        state->locked = false;
        sm8850_resource_leave(state->common);
    }
}

const ArmSmmu4KIo *nezha_camera_native_smmu_io(const NezhaCameraNativeSmmu *state)
{
    return state == &domain && state->has_context && state->irq_attached ? &state->io : nullptr;
}

bool nezha_camera_native_smmu_capture_enter(void *cookie)
{
    auto *state = static_cast<NezhaCameraNativeSmmu *>(cookie);
    return state == &domain && state->has_context &&
        nezha_camera_native_smmu_enter(state, state->memory) == ArmSmmu4KSuccess;
}

void nezha_camera_native_smmu_capture_leave(void *cookie)
{
    nezha_camera_native_smmu_leave(static_cast<NezhaCameraNativeSmmu *>(cookie));
}

const ArmSmmu4KContextConfig *nezha_camera_native_smmu_context(const NezhaCameraNativeSmmu *state)
{
    return state == &domain && state->has_context ? &state->context : nullptr;
}

uint32_t nezha_camera_native_smmu_fault(const NezhaCameraNativeSmmu *state)
{
    return state == &domain ? __atomic_load_n(&state->fault, __ATOMIC_ACQUIRE) : UINT32_MAX;
}

bool nezha_camera_native_smmu_release(NezhaCameraNativeSmmu **out, bool dma_detached)
{
    if (!out || *out != &domain || !domain.lease || domain.locked) {
        return false;
    }
    if (!dma_detached && domain.has_context) {
        (void)sm8850_resource_quarantine(domain.common, CameraResource, CameraOwner, domain.lease);
        return false;
    }
    __atomic_store_n(&domain.irq_attached, false, __ATOMIC_RELEASE);
    if (sm8850_resource_release(domain.common, CameraResource, CameraOwner, domain.lease, 1)) {
        return false;
    }
    domain.lease = 0;
    domain.has_context = false;
    domain.memory = nullptr;
    *out = nullptr;
    return true;
}
