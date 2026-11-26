/* SPDX-License-Identifier: MIT */
/* Pure-C11, Vulkan-free arm64e PAC boundary for linking libnative_metal_translate.a
 * (compiled by rustc arm64e-apple-ios without pointer authentication) into an
 * arm64e iOS 27 Mach-O binary. On non-arm64e hosts this unit is a no-op stub. */

#if defined(__APPLE__) && defined(__arm64e__)

#include <mach-o/dyld.h>
#include <pthread.h>
#include <ptrauth.h>
#include <stdint.h>
#include <stdlib.h>
#include <unwind.h>

extern void _tlv_atexit(void (*destructor)(void *), void *object);

#define A840_PAC_STRIP(f) ptrauth_strip((void *)(f), ptrauth_key_function_pointer)
#define A840_PAC_SIGN(f) \
    ptrauth_sign_unauthenticated(A840_PAC_STRIP(f), ptrauth_key_function_pointer, 0)

/* Rust -> C indirect branch stripping trampolines (used by rust_pac_rewrite.py). */
#define A840_PAC_TRAMP(n) \
    __attribute__((naked, used, visibility("default"))) void nmb_pac_call_x##n(void) { \
        __asm__ volatile("xpaci x" #n "\n\tbr x" #n); \
    }

A840_PAC_TRAMP(0)  A840_PAC_TRAMP(1)  A840_PAC_TRAMP(2)  A840_PAC_TRAMP(3)  A840_PAC_TRAMP(4)
A840_PAC_TRAMP(5)  A840_PAC_TRAMP(6)  A840_PAC_TRAMP(7)  A840_PAC_TRAMP(8)  A840_PAC_TRAMP(9)
A840_PAC_TRAMP(10) A840_PAC_TRAMP(11) A840_PAC_TRAMP(12) A840_PAC_TRAMP(13) A840_PAC_TRAMP(14)
A840_PAC_TRAMP(15) A840_PAC_TRAMP(16) A840_PAC_TRAMP(17) A840_PAC_TRAMP(18) A840_PAC_TRAMP(19)
A840_PAC_TRAMP(20) A840_PAC_TRAMP(21) A840_PAC_TRAMP(22) A840_PAC_TRAMP(23) A840_PAC_TRAMP(24)
A840_PAC_TRAMP(25) A840_PAC_TRAMP(26) A840_PAC_TRAMP(27) A840_PAC_TRAMP(28) A840_PAC_TRAMP(29)

/* C -> Rust callback PAC-signing wrappers (renamed via llvm-objcopy --redefine-sym). */
__attribute__((used, visibility("default")))
int nmb_pac_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *start, void *arg)
{
    return pthread_create(thread, attr, (void *(*)(void *))A840_PAC_SIGN(start), arg);
}

__attribute__((used, visibility("default")))
void nmb_pac_tlv_atexit(void *destructor, void *object)
{
    _tlv_atexit((void (*)(void *))A840_PAC_SIGN(destructor), object);
}

__attribute__((used, visibility("default")))
int nmb_pac_atexit(void *function)
{
    return atexit((void (*)(void))A840_PAC_SIGN(function));
}

__attribute__((used, visibility("default")))
void nmb_pac_dyld_register_func_for_add_image(void *function)
{
    _dyld_register_func_for_add_image(
        (void (*)(const struct mach_header *, intptr_t))A840_PAC_SIGN(function));
}

__attribute__((used, visibility("default")))
_Unwind_Reason_Code nmb_pac_Unwind_Backtrace(void *trace, void *context)
{
    return _Unwind_Backtrace((_Unwind_Trace_Fn)A840_PAC_SIGN(trace), context);
}

#else
typedef int a840_rust_pac_boundary_non_arm64e_guard_t;
#endif
