/* Host check of the iOS 27 bridge values derived by tools/baseband27/derive-bridge27.py. Not part of the driver. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "coachz_bb_data.h"
#include "coachz_bb_sahara.h"

int main(void)
{
    uint8_t r[0x60];
    cbs_read_register(3, r, 0x18);
    assert(cbs_get32(r + 4) == 0x001440e1u);
    cbs_read_register(4, r, sizeof r);
    assert(sizeof cbs_default_pk_hash == 48 && !memcmp(r, cbs_default_pk_hash, 48) && r[48] == 0 && r[0x5f] == 0);
    assert(r[0] == 0xd4 && r[47] == 0x87);

    uint8_t sdu[128];
    uint32_t n = cbd_dpm_open(sdu, sizeof sdu, 1);
    assert(n && sdu[7] == 0x11);
    assert(cbd_rd32(sdu + 10 + 9) == 2u && cbd_rd32(sdu + 10 + 13) == 24u);   /* TLV header 3 bytes, then the port */

    struct cbd_setup s = { 0 };
    s.has_dpm = s.has_wda = 1;
    uint32_t service = 0;
    n = cbd_next(&s, sdu, sizeof sdu, &service);
    assert(n && service == CBD_SVC_DPM);
    n = cbd_next(&s, sdu, sizeof sdu, &service);
    assert(n && service == CBD_SVC_WDA && cbd_u32(sdu, n, 0x12) == CBD_DAP_QMAP && cbd_u32(sdu, n, 0x13) == CBD_DAP_QMAP);
    s.wda_error = 0; s.dl_dap = CBD_DAP_QMAP;
    assert(!cbd_next(&s, sdu, sizeof sdu, &service) && s.step == CBD_DONE);   /* one format only: no fallback request */
    printf("bridge27 values: ok\n");
    return 0;
}
