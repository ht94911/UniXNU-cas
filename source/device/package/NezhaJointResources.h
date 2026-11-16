/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_JOINT_RESOURCES_H
#define NEZHA_JOINT_RESOURCES_H

/* ALS017's accepted board assignment is reserved even before its worker
 * programs it. Radio allocators must not turn an idle reserved entry into
 * their own route. Existing active routes still undergo the original checks. */
enum {
    NezhaSensorSmmuBank = 7,
    NezhaSensorSmmuAsid = 0xfe00,
    NezhaSensorFirstSlot = 9,
    NezhaSensorLastSlot = 12,
    /* Same logical lease as ALS's NaResourceAdspTcs. It serializes complete
     * asynchronous ALS votes with radio votes without holding a mutex. */
    NezhaJointRscTransaction = 6,
    NezhaWcnResourceOwner = 0x5743
};
static inline int nezha_sensor_smmu_slot(unsigned slot)
{
    return slot >= NezhaSensorFirstSlot && slot <= NezhaSensorLastSlot;
}

#ifdef __cplusplus
extern "C" {
#endif
/* Published only after the real WCN ARC transaction succeeds. Wi-Fi retains
 * this vote through radio toggles; ALS must not erase it on proxy handover. */
unsigned nezha_wcn_lmx_level(void);
#ifdef __cplusplus
}
#endif

#endif
