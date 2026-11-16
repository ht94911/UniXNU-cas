/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef NB_SCM_H
#define NB_SCM_H
#include "NbCommon.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Uses the existing NezhaScm global lock/WAITQ implementation. */
int nezha_bb_scm_pas(unsigned command,unsigned image,uint64_t a,uint64_t b);
/* Read-only PIL support (7) and permitted-memory-area (9) queries.
 * Query data in a1 is not a setter status and cannot prove a stopped remote. */
int nezha_bb_scm_query(unsigned command,unsigned image,uint64_t result[4]);
int nezha_bb_scm_assign(void *page,uint64_t page_pa,int *bridge_state,uint64_t *bridge_handle,
                        NbRange region,int to_remote,int shared);
/* Pure construction of the bounded descriptor page; separate host coverage. */
int nb_scm_assignment_page(void *page,uint64_t page_pa,NbRange region,int to_remote,int shared);
#ifdef __cplusplus
}
#endif
#endif
