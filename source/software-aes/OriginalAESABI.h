// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>

// Instance sizes are shared by the measured iOS17 and iPhone14,4/24A446
// families. arm64e also requires the original declaring class for each PAC
// discriminator; the selected build must qualify all compiled virtual slots.
class IOCryptoAccelerator : public IOService {
  OSDeclareDefaultStructors(IOCryptoAccelerator)
#if defined(__arm64e__)
public:
  virtual const char *getAlgorithmName();
#endif
};
struct IOAESIV { unsigned char bytes[16]; };
struct IOAESKeyData {
  uint32_t bits;
  unsigned char bytes[32];
  uint32_t selector;
};
enum IOAESOperation : unsigned { IOAESEncrypt = 0, IOAESDecrypt = 1 };
class IOAESAcceleratorCommand;
using IOAESCompletion = void (*)(void *, int);
class IOAESAccelerator : public IOCryptoAccelerator {
  OSDeclareDefaultStructors(IOAESAccelerator)
  unsigned char originalFields[0x98];
public:
  bool start(IOService *) override;
  IOWorkLoop *getWorkLoop() const override;
  IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) override;
#if defined(__arm64e__)
  const char *getAlgorithmName() override;
#else
  virtual const char *getAlgorithmName();
#endif
  virtual IOReturn performAES(void *, void *, task_t, unsigned long long, IOAESIV *,
                             IOAESOperation, IOAESKeyData *, IOAESCompletion, void *);
  virtual IOReturn performAES(IOMemoryDescriptor *, IOMemoryDescriptor *, unsigned long long,
                             IOAESIV *, IOAESOperation, IOAESKeyData *, unsigned long long,
                             unsigned long long, IOAESCompletion, void *);
  virtual bool isKeySupported(unsigned);
  virtual unsigned getSupportedOptions();
  virtual IOReturn performAESQuantum(IOAESAcceleratorCommand *);
  virtual void completeAESQuantum(IOAESAcceleratorCommand *);
  virtual IOReturn performImmediateAES(IOAESAcceleratorCommand *);
  virtual void disableHardwareKey(unsigned);
  virtual void createSpecialKeys();
  virtual IOReturn _queueLocked(IOAESAcceleratorCommand *);
  virtual void _startCommand();
  virtual void _completeAES(IOAESAcceleratorCommand *);
};
static_assert(sizeof(IOService) == 0x88, "source IOService ABI changed");
static_assert(sizeof(IOCryptoAccelerator) == 0x88, "original crypto parent ABI changed");
static_assert(sizeof(IOAESAccelerator) == 0x120, "original AES instance ABI changed");
static_assert(sizeof(IOAESIV) == 16 && __builtin_offsetof(IOAESKeyData, selector) == 36,
              "original raw key/IV ABI changed");
