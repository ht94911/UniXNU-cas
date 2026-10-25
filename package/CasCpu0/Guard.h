// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef CAS_CPU0_GUARD_H
#define CAS_CPU0_GUARD_H
EFI_STATUS CasCpu0Preflight(UINT64 Address,UINT64 Size);
EFI_STATUS CasCpu0ProbeHeld(UINT64 Address,UINT64 Size);
#endif
