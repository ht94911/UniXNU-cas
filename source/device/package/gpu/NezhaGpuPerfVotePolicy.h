/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>
#include <stdbool.h>
/* A failed performance vote has unknown hardware outcome. Latch off normal
 * control until reboot, then try at most three explicit160MHz/BW1 down-votes.
 * A lease deadline alone is not proof that an unresponsive GMU downclocked. */
struct nezha_gpu_vote_recovery {
    uint64_t next_attempt_us;
    uint32_t attempts;
    bool latched;
    bool safe_acknowledged;
};
static inline void nezha_gpu_vote_failed(struct nezha_gpu_vote_recovery *state,uint64_t now)
{
    if(!state->latched) {
        state->latched=true;state->safe_acknowledged=false;
        state->attempts=0;state->next_attempt_us=now;
    }
}
static inline bool nezha_gpu_safe_vote_due(const struct nezha_gpu_vote_recovery *state,
    uint64_t now,bool shutting_down)
{
    return state->latched && !state->safe_acknowledged && state->attempts<3 &&
        now>=state->next_attempt_us && !shutting_down;
}
static inline void nezha_gpu_safe_vote_result(struct nezha_gpu_vote_recovery *state,
    uint64_t now,bool acknowledged)
{
    ++state->attempts;
    state->safe_acknowledged=acknowledged;
    state->next_attempt_us=UINT64_MAX-now<500000?UINT64_MAX:now+500000;
}
