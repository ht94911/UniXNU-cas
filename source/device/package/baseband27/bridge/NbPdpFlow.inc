/* SPDX-License-Identifier: BSD-2-Clause */
/* Called from the existing user client; flow values share the PDP lock. */
static struct nb_pdp_flow bb_pdp_flows;
static IOReturn bb_pdp_flow(uint32_t selector, IOExternalMethodArguments *args)
{
  if (!bb_pdp_lock) return kIOReturnNotReady;
  if (args->structureInputDescriptor || args->structureOutputDescriptor || args->structureOutputSize)
    return kIOReturnBadArgument;
  uint32_t count = args->scalarOutputCount;
  IOLockLock(bb_pdp_lock);
  int result = nb_pdp_flow_call(&bb_pdp_flows, selector, args->scalarInput, args->scalarInputCount,
                              args->structureInput, args->structureInputSize, args->scalarOutput, &count);
  IOLockUnlock(bb_pdp_lock);
  if (!result) args->scalarOutputCount = count;
  return result;
}
