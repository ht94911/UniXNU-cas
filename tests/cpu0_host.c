// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host UEFI service double around the ENTIRE transformed DarwinBoot.c.
// Only system registers and the trace SP read are replaced for x86_64.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <setjmp.h>
#undef NULL
#include "DarwinBoot.c"
#include "DiagCore.h"
static jmp_buf ExitPoint;
static int EbsCalls,BranchCalls,DeadCalls,PreflightCalls,HeldCalls,HeldPassed;
static void VerifyLoaded(UINT64);
static void *Pools[4096];static UINTN PoolSlots;
static UINT8 *KernelInput,*TreeInput;static UINTN KernelBytes,TreeBytes;
static KERNEL ExpectedKernel;static int Mode,Allocated,AllocCount,FreeCount,Verified,PoolCount;
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;
STATIC UINT64 CasHostReadReg(CONST CHAR8 *Name){if(!strcmp(Name,"mpidr_el1"))return Mode==28?0x80000001ULL:0x80000000ULL;return 4;}

EFI_GUID gEfiFileInfoGuid,gEfiSimpleFileSystemProtocolGuid,gEfiGraphicsOutputProtocolGuid;
EFI_GUID gEfiMemoryAttributeProtocolGuid,gEfiLoadedImageProtocolGuid,gUniXNULoaderProtocolGuid;
VOID *EFIAPI AllocatePool(UINTN N){VOID *P;if(EbsCalls)abort();P=malloc(N);if(P){if(PoolSlots>=4096)abort();Pools[PoolSlots++]=P;PoolCount++;}return P;}
VOID EFIAPI FreePool(VOID *P){UINTN I;if(EbsCalls)abort();if(P){for(I=0;I<PoolSlots && Pools[I]!=P;I++){}if(I==PoolSlots)abort();Pools[I]=NULL;PoolCount--;free(P);}}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI CopyMem(VOID *P,CONST VOID *S,UINTN N){return memcpy(P,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI ScanMem8(CONST VOID *P,UINTN N,UINT8 V){return memchr(P,V,N);}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *P){return strlen(P);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
UINTN EFIAPI StrSize(CONST CHAR16 *P){UINTN N=0;while(P[N])N++;return (N+1)*2;}
RETURN_STATUS EFIAPI StrCatS(CHAR16 *D,UINTN N,CONST CHAR16 *S){UINTN I=0,J=0;while(D[I])I++;while(S[J]){if(I+1>=N)return RETURN_BUFFER_TOO_SMALL;D[I++]=S[J++];}D[I]=0;return RETURN_SUCCESS;}
UINT32 EFIAPI ReadUnaligned32(CONST VOID *P){UINT32 V;memcpy(&V,P,4);return V;}
UINT64 EFIAPI ReadUnaligned64(CONST VOID *P){UINT64 V;memcpy(&V,P,8);return V;}
UINT64 EFIAPI WriteUnaligned64(VOID *P,UINT64 V){memcpy(P,&V,8);return V;}
VOID EFIAPI MemoryFence(VOID){__sync_synchronize();}
UINTN EFIAPI Print(CONST CHAR16 *F,...){(void)F;return 0;}
UINTN EFIAPI UnicodeSPrint(CHAR16 *B,UINTN N,CONST CHAR16 *F,...){(void)F;if(N>=2)B[0]=0;return 0;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(void)L;(void)F;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(void)L;return FALSE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"assert %s:%zu %s\n",F,L,D);abort();}
VOID EFIAPI CpuDeadLoop(VOID){if(Mode<10 || Mode>12)abort();DeadCalls++;longjmp(ExitPoint,2);}
UINT64 EFIAPI DarwinCurrentEl(VOID){return 4;}
VOID EFIAPI DarwinEnter(UINT64 A,UINT64 E,UINT64 M,UINT64 S,UINT64 V,UINT64 B){(void)V;(void)B;
 if(!EbsCalls || !HeldPassed || S!=SIZE_256MB || M!=0x100004000ULL || E!=M+ExpectedKernel.Entry-ExpectedKernel.Base || A!=ALIGN_VALUE(M+ExpectedKernel.End-ExpectedKernel.Base,SIZE_16KB) || mTrace.Stage!=0x10 || mTrace.Reserved[0]!=mTrace.BootStamp || mTrace.Reserved[1]!=M || mTrace.Reserved[2]!=S || mTrace.Reserved[3]!=1 || mTrace.Reserved[4]!=CAS_CPU0_CANDIDATE_TAG || mTrace.Reserved[6]!=0)abort();
 VerifyLoaded(M);BranchCalls++;longjmp(ExitPoint,1);}

static EFI_STATUS EFIAPI Crc(VOID *Data,UINTN N,UINT32 *Out){UINT32 V=~0U;UINTN I,J;if(EbsCalls)abort();for(I=0;I<N;I++){V^=((UINT8 *)Data)[I];for(J=0;J<8;J++)V=(V>>1)^((V&1)?0xedb88320:0);}*Out=~V;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Map(UINTN *Size,EFI_MEMORY_DESCRIPTOR *Data,UINTN *Key,UINTN *Stride,UINT32 *Ver){
 UINTN Need=2*sizeof(*Data);*Stride=sizeof(*Data);*Key=1;*Ver=1;
 if((Mode==12 && EbsCalls) || (Mode==13 && mTrace.Stage==4))return EFI_DEVICE_ERROR;
 if(!Data || *Size<Need){*Size=Need;return EFI_BUFFER_TOO_SMALL;}
 memset(Data,0,Need);*Size=Need;Data[0].Type=EfiReservedMemoryType;Data[0].PhysicalStart=0x9f940000;Data[0].NumberOfPages=0x40;Data[0].Attribute=EFI_MEMORY_UC|EFI_MEMORY_XP;
 if(Mode==4)Data[0].Type=EfiBootServicesData;
 Data[1].Type=Mode==3?EfiReservedMemoryType:EfiConventionalMemory;Data[1].PhysicalStart=0x100000000;Data[1].NumberOfPages=0x20000;Data[1].Attribute=EFI_MEMORY_WB|EFI_MEMORY_XP;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Pages(EFI_ALLOCATE_TYPE Type,EFI_MEMORY_TYPE Kind,UINTN N,EFI_PHYSICAL_ADDRESS *Address){
 if(Type!=AllocateAddress || Kind!=EfiLoaderCode || N!=0x10000 || *Address!=0x100004000ULL || Allocated)abort();
 if(mmap((VOID *)(UINTN)*Address,N*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED){perror("mmap owned");abort();}
 Allocated=1;AllocCount++;return EFI_SUCCESS;
}
static void VerifyLoaded(UINT64 A){
 UINTN I;DARWIN_BOOT_ARGS *Args;UINT64 ArgsPA=ALIGN_VALUE(A+ExpectedKernel.End-ExpectedKernel.Base,SIZE_16KB);

  for(I=0;I<ExpectedKernel.Count;I++){SEGMENT *S=&ExpectedKernel.Segments[I];UINT8 *P=(VOID *)(UINTN)(A+S->Address-ExpectedKernel.Base);UINTN J;
   if(memcmp(P,KernelInput+S->FileOffset,S->FileSize))abort();
   for(J=S->FileSize;J<S->Size;J++)if(P[J])abort();
  }
  Args=(VOID *)(UINTN)ArgsPA;
  if(Args->Revision!=2 || Args->Version!=2 || Args->VirtualBase!=ExpectedKernel.Base || Args->PhysicalBase!=A || Args->MemorySize!=0x10000000 || Args->MemorySizeActual!=0x10000000 || Args->DeviceTree!=ExpectedKernel.Base+ArgsPA+4096-A || Args->DeviceTreeLength!=TreeBytes || strcmp(Args->CommandLine,Mode==14?"-v keepsyms=1 debug=0x14e cpus=1 cas-validate=1":"-v keepsyms=1 debug=0x14e cpus=1"))abort();
  if(memcmp((VOID *)(UINTN)(ArgsPA+4096),TreeInput,TreeBytes))abort();
  Verified++;
}
static EFI_STATUS EFIAPI Release(EFI_PHYSICAL_ADDRESS A,UINTN N){
 if(EbsCalls || !Allocated || A!=0x100004000ULL || N!=0x10000)abort();
 if(mTrace.Stage==4)VerifyLoaded(A);
 if(munmap((VOID *)(UINTN)A,N*4096))abort();Allocated=0;FreeCount++;return EFI_SUCCESS;
}
static int PatternWrite(void *C,CD_RANGE R,CD_U32 Pass){CD_U64 I;volatile CD_U64 *P=(VOID *)(UINTN)R.Base;(void)C;
 if(!Allocated || R.Base!=0x100004000ULL || R.Size!=SIZE_256MB)abort();for(I=0;I<R.Size/8;I++)P[I]=CasDiagPattern(R.Base+I*8,Pass);return 0;}
static int PatternRead(void *C,CD_RANGE R,CD_U32 Pass){CD_U64 I;volatile CD_U64 *P=(VOID *)(UINTN)R.Base;(void)C;
 if(Mode==8 && Pass==1)P[R.Size/8-1]^=1;
 for(I=0;I<R.Size/8;I++)if(P[I]!=CasDiagPattern(R.Base+I*8,Pass))return 1;return 0;}
static int PatternCache(void *C,CD_RANGE R){(void)C;(void)R;__sync_synchronize();return 0;}
static int PatternClear(void *C,CD_RANGE R){(void)C;memset((void *)(UINTN)R.Base,0,R.Size);return 0;}
EFI_STATUS CasCpu0Preflight(UINT64 A,UINT64 N){if(Allocated || PreflightCalls || A!=0x100004000ULL || N!=SIZE_256MB)abort();PreflightCalls++;return Mode==7?EFI_ACCESS_DENIED:EFI_SUCCESS;}
EFI_STATUS CasCpu0ProbeHeld(UINT64 A,UINT64 N){CD_HELD_PROBE_IO Io={PatternWrite,PatternCache,PatternRead,PatternClear};CD_PROBE_RESULT R;
 if(!Allocated || PreflightCalls!=1 || HeldCalls)abort();HeldCalls++;R=CasDiagRunHeldProbe((CD_RANGE){A,N},0,&Io,NULL);if(R.Result || R.CleanupFailed)return EFI_DEVICE_ERROR;HeldPassed=1;return EFI_SUCCESS;}
typedef struct{EFI_FILE_PROTOCOL Protocol;UINT8 *Data;UINTN Size;} HOST_FILE;
static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *This){free(This);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *This,EFI_GUID *G,UINTN *Size,VOID *Buffer){HOST_FILE *F=(VOID *)This;(void)G;
 if(!Buffer || *Size<sizeof(EFI_FILE_INFO)){*Size=sizeof(EFI_FILE_INFO);return EFI_BUFFER_TOO_SMALL;}
 memset(Buffer,0,*Size);((EFI_FILE_INFO *)Buffer)->Size=sizeof(EFI_FILE_INFO);((EFI_FILE_INFO *)Buffer)->FileSize=F->Size;*Size=sizeof(EFI_FILE_INFO);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *This,UINTN *N,VOID *P){HOST_FILE *F=(VOID *)This;if(*N!=F->Size)abort();memcpy(P,F->Data,*N);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *Root,EFI_FILE_PROTOCOL **Out,CHAR16 *Path,UINT64 Flags,UINT64 Attr){
 char Name[256];UINTN I;HOST_FILE *F;(void)Root;(void)Attr;if(Flags!=EFI_FILE_MODE_READ)abort();
 for(I=0;Path[I] && I<255;I++)Name[I]=(char)Path[I];Name[I]=0;
 if(strcmp(Name,"\\XNU\\kernel.macho") && strcmp(Name,"\\XNU\\kernel.macho.adt"))return EFI_NOT_FOUND;
 if(Mode==5 && strstr(Name,".adt"))return EFI_NOT_FOUND;
 F=calloc(1,sizeof(*F));F->Protocol.Close=Close;F->Protocol.Read=Read;F->Protocol.GetInfo=Info;
 F->Data=strstr(Name,".adt")?TreeInput:KernelInput;F->Size=strstr(Name,".adt")?TreeBytes:KernelBytes;*Out=&F->Protocol;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,EFI_FILE_PROTOCOL **Out){HOST_FILE *F=calloc(1,sizeof(*F));(void)This;F->Protocol.Open=Open;F->Protocol.Close=Close;*Out=&F->Protocol;return EFI_SUCCESS;}
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs={0,Volume};
static EFI_STATUS EFIAPI Attributes(EFI_MEMORY_ATTRIBUTE_PROTOCOL *This,EFI_PHYSICAL_ADDRESS Base,UINT64 Length,UINT64 *Out){(void)This;if(Base!=0x100004000ULL || Length!=SIZE_256MB)abort();*Out=Mode==2?EFI_MEMORY_RO:(Mode==6?0:EFI_MEMORY_XP);return EFI_SUCCESS;}
static EFI_MEMORY_ATTRIBUTE_PROTOCOL MemoryAttributes={Attributes,NULL,NULL};
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *Registration,VOID **Out){(void)Registration;if(G==&gEfiMemoryAttributeProtocolGuid){*Out=&MemoryAttributes;return EFI_SUCCESS;}return EFI_NOT_FOUND;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **Out){(void)H;if(G==&gEfiSimpleFileSystemProtocolGuid){*Out=&Fs;return EFI_SUCCESS;}return EFI_NOT_FOUND;}
static EFI_STATUS EFIAPI Ebs(EFI_HANDLE I,UINTN Key){(void)I;(void)Key;
 if(!HeldPassed || !Allocated || mTrace.Stage!=0x10)abort();EbsCalls++;
 if(Mode==10)return EFI_DEVICE_ERROR;
 if(Mode==11 || (Mode==9 && EbsCalls==1) || (Mode==12 && EbsCalls==1))return EFI_INVALID_PARAMETER;
 return EFI_SUCCESS;}

static UINT8 *Input(const char *Path,UINTN *Size){FILE *F=fopen(Path,"rb");long N;UINT8 *P;if(!F)abort();fseek(F,0,SEEK_END);N=ftell(F);rewind(F);P=malloc(N);if(fread(P,1,N,F)!=(size_t)N)abort();fclose(F);*Size=N;return P;}
static CAS_HANDOFF_TRACE BeforeRequest;
static void SeedPriorValidation(void){
 CAS_HANDOFF_TRACE T={0};UINT32 C;
 if(Mode!=14 && Mode!=20){
  T.Magic=CAS_TRACE_MAGIC;T.Version=1;T.Bytes=256;T.BootStamp=0x50001;T.Sequence=1;T.Stage=5;T.RawStage=5;T.RawStageInverse=~5ULL;
  T.Reserved[0]=T.BootStamp;T.Reserved[1]=0x100004000ULL;T.Reserved[2]=SIZE_256MB;T.Reserved[3]=1;T.Reserved[4]=CAS_CPU0_CANDIDATE_TAG;T.Reserved[5]=0x80000000ULL;T.Reserved[6]=0;
  if(Mode==17)T.Reserved[4]^=1;
  if(Mode==18){T.BootStamp=0x60001;T.Reserved[0]=T.BootStamp;}
  if(Mode==19){T.Stage=0x10;T.RawStage=0x41;T.RawStageInverse=~0x41ULL;}
  if(Mode==21)T.Version=2;
  if(Mode==22)T.RawStageInverse^=1;
  if(Mode==23)T.Status=EFI_DEVICE_ERROR;
  if(Mode==24)T.Reserved[1]=0x100000000ULL;
  if(Mode==25)T.Reserved[3]=0;
  if(Mode==26){T.BootStamp=0;T.Reserved[0]=0;}
  if(Mode==27)T.Sequence=0;
  if(Mode==29){T.Reserved[5]=0x80000001ULL;T.Reserved[6]=1;}
  Crc(&T,232,&C);T.Crc32=C;if(Mode==16)T.Address^=1;
 }
 memcpy((VOID *)(UINTN)CAS_TRACE_BASE,&T,256);BeforeRequest=T;
}
int main(int Argc,char **Argv){
 EFI_BOOT_SERVICES Services={0};EFI_SYSTEM_TABLE System={0};UNIXNU_LOADER_BOOT_REQUEST Request={0};EFI_STATUS Status;UINT64 *Header;
 if(Argc!=5)return 2;Mode=atoi(Argv[3]);KernelInput=Input(Argv[1],&KernelBytes);TreeInput=Input(Argv[2],&TreeBytes);
 if(ParseKernel(KernelInput,KernelBytes,&ExpectedKernel)!=EFI_SUCCESS)abort();if(Mode==1)KernelInput[8]=1;
 if(mmap((VOID *)(UINTN)0x9f940000,0x40000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED)abort();
 Header=(VOID *)(UINTN)0x9f940000;Header[0]=0x0000000148534143ULL;Header[1]=128;Header[13]=0x60001;
 Services.GetMemoryMap=Map;Services.AllocatePages=Pages;Services.FreePages=Release;Services.CalculateCrc32=Crc;Services.LocateProtocol=Locate;Services.HandleProtocol=Handle;Services.ExitBootServices=Ebs;gBS=&Services;gST=&System;
 Request.Volume=(VOID *)1;Request.KernelPath=L"\\XNU\\kernel.macho";Request.BootArgs=L"-v keepsyms=1 debug=0x14e cpus=1";
 if(Mode==14)Request.BootArgs=L"-v keepsyms=1 debug=0x14e cpus=1 cas-validate=1";
 if(Mode==15)Request.BootArgs=L"-v cpus=8";
 SeedPriorValidation();
 {int Jump=setjmp(ExitPoint);if(Jump==0)Status=Boot(NULL,&Request);else Status=Jump==1?EFI_SUCCESS:EFI_ABORTED;}
 {FILE *F=fopen(Argv[4],"wb");if(!F || fwrite((VOID *)(UINTN)CAS_TRACE_BASE,1,256,F)!=256)abort();fclose(F);}
 if((Mode==0 || Mode==6 || Mode==9 || (!CAS_CPU0_REQUIRE_RECOVERED && Mode>=16 && Mode!=28)) && (Status!=EFI_SUCCESS || BranchCalls!=1 || Verified!=1 || !HeldPassed || FreeCount!=0 || !Allocated))abort();
 if((Mode==10 || Mode==11 || Mode==12) && (DeadCalls!=1 || BranchCalls || !Allocated))abort();
 if(Mode!=0 && Mode!=6 && Mode!=9 && Mode!=10 && Mode!=11 && Mode!=12 && Mode!=14 && (CAS_CPU0_REQUIRE_RECOVERED || Mode<16 || Mode==28) && (!EFI_ERROR(Status) || EbsCalls || BranchCalls || Allocated || PoolCount))abort();
 if(Mode==14 && (Status!=EFI_SUCCESS || EbsCalls || BranchCalls || !HeldPassed || FreeCount!=1 || Verified!=1 || Allocated || PoolCount || mTrace.Stage!=5))abort();
 if(Mode==15 && (PreflightCalls || AllocCount || EbsCalls || BranchCalls))abort();
 if(CAS_CPU0_REQUIRE_RECOVERED && Mode>=16 && Mode!=28 && (PreflightCalls || HeldCalls || AllocCount || EbsCalls || BranchCalls || memcmp((VOID *)(UINTN)CAS_TRACE_BASE,&BeforeRequest,256)))abort();
 if(Mode==9 && EbsCalls!=2)abort();if(Mode==11 && EbsCalls!=3)abort();
 printf("{\"mode\":%d,\"status\":\"0x%lx\",\"allocations\":%d,\"releases_before_EBS\":%d,\"layout_verified\":%d,\"ebs_calls\":%d,\"handoff_stub_calls\":%d,\"dead_loop_stub_calls\":%d,\"fresh_preflight_calls\":%d,\"held_probe_calls\":%d,\"held_passed\":%d,\"retained_pool_objects\":%d,\"XNU_executed\":false}\n",Mode,Status,AllocCount,FreeCount,Verified,EbsCalls,BranchCalls,DeadCalls,PreflightCalls,HeldCalls,HeldPassed,PoolCount);
 if(Mode==28 && (PreflightCalls || AllocCount || EbsCalls || BranchCalls || mTrace.Reserved[6]!=1))abort();
 // Host-only teardown, outside the firmware/loader. No boot service after EBS.
 if(Allocated)munmap((void *)(UINTN)0x100004000ULL,SIZE_256MB);
 {UINTN I;for(I=0;I<PoolSlots;I++)if(Pools[I])free(Pools[I]);}
 free(KernelInput);free(TreeInput);return 0;
}
