// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapted from reAAAq/UniXNU-umi@3eb72578597d2ca16471b172d45eddc096a34a04.
// CPU0-only observations. No register writes except temporary IRQ/FIQ masking;
// the driver retains its original GIC setup, timer programming and dispatch.
extern "C" {
volatile uint64_t cas_armcpu_observation[32] = {1};
volatile uint64_t cas_armcpu_timer_completed = 0;
volatile uint64_t cas_armcpu_timer_returned = 0;
volatile uint64_t cas_armcpu_timer_first_counter = 0;
volatile uint64_t cas_armcpu_timer_last_counter = 0;
void cas_irq_publish(volatile uint64_t const *);
// Called only after the eighth real timer IRQ has returned from rtclock_intr
// and executed ICC_EOIR1_EL1. The debugger observes; it never invokes this.
__attribute__((noinline)) void cas_armcpu_timer_checkpoint(void)
{
  __asm__ volatile ("" ::: "memory");
}
}

static void casArmCpuObserve(uint64_t phase, uint64_t detail)
{
  uint64_t daif, frequency, count, deadline, control;
  __asm__ volatile ("mrs %0, daif\n\tmsr daifset, #3" : "=r"(daif) :: "memory");
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(frequency));
  __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r"(count));
  __asm__ volatile ("mrs %0, cntv_cval_el0" : "=r"(deadline));
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r"(control));
  cas_armcpu_observation[1] = phase;
  cas_armcpu_observation[2] = armcpu_registered;
  cas_armcpu_observation[3] = armcpu_gic_ready;
  cas_armcpu_observation[4] = armcpu_timer_interrupts;
  cas_armcpu_observation[5] = cas_armcpu_timer_completed;
  cas_armcpu_observation[6] = armcpu_irq_id;
  cas_armcpu_observation[7] = armcpu_irq_phase;
  cas_armcpu_observation[8] = frequency;
  cas_armcpu_observation[9] = count;
  cas_armcpu_observation[10] = deadline;
  cas_armcpu_observation[11] = control;
  cas_armcpu_observation[12] = daif;
  cas_armcpu_observation[13] = detail;
  if (phase == 4) { cas_armcpu_observation[14] = detail; }
  cas_armcpu_observation[15] = armcpu_boot_mpidr;
  for (unsigned i = 0; i < 8; ++i) {
    cas_armcpu_observation[16 + i] = armcpu_gic_boot[i];
  }
  cas_armcpu_observation[24] = armcpu_quiesce_calls;
  cas_armcpu_observation[25] = armcpu_halt_calls;
  cas_armcpu_observation[26] = cas_armcpu_timer_returned;
  cas_armcpu_observation[27] = cas_armcpu_timer_first_counter;
  cas_armcpu_observation[28] = cas_armcpu_timer_last_counter;
  cas_armcpu_observation[29] = cas_armcpu_observation[29] | (1ULL << phase);
  cas_irq_publish(cas_armcpu_observation);
  __asm__ volatile ("msr daif, %0" :: "r"(daif) : "memory");
}

static bool casArmCpuSampleTimer(void)
{
  uint64_t count = armcpu_timer_interrupts;
  return count == 1 || count == 2 || count == 8 || (count & 63) == 0;
}
