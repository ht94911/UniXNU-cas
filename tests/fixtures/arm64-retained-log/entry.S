// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <CasPlatform.h>
.section .text, "ax"
.global _start
_start:
  msr daifset, #15
  ldr x0, =_stack_end
  mov sp, x0
  ldr x5, =CAS_LOG_BASE
  ldr x6, =(CAS_LOG_BASE + CAS_LOG_SIZE)
  mov x7, #0xa5a5
  movk x7, #0xa5a5, lsl #16
  movk x7, #0xa5a5, lsl #32
  movk x7, #0xa5a5, lsl #48
1:
  stp x7, x7, [x5], #16
  cmp x5, x6
  b.lo 1b
  // Execute the exact production object, not a Python reimplementation.
  bl CasBootClearVolatileLog
  ldr x5, =CAS_LOG_BASE
  ldr x6, =(CAS_LOG_BASE + CAS_LOG_SIZE)
  ldr x8, =(CAS_LOG_BASE + CAS_BOOT_LOG_CLEAR_SIZE)
2:
  ldp x9, x10, [x5]
  cmp x5, x8
  b.hs 3f
  orr x11, x9, x10
  cbnz x11, _fail
  b 4f
3:
  cmp x9, x7
  b.ne _fail
  cmp x10, x7
  b.ne _fail
4:
  add x5, x5, #16
  cmp x5, x6
  b.lo 2b
  mov x0, #4
  ldr x1, =_pass
  hlt #0xf000
  b _exit
_fail:
  mov x0, #4
  ldr x1, =_bad
  hlt #0xf000
_exit:
  mov x0, #0x20
  ldr x1, =_exit_block
  hlt #0xf000
  b .
.ltorg
.section .rodata, "a"
_pass: .asciz "ACTUAL-LOG-CLEAR: PASS\n"
_bad: .asciz "ACTUAL-LOG-CLEAR: FAIL\n"
.balign 8
_exit_block: .quad 0x20026, 0
.section .bss, "aw", %nobits
.balign 16
_stack: .skip 0x4000
_stack_end:
