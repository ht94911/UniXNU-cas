/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Source-only CI contains no firmware. A checked local finalizer fills this
 * file-backed section before strict prelink; the empty slot refuses startup. */
.section __DATA_CONST,__nezgfw
.p2align 12
.globl _nezha_gpu_fwpack
_nezha_gpu_fwpack:
.space 262144
.section __DATA_CONST,__const
.p2align 3
.globl _nezha_gpu_fwpack_size
_nezha_gpu_fwpack_size:
.quad 262144
