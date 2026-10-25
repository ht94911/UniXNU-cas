// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read only: never initialize, clear, repair or append a trace record.
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "TraceAbi.h"
STATIC BOOLEAN LogRegionReadable(VOID) {
 EFI_MEMORY_DESCRIPTOR *Map,*D;EFI_STATUS S;UINTN N=0,Cap,Key,Stride=sizeof(*Map),I;UINT32 Version;BOOLEAN Found=FALSE;
 S=gBS->GetMemoryMap(&N,NULL,&Key,&Stride,&Version);
 if(S!=EFI_BUFFER_TOO_SMALL || N>0x100000 || Stride<sizeof(*Map) || Stride>4096)return FALSE;
 Cap=N+16*Stride;Map=AllocatePool(Cap);if(!Map)return FALSE;N=Cap;
 S=gBS->GetMemoryMap(&N,Map,&Key,&Stride,&Version);
 if(!EFI_ERROR(S) && N<=Cap && Stride>=sizeof(*Map) && Stride<=4096 && N%Stride==0)
  for(I=0;I<N/Stride;I++) {D=(VOID *)((UINT8 *)Map+I*Stride);
   if(D->Type==EfiReservedMemoryType && D->PhysicalStart==0x9f940000 && D->NumberOfPages==0x40 && !(D->Attribute&EFI_MEMORY_RP))Found=TRUE;
  }
 FreePool(Map);return Found;
}
STATIC CONST CHAR16 *Phase(UINT64 Stage) {
 switch(Stage) {
 case 1:return L"loader request";case 2:return L"Mach-O parsed";
 case 3:return L"owned allocation/permissions";case 4:return L"kernel/ADT/args copied";
 case 5:return L"dry-run released and returned";case 0x10:return L"before ExitBootServices";
 case 0x20:return L"loader assembly after EBS";case 0x21:return L"loader before MMU off";
 case 0x22:return L"loader MMU off, before branch";case 0x40:return L"XNU __start";
 case 0x41:return L"XNU start_first_cpu";
 case 0x42:return L"OSLAR completed";case 0x43:return L"vectors and boot_args";
 case 0x44:return L"bootstrap tables and mailbox";case 0x45:return L"before MMU enable";
 case 0x46:return L"MMU on, diagnostic vectors";case 0x47:return L"before arm_init";
 case 0x50:return L"arm_init rebased";case 0x51:return L"early PE(FALSE) returned";
 case 0x52:return L"before arm_vm_init";case 0x56:return L"VM tables ready, before TTBR switch";
 case 0x53:return L"VM returned, mailbox IO mapped";case 0x54:return L"early PE(TRUE) returned";
 case 0x55:return L"diagnostic stop before clocks/IOKit";case 0xe0:return L"terminal early exception";
 case 0x7f:return L"pre-EBS failure/cleanup";
 default:return L"UNKNOWN (not accepted)";
 }
}
STATIC BOOLEAN KnownPhase(UINT64 Stage) {
 return Phase(Stage)[0]!=L'U';
}
STATIC BOOLEAN ExceptionValid(CONST CAS_VM_EXCEPTION *E, CONST CAS_VM_EXCEPTION *Other,
                              CONST CAS_HANDOFF_TRACE *A) {
 CONST UINT64 *Q=(CONST UINT64 *)E; UINT64 X=0; UINTN I;
 if(CompareMem(E,Other,sizeof(*E)) || E->Magic!=CAS_VM_EXCEPTION_MAGIC || E->MagicInverse!=~E->Magic ||
    E->BootStamp!=A->BootStamp || E->Candidate!=A->Reserved[4] || E->Vector>=16 ||
    !KnownPhase(E->PreviousStage) || E->PreviousStage==0xe0)return FALSE;
 for(I=1;I<14;I++)X^=Q[I];
 return X==E->PayloadXor;
}
EFI_STATUS EFIAPI CasTraceViewEntry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
 CAS_HANDOFF_TRACE A,B;CAS_VM_EXCEPTION E,F;UINTN I,Index;UINT32 Crc=0;BOOLEAN Valid,RawValid;EFI_INPUT_KEY Key;
 volatile CONST UINT8 *P=(VOID *)(UINTN)CAS_TRACE_BASE;CONST UINT64 *Boot=(VOID *)(UINTN)0x9f940000;
 (VOID)Image;(VOID)System;
 if(!gST->ConIn || !gST->ConOut || !LogRegionReadable())return EFI_UNSUPPORTED;
 for(I=0;I<sizeof(A);I++)((UINT8 *)&A)[I]=P[I];MemoryFence();
 for(I=0;I<sizeof(B);I++)((UINT8 *)&B)[I]=P[I];MemoryFence();
 for(I=0;I<sizeof(E);I++)((UINT8 *)&E)[I]=P[CAS_VM_EXCEPTION_OFFSET+I];MemoryFence();
 for(I=0;I<sizeof(F);I++)((UINT8 *)&F)[I]=P[CAS_VM_EXCEPTION_OFFSET+I];MemoryFence();
 Valid=CompareMem(&A,&B,sizeof(A))==0 && A.Magic==CAS_TRACE_MAGIC && A.Version==1 && A.Bytes==sizeof(A) && A.Sequence>0 && A.BootStamp>0 &&
   !EFI_ERROR(gBS->CalculateCrc32(&A,232,&Crc)) && A.Crc32==Crc;
 RawValid=Valid && A.RawStageInverse==~A.RawStage && KnownPhase(A.RawStage);
 gST->ConOut->ClearScreen(gST->ConOut);
 Print(L"CAS X06 HANDOFF TRACE (read only)\r\n\r\n");
 Print(L"Current boot: %016lx\r\nRecord boot:  %016lx\r\n",Boot[13],A.BootStamp);
 Print(L"Record status: %s\r\n",Valid ? L"CRC/ABI/stable valid" : L"invalid or absent; no stage accepted");
 if(Valid) {
  Print(L"Sequence %lu  C stage %lx: %s\r\n",A.Sequence,A.Stage,Phase(A.Stage));
  Print(L"Raw pair %s  stage %lx: %s\r\n",RawValid ? L"valid" : L"INVALID",A.RawStage,RawValid ? Phase(A.RawStage) : L"none");
  Print(L"Status %lx  window %lx\r\nPA %lx  args %lx\r\nentry %lx\r\n",A.Status,A.Window,A.Address,A.Args,A.Entry);
  Print(L"Pre-EBS EL %lu  SCTLR %lx\r\nTCR %lx  stack %lx\r\n",A.CurrentEl>>2,A.Sctlr,A.Tcr,A.Stack);
  Print(L"%s\r\n",A.BootStamp==Boot[13] ? L"Record belongs to this firmware boot." : L"Record is from a DIFFERENT boot; keep its identity.");
  if(RawValid && A.RawStage==0xe0) {
   BOOLEAN Good=ExceptionValid(&E,&F,&A);
   Print(L"Exception record: %s\r\n",Good ? L"valid identity/XOR/inverse/stable" : L"INVALID; no exception accepted");
   if(Good) {
    Print(L"Vector %lu previous %lx: %s\r\n",E.Vector,E.PreviousStage,Phase(E.PreviousStage));
    Print(L"ESR %016lx FAR %016lx\r\nELR %016lx SPSR %016lx\r\n",E.Esr,E.Far,E.Elr,E.Spsr);
    Print(L"SCTLR %lx TCR %lx\r\nTTBR0 %lx TTBR1 %lx\r\nSP %lx\r\n",E.Sctlr,E.Tcr,E.Ttbr0,E.Ttbr1,E.Stack);
   }
  }
 }
 Print(L"\r\nPower or Hold: return. No record was changed.\r\n");
 for(;;)if(!EFI_ERROR(gBS->WaitForEvent(1,&gST->ConIn->WaitForKey,&Index)) && !EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn,&Key)))
  if(Key.ScanCode==SCAN_ESC || Key.UnicodeChar==L'\r')return EFI_SUCCESS;
}
