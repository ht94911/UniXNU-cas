/* SPDX-License-Identifier: MIT */
/* Pinned XNU osfmk/kern/sched.h: normal preempting kernel priority. */
#define NRQS_MAX 128
#define MAXPRI (NRQS_MAX-1)
#define MAXPRI_KERNEL 95
#define BASEPRI_PREEMPT (MAXPRI_KERNEL-3)
