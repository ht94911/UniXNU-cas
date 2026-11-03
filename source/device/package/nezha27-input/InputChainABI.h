// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only use of ShengHidInput's arm64e virtual-call authentication.
// Slots and diversity values were extracted independently from the exact
// iPhone24A446 kernelcache. No vtable or original instruction is changed.
#pragma once
#include <ptrauth.h>
#include <IOKit/IOService.h>

using ChainEventAction = void (*)(OSObject *, OSObject *, void *, OSObject *, IOOptionBits);

template <typename Result, typename... Arguments>
static Result chainCall(void *object, unsigned slot, uint16_t diversity, Arguments... arguments) {
    void **table = reinterpret_cast<void **>(ptrauth_auth_data(
        *reinterpret_cast<void **>(object), ptrauth_key_process_independent_data,
        ptrauth_blend_discriminator(object, 0xcda1)));
    void **entry = table + slot;
    void *function = ptrauth_auth_and_resign(
        *entry, ptrauth_key_function_pointer, ptrauth_blend_discriminator(entry, diversity),
        ptrauth_key_function_pointer, 0);
    return reinterpret_cast<Result (*)(void *, Arguments...)>(function)(object, arguments...);
}

static unsigned chainType(void *event) {
    return chainCall<unsigned>(event, 18, 0x8758);
}

static int32_t chainInteger(void *event, uint32_t field) {
    return chainCall<int32_t, uint32_t, IOOptionBits>(event, 23, 0x02c5, field, 0);
}

static int32_t chainFixed(void *event, uint32_t field) {
    return chainCall<int32_t, uint32_t, IOOptionBits>(event, 25, 0xf637, field, 0);
}

static size_t chainLength(void *event) {
    return chainCall<size_t>(event, 28, 0xab71);
}

static IOByteCount chainRead(void *event, void *buffer, IOByteCount length) {
    return chainCall<IOByteCount, void *, IOByteCount>(event, 29, 0x5f75, buffer, length);
}

static bool chainOpen(void *service, IOService *client, ChainEventAction action) {
    return chainCall<bool, IOService *, IOOptionBits, void *, ChainEventAction>(
        service, 197, 0xc0d0, client, 0, nullptr, action);
}
