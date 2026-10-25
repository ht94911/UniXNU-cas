// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef CAS_HANDOFF_TRACE_ABI_H
#define CAS_HANDOFF_TRACE_ABI_H
#define CAS_TRACE_BASE 0x9f960000ULL
#define CAS_TRACE_MAGIC 0x363048534143ULL
typedef struct {
  UINT64 Magic, Version, Bytes, BootStamp, Sequence, Stage, Status;
  UINT64 Address, Args, Entry, Window, CurrentEl, Sctlr, Tcr, Stack;
  UINT64 Reserved[14];
  UINT32 Crc32, CrcPadding;
  UINT64 RawStage, RawStageInverse;
} CAS_HANDOFF_TRACE;
STATIC_ASSERT (sizeof (CAS_HANDOFF_TRACE) == 256, "trace ABI");
STATIC_ASSERT (OFFSET_OF (CAS_HANDOFF_TRACE, RawStage) == 240, "raw marker ABI");
#define CAS_VM_EXCEPTION_MAGIC 0x315845534143ULL
#define CAS_VM_EXCEPTION_OFFSET 256
typedef struct {
  UINT64 Magic, BootStamp, Candidate, Vector, Esr, Far, Elr, Spsr;
  UINT64 Sctlr, Tcr, Ttbr0, Ttbr1, Stack, PreviousStage, PayloadXor, MagicInverse;
} CAS_VM_EXCEPTION;
STATIC_ASSERT (sizeof (CAS_VM_EXCEPTION) == 128, "VM exception extension ABI");
STATIC_ASSERT (OFFSET_OF (CAS_VM_EXCEPTION, PayloadXor) == 112, "exception checksum ABI");
#endif
