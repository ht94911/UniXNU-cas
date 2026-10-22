// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host emulator only: invoke the real validator with an MMU-off ARM64 CPU.
// No Qualcomm MMIO, firmware image, Android kernel or private payload is run.
.section .text, "ax"
.global _start
_start:
  msr daifset, #0xf
  adr x0, _vectors
  mrs x1, CurrentEL
  cmp x1, #4
  b.eq 1f
  msr vbar_el2, x0
  mrs x1, sctlr_el2
  bic x1, x1, #1
  bic x1, x1, #2
  bic x1, x1, #4
  bic x1, x1, #0x1000
#if PROBE_ALIGNMENT_CHECK
  orr x1, x1, #2
#endif
  msr sctlr_el2, x1
  b 2f
1:
  msr vbar_el1, x0
  mrs x1, sctlr_el1
  bic x1, x1, #1
  bic x1, x1, #2
  bic x1, x1, #4
  bic x1, x1, #0x1000
#if PROBE_ALIGNMENT_CHECK
  orr x1, x1, #2
#endif
  msr sctlr_el1, x1
2:
  isb
  ldr x0, =_stack_end
  mov sp, x0
  ldr x1, =_el_label
  bl _print
  mrs x0, CurrentEL
  bl _hex
  ldr x1, =_sctlr_label
  bl _print
  mrs x0, CurrentEL
  cmp x0, #4
  b.eq 7f
  mrs x0, sctlr_el2
  b 8f
7:
  mrs x0, sctlr_el1
8:
  bl _hex
  ldr x0, =0xa0080000
  ldr x1, =0x807000
  ldr x2, =0xafdfe000
  mov x3, x2
  ldr x4, =_info
  bl CasValidateBoot
  mov x19, x0
  ldr x1, =_result_label
  bl _print
  mov x0, x19
  bl _hex
  b _finish

.balign 2048
_vectors:
.rept 16
  b _exception
  .balign 128
.endr
_exception:
  mrs x0, CurrentEL
  cmp x0, #4
  b.eq 3f
  mrs x19, esr_el2
  mrs x20, far_el2
  mrs x21, elr_el2
  b 4f
3:
  mrs x19, esr_el1
  mrs x20, far_el1
  mrs x21, elr_el1
4:
  ldr x0, =_stack_end
  mov sp, x0
  ldr x1, =_esr_label
  bl _print
  mov x0, x19
  bl _hex
  ldr x1, =_far_label
  bl _print
  mov x0, x20
  bl _hex
  ldr x1, =_pc_label
  bl _print
  mov x0, x21
  bl _hex
  b _finish

_print:
  mov x0, #4
  hlt #0xf000
  ret
_hex:
  sub sp, sp, #32
  mov x10, #16
  mov x11, sp
5:
  lsr x12, x0, #60
  cmp x12, #10
  add x13, x12, #48
  add x12, x12, #87
  csel x12, x13, x12, lo
  strb w12, [x11], #1
  lsl x0, x0, #4
  subs x10, x10, #1
  b.ne 5b
  mov w12, #10
  strb w12, [x11], #1
  strb wzr, [x11]
  mov x1, sp
  mov x0, #4
  hlt #0xf000
  add sp, sp, #32
  ret
_finish:
  mov x0, #0x20
  ldr x1, =_exit_block
  hlt #0xf000
6:
  wfe
  b 6b
  .ltorg

.section .rodata, "a"
_result_label: .asciz "CRESULT="
_el_label: .asciz "EL="
_sctlr_label: .asciz "SCTLR="
_esr_label: .asciz "ESR="
_far_label: .asciz "FAR="
_pc_label: .asciz "PC="
.balign 8
_exit_block: .quad 0x20026, 0
.section .bss, "aw", %nobits
.balign 16
_info: .skip 128
_stack: .skip 0x8000
_stack_end:
