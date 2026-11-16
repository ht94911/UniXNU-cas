/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SM8850_BT_H
#define SM8850_BT_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The caller holds the common GCC/SPMI/SMMU owner lock for each complete
 * operation. Register callbacks complete their ordering barrier. */
struct sm8850_bt_io {
    void *context;
    uint32_t (*read)(void *, unsigned window, uint32_t offset);
    void (*write)(void *, unsigned window, uint32_t offset, uint32_t value);
    void (*delay_us)(void *, unsigned);
};
enum { SM8850_BT_GCC, SM8850_BT_SMMU, SM8850_BT_PMIC_CORE,
       SM8850_BT_PMIC_MAP, SM8850_BT_PMIC_OWNER, SM8850_BT_PMIC_OBSERVER,
       SM8850_BT_PMIC_WRITE };
#define SM8850_BT_GCC_BASE UINT64_C(0x00100000)
#define SM8850_BT_GCC_BYTES 0xb0000u /* includes UART18 RCG 0xa8640 */
#define SM8850_BT_SMMU_BASE UINT64_C(0x15000000)
#define SM8850_BT_PMIC_CORE_BASE UINT64_C(0x0c400000)
#define SM8850_BT_PMIC_MAP_BASE UINT64_C(0x0c403000)
#define SM8850_BT_PMIC_OWNER_BASE UINT64_C(0x0c43b000) /* bus 1 */
#define SM8850_BT_PMIC_OBSERVER_BASE UINT64_C(0x0c4c0000)
#define SM8850_BT_PMIC_WRITE_BASE UINT64_C(0x0c900000)
struct sm8850_bt_clock {
    uint32_t added_votes, rcg[5], dfsr, selector;
    bool acquired, source_saved, failed;
};
int sm8850_bt_clock_get(const struct sm8850_bt_io *, struct sm8850_bt_clock *);
int sm8850_bt_clock_source(const struct sm8850_bt_io *, struct sm8850_bt_clock *,
                          uint32_t baud, uint32_t *source_hz, uint32_t *selector);
int sm8850_bt_clock_put(const struct sm8850_bt_io *, struct sm8850_bt_clock *);
struct sm8850_bt_pmic {
    uint16_t apid, base_apid;
    uint8_t subtype;
    bool ready, failed;
};
int sm8850_bt_pmic_prepare(const struct sm8850_bt_io *, struct sm8850_bt_pmic *);
/* PMH0104 bus 1 SID 9 GPIO5, peripheral 0x8c00. Never accesses GPIO6,
 * NVM, SDAM, PON or a reboot reason. The WCN owner excludes an active UWB. */
int sm8850_bt_pmic_enable(const struct sm8850_bt_io *, struct sm8850_bt_pmic *, bool);
struct sm8850_bt_dma_route {
    uint32_t slot, route, old_smr, old_route, bank_offset, bank_sctlr;
    bool acquired, installed, failed;
};
/* Reuse the verified UFS firmware bypass context. Only an unused exact SMR
 * for QUP3 SID 0x4c3 may be added; no context/global/USB route is changed.
 * Existing QUP3 matches are borrowed without alteration. */
int sm8850_bt_dma_get(const struct sm8850_bt_io *, struct sm8850_bt_dma_route *,
                     uint64_t physical, uint32_t bytes);
/* Caller has independently proved UART + DMA idle and drained IRQ callbacks. */
int sm8850_bt_dma_put(const struct sm8850_bt_io *, struct sm8850_bt_dma_route *);
#ifdef __cplusplus
}
#endif
#endif
