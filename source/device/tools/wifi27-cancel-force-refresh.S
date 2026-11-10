// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fixed24A446 private async refresh contract. Only the original cancellation
// caller selects a forced task. Ordinary callers retain coalescing unchanged.
// A forced request inside an existing window enters the original query/publish
// body without changing the window flag or creating another reset timer.
.arch armv8.3-a
.text
.p2align 2
.global wifi27_refresh_descriptor_select
.type wifi27_refresh_descriptor_select, %function
wifi27_refresh_descriptor_select:
    ldr x8, [x29, #8]              // wrapper's original saved PAC return address
    add x9, x29, #16               // SP used by its original PACIBSP
    autib x8, x9                   // authenticate a copy; saved return untouched
    adrp x9, wifi27_cancel_refresh_return
    add x9, x9, :lo12:wifi27_cancel_refresh_return
    eor x8, x8, x9
    adrp x16, wifi27_original_refresh_descriptor
    add x16, x16, :lo12:wifi27_original_refresh_descriptor
    cbnz x8, .Ldescriptor_done
    adr x16, wifi27_forced_refresh_descriptor
.Ldescriptor_done:
    b wifi27_descriptor_resume    // original PACDA/signing and task creation
.size wifi27_refresh_descriptor_select, .-wifi27_refresh_descriptor_select

.global wifi27_normal_refresh_entry
.type wifi27_normal_refresh_entry, %function
wifi27_normal_refresh_entry:
    mov x0, #0                    // original async Void result argument is dead
    pacibsp                       // displaced original instruction, same SP/LR
    b wifi27_original_task_resume
.size wifi27_normal_refresh_entry, .-wifi27_normal_refresh_entry

.global wifi27_forced_refresh_entry
.type wifi27_forced_refresh_entry, %function
wifi27_forced_refresh_entry:
    mov x0, #1
    pacibsp
    b wifi27_original_task_resume
.size wifi27_forced_refresh_entry, .-wifi27_forced_refresh_entry

.global wifi27_store_refresh_mode
.type wifi27_store_refresh_mode, %function
wifi27_store_refresh_mode:
    str x0, [x22, #0x5d8]         // verified8-byte padding in the0x5e0 async frame
    str x4, [x22, #0x590]         // displaced original instruction
    b wifi27_refresh_entry_resume
.size wifi27_store_refresh_mode, .-wifi27_store_refresh_mode

.global wifi27_refresh_window_gate
.type wifi27_refresh_window_gate, %function
wifi27_refresh_window_gate:
    ldr x10, [x22, #0x5d8]
    tbnz w10, #0, .Lforced_marker
    tbz w9, #0, .Lnormal_admit
    b .Lnormal_coalesce
.Lforced_marker:
    // One bounded existing kdebug marker proves this forced branch actually
    // ran. No new timer, credential, identity or connection event is produced.
    sub sp, sp, #0xb0
    stp x0, x1, [sp]
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    stp x8, x9, [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x16, x17, [sp, #0x80]
    stp x18, x30, [sp, #0x90]
    mrs x8, nzcv
    str x8, [sp, #0xa0]
    mov x1, x23                   // original WiFiInterface identity
    mov x2, x10                   // forced task mode1
    mov x3, x9                    // existing coalescing flag
    mov x4, #0
    mov w0, #0xe300
    movk w0, #0x2b28, lsl #16
    mov x16, #0xb3                // original __kdebug_trace64 syscall
    svc #0x80
    ldr x8, [sp, #0xa0]
    msr nzcv, x8
    ldp x0, x1, [sp]
    ldp x2, x3, [sp, #0x10]
    ldp x4, x5, [sp, #0x20]
    ldp x6, x7, [sp, #0x30]
    ldp x8, x9, [sp, #0x40]
    ldp x10, x11, [sp, #0x50]
    ldp x12, x13, [sp, #0x60]
    ldp x14, x15, [sp, #0x70]
    ldp x16, x17, [sp, #0x80]
    ldp x18, x30, [sp, #0x90]
    add sp, sp, #0xb0
    tbz w9, #0, .Lnormal_admit
    mov x28, x27                  // reproduce query-body live string constant
    b wifi27_original_query_body
.Lnormal_admit:
    b wifi27_original_window_admit
.Lnormal_coalesce:
    b wifi27_original_coalesced_return
.size wifi27_refresh_window_gate, .-wifi27_refresh_window_gate

.p2align 2
.global wifi27_forced_refresh_descriptor
.type wifi27_forced_refresh_descriptor, %object
wifi27_forced_refresh_descriptor:
    .word wifi27_forced_refresh_entry - wifi27_forced_refresh_descriptor
    .word 32                     // same closure frame size as original descriptor
.size wifi27_forced_refresh_descriptor, .-wifi27_forced_refresh_descriptor
