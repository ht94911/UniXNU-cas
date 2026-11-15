/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM8850_RESOURCES_H
#define SM8850_RESOURCES_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {Sm8850Okay=0,Sm8850Busy=-1,Sm8850Invalid=-2,Sm8850Conflict=-3,Sm8850Io=-4};
enum {Sm8850Smmu=0,Sm8850Rsc=1,Sm8850Gcc=2,Sm8850Mutex=3,Sm8850Ipcc=4};
enum {Sm8850ResourceCount=16,Sm8850IpccRoutes=16};
typedef struct {
    void *context;
    /* One short configuration lock shared by every registered consumer.
     * Never sleep, invoke a client callback, or wait for a DSP under it. */
    int (*lock)(void *);void (*unlock)(void *);
    int (*read32)(void *,unsigned,uint32_t,uint32_t *);
    int (*write32)(void *,unsigned,uint32_t,uint32_t);
    int (*read64)(void *,unsigned,uint32_t,uint64_t *);
    int (*barrier)(void *);
} Sm8850ResourceIo;
typedef struct {uint32_t id;void *context;void (*event)(void *,uint32_t);} Sm8850IpccRoute;
typedef struct {
    Sm8850ResourceIo io;
    uint64_t sequence,lease[Sm8850ResourceCount];
    uint32_t owner[Sm8850ResourceCount],quarantined[Sm8850ResourceCount];
    uint32_t initialized,smem_owner,ipcc_busy,ipcc_sealed,ipcc_count,ipcc_unknown_count,ipcc_last_unknown;
    Sm8850IpccRoute ipcc[Sm8850IpccRoutes];
} Sm8850Resources;
typedef struct {uint32_t bank,asid,slot[4],sid[4],count; /* count 0 preserves legacy four-stream callers */} Sm8850SmmuAssignment;
/* No probing or writes here. Board supplies explicit resource numbers and
 * assignments. Registering this object is not permission for a second owner
 * to access an already-owned controller outside the same lock. */
int sm8850_resources_init(Sm8850Resources *,const Sm8850ResourceIo *);
int sm8850_resource_acquire(Sm8850Resources *,unsigned resource,uint32_t owner,uint64_t *lease);
int sm8850_resource_release(Sm8850Resources *,unsigned resource,uint32_t owner,uint64_t lease,int quiescent);
int sm8850_resource_quarantine(Sm8850Resources *,unsigned resource,uint32_t owner,uint64_t lease);
/* Holds the common configuration lock on success; caller must leave it.
 * It protects an operation, whereas a lease survives waits between operations. */
int sm8850_resource_enter(Sm8850Resources *,unsigned resource,uint32_t owner,uint64_t lease);
void sm8850_resource_leave(Sm8850Resources *);
/* Called with the configuration lock held. Rejects enabled/routed/nested
 * banks, masked SID overlap and ASID collisions. Never chooses a free bank.
 * Includes disabled banks' ASIDs to coexist with QcomUsbDmaDomain checks. */
int sm8850_smmu_check_assignment(Sm8850Resources *,const Sm8850SmmuAssignment *);
/* Caller already entered the resource. Exact TCS writes only. */
int sm8850_tcs_idle(Sm8850Resources *,unsigned tcs);
int sm8850_tcs_write(Sm8850Resources *,unsigned tcs,uint32_t offset,uint32_t value);
/* Change only this TCS's IRQ-enable bit under the common configuration lock;
 * return its previous value so a completed lease can restore it. */
int sm8850_tcs_irq_update(Sm8850Resources *,unsigned tcs,unsigned enabled,unsigned *previous);
/* Read/modify/write only the supplied owned bits, under the common lock. */
int sm8850_gcc_update(Sm8850Resources *,uint32_t offset,uint32_t allowed,uint32_t mask,uint32_t value);
/* TCSR mutex 3: local gate prevents two APPS threads both observing owner1.
 * One bounded hardware try; no unlock if a remote owns the mutex. */
int sm8850_smem_trylock(Sm8850Resources *,uint32_t owner);
int sm8850_smem_unlock(Sm8850Resources *,uint32_t owner);
/* Routes are immutable for the owner's lifetime; dispatcher is the sole
 * RX_ID/CLEAR consumer. Unknown IDs are counted before acknowledgement.
 * Callbacks must only publish/wake and are invoked outside the config lock. */
int sm8850_ipcc_register(Sm8850Resources *,uint32_t id,void (*event)(void *,uint32_t),void *);
int sm8850_ipcc_send(Sm8850Resources *,uint32_t id);
int sm8850_ipcc_dispatch(Sm8850Resources *,unsigned budget);
#ifdef __cplusplus
}
#endif
#endif
