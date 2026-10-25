// SPDX-License-Identifier: BSD-2-Clause-Patent
// Independent X06 RAM mailbox. Never evidence that the kernel has executed.
#ifndef CAS_HANDOFF_TRACE_H
#define CAS_HANDOFF_TRACE_H
#include "TraceAbi.h"
STATIC BOOLEAN mTraceReady;
STATIC CAS_HANDOFF_TRACE mTrace;
STATIC VOID CasTrace (UINT64 Stage, EFI_STATUS Status, UINT64 Address,
                     UINT64 Args, UINT64 Entry, UINT64 Window) {
  UINT32 Crc; UINTN I; volatile UINT8 *P=(VOID *)(UINTN)CAS_TRACE_BASE;
  if (!mTraceReady) { return; }
  mTrace.Sequence++; mTrace.Stage=Stage; mTrace.Status=Status;
  mTrace.Address=Address; mTrace.Args=Args; mTrace.Entry=Entry; mTrace.Window=Window;
  mTrace.CurrentEl=READ_SYSREG(CurrentEL); mTrace.Sctlr=READ_SYSREG(sctlr_el1);
  mTrace.Tcr=READ_SYSREG(tcr_el1); __asm__ volatile ("mov %0, sp" : "=r"(mTrace.Stack));
  mTrace.Crc32=0; mTrace.RawStage=Stage; mTrace.RawStageInverse=~Stage;
  if (EFI_ERROR(gBS->CalculateCrc32(&mTrace,232,&Crc))) { mTraceReady=FALSE; return; }
  mTrace.Crc32=Crc;
  for(I=0;I<8;I++)P[I]=0;
  MemoryFence(); for(I=8;I<sizeof(mTrace);I++)P[I]=((UINT8 *)&mTrace)[I];
  MemoryFence(); for(I=0;I<8;I++)P[I]=((UINT8 *)&mTrace)[I]; MemoryFence();
}
STATIC BOOLEAN CasTraceStart (VOID) {
  EFI_MEMORY_DESCRIPTOR *Map,*D; EFI_STATUS Status; UINTN N=0,Key,Stride=sizeof(*Map),Cap,I;
  UINT32 Version; BOOLEAN Found=FALSE; CONST UINT64 *Boot=(VOID *)(UINTN)0x9f940000;
  mTraceReady=FALSE;
  Status=gBS->GetMemoryMap(&N,NULL,&Key,&Stride,&Version);
  if(Status!=EFI_BUFFER_TOO_SMALL || Stride<sizeof(*Map) || Stride>4096 || N>0x100000)return FALSE;
  Cap=N+16*Stride;Map=AllocatePool(Cap);if(!Map)return FALSE;N=Cap;
  Status=gBS->GetMemoryMap(&N,Map,&Key,&Stride,&Version);
  if(!EFI_ERROR(Status) && N<=Cap && Stride>=sizeof(*Map) && Stride<=4096 && N%Stride==0)
    for(I=0;I<N/Stride;I++) {D=(VOID *)((UINT8 *)Map+I*Stride);
      if(D->Type==EfiReservedMemoryType && D->PhysicalStart==0x9f940000 && D->NumberOfPages==0x40 && !(D->Attribute&EFI_MEMORY_RP))Found=TRUE;
    }
  FreePool(Map);if(!Found)return FALSE;
  // CAS_BOOT_INFO: magic/version at 0, size/stage at 8, per-boot stamp at 104.
  if(Boot[0]!=0x0000000148534143ULL || (UINT32)Boot[1]!=128 || Boot[13]==0)return FALSE;
  // Only a new loader request clears the extension; the viewer is read only.
  { volatile UINT8 *Extension=(VOID *)(UINTN)(CAS_TRACE_BASE+CAS_VM_EXCEPTION_OFFSET);
    for(I=0;I<sizeof(CAS_VM_EXCEPTION);I++)Extension[I]=0;MemoryFence(); }
  ZeroMem(&mTrace,sizeof(mTrace));mTrace.Magic=CAS_TRACE_MAGIC;mTrace.Version=1;
  mTrace.Bytes=sizeof(mTrace);mTrace.BootStamp=Boot[13];mTraceReady=TRUE;
  CasTrace(1,EFI_SUCCESS,0,0,0,0);return mTraceReady;
}
// Called before the first write to the selected owned allocation. This X06
// profile accepts exactly one fixed 4KiB-aligned window inside the mapped 512MiB.
STATIC EFI_STATUS CasWindowPermissions (UINT64 Address,UINT64 Size) {
  EFI_MEMORY_ATTRIBUTE_PROTOCOL *P=NULL; EFI_STATUS S; UINT64 A=0,B=0;
  if(Address!=0x100004000ULL || Size!=SIZE_256MB || Address+Size>0x120000000ULL)return EFI_ACCESS_DENIED;
  S=gBS->LocateProtocol(&gEfiMemoryAttributeProtocolGuid,NULL,(VOID **)&P);
  if(EFI_ERROR(S) || !P || !P->GetMemoryAttributes)return EFI_UNSUPPORTED;
  S=P->GetMemoryAttributes(P,Address,Size,&A);if(EFI_ERROR(S) || A!=EFI_MEMORY_XP)return EFI_ACCESS_DENIED;
  S=P->GetMemoryAttributes(P,Address,Size,&B);return EFI_ERROR(S) || B!=A ? EFI_ACCESS_DENIED : EFI_SUCCESS;
}
#endif
