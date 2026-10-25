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
 case 0x41:return L"XNU start_first_cpu";case 0x7f:return L"pre-EBS failure/cleanup";
 default:return L"UNKNOWN (not accepted)";
 }
}
EFI_STATUS EFIAPI CasTraceViewEntry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
 CAS_HANDOFF_TRACE A,B;UINTN I,Index;UINT32 Crc=0;BOOLEAN Valid,RawValid;EFI_INPUT_KEY Key;
 volatile CONST UINT8 *P=(VOID *)(UINTN)CAS_TRACE_BASE;CONST UINT64 *Boot=(VOID *)(UINTN)0x9f940000;
 (VOID)Image;(VOID)System;
 if(!gST->ConIn || !gST->ConOut || !LogRegionReadable())return EFI_UNSUPPORTED;
 for(I=0;I<sizeof(A);I++)((UINT8 *)&A)[I]=P[I];MemoryFence();
 for(I=0;I<sizeof(B);I++)((UINT8 *)&B)[I]=P[I];MemoryFence();
 Valid=CompareMem(&A,&B,sizeof(A))==0 && A.Magic==CAS_TRACE_MAGIC && A.Version==1 && A.Bytes==sizeof(A) && A.Sequence>0 && A.BootStamp>0 &&
   !EFI_ERROR(gBS->CalculateCrc32(&A,232,&Crc)) && A.Crc32==Crc;
 RawValid=Valid && A.RawStageInverse==~A.RawStage && ((A.RawStage>=1 && A.RawStage<=5) || A.RawStage==0x10 || A.RawStage==0x20 || A.RawStage==0x21 || A.RawStage==0x22 || A.RawStage==0x40 || A.RawStage==0x41 || A.RawStage==0x7f);
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
 }
 Print(L"\r\nPower or Hold: return. No record was changed.\r\n");
 for(;;)if(!EFI_ERROR(gBS->WaitForEvent(1,&gST->ConIn->WaitForKey,&Index)) && !EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn,&Key)))
  if(Key.ScanCode==SCAN_ESC || Key.UnicodeChar==L'\r')return EFI_SUCCESS;
}
