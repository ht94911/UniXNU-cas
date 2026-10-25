// SPDX-License-Identifier: BSD-2-Clause-Patent
// Same reviewed facts producer, staged by source hashes; no alternate RAM parser.
#include "CasDiag.c"
#include "Guard.h"
STATIC BOOLEAN mCpuAttempted,mCpuArmed;
STATIC EFI_STATUS Context(VOID) {
  mBoot=(VOID *)(UINTN)CAS_LOG_BASE;
  if(!gST->ConOut || !gST->ConIn)return EFI_NOT_READY;
  // App is specific to the bounded cas firmware. Arbitrary UEFI launches refuse.
  // The profile places this fixed control region in the UEFI map; validate it before use.
  { EFI_MEMORY_DESCRIPTOR *Map=NULL;UINTN Size=0,KeyValue,Stride=sizeof(*Map),Capacity,I;UINT32 Version;BOOLEAN Found=FALSE;EFI_STATUS S;
    S=gBS->GetMemoryMap(&Size,NULL,&KeyValue,&Stride,&Version);
    if(S!=EFI_BUFFER_TOO_SMALL || Size>0x100000 || Stride<sizeof(*Map) || Stride>4096)return EFI_UNSUPPORTED;
    Capacity=Size+16*Stride;Map=AllocatePool(Capacity);if(!Map)return EFI_OUT_OF_RESOURCES;Size=Capacity;
    S=gBS->GetMemoryMap(&Size,Map,&KeyValue,&Stride,&Version);
    if(!EFI_ERROR(S) && Stride>=sizeof(*Map) && Stride<=4096 && Size<=Capacity && Size%Stride==0)
      for(I=0;I<Size/Stride;++I) {EFI_MEMORY_DESCRIPTOR *D=(VOID *)((UINT8 *)Map+I*Stride);
        if(D->Type==EfiReservedMemoryType && D->PhysicalStart==CAS_LOG_BASE && D->NumberOfPages==CAS_LOG_SIZE/4096 && !(D->Attribute&EFI_MEMORY_RP))Found=TRUE;}
    FreePool(Map);if(!Found)return EFI_UNSUPPORTED;
  }
  if(mBoot->Magic!=CAS_BOOT_MAGIC || mBoot->Version!=CAS_BOOT_VERSION || mBoot->Size!=sizeof(*mBoot) ||
      mBoot->FdtCopy!=CAS_FDT_BASE || mBoot->FdtSize<40 || mBoot->FdtSize>CAS_FDT_SIZE ||
      !mBoot->Reserved[0] || (mBoot->EntryEl!=1 && mBoot->EntryEl!=2))return EFI_UNSUPPORTED;
  mBootStamp=mBoot->Reserved[0];return EFI_SUCCESS;
}
EFI_STATUS CasCpu0Preflight(UINT64 Address,UINT64 Size) {
 CD_RANGE C={Address,Size},Container={CD_PUBLISH_BASE,CD_PUBLISH_SIZE};EFI_STATUS S;int Gate;
 if(mCpuAttempted)return EFI_ACCESS_DENIED;
 mCpuAttempted=TRUE;S=Context();if(EFI_ERROR(S))return S;
 LoadPrevious();mUsed=0;mFlags=0;mDropped=FALSE;
 Line("CPU0 GUARD begin boot=%016Lx old-status=%d; old flags never authorize this request\n",mBootStamp,mPreviousStatus);
 Enumerate();if(!UefiFacts(FALSE))mComplete&=~4U;
 Gate=CasDiagCpu0Eligible(C,mBanks,mBankCount,mSmem,mSmemCount,mFree,mFreeCount,mReserved,mReservedCount,mComplete);
 Line("CPU0 exact admission base=%016Lx bytes=%016Lx gate=%d mask=%u\n",Address,Size,Gate,mComplete);
 if(Gate!=CdAllowed || mDropped || !HighGcd(Container,TRUE) || !HighPermissions(C)) {
  Line("CPU0 REFUSED before owned allocation; recover to Android before another attempt\n");Persist();return EFI_ACCESS_DENIED;
 }
 mCpuArmed=TRUE;Line("CPU0 current facts passed; held full-range readback still required\n");Persist();return EFI_SUCCESS;
}
STATIC BOOLEAN HeldDescriptor(CD_RANGE C) {
 UINTN N=sizeof(mHighMap),KeyValue,Stride=sizeof(EFI_MEMORY_DESCRIPTOR),I;UINT32 Version;EFI_STATUS S;
 CD_RANGE Owned[CD_MAX_RANGES];CD_U32 Count=0;
 S=gBS->GetMemoryMap(&N,(VOID *)mHighMap,&KeyValue,&Stride,&Version);
 if(EFI_ERROR(S)||Version!=EFI_MEMORY_DESCRIPTOR_VERSION||Stride<sizeof(EFI_MEMORY_DESCRIPTOR)||Stride>4096||!N||N>sizeof(mHighMap)||N%Stride||N/Stride>1024)return FALSE;
 for(I=0;I<N/Stride;I++) {
  EFI_MEMORY_DESCRIPTOR *P=(VOID *)((UINT8 *)mHighMap+I*Stride);CD_RANGE R;
  if(P->NumberOfPages>MAX_UINT64/4096)return FALSE;
  R.Base=P->PhysicalStart;R.Size=P->NumberOfPages*4096;
  if(!CasDiagRangeValid(R,4096))return FALSE;
  if(R.Base<C.Base+C.Size && C.Base<R.Base+R.Size) {
   if(P->Type!=EfiLoaderCode || !(P->Attribute&EFI_MEMORY_WB) || P->Attribute&(EFI_MEMORY_RUNTIME|EFI_MEMORY_RO|EFI_MEMORY_RP) || Count>=CD_MAX_RANGES)return FALSE;
   Owned[Count++]=R;
  }
 }
 return CasDiagHeldCoverage(C,Owned,Count)!=0;
}
EFI_STATUS CasCpu0ProbeHeld(UINT64 Address,UINT64 Size) {
 CD_RANGE C={Address,Size};CD_PROBE_RESULT R;
 STATIC CONST CD_HELD_PROBE_IO Io={ProbeWrite,ProbeCache,ProbeRead,ProbeClear};
 if(!mCpuArmed || !mBoot || mBootStamp!=mBoot->Reserved[0])return EFI_ACCESS_DENIED;
 mCpuArmed=FALSE;
 if(Address!=CD_CPU0_BASE || Size!=CD_REQUEST || !HeldDescriptor(C)) {
  Line("CPU0 HELD descriptor rejected; no RAM data access\n");Persist();return EFI_ACCESS_DENIED;
 }
 Line("CPU0 HELD INTENT base=%016Lx bytes=%016Lx; SAME allocation retained\n",Address,Size);Persist();
 R=CasDiagRunHeldProbe(C,CdAllowed,&Io,NULL);
 Line("CPU0 HELD result=%d cleanup=%d; no release claimed\n",R.Result,R.CleanupFailed);
 if(R.Result!=CdProbePass || R.CleanupFailed || mDropped) {Persist();return EFI_DEVICE_ERROR;}
 Line("CPU0 HELD TWO-PATTERN/CLEAR PASS; retained for this kernel load only\n");Persist();return EFI_SUCCESS;
}
