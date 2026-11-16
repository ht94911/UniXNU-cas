/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NB_POWER_H
#define NB_POWER_H
#include "NbCommon.h"
#include <Sm8850Resources.h>
#include "../upstream/QcomRpmh.h"
#include "../upstream/QcomQmp.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Same common ledger IDs as the existing audio owner. TCS1 and QMP are
 * leased per acknowledged transaction. Bank8 has a separate ledger slot. */
enum {NB_OWNER=0x4e42,NB_RES_QMP=5,NB_RES_TCS=6,NB_RES_SMMU=8,NB_POWER_RESOURCES=11};
enum {NB_POWER_IPA,NB_POWER_MSS};
typedef struct {
    Sm8850Resources *common;
    QcomRscLayout layout;
    QcomRpmhResource resource[NB_POWER_RESOURCES];
    uint32_t vote[2][NB_POWER_RESOURCES],sent[NB_POWER_RESOURCES],desired[NB_POWER_RESOURCES];
    unsigned want[2],active,cursor,busy,irq_previous,qmp_busy,qmp_sent,load_state;
    uint64_t lease,qmp_lease,deadline,qmp_deadline,request_deadline;
    QcomQmp qmp;
    char message[QCOM_QMP_MESSAGE];
    int error;
} NbPower;
int nb_power_init(NbPower *,Sm8850Resources *,volatile uint32_t *,const uint8_t *,size_t);
int nb_power_set(NbPower *,unsigned kind,int on,uint64_t now_ms);
int nb_power_message(NbPower *,const char *,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
