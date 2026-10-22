// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compile the actual driver with real UEFI type definitions and mock tables.
#include "CasNoCapsuleDxe.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static EFI_BOOT_SERVICES Boot;
static EFI_RUNTIME_SERVICES Runtime;
EFI_BOOT_SERVICES *gBS = &Boot;
EFI_RUNTIME_SERVICES *gRT = &Runtime;
EFI_GUID gEfiCapsuleArchProtocolGuid = EFI_CAPSULE_ARCH_PROTOCOL_GUID;
static EFI_STATUS InstallResult;
static unsigned Installs;

static EFI_STATUS EFIAPI PreviousUpdate (
  EFI_CAPSULE_HEADER **Headers, UINTN Count, EFI_PHYSICAL_ADDRESS List)
{
  return EFI_NOT_READY;
}

static EFI_STATUS EFIAPI PreviousQuery (
  EFI_CAPSULE_HEADER **Headers, UINTN Count, UINT64 *Maximum, EFI_RESET_TYPE *Reset)
{
  return EFI_NOT_READY;
}

static EFI_STATUS EFIAPI Install (EFI_HANDLE *Handle, ...)
{
  va_list Args;
  ++Installs;
  assert(*Handle == NULL);
  // The new callbacks must be usable before the architecture notification.
  assert(gRT->UpdateCapsule == RejectUpdateCapsule);
  assert(gRT->QueryCapsuleCapabilities == RejectQueryCapsuleCapabilities);
  va_start(Args, Handle);
  assert(va_arg(Args, EFI_GUID *) == &gEfiCapsuleArchProtocolGuid);
  assert(va_arg(Args, VOID *) == NULL);
  assert(va_arg(Args, EFI_GUID *) == NULL);
  va_end(Args);
  return InstallResult;
}

int main (int Count, char **Arguments)
{
  assert(Count == 2);
  Runtime.UpdateCapsule = PreviousUpdate;
  Runtime.QueryCapsuleCapabilities = PreviousQuery;
  // Every other boot/runtime service is NULL: unexpected calls must fail.
  Boot.InstallMultipleProtocolInterfaces = Install;
  if (strcmp(Arguments[1], "reject") == 0) {
    UINT64 Maximum = 0x123456789abcdef0ULL;
    EFI_RESET_TYPE Reset = EfiResetShutdown;
    assert(CasNoCapsuleInitialize(NULL, NULL) == EFI_SUCCESS);
    assert(gRT->UpdateCapsule(NULL, 0, 0) == EFI_UNSUPPORTED);
    assert(gRT->QueryCapsuleCapabilities(NULL, 0, NULL, NULL) == EFI_UNSUPPORTED);
    // Unmapped input pointers, an overflowing count and a physical address
    // must not be touched, interpreted as a format, or queued across reset.
    assert(gRT->UpdateCapsule((VOID *)1, ~(UINTN)0, ~(UINT64)0) == EFI_UNSUPPORTED);
    assert(gRT->QueryCapsuleCapabilities((VOID *)1, ~(UINTN)0, &Maximum, &Reset) == EFI_UNSUPPORTED);
    assert(Maximum == 0x123456789abcdef0ULL && Reset == EfiResetShutdown);
    assert(Installs == 1);
  } else if (strcmp(Arguments[1], "install") == 0) {
    assert(CasNoCapsuleInitialize(NULL, NULL) == EFI_SUCCESS);
    assert(Installs == 1);
    assert(gRT->UpdateCapsule == RejectUpdateCapsule);
    assert(gRT->QueryCapsuleCapabilities == RejectQueryCapsuleCapabilities);
  } else {
    assert(strcmp(Arguments[1], "failure") == 0);
    InstallResult = EFI_OUT_OF_RESOURCES;
    assert(CasNoCapsuleInitialize(NULL, NULL) == EFI_OUT_OF_RESOURCES);
    assert(Installs == 1);
    assert(gRT->UpdateCapsule == PreviousUpdate);
    assert(gRT->QueryCapsuleCapabilities == PreviousQuery);
  }
  puts("PASS");
  return 0;
}
