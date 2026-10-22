// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "CasRamDiskDxe.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiBlockIoProtocolGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
VOID *EFIAPI CopyMem(VOID *Dst, CONST VOID *Src, UINTN N) { return memcpy(Dst,Src,N); }
static unsigned Installs;
static EFI_STATUS EFIAPI Install(EFI_HANDLE *Handle, ...) {
  assert(mMedia.ReadOnly && mMedia.MediaPresent && mMedia.LastBlock == 7);
  ++Installs;return EFI_SUCCESS;
}
int main(int argc, char **argv) {
  CAS_BOOT_INFO info = {0};UINT8 data[4096], output[1024];
  assert(argc==2);
  info.Magic=CAS_BOOT_MAGIC;info.Version=CAS_BOOT_VERSION;info.Size=sizeof(info);
  info.InitrdCopy=CAS_PAYLOAD_BASE;info.InitrdSize=sizeof(data);
  if(strcmp(argv[1],"info")==0) {
    assert(CasRamDiskInfoValid(&info));
    info.InitrdCopy++;assert(!CasRamDiskInfoValid(&info));info.InitrdCopy--;
    info.InitrdSize=0;assert(!CasRamDiskInfoValid(&info));
    info.InitrdSize=513;assert(!CasRamDiskInfoValid(&info));
    info.InitrdSize=CAS_PAYLOAD_SIZE+512ULL;assert(!CasRamDiskInfoValid(&info));
    info.InitrdSize=CAS_PAYLOAD_SIZE;assert(CasRamDiskInfoValid(&info));
    info.Magic=0;assert(!CasRamDiskInfoValid(&info));assert(!CasRamDiskInfoValid(NULL));
  } else if(strcmp(argv[1],"io")==0) {
    memset(data,0x5a,sizeof(data));mBase=data;mMedia.MediaId=1;mMedia.LastBlock=7;
    assert(Read(&mBlock,1,6,1024,output)==EFI_SUCCESS);assert(memcmp(output,data,1024)==0);
    assert(Read(&mBlock,2,0,512,output)==EFI_MEDIA_CHANGED);
    assert(Read(&mBlock,1,7,1024,output)==EFI_INVALID_PARAMETER);
    assert(Read(&mBlock,1,~(UINT64)0,512,output)==EFI_INVALID_PARAMETER);
    assert(Read(&mBlock,1,0,513,output)==EFI_BAD_BUFFER_SIZE);
    assert(Read(&mBlock,1,0,512,NULL)==EFI_INVALID_PARAMETER);
    assert(Read(&mBlock,1,0,0,NULL)==EFI_SUCCESS);
    assert(Write(&mBlock,1,~(UINT64)0,~(UINTN)0,(VOID*)1)==EFI_WRITE_PROTECTED);
    assert(Write(&mBlock,2,0,512,output)==EFI_MEDIA_CHANGED);
    for(unsigned i=0;i<sizeof(data);i++)assert(data[i]==0x5a);
  } else {
    assert(strcmp(argv[1],"install")==0);
    EFI_BOOT_SERVICES boot={0};boot.InstallMultipleProtocolInterfaces=Install;gBS=&boot;
    void *a=mmap((void*)CAS_LOG_BASE,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    void *b=mmap((void*)CAS_PAYLOAD_BASE,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(a!=(void*)-1 && b!=(void*)-1);memcpy(a,&info,sizeof(info));
    assert(CasRamDiskInitialize(NULL,NULL)==EFI_VOLUME_CORRUPTED);assert(Installs==0);
    ((UINT8*)b)[510]=0x55;((UINT8*)b)[511]=0xaa;
    assert(CasRamDiskInitialize(NULL,NULL)==EFI_SUCCESS);assert(Installs==1);
    ((CAS_BOOT_INFO*)a)->InitrdCopy=0;
    assert(CasRamDiskInitialize(NULL,NULL)==EFI_NOT_FOUND);assert(Installs==1);
  }
  puts("PASS");return 0;
}
