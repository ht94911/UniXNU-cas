/* SPDX-License-Identifier: MIT */
#include "NezhaCameraEepromAf.h"
#include <stddef.h>

/* Original four module EEPROM read maps use0xa2/address2/data1, and
 * both original OEM checksum functions validate flag27/data28..42/43.
 * Reuse the same repeated-START CCI read proved for vendor-ID selection. */
enum NezhaCameraCciStatus nezha_camera_eeprom_read_af(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraEepromAfRead *result)
{
    if (!result) return NezhaCciInvalid;
    *result=(struct NezhaCameraEepromAfRead){0};
    if (!io || !io->read || !io->write || !io->wait_ms) return NezhaCciInvalid;
    while (result->bytes_read<NezhaCameraEepromAfPrefixBytes) {
        const unsigned left=NezhaCameraEepromAfPrefixBytes-result->bytes_read;
        const unsigned count=left<3 ? left:3;
        const enum NezhaCameraCciStatus status=nezha_camera_cci_read(io,1,0x51,
            (uint16_t)result->bytes_read,result->bytes+result->bytes_read,count,&result->transfer);
        if (status!=NezhaCciOk) return status;
        result->bytes_read+=count;
    }
    const uint8_t *b=result->bytes;
    if (b[0x27]!=1) return NezhaCciFifo;
    unsigned sum=0;
    for (unsigned i=0x28;i<=0x42;++i) sum+=b[i];
    if (b[0x43]!=sum%255u+1u) return NezhaCciFifo;
    const uint16_t macro=(uint16_t)((uint16_t)b[0x2d]<<8 | b[0x2e]);
    const uint16_t infinity=(uint16_t)((uint16_t)b[0x31]<<8 | b[0x32]);
    if (macro>4095 || infinity>4095 || macro==infinity) return NezhaCciFifo;
    result->AF_checksum_valid=true;
    result->macro_dac=macro;
    result->infinity_dac=infinity;
    return NezhaCciOk;
}
