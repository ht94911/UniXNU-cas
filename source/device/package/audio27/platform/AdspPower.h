/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_ADSP_POWER_H
#define NEZHA_ADSP_POWER_H
#include "NezhaAudioPlatform.h"
#include "../upstream/QcomQmp.h"
#include "../upstream/QcomRpmh.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {NaPowerMailbox,NaPowerTcs};
typedef struct {
    void *context;
    /* Lease arbiter belongs to the selected package's COMMON resource owner.
     * A lease is not a lock held while sleeping. Busy implies no acquisition.
     * Never infer a free TCS by scanning registers or seize a Wi-Fi mailbox. */
    int (*acquire)(void *,uint32_t resource,uint64_t *lease);
    int (*release)(void *,uint32_t resource,uint64_t lease);
    uint32_t (*read_rsc)(void *,uint32_t offset);
    void (*write_rsc)(void *,uint32_t offset,uint32_t value);
    void (*kick_aop)(void *);
} NaProxyOps;
typedef struct {
    NaProxyOps ops;QcomQmp qmp;QcomRscLayout layout;
    QcomRpmhResource resource[3];uint32_t values[3];
    uint64_t lease,deadline;
    uint32_t tcs,stage,want,held,possible,busy,active_resource,load_state_on;
    uint32_t last_irq,last_status,last_command_status,last_irq_enable;
    int error;
} NaProxyPower;
/* Mappings: stock SM8850 QMP0x0c300000, RSC0x18920000/DRV2/TCS+0xd00,
 * cmd-db0x81c60000. tcs is explicitly assigned by selected resource plan.
 * GPU015 observed RSC4.3 uses the existing major3 layout. Validate geometry;
 * LP0 only, never votes shared MC0/SH0/ACV. */
int na_proxy_layout(uint32_t version,uint32_t configuration,QcomRscLayout *);
int na_proxy_init(NaProxyPower *,volatile uint32_t *qmp,uint32_t qmp_bytes,
                   const uint8_t *cmd_db,uint32_t db_bytes,uint32_t tcs,const NaProxyOps *);
/* on=1 boots; on=0 is real DSP handover, leaves load_state on. */
int na_proxy_request(NaProxyPower *,int on);
int na_proxy_poll(NaProxyPower *,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
