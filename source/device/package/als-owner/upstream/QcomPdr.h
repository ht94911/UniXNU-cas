/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* QMI servreg notifier wire contract from Linux qcom_pdr_msg.c.
 * This client shares the owner's QRTR; it never restarts a protection domain. */
#ifndef QCOM_PDR_H
#define QCOM_PDR_H
#include "QcomQrtr.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {QcomPdrOkay=0,QcomPdrAgain=1,QcomPdrArgument=-1,QcomPdrWire=-2,
      QcomPdrRemote=-3,QcomPdrTimeout=-4,QcomPdrLost=-5,QcomPdrFull=-6};
enum {QCOM_PDR_SERVICE=0x42,QCOM_PDR_REGISTER=0x20,QCOM_PDR_UPDATED=0x22,QCOM_PDR_ACK=0x23};
#define QCOM_PDR_DOWN UINT32_C(0x0fffffff)
#define QCOM_PDR_UP UINT32_C(0x1fffffff)
#define QCOM_PDR_EARLY_DOWN UINT32_C(0x2fffffff)
#define QCOM_PDR_UNINIT UINT32_C(0x7fffffff)
#define QCOM_PDR_ACK_SLOTS 8u
typedef struct {
    void *context;
    /* Again is definitely unsubmitted. The QRTR send callback must retain
     * the packet after success, just as for the existing SNS client. */
    int (*room)(void *,uint32_t packet_bytes);
} QcomPdrOps;
typedef struct {
    QcomQrtr *router;QcomPdrOps ops;
    char path[65];uint32_t instance,local_port,node,port,state;
    uint32_t selected,registered,revision,indications,stale,acks;
    uint32_t head,tail,register_indications,pending_kind;
    uint16_t transaction,pending_transaction,ack[QCOM_PDR_ACK_SLOTS];
    uint64_t deadline_us;
    int error;
} QcomPdr;
/* instance is the domain instance from the board's actual locator table;
 * node/port are selected only from this router's current announcements. */
int qcom_pdr_prepare(QcomPdr *,QcomQrtr *,uint32_t local_port,
                     const char *path,uint32_t instance,const QcomPdrOps *);
int qcom_pdr_poll(QcomPdr *,uint64_t now_us);
int qcom_pdr_is_up(const QcomPdr *);
void qcom_pdr_lost(QcomPdr *,int error);
#ifdef __cplusplus
}
#endif
#endif
