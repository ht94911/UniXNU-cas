// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * nezha N5: CPU frequency (SCMI perf to the CPUCP firmware) and thermal control.
 *
 * Port of data-a-live 91f1f246 wsl/pandora_cpu.c (patches 0200-0205, 0213: proven on the Xiaomi 17 Pro, same
 * SM8850). Only the logic and register values are taken; written for this tree (bsd_init.c compile record,
 * linked with -force_load, started from nezha_input_start).
 *
 * Frequency: Android's scmi-cpufreq talks SCMI performance protocol 0x13 to CPUCP over the "pdp" channel
 * (DT qcom,pdp-scmi; mailbox qcom,pdp0 qcom,cpucp-v2, doorbell 0x19980000 + 0x100 + ch * 8 + 4; shared memory
 * pdp-sram 0x81f08000, request area at +0x100, 0x80 bytes, arm,scmi-shmem layout). nezha's DT has the same
 * nodes (evidence/n0-20261026/live-props.txt). Cores 0-5 are domain 0, cores 6-7 domain 1. pandora found that
 * domain 1 only follows its SCMI fast channel (describe-fast-channel, message 0xb); the start-up check below tries
 * each way and keeps the one the firmware follows (pandora 0202/0203).
 *
 * Thermal (pandora 0213, thresholds from the phone's DT thermal zones): tsens-v2, sensor S at TM + 0xa0 + 4 S
 * (bits 0-11 signed tenths of a degree, bit 21 valid); tsens1 0xc22a000 cores 0-5 (2N, 2N+1), tsens3 0xc22c000
 * cores 6-7 (0-1, 2-3), tsens5 0xc22e000 GPU 0-10. Every 100 ms: a core at 105 C lowers its domain's level cap one
 * level (back up below 100 C); 114 C takes the core from the scheduler (Apple's performance-controller interface,
 * nezha_cpu_recommend; back below 108 C; CPU 0 stays); 118 C drops the domain to level 0; the GPU at 95 C lowers
 * pandora_gpu_thermal_top (package/gpu/NezhaGpu.c) one level. CPUCP limits clocks by itself above that.
 *
 * Boot-args (each feature on its own):
 *   nezha-cpufreq=0  off (default: nothing touched, the bootloader's clocks stay)
 *                 1  probe only: SCMI version, domains, levels, current level, measured clocks (no change)
 *                 2  fixed: each domain at the firmware's sustained level (the "normal" clock), thermal caps apply
 *                 3  governor (pandora 0201-0204: schedutil-like, 16 ms windows, floors 1.67 / 1.75 GHz)
 *   nezha-cpu-fix0=N / nezha-cpu-fix1=N  a fixed level for domain 0 / 1 (modes 2, 3)
 *   nezha-thermal=0  no thermal control (default 1 when nezha-cpufreq >= 2; 1 with nezha-cpufreq=0/1: temperatures
 *                    are still read and shown, GPU cap applied, nothing set on the CPU)
 *   nezha-thermal-test=N  every threshold N degrees lower (to exercise the paths at an ordinary load)
 *   nezha-idle=1     PSCI core retention in cpu_idle (patches/xnu/nezha-cpu-idle.patch, myron-next zzz-myron-cpu-idle),
 *                    armed from here 3 s after start; default WFI only
 * Status: boot-progress row 39 and the log ring ("CPU" row) show native_cpufreq_row.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/processor.h>
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>
#include <kern/cpu_number.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef NEZHA_CPU_IOS27
#include <mach/processor_info.h>
#include "NezhaCpu27Policy.h"
extern kern_return_t processor_info(processor_t, processor_flavor_t, host_t *,
    processor_info_t, mach_msg_type_number_t *);
extern kern_return_t thread_terminate(thread_t);
extern int system_inshutdown;
static uint32_t nz27_cpu_valid;
#endif

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void IOSleep(unsigned int milliseconds);
extern processor_t thread_bind(processor_t processor);
extern processor_t cpu_to_processor(int cpu);
extern kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
extern void thread_deallocate(thread_t);
/* patches/xnu/nezha-cpu-idle.patch (osfmk/arm64/cpu.c) */
#ifndef NEZHA_CPU_IOS27
extern uint64_t nezha_cpu_idle_mach(int cpu);
extern void nezha_cpu_recommend(uint64_t cores);
extern void nezha_idle_start(void);
extern void nezha_idle_log(void);
#endif
/* package/gpu/NezhaGpu.c (pandora 0213 GPU cap); weak so this file does not need the GPU archive */
extern uint32_t pandora_gpu_thermal_top __attribute__((weak_import));
extern const uint32_t pandora_gpu_level_count __attribute__((weak_import));
/* n5-metal-017 (UniXNU-sheng 36ec562 CpuFreqProbe.cpp): touch reports (input driver) and GPU submissions (NezhaGpu.c) */
extern volatile uint32_t native_hid_input_events[8] __attribute__((weak_import));
extern volatile uint32_t nezha_gpu_submit_count __attribute__((weak_import));
#ifdef NEZHA_CPU_IOS27
extern uint64_t native_adreno_perf_jobs[14];
static bool cpu_gpu_activity_available(void) {return true;}
static uint32_t cpu_gpu_activity(void) {return (uint32_t)__atomic_load_n(&native_adreno_perf_jobs[0],__ATOMIC_RELAXED);}
#else
static bool cpu_gpu_activity_available(void) {return &nezha_gpu_submit_count!=NULL;}
static uint32_t cpu_gpu_activity(void) {return &nezha_gpu_submit_count?nezha_gpu_submit_count:0;}
#endif
/* [0] touch boosts started, [1] frame floors started, [2] 16 ms windows in a frame floor */
volatile uint32_t nezha_cpufreq_boost[3];
/* n5-metal-018: GPU clock now and the level lookup for the thermal stages (package/gpu/NezhaGpu.c) */
extern volatile uint32_t nezha_gpu_dcvs_mhz __attribute__((weak_import));
extern uint32_t pandora_gpu_level_at_most(uint32_t mhz) __attribute__((weak_import));
/* boot-progress row 91 / log line "NEZHA TH": hottest sensor, thermal stage, caps, time at the top level, mean clocks, per 5 s */
char native_thermal_row[120];

/* boot-progress (package/boot-progress/BootProgress.c weak imports) */
char native_cpufreq_row[120];
volatile uint32_t native_cpufreq_stage;

#define PDP_MBOX_TX_PA          0x19980000ull   /* qcom,pdp0 "tx" registers */
#define PDP_MBOX_TX_CMD(ch)     (0x100u + (ch) * 8u + 4u)
#define PDP1_MBOX_TX_PA         0x1a980000ull   /* qcom,pdp1 "tx" registers */
#define PDP_SHMEM_PA            0x81f08000ull   /* pdp-sram: rx area at +0x000, tx area at +0x100 */
#define PDP_SHMEM_TX            0x100u
#define PDP_SHMEM_SIZE          0x80u
#define SCMI_PROTO_PERF         0x13u
#define SCMI_FREE               0x1u
#define SCMI_ERROR              0x2u
#define CPU_IOS_RAM_PA          0x8c0000000ull  /* iOS's own memory starts here: never map a firmware address above */
#define NZ_LEVELS_MAX           40u

static vm_offset_t cpu_mbox, cpu_shmem, cpu_mbox1;
static uint32_t cpu_token;
static int nz_mode;
static bool nz_thermal_on;

static uint32_t
rd32(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static void
wr32(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

/* One SCMI command on the pdp channel, by polling the request area (channel_status +4, flags +0x10, length +0x14,
 * header +0x18, payload +0x1c). Returns the response's 32-bit words (status first) in OUT. */
static int
scmi_call(uint32_t proto, uint32_t msg, const uint32_t *in, uint32_t nin, uint32_t *out, uint32_t maxout,
    uint32_t *nout)
{
	const vm_offset_t sh = cpu_shmem + PDP_SHMEM_TX;
	uint64_t deadline;
    if(!nout || (nin && !in) || (maxout && !out) || nin>25)return -4;
    *nout = 0;
#ifdef NEZHA_CPU_IOS27
    if(__atomic_load_n(&system_inshutdown,__ATOMIC_RELAXED))return -6;
#endif
	if (!cpu_mbox || !cpu_shmem) {
		return -6;
	}
	clock_interval_to_deadline(30, NSEC_PER_MSEC, &deadline);
	while (!(rd32(sh, 0x4) & SCMI_FREE)) {
#ifdef NEZHA_CPU_IOS27
        if(__atomic_load_n(&system_inshutdown,__ATOMIC_RELAXED))return -6;
#endif
		if (mach_absolute_time() > deadline) {
			return -1;
		}
	}
#ifdef NEZHA_CPU_IOS27
    if(__atomic_load_n(&system_inshutdown,__ATOMIC_RELAXED))return -6;
#endif
	const uint32_t token = (++cpu_token) & 0x3ffu;
	wr32(sh, 0x4, 0);
	wr32(sh, 0x10, 0);
	wr32(sh, 0x14, 4 + nin * 4);
	wr32(sh, 0x18, (msg & 0xff) | (proto & 0xff) << 10 | token << 18);
	for (uint32_t i = 0; i < nin; i++) {
		wr32(sh, 0x1c + i * 4, in[i]);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	wr32(cpu_mbox, PDP_MBOX_TX_CMD(0), token | 1u);
	clock_interval_to_deadline(50, NSEC_PER_MSEC, &deadline);
	for (;;) {
		const uint32_t st = rd32(sh, 0x4);
		if ((st & (SCMI_FREE | SCMI_ERROR)) && ((rd32(sh, 0x18) >> 18) & 0x3ffu) == token) {
			if (st & SCMI_ERROR) {
				return -3;
			}
			break;
		}
		if (mach_absolute_time() > deadline) {
			return -2;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	uint32_t len = rd32(sh, 0x14);
#ifdef NEZHA_CPU_IOS27
    const uint32_t expected=(msg&0xffu)|((proto&0xffu)<<10)|(token<<18);
    if(!nz27_scmi_envelope(expected,rd32(sh,0x18),len))return -4;
#endif
	uint32_t words = len >= 4 ? (len - 4) / 4 : 0;
	if (words > (PDP_SHMEM_SIZE - 0x1c) / 4) {
		words = (PDP_SHMEM_SIZE - 0x1c) / 4;
	}
	for (uint32_t i = 0; i < words && i < maxout; i++) {
		out[i] = rd32(sh, 0x1c + i * 4);
	}
	*nout = words < maxout ? words : maxout;
	return 0;
}

/* pandora's Android tables (17 Pro); replaced by the firmware's own when it describes kHz (SCMI perf v4). */
static const uint32_t pandora_khz0[] = { 384000, 537600, 691200, 787200, 883200, 998400, 1113600, 1228800, 1324800,
	1440000, 1555200, 1670400, 1785600, 1900800, 1996800, 2112000, 2227200, 2361600, 2496000, 2611200, 2745600,
	2899200, 3033600, 3187200, 3302400, 3398400, 3513600, 3628800 };
static const uint32_t pandora_khz1[] = { 768000, 883200, 1017600, 1132800, 1267200, 1382400, 1497600, 1632000,
	1747200, 1862400, 1977600, 2112000, 2227200, 2438400, 2668800, 2880000, 3129600, 3398400, 3648000, 3763200,
	3859200, 3974400, 4070400, 4185600, 4262400, 4339200, 4396800 };

struct cpu_domain {
	uint32_t khz[NZ_LEVELS_MAX];
	uint32_t levels, first, count;   /* table, CPUs first .. first + count - 1 */
	uint32_t level, top, fixed;      /* current, cap (sustained), fixed level or ~0 */
	uint32_t down_windows, want_max_down, changes, set_errors;
	uint64_t busy_sum, khz_sum, windows;
	int method;                      /* 0 SCMI, 1 SCMI + pdp1 doorbell, 2 fast channel, 3 limits; -1 none */
	uint32_t floor_khz, floor, quiet;
	vm_offset_t fast, fast_db;
	uint64_t fast_db_set, fast_db_keep;
	uint32_t mhz_now;                /* last measured clock of the first CPU */
	bool from_fw;                    /* table from the firmware */
};
static struct cpu_domain cpu_dom[2];
static uint64_t cpu_busy_sum[8];
static uint32_t nz_busy_pct[2];

static void
cpu_domains_default(void)
{
	struct cpu_domain *d0 = &cpu_dom[0], *d1 = &cpu_dom[1];
	d0->levels = sizeof(pandora_khz0) / 4;
	memcpy(d0->khz, pandora_khz0, sizeof(pandora_khz0));
	d0->first = 0; d0->count = 6; d0->top = d0->levels - 1; d0->fixed = ~0u; d0->floor_khz = 1670400;
	d1->levels = sizeof(pandora_khz1) / 4;
	memcpy(d1->khz, pandora_khz1, sizeof(pandora_khz1));
	d1->first = 6; d1->count = 2; d1->top = d1->levels - 1; d1->fixed = ~0u; d1->floor_khz = 1747200;
	d1->method = -1;
}

static void
scmi_name(const uint32_t *w, char name[17])
{
	memcpy(name, w, 16);
	name[16] = 0;
	for (int i = 0; i < 16 && name[i]; i++) {
		if (name[i] < 0x20 || name[i] > 0x7e) {
			name[i] = '?';
		}
	}
}

/* Version, domains, levels (into cpu_dom when the firmware gives kHz), limits, current level. */
static bool
cpu_scmi_probe(void)
{
	uint32_t out[32], n = 0, in[2];
	cpu_mbox = ml_io_map(PDP_MBOX_TX_PA, 0x1000);
	cpu_shmem = ml_io_map(PDP_SHMEM_PA, 0x1000);
	if (!cpu_mbox || !cpu_shmem) {
		printf("NEZHA CPU: SCMI: no mapping\n");
		return false;
	}
	printf("NEZHA CPU: SCMI request area: status 0x%x flags 0x%x length %u header 0x%x\n",
	    rd32(cpu_shmem + PDP_SHMEM_TX, 0x4), rd32(cpu_shmem + PDP_SHMEM_TX, 0x10),
	    rd32(cpu_shmem + PDP_SHMEM_TX, 0x14), rd32(cpu_shmem + PDP_SHMEM_TX, 0x18));
	int r = scmi_call(SCMI_PROTO_PERF, 0x0, NULL, 0, out, 32, &n);
	if (r || n < 2 || out[0] != 0) {
		printf("NEZHA CPU: SCMI perf version: error %d (%u words, status %d)\n", r, n, n ? (int)out[0] : 0);
		return false;
	}
	const uint32_t perf_version = out[1];
	printf("NEZHA CPU: SCMI perf version 0x%x (status %d)\n", perf_version, (int)out[0]);
	r = scmi_call(SCMI_PROTO_PERF, 0x1, NULL, 0, out, 32, &n);
	const uint32_t domains = (!r && n >= 2 && out[0] == 0) ? (out[1] & 0xffff) : 0;
	printf("NEZHA CPU: SCMI perf attributes: error %d attributes 0x%x -> %u domains\n", r, n >= 2 ? out[1] : 0, domains);
	for (uint32_t d = 0; d < domains && d < 4; d++) {
		char name[17] = "";
		uint32_t khz[NZ_LEVELS_MAX], got_levels = 0;
		bool khz_ok = (perf_version >> 16) >= 4, levels_complete=false;
		in[0] = d;
		r = scmi_call(SCMI_PROTO_PERF, 0x3, in, 1, out, 32, &n);
		if (!r && n >= 9 && out[0] == 0) {
			scmi_name(&out[5], name);
		}
#ifdef NEZHA_CPU_IOS27
        /* PERF v4 bit25 selects index-valued LEVEL_SET/GET. The older
         * perf-unit mode must not be driven with our array indices. This
         * phone reported flags0xea000000 and rate0 in the retained17 logs. */
        khz_ok = khz_ok && !r && n>=9 && !out[0] &&
            (out[1]&(1u<<30)) && (out[1]&(1u<<25)) && (out[2]&0xfffffu)<=16000;
#endif
		printf("NEZHA CPU: domain %u '%s': error %d flags 0x%x rate limit %u us sustained %u kHz level %u\n",
		    d, name, r, n > 1 ? out[1] : 0, n > 2 ? out[2] : 0, n > 3 ? out[3] : 0, n > 4 ? out[4] : 0);
		const uint32_t entry = (perf_version >> 16) >= 4 ? 5 : 3;
		uint32_t index = 0;
		for (int guard = 0; guard < 64; guard++) {
			in[0] = d;
			in[1] = index;
			r = scmi_call(SCMI_PROTO_PERF, 0x4, in, 2, out, 32, &n);
			if (r || n < 2 || (int)out[0] != 0) {
				printf("NEZHA CPU: domain %u levels from %u: error %d status %d\n", d, index, r, n ? (int)out[0] : 0);
				break;
			}
			const uint32_t got = out[1] & 0xfff, left = out[1] >> 16;
            if(got>(n-2)/entry || index>NZ_LEVELS_MAX || got>NZ_LEVELS_MAX-index) {
                khz_ok=false;break;
            }
			for (uint32_t i = 0; i < got && 2 + (i + 1) * entry <= n; i++) {
				const uint32_t *e = &out[2 + i * entry];
				if (entry == 5) {
					printf("NEZHA CPU: domain %u level %u: perf %u power %u latency %u us freq %u kHz index %u\n", d,
					    index + i, e[0], e[1], e[2] & 0xffff, e[3], e[4]);
					if (index + i < NZ_LEVELS_MAX) {
						khz[index + i] = e[3];
						khz_ok = khz_ok && e[3] >= 100000 && e[3] <= 6000000 && e[4] == index + i && ((!index && !i) || e[3] > khz[index+i-1]);
					}
				} else {
					printf("NEZHA CPU: domain %u level %u: perf %u power %u latency %u us\n", d, index + i, e[0],
					    e[1], e[2] & 0xffff);
				}
			}
			index += got;
			got_levels = index;
			if (!left || !got) {
                levels_complete=!left;
				break;
			}
		}
		if (d < 2 && khz_ok && levels_complete && got_levels >= 2 && got_levels <= NZ_LEVELS_MAX) {
			memcpy(cpu_dom[d].khz, khz, got_levels * 4);
			cpu_dom[d].levels = got_levels;
			cpu_dom[d].top = got_levels - 1;
			cpu_dom[d].from_fw = true;
		}
		in[0] = d;
		r = scmi_call(SCMI_PROTO_PERF, 0x6, in, 1, out, 32, &n);
		printf("NEZHA CPU: domain %u limits: error %d max %u min %u\n", d, r, n > 1 ? out[1] : 0, n > 2 ? out[2] : 0);
		r = scmi_call(SCMI_PROTO_PERF, 0x8, in, 1, out, 32, &n);
		printf("NEZHA CPU: domain %u level now: error %d level %u\n", d, r, n > 1 ? out[1] : 0);
	}
	for (uint32_t d = 0; d < 2; d++) {
		printf("NEZHA CPU: domain %u table: %u levels %u..%u MHz (%s)\n", d, cpu_dom[d].levels, cpu_dom[d].khz[0] / 1000,
		    cpu_dom[d].khz[cpu_dom[d].levels - 1] / 1000, cpu_dom[d].from_fw ? "firmware" : "pandora's Android table");
	}
#ifdef NEZHA_CPU_IOS27
    return domains==2 && cpu_dom[0].from_fw && cpu_dom[1].from_fw;
#else
    return domains >= 2;
#endif
}

/* The clock of the CPU this runs on, in MHz: dependent adds (one cycle each) over 10 ms. pandora noted the
 * measure reads about 1.36 x the clock on Oryon (two adds retire per cycle at times); used for "did it move". */
static uint32_t
cpu_clock_mhz(void)
{
	uint64_t span, start, now, x = 1, rounds = 0;
	nanoseconds_to_absolutetime(10 * NSEC_PER_MSEC, &span);
	start = mach_absolute_time();
	do {
		for (int i = 0; i < 1000; i++) {
			__asm__ volatile (
                                "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                : "+r"(x));
		}
		rounds += 1000;
		now = mach_absolute_time();
	} while (now - start < span);
	uint64_t ns;
	absolutetime_to_nanoseconds(now - start, &ns);
	return ns ? (uint32_t)(rounds * 16 * 1000 / ns) : 0;
}

static uint32_t
cpu_clock_on(int cpu)
{
	processor_t p = cpu_to_processor(cpu);
	if (p == PROCESSOR_NULL) {
		return 0;
	}
	thread_bind(p);
	thread_block(THREAD_CONTINUE_NULL);
	const uint32_t mhz = cpu_clock_mhz();
	thread_bind(PROCESSOR_NULL);
	thread_block(THREAD_CONTINUE_NULL);
	return mhz;
}

static void
cpu_clocks(const char *when)
{
	cpu_dom[0].mhz_now = cpu_clock_on(0);
	cpu_dom[1].mhz_now = cpu_clock_on(6);
	printf("NEZHA CPU: %s: cpu 0 (domain 0) about %u MHz, cpu 6 (domain 1) about %u MHz (measure ~1.36 x clock)\n", when,
	    cpu_dom[0].mhz_now, cpu_dom[1].mhz_now);
}

static int
cpu_set_level_via(uint32_t d, uint32_t level, int method)
{
#ifdef NEZHA_CPU_IOS27
    if(__atomic_load_n(&system_inshutdown,__ATOMIC_RELAXED))return -6;
#endif
	struct cpu_domain *dm = &cpu_dom[d];
	uint32_t in[3] = { d, level, level }, out[4], n = 0;
	int r;
	switch (method) {
	case 2:
		if (!dm->fast) {
			return -5;
		}
		*(volatile uint32_t *)dm->fast = level;
		__asm__ volatile ("dsb sy" ::: "memory");
		if (dm->fast_db) {
			const uint32_t keep = *(volatile uint32_t *)dm->fast_db & (uint32_t)dm->fast_db_keep;
			*(volatile uint32_t *)dm->fast_db = keep | (uint32_t)dm->fast_db_set;
		}
		return 0;
	case 3:
#ifdef NEZHA_CPU_IOS27
        return -5; // Do not alter firmware limits in the27 governor.
#else
		r = scmi_call(SCMI_PROTO_PERF, 0x5, in, 3, out, 4, &n);   /* limits: max = min = level */
		return r ? r : (n ? (int)out[0] : -4);
#endif
	default:
#ifdef NEZHA_CPU_IOS27
        if(method!=0)return -5;
#endif
		r = scmi_call(SCMI_PROTO_PERF, 0x7, in, 2, out, 4, &n);
		if (!r && method == 1 && cpu_mbox1) {
			wr32(cpu_mbox1, PDP_MBOX_TX_CMD(0), 1u);
		}
		return r ? r : (n ? (int)out[0] : -4);
	}
}

static int
cpu_set_level(uint32_t d, uint32_t level)
{
	return cpu_set_level_via(d, level, cpu_dom[d].method < 0 ? 0 : cpu_dom[d].method);
}

static uint32_t
cpu_get(uint32_t d, uint32_t msg, uint32_t word)
{
	uint32_t in[1] = { d }, out[8], n = 0;
	return (!scmi_call(SCMI_PROTO_PERF, msg, in, 1, out, 8, &n) && n > word && (int)out[0] == 0) ? out[word] : ~0u;
}

static void
cpu_fast_channel(uint32_t d, uint32_t msg)
{
	struct cpu_domain *dm = &cpu_dom[d];
	uint32_t in[2] = { d, msg }, out[16], n = 0;
	const int r = scmi_call(SCMI_PROTO_PERF, 0xb, in, 2, out, 16, &n);
	printf("NEZHA CPU: domain %u fast channel for message %u: error %d, %u words:", d, msg, r, n);
	for (uint32_t i = 0; i < n && i < 12; i++) {
		printf(" %08x", out[i]);
	}
	printf("\n");
	if (r || n < 6 || out[0] != 0 || msg != 0x7) {
		return;
	}
	const uint64_t chan = (uint64_t)out[4] << 32 | out[3], db = n > 7 ? ((uint64_t)out[7] << 32 | out[6]) : 0;
#ifdef NEZHA_CPU_IOS27
    /* SCMI DOORBELL_REG_WIDTH is attributes[2:1], log2(bytes).
     * This existing implementation uses32-bit doorbells; reject other widths. */
    if(out[2]>16000)return;
    if((out[1]&1u) && (((out[1]>>1)&3u)!=2u || n<12 || !db || (db&3) ||
        db>=CPU_IOS_RAM_PA-4 || out[9] || out[11]))return;
#endif
	if (chan && !(chan&3) && chan < CPU_IOS_RAM_PA-4 && (chan&0xfff)<=4092 && out[5] == 4) {
		const vm_offset_t page = ml_io_map(chan & ~0xfffull, 0x1000);
		dm->fast = page ? page + (chan & 0xfff) : 0;
	}
	if (db && !(db&3) && db < CPU_IOS_RAM_PA-4 && (db&0xfff)<=4092 && (out[1] & 0x1) && n>=12 && !out[9] && !out[11]) {
		const vm_offset_t page = ml_io_map(db & ~0xfffull, 0x1000);
		dm->fast_db = page ? page + (db & 0xfff) : 0;
		dm->fast_db_set = n > 9 ? ((uint64_t)out[9] << 32 | out[8]) : 1;
		dm->fast_db_keep = n > 11 ? ((uint64_t)out[11] << 32 | out[10]) : 0;
	}
#ifdef NEZHA_CPU_IOS27
    if((out[1]&1u) && !dm->fast_db)dm->fast=0;
#endif
	printf("NEZHA CPU: domain %u fast channel at 0x%llx (%s), doorbell 0x%llx (%s)\n", d, chan, dm->fast ? "mapped" : "not used",
	    db, dm->fast_db ? "mapped" : "not used");
}

static uint32_t
lv(uint32_t d, uint32_t level)
{
	return level < cpu_dom[d].levels ? level : cpu_dom[d].levels - 1;
}

static bool
cpu_try(uint32_t d, uint32_t level, int method, const char *how)
{
	struct cpu_domain *dm = &cpu_dom[d];
	level = lv(d, level);
#ifdef NEZHA_CPU_IOS27
    if(level>dm->top)level=dm->top;
#endif
	const int r = cpu_set_level_via(d, level, method);
	IOSleep(20);
	const uint32_t fw = cpu_get(d, 0x8, 1);
	dm->mhz_now = cpu_clock_on((int)dm->first);
	printf("NEZHA CPU: check: domain %u level %u (%u MHz) by %s: result %d, firmware level now %d, cpu %u about %u MHz\n",
	    d, level, dm->khz[level] / 1000, how, r, (int)fw, dm->first, dm->mhz_now);
	return r == 0 && fw == level;
}

/* pandora 0202/0203: does a level set move the firmware? Domain 0 top and bottom by SCMI, then its fast channel;
 * domain 1 four ways, each towards its own level; the first the firmware follows is kept. */
static void
cpu_check_levels(void)
{
#ifdef NEZHA_CPU_IOS27
    /* Preserve firmware limits and other mailboxes. Check only the existing
     * SCMI and firmware-described fast channel within the selected ceiling. */
    for(uint32_t d=0;d<2;++d) {
        struct cpu_domain *dm=&cpu_dom[d];
        dm->method=cpu_try(d,dm->top,0,"SCMI capped")?0:-1;
        cpu_fast_channel(d,0x7);
        if(dm->fast && !(dm->fast_db_set>>32) && !(dm->fast_db_keep>>32) &&
            cpu_try(d,dm->top?dm->top-1:0,2,"fast channel capped"))dm->method=2;
        printf("NEZHA CPU27: domain %u verified method %d cap %u kHz\n",d,dm->method,dm->khz[dm->top]);
    }
#else
	cpu_mbox1 = ml_io_map(PDP1_MBOX_TX_PA, 0x1000);
	(void)cpu_try(0, cpu_dom[0].top, 0, "SCMI");
	cpu_dom[0].method = cpu_try(0, 0, 0, "SCMI") ? 0 : -1;
	cpu_fast_channel(0, 0x7);
	cpu_fast_channel(1, 0x7);
	if (cpu_dom[0].method == 0 && cpu_dom[0].fast && cpu_try(0, 5, 2, "fast channel")) {
		cpu_dom[0].method = 2;
	}
	struct cpu_domain *d1 = &cpu_dom[1];
	const uint32_t back = cpu_get(1, 0x8, 1);
	if (cpu_try(1, d1->top, 0, "SCMI")) {
		d1->method = 0;
	} else if (cpu_try(1, 20, 1, "SCMI + pdp1 doorbell")) {
		d1->method = 1;
	} else if (d1->fast && cpu_try(1, 14, 2, "fast channel")) {
		d1->method = 2;
	} else if (cpu_try(1, 10, 3, "limits")) {
		d1->method = 3;
	}
	if (d1->method != 3) {   /* the limits back to the whole range */
		uint32_t in[3] = { 1, d1->levels + 1, 0 }, out[4], n = 0;
		(void)scmi_call(SCMI_PROTO_PERF, 0x5, in, 3, out, 4, &n);
	}
	if (d1->method < 0 && back != ~0u) {
		(void)cpu_set_level_via(1, back, 0);
	}
	printf("NEZHA CPU: check: domain 0 method %d, domain 1 method %d (0 SCMI, 1 SCMI + pdp1 doorbell, 2 fast channel, "
	    "3 limits, -1 none)\n", cpu_dom[0].method, d1->method);
#endif
}

/* ---- temperatures and thermal control (pandora 0205, 0213) ---- */
static const struct {
	uint64_t tm;
	uint32_t first, count;
	const char *what;
} cpu_tsens[] = {
	{ 0xc22a000ull, 0, 12, "little" },
	{ 0xc22c000ull, 0, 4, "big" },
	{ 0xc22e000ull, 0, 11, "gpu" },
	{ 0xc22e000ull, 11, 1, "ddr" },
};
static vm_offset_t cpu_tsens_va[4];

#define T_HAL       1050    /* tenths of a degree */
#define T_HAL_HYST  50
#define T_PLUG      1140
#define T_PLUG_HYST 60
#define T_PAUSE     1180
#define T_GPU       950
static uint32_t cpu_tcap[2] = { ~0u, ~0u };
/* n5-metal-018 thermal stages (new for this phone; 017 went from 45 to 100 C in about 40 s with the pandora limits below, which act
 * only from 95/105 C). Tmax is the hottest of all 28 TSENS sensors (both CPU clusters, the GPU and the DDR). At or above nezha-th-soft
 * (tenths of a degree, default 750) the stage goes up one step at once and then every 500 ms while it stays there; at or above
 * nezha-th-hard (default 850) it goes straight to the last stage; under soft - 3 C it comes back one step a second. Every stage caps
 * both CPU domains and the GPU (MHz below; the last row is the hard cap). The thermal round runs every 96 ms, so a cap is in force
 * within about 0.1 s. The pandora 0213 limits (105/114/118 C per core, 95 C GPU) stay as the last line. */
#define TH_STAGES 6
static const uint32_t th_mhz[3][TH_STAGES] = {
	{ 0, 3033, 2496, 2112, 1785, 1555 },   /* domain 0, cpus 0-5 */
	{ 0, 3398, 2880, 2438, 2112, 1747 },   /* domain 1, cpus 6-7 */
	{ 0, 826, 726, 646, 578, 500 },        /* GPU */
};
static uint32_t th_stage, th_hot_rounds, th_cool_rounds, th_steps, th_soft = 750, th_hard = 850;
static int32_t th_max = INT32_MIN, th_win_max = INT32_MIN;
static uint32_t th_max_at, th_win_max_at, th_win_stage_max;
static uint32_t cpu_scap[2] = { ~0u, ~0u }, gpu_scap, gpu_tcap95;
static uint64_t cpu_t_out;
static int32_t cpu_t_test;
static uint32_t cpu_t_logged;
static int32_t nz_t_hot[3] = { INT32_MIN, INT32_MIN, INT32_MIN };

/* Per-sensor peak (tenths) since the last raw log, sampled every thermal round: groups little 12, big 4, gpu 11 + ddr 1. */
#define T_NSENS 28
static int16_t cpu_t_peak[T_NSENS];
static bool cpu_t_peak_set;

static int32_t
cpu_tsens_raw(uint32_t i)
{
	const uint32_t g = i < 12 ? 0 : i < 16 ? 1 : 2, s = i < 12 ? i : i < 16 ? i - 12 : i - 16;
	if (!cpu_tsens_va[g]) {
		return INT32_MIN;
	}
	const uint32_t v = rd32(cpu_tsens_va[g], 0xa0 + s * 4);
	return (v & (1u << 21)) ? (int32_t)(v << 20) >> 20 : INT32_MIN;
}

static void
cpu_t_sample(void)
{
	for (uint32_t i = 0; i < T_NSENS; i++) {
		const int32_t t = cpu_tsens_raw(i);
		if (t != INT32_MIN && (!cpu_t_peak_set || t > cpu_t_peak[i])) {
			cpu_t_peak[i] = (int16_t)t;
		}
	}
	cpu_t_peak_set = true;
}

/* "NEZHA TSENS:" now and peak (whole degrees, -1 invalid) of every CPU/GPU/DDR sensor, in DT order:
 * little = tsens1 cpu-0-{0..5}-{0,1}, big = tsens3 cpu-1-{0,1}-{0,1}, gpu = tsens5 gpuss-0..10, ddr = tsens5 11. */
static void
cpu_t_log(void)
{
	char b[320];
	int n = 0;
	static const char *const tag[] = { "L", "B", "G", "D" };
	n += snprintf(b + n, sizeof(b) - (size_t)n, "NEZHA TSENS: now/peak");
	for (uint32_t i = 0; i < T_NSENS && n < (int)sizeof(b) - 16; i++) {
		if (i == 0 || i == 12 || i == 16 || i == 27) {
			n += snprintf(b + n, sizeof(b) - (size_t)n, " %s", tag[i == 0 ? 0 : i == 12 ? 1 : i == 16 ? 2 : 3]);
		}
		const int32_t t = cpu_tsens_raw(i);
		n += snprintf(b + n, sizeof(b) - (size_t)n, " %d/%d", t == INT32_MIN ? -1 : t / 10, cpu_t_peak_set ? cpu_t_peak[i] / 10 : -1);
	}
	printf("%s\n", b);
	cpu_t_peak_set = false;
}

static int32_t
cpu_temp_of(uint32_t g, uint32_t first, uint32_t count)
{
	int32_t best = INT32_MIN;
	if (!cpu_tsens_va[g]) {
		return best;
	}
	for (uint32_t s = first; s < first + count; s++) {
		const uint32_t v = rd32(cpu_tsens_va[g], 0xa0 + s * 4);
		if (!(v & (1u << 21))) {
			continue;
		}
		const int32_t t = (int32_t)(v << 20) >> 20;
		best = t > best ? t : best;
	}
	return best;
}

static void
cpu_temps(bool all)
{
	for (uint32_t g = 0; g < sizeof(cpu_tsens) / sizeof(cpu_tsens[0]); g++) {
		if (!cpu_tsens_va[g]) {
			cpu_tsens_va[g] = ml_io_map(cpu_tsens[g].tm, 0x1000);
		}
		if (all && cpu_tsens_va[g]) {
			printf("NEZHA CPU: temperature %s (0x%llx):", cpu_tsens[g].what, cpu_tsens[g].tm);
			for (uint32_t s = cpu_tsens[g].first; s < cpu_tsens[g].first + cpu_tsens[g].count; s++) {
				printf(" %08x", rd32(cpu_tsens_va[g], 0xa0 + s * 4));
			}
			printf("\n");
		}
	}
	const int32_t l = cpu_temp_of(0, 0, 12), b = cpu_temp_of(1, 0, 4), g = cpu_temp_of(2, 0, 11);
	if (!all && l != INT32_MIN) {
		cpu_t_log();
	}
	if (all || l != INT32_MIN) {
		printf("NEZHA CPU: temperature: little %d.%d big %d.%d gpu %d.%d C\n", l / 10, (l < 0 ? -l : l) % 10,
		    b / 10, (b < 0 ? -b : b) % 10, g / 10, (g < 0 ? -g : g) % 10);
	}
}

static void
cpu_thermal(void)
{
#ifdef NEZHA_CPU_IOS27
    if(__atomic_load_n(&system_inshutdown,__ATOMIC_RELAXED))return;
#endif
	int32_t hot[2] = { INT32_MIN, INT32_MIN };
#ifndef NEZHA_CPU_IOS27
	uint64_t out = cpu_t_out;
#endif
	bool pause[2] = { false, false }, changed = false;
	cpu_t_sample();
	for (int c = 0; c < 8; c++) {
		int32_t t = c < 6 ? cpu_temp_of(0, 2 * (uint32_t)c, 2) : cpu_temp_of(1, 2 * (uint32_t)(c - 6), 2);
		if (t == INT32_MIN) {
			continue;
		}
		t += cpu_t_test;
		const int d = c < 6 ? 0 : 1;
		hot[d] = t > hot[d] ? t : hot[d];
#ifndef NEZHA_CPU_IOS27
		if (c != 0 && t >= T_PLUG) {
			out |= 1ull << c;
		} else if (t < T_PLUG - T_PLUG_HYST) {
			out &= ~(1ull << c);
		}
#endif
#ifdef NEZHA_CPU_IOS27
        if(t>=T_PLUG)pause[d]=true;
#endif
		if (t >= T_PAUSE) {
			pause[d] = true;
		}
	}
	nz_t_hot[0] = hot[0];
	nz_t_hot[1] = hot[1];
	{
		int32_t tmax = INT32_MIN;
		uint32_t at = 0;
		for (uint32_t i = 0; i < T_NSENS; i++) {
			int32_t t = cpu_tsens_raw(i);
			if (t == INT32_MIN) {
				continue;
			}
			t += cpu_t_test;
			if (t > tmax) {
				tmax = t;
				at = i;
			}
		}
		th_max = tmax;
		th_max_at = at;
		if (tmax != INT32_MIN && (th_win_max == INT32_MIN || tmax > th_win_max)) {
			th_win_max = tmax;
			th_win_max_at = at;
		}
		uint32_t stage = th_stage;
		if (tmax != INT32_MIN && (int32_t)th_hard > 0 && tmax >= (int32_t)th_hard) {
			stage = TH_STAGES - 1;
			th_hot_rounds = th_cool_rounds = 0;
		} else if (tmax != INT32_MIN && (int32_t)th_soft > 0 && tmax >= (int32_t)th_soft) {
			th_cool_rounds = 0;
			if (th_hot_rounds == 0 && stage < TH_STAGES - 1) {
				stage++;
			}
			if (++th_hot_rounds >= 5) {
				th_hot_rounds = 0;
			}
		} else if (tmax != INT32_MIN && tmax < (int32_t)th_soft - 30) {
			th_hot_rounds = 0;
			if (stage && ++th_cool_rounds >= 10) {
				th_cool_rounds = 0;
				stage--;
			}
		} else {
			th_hot_rounds = th_cool_rounds = 0;
		}
		if (stage > th_win_stage_max) {
			th_win_stage_max = stage;
		}
		if (stage != th_stage) {
			th_stage = stage;
			th_steps++;
			for (uint32_t d = 0; d < 2; d++) {
				uint32_t l = 0;
				while (stage && l + 1 < cpu_dom[d].levels && cpu_dom[d].khz[l + 1] <= th_mhz[d][stage] * 1000u) {
					l++;
				}
				cpu_scap[d] = stage ? l : ~0u;
			}
			gpu_scap = stage && &pandora_gpu_level_at_most ? pandora_gpu_level_at_most(th_mhz[2][stage]) : 0;
			changed = true;
		}
	}
	if (nz_mode >= 2) {
		for (uint32_t d = 0; d < 2; d++) {
			const struct cpu_domain *dm = &cpu_dom[d];
			uint32_t cap = cpu_tcap[d];
#ifdef NEZHA_CPU_IOS27
            if(hot[d]==INT32_MIN)pause[d]=true;
#endif
			if (pause[d]) {
				cap = 0;
			} else if (hot[d] != INT32_MIN && hot[d] >= T_HAL) {
				const uint32_t from = cap < dm->level ? cap : dm->level;
				cap = from ? from - 1 : 0;
			} else if (hot[d] != INT32_MIN && hot[d] < T_HAL - T_HAL_HYST && cap != ~0u) {
				cap = cap + 1 >= dm->top ? ~0u : cap + 1;
			}
			if (cap != cpu_tcap[d]) {
				cpu_tcap[d] = cap;
				changed = true;
			}
		}
#ifndef NEZHA_CPU_IOS27
		if (out != cpu_t_out) {
			nezha_cpu_recommend(~out & 0xffull);
			cpu_t_out = out;
			changed = true;
		}
#endif
	}
	int32_t g = cpu_temp_of(2, 0, 11);
	uint32_t gcap = 0;
	if (g != INT32_MIN) {
		g += cpu_t_test;
	}
	nz_t_hot[2] = g;
#ifdef NEZHA_CPU_IOS27
    if(g==INT32_MIN && &pandora_gpu_thermal_top)
        __atomic_store_n(&pandora_gpu_thermal_top,1u,__ATOMIC_RELAXED);
#endif
	if (&pandora_gpu_thermal_top && &pandora_gpu_level_count && g != INT32_MIN && pandora_gpu_level_count > 1) {
		const uint32_t levels = pandora_gpu_level_count, top = gpu_tcap95;
		gcap = top;
		if (g >= T_GPU) {
			const uint32_t from = top ? top : levels;
			gcap = from > 1 ? from - 1 : 1;
		} else if (top) {
			gcap = top + 1 >= levels ? 0 : top + 1;
		}
		gpu_tcap95 = gcap;
		const uint32_t combined = !gcap ? gpu_scap : !gpu_scap ? gcap : (gcap < gpu_scap ? gcap : gpu_scap);
		if (combined != __atomic_load_n(&pandora_gpu_thermal_top, __ATOMIC_RELAXED)) {
			__atomic_store_n(&pandora_gpu_thermal_top, combined, __ATOMIC_RELAXED);
			changed = true;
		}
		gcap = combined;
	}
	if (changed && cpu_t_logged < 400) {
		cpu_t_logged++;
		printf("NEZHA THERMAL: hottest core little %d.%d big %d.%d C, gpu %d.%d C%s: level caps %d %d, cores off 0x%llx, "
		    "gpu cap %u\n", hot[0] / 10, (hot[0] < 0 ? -hot[0] : hot[0]) % 10, hot[1] / 10, (hot[1] < 0 ? -hot[1] : hot[1]) % 10,
		    g / 10, (g < 0 ? -g : g) % 10, cpu_t_test ? " (test offset)" : "", (int)cpu_tcap[0], (int)cpu_tcap[1], cpu_t_out,
		    gcap);
	}
}

static int
tdeg(int32_t t)
{
	return t == INT32_MIN ? -1 : t / 10;
}

#ifdef NEZHA_CPU_IOS27
/* Keep initial CPU operation within this phone's observed normal-use clocks.
 * Thermal caps may reduce them; no fixed maximum-frequency stress probe. */
static bool cpu27_limit_levels(void)
{
    const uint32_t caps[2]={1996800,2227200};
    for(unsigned d=0;d<2;++d)
        if(!cpu_dom[d].levels || cpu_dom[d].khz[0]>caps[d])return false;
    for(unsigned d=0;d<2;++d) {
        uint32_t cap=nz27_cpu_ceiling(cpu_dom[d].khz,cpu_dom[d].levels,caps[d]);
        if(cap>cpu_tcap[d])cap=cpu_tcap[d];
        if(cap>cpu_scap[d])cap=cpu_scap[d];
        if(cpu_dom[d].top>cap)cpu_dom[d].top=cap;
    }
    return true;
}
static uint32_t cpu27_busy(unsigned cpu)
{
    static uint32_t old[8][4],last_busy[8],seeded;
    processor_t p=cpu_to_processor((int)cpu);
    processor_cpu_load_info_data_t load;
    mach_msg_type_number_t count=PROCESSOR_CPU_LOAD_INFO_COUNT;host_t host=HOST_NULL;
    if(!p || processor_info(p,PROCESSOR_CPU_LOAD_INFO,&host,(processor_info_t)&load,&count)!=KERN_SUCCESS ||
        count!=PROCESSOR_CPU_LOAD_INFO_COUNT) {nz27_cpu_valid&=~(1u<<cpu);return 0;}
    if(seeded&(1u<<cpu)) {
        uint32_t busy;
        if(nz27_cpu_load_delta(load.cpu_ticks,old[cpu],CPU_STATE_IDLE,&busy))last_busy[cpu]=busy;
    }
    memcpy(old[cpu],load.cpu_ticks,sizeof(old[cpu]));seeded|=1u<<cpu;nz27_cpu_valid|=1u<<cpu;
    return last_busy[cpu];
}
static uint32_t cpu27_sequence;
static int32_t cpu27_sample[20];
static void cpu27_publish(void)
{
    const int32_t v[20]={1,nz_mode,(int32_t)native_cpufreq_stage,(int32_t)nz27_cpu_valid,
        (int32_t)cpu_dom[0].level,(int32_t)cpu_dom[0].khz[lv(0,cpu_dom[0].level)],
        (int32_t)cpu_dom[1].level,(int32_t)cpu_dom[1].khz[lv(1,cpu_dom[1].level)],
        nz_t_hot[0],nz_t_hot[1],nz_t_hot[2],(int32_t)th_stage,
        cpu_dom[0].method,cpu_dom[1].method,(int32_t)cpu_dom[0].top,(int32_t)cpu_dom[1].top,
        (int32_t)cpu_dom[0].set_errors,(int32_t)cpu_dom[1].set_errors,
        (int32_t)nezha_cpufreq_boost[0],(int32_t)nezha_cpufreq_boost[1]};
    __atomic_add_fetch(&cpu27_sequence,1u,__ATOMIC_ACQ_REL);
    for(unsigned i=0;i<20;++i)__atomic_store_n(&cpu27_sample[i],v[i],__ATOMIC_RELAXED);
    __atomic_add_fetch(&cpu27_sequence,1u,__ATOMIC_RELEASE);
}
bool nezha_cpu27_snapshot(int32_t out[20]);
bool nezha_cpu27_snapshot(int32_t out[20])
{
    if(!out)return false;
    uint32_t before=__atomic_load_n(&cpu27_sequence,__ATOMIC_ACQUIRE);
    if(!before || (before&1))return false;
    for(unsigned i=0;i<20;++i)out[i]=__atomic_load_n(&cpu27_sample[i],__ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return before==__atomic_load_n(&cpu27_sequence,__ATOMIC_RELAXED);
}
#endif

/* boot-progress row 39 / log ring "CPU" row */
static void
nz_cpu_row(void)
{
#ifdef NEZHA_CPU_IOS27
    cpu27_publish();
#endif
	const struct cpu_domain *a = &cpu_dom[0], *b = &cpu_dom[1];
	snprintf(native_cpufreq_row, sizeof(native_cpufreq_row),
	    "N5 CPU m=%d st=%u d0 L%u/%u %uMHz b%u%% m%d | d1 L%u/%u %uMHz b%u%% m%d | T %d/%d/%dC cap %d %d off %#llx",
	    nz_mode, native_cpufreq_stage, a->level, a->top, a->khz[lv(0, a->level)] / 1000, nz_busy_pct[0], a->method,
	    b->level, b->top, b->khz[lv(1, b->level)] / 1000, nz_busy_pct[1], b->method, tdeg(nz_t_hot[0]), tdeg(nz_t_hot[1]),
	    tdeg(nz_t_hot[2]), (int)cpu_tcap[0], (int)cpu_tcap[1], cpu_t_out);
}

static void
cpu_start_levels(void)
{
	for (uint32_t d = 0; d < 2; d++) {
		struct cpu_domain *dm = &cpu_dom[d];
		const uint32_t top = cpu_get(d, 0x3, 4);   /* sustained level: boost levels above it are left alone */
		if (top != ~0u && top < dm->levels && top < dm->top) {
			dm->top = top;
		}
		int fix = -1;
		(void)PE_parse_boot_argn(d ? "nezha-cpu-fix1" : "nezha-cpu-fix0", &fix, sizeof(fix));
		dm->fixed = fix >= 0 && (uint32_t)fix < dm->levels ? (uint32_t)fix : ~0u;
		if (nz_mode == 2 && dm->fixed == ~0u) {
			dm->fixed = dm->top;
		}
		dm->floor = 0;
		while (dm->floor + 1 < dm->levels && dm->khz[dm->floor] < dm->floor_khz) {
			dm->floor++;
		}
		if (dm->method < 0) {
			const uint32_t now = cpu_get(d, 0x8, 1);
			dm->fixed = now != ~0u && now < dm->levels ? now : 0;
			printf("NEZHA CPU: domain %u cannot be set, left at level %u\n", d, dm->fixed);
			dm->level = dm->fixed;
			continue;
		}
		dm->level = dm->fixed != ~0u ? dm->fixed : dm->top;
#ifdef NEZHA_CPU_IOS27
        if(dm->level>dm->top)dm->level=dm->top;
#endif
		const int r = cpu_set_level(d, dm->level);
#ifdef NEZHA_CPU_IOS27
        if(r) {
            ++dm->set_errors;dm->method=-1;
            const uint32_t actual=cpu_get(d,0x8,1);
            if(actual<dm->levels)dm->level=actual;
        }
#endif
		printf("NEZHA CPU: domain %u cpus %u-%u, %u levels, sustained %u (%u MHz)%s, start at %u (%u MHz): %d\n", d,
		    dm->first, dm->first + dm->count - 1, dm->levels, dm->top, dm->khz[dm->top] / 1000,
		    dm->fixed != ~0u ? ", fixed" : "", dm->level, dm->khz[dm->level] / 1000, r);
	}
}

/* Mode 2 (fixed) and 3 (governor): every 16 ms; thermal every ~100 ms; logs every 2 s. Does not return. */
static void
cpu_loop(void)
{
	uint64_t now, last_log, two, ten, last_ten;
#ifndef NEZHA_CPU_IOS27
	uint64_t idle[8] = { 0 };
#endif
    uint64_t stamp;
	uint32_t tlog = 0;
	nanoseconds_to_absolutetime(2000 * NSEC_PER_MSEC, &two);
	nanoseconds_to_absolutetime(10000 * NSEC_PER_MSEC, &ten);
	for (int c = 0; c < 8; c++) {
#ifdef NEZHA_CPU_IOS27
        (void)cpu27_busy((unsigned)c);
#else
		idle[c] = nezha_cpu_idle_mach(c);
#endif
	}
	stamp = last_log = last_ten = mach_absolute_time();
	/* n5-metal-017, sheng 36ec562: a touch report holds both domains (all of nezha's cores are big ones) at their highest allowed
	 * level until nezha-cpufreq-boost-ms (default 1000, 0 off) after the last one, so a scroll starts at full speed instead of
	 * ramping 1.25x per window. GPU submissions in 2 of the last 4 windows keep each domain at or above nezha-cpufreq-frame-pct
	 * (default 60) % of its highest level for nezha-cpufreq-frame-ms (default 300). */
	/* n5-metal-018: the touch boost is a floor at nezha-cpufreq-boost-mhz0/1 (default 1996 / 2227 MHz, the scaling_max_freq Android's
	 * thermal engine keeps on policy0 / policy6 of this phone in normal use), not the top level; the frame floor default drops to 35 %.
	 * 017 held 3.6 / 4.3 GHz through every interaction and reached 105 C. */
	uint32_t boost_ms = 1000, frame_ms = 300, frame_pct = 35, boost_mhz[2] = { 1996, 2227 };
	(void)PE_parse_boot_argn("nezha-cpufreq-boost-mhz0", &boost_mhz[0], sizeof(boost_mhz[0]));
	(void)PE_parse_boot_argn("nezha-cpufreq-boost-mhz1", &boost_mhz[1], sizeof(boost_mhz[1]));
	(void)PE_parse_boot_argn("nezha-th-soft", &th_soft, sizeof(th_soft));
	(void)PE_parse_boot_argn("nezha-th-hard", &th_hard, sizeof(th_hard));
	uint32_t boost_floor[2] = { 0, 0 };
	for (uint32_t d = 0; d < 2; d++) {
		while (boost_floor[d] + 1 < cpu_dom[d].levels && cpu_dom[d].khz[boost_floor[d] + 1] <= boost_mhz[d] * 1000u) {
			boost_floor[d]++;
		}
	}
	uint64_t five = 0, last_five = 0;
	nanoseconds_to_absolutetime(5000 * NSEC_PER_MSEC, &five);
	uint32_t win_rounds = 0, win_top[2] = { 0, 0 }, win_gtop = 0;
	uint64_t win_khz[2] = { 0, 0 }, win_gmhz = 0;
	(void)PE_parse_boot_argn("nezha-cpufreq-boost-ms", &boost_ms, sizeof(boost_ms));
	(void)PE_parse_boot_argn("nezha-cpufreq-frame-ms", &frame_ms, sizeof(frame_ms));
	(void)PE_parse_boot_argn("nezha-cpufreq-frame-pct", &frame_pct, sizeof(frame_pct));
	if (frame_pct > 100) {
		frame_pct = 100;
	}
	uint64_t boost_abs = 0, frame_abs = 0, boost_end = 0, frame_end = 0;
	nanoseconds_to_absolutetime((uint64_t)boost_ms * NSEC_PER_MSEC, &boost_abs);
	nanoseconds_to_absolutetime((uint64_t)frame_ms * NSEC_PER_MSEC, &frame_abs);
	uint32_t touches = &native_hid_input_events ? native_hid_input_events[6] : 0;
	uint32_t submits = cpu_gpu_activity(), recent = 0;
	for (uint64_t round = 0;; round++) {
#ifdef NEZHA_CPU_IOS27
        if(__atomic_load_n(&system_inshutdown,__ATOMIC_RELAXED))return;
#endif
		IOSleep(16);
		now = mach_absolute_time();
		if (boost_abs && &native_hid_input_events && native_hid_input_events[6] != touches) {
			touches = native_hid_input_events[6];
			if (now >= boost_end) {
				nezha_cpufreq_boost[0] = nezha_cpufreq_boost[0] + 1;
			}
			boost_end = now + boost_abs;
		}
		if (frame_abs && cpu_gpu_activity_available()) {
			const uint32_t s = cpu_gpu_activity();
			recent = ((recent << 1) | (s != submits ? 1u : 0u)) & 15u;
			submits = s;
			if (__builtin_popcount(recent) >= 2) {
				if (now >= frame_end) {
					nezha_cpufreq_boost[1] = nezha_cpufreq_boost[1] + 1;
				}
				frame_end = now + frame_abs;
			}
		}
		const bool boosting = now < boost_end, framing = now < frame_end;
		if (framing) {
			nezha_cpufreq_boost[2] = nezha_cpufreq_boost[2] + 1;
		}
		const uint64_t span = now - stamp;
		stamp = now;
		uint32_t busy_pm[8];
		for (int c = 0; c < 8; c++) {
#ifdef NEZHA_CPU_IOS27
            busy_pm[c]=cpu27_busy((unsigned)c);
#else
			const uint64_t i = nezha_cpu_idle_mach(c), di = i - idle[c];
			idle[c] = i;
			busy_pm[c] = span ? (uint32_t)(di >= span ? 0 : 1000 - di * 1000 / span) : 0;
#endif
			cpu_busy_sum[c] += busy_pm[c];
		}
		if (nz_thermal_on && round % 6 == 0) {
			cpu_thermal();
		}
		win_rounds++;
		for (uint32_t d = 0; d < 2; d++) {
			win_khz[d] += cpu_dom[d].khz[lv(d, cpu_dom[d].level)];
			win_top[d] += cpu_dom[d].level >= cpu_dom[d].top;
		}
		if (&nezha_gpu_dcvs_mhz) {
			win_gmhz += nezha_gpu_dcvs_mhz;
			win_gtop += nezha_gpu_dcvs_mhz >= 902;
		}
		if (!last_five) {
			last_five = now;
		}
		if (now - last_five >= five && win_rounds) {
			last_five = now;
			static const char grp[] = "LBGD";
			const uint32_t mi = th_win_max_at, g = mi < 12 ? 0 : mi < 16 ? 1 : mi < 27 ? 2 : 3;
			const uint32_t gi = mi < 12 ? mi : mi < 16 ? mi - 12 : mi < 27 ? mi - 16 : 0;
			const int32_t tm = th_win_max == INT32_MIN ? 0 : th_win_max;
			const uint32_t c0 = cpu_scap[0] != ~0u ? cpu_dom[0].khz[lv(0, cpu_scap[0])] / 1000 : 0;
			const uint32_t c1 = cpu_scap[1] != ~0u ? cpu_dom[1].khz[lv(1, cpu_scap[1])] / 1000 : 0;
			const uint32_t gc = th_stage ? th_mhz[2][th_stage] : 0;
			uint64_t secs = 0;
			absolutetime_to_nanoseconds(now, &secs);
			snprintf(native_thermal_row, sizeof(native_thermal_row),
			    "TH %llus max %d.%dC %c%u st%u/%u cap %u/%u/%u top%% %u/%u/%u avg %llu/%llu/%llu tb%u ff%u",
			    secs / 1000000000ull, tm / 10, (tm < 0 ? -tm : tm) % 10, grp[g], gi, th_stage, th_win_stage_max, c0, c1, gc,
			    win_top[0] * 100 / win_rounds, win_top[1] * 100 / win_rounds, win_gtop * 100 / win_rounds,
			    win_khz[0] / win_rounds / 1000, win_khz[1] / win_rounds / 1000, win_gmhz / win_rounds,
			    nezha_cpufreq_boost[0], nezha_cpufreq_boost[1]);
			printf("NEZHA %s\n", native_thermal_row);
			win_rounds = win_top[0] = win_top[1] = win_gtop = 0;
			win_khz[0] = win_khz[1] = win_gmhz = 0;
			th_win_max = INT32_MIN;
			th_win_stage_max = th_stage;
		}
		for (uint32_t d = 0; d < 2; d++) {
			struct cpu_domain *dm = &cpu_dom[d];
			uint32_t busy = 0;
			for (uint32_t c = dm->first; c < dm->first + dm->count; c++) {
				busy = busy_pm[c] > busy ? busy_pm[c] : busy;
			}
			dm->busy_sum += busy;
			dm->khz_sum += dm->khz[lv(d, dm->level)];
			dm->windows++;
			if (dm->method < 0) {
				continue;
			}
			uint32_t top = cpu_tcap[d] < dm->top ? cpu_tcap[d] : dm->top;   /* the temperature cap */
#ifdef NEZHA_CPU_IOS27
            const uint32_t mask=d?0xc0u:0x3fu;
            if((nz27_cpu_valid&mask)!=mask)top=0;
#endif
			if (cpu_scap[d] < top) {
				top = cpu_scap[d];   /* n5-metal-018 thermal stage */
			}
			uint32_t want;
			if (dm->fixed != ~0u) {
				want = dm->fixed < top ? dm->fixed : top;
				if (want == dm->level) {
					continue;
				}
			} else {
				/* schedutil: 1.25 x what was used at the current clock; nearly always busy: at least twice */
				uint64_t need = (uint64_t)dm->khz[lv(d, dm->level)] * busy * 5 / 4 / 1000;
				if (busy >= 900 && need < 2ull * dm->khz[lv(d, dm->level)]) {
					need = 2ull * dm->khz[lv(d, dm->level)];
				}
				dm->quiet = busy < 100 ? dm->quiet + 1 : 0;
				const uint32_t floor = dm->quiet >= 12 ? 0 : (dm->floor < top ? dm->floor : top);
				want = floor;
				while (want < top && dm->khz[want] < need) {
					want++;
				}
				if (framing) {
					while (want < top && (uint64_t)dm->khz[want] * 100 < (uint64_t)dm->khz[top] * frame_pct) {
						want++;
					}
				}
				if (boosting && want < boost_floor[d]) {
					want = boost_floor[d] < top ? boost_floor[d] : top;
				}
				if (dm->level > top) {
					dm->down_windows = 0;
				} else if (want > dm->level) {
					dm->down_windows = 0;
				} else if (want < dm->level) {
					dm->want_max_down = dm->down_windows ? (want > dm->want_max_down ? want : dm->want_max_down) : want;
					if (++dm->down_windows < 4) {
						continue;
					}
					want = dm->want_max_down < top ? dm->want_max_down : top;
					dm->down_windows = 0;
				} else {
					dm->down_windows = 0;
					continue;
				}
			}
			if (want != dm->level) {
				if (cpu_set_level(d, want) == 0) {
					dm->level = want;
					dm->changes++;
				} else {
                    ++dm->set_errors;
#ifdef NEZHA_CPU_IOS27
                    /* An ambiguous SCMI failure is not a reason to issue
                     * the same hardware request on every governor tick. */
                    dm->method=-1;
#endif
                    if(dm->set_errors<=5)
                        printf("NEZHA CPU: domain %u level %u refused; method %d\n",d,want,dm->method);
                }
			}
		}
#ifdef NEZHA_CPU_IOS27
        cpu27_publish();
#endif
		if (now - last_log >= two) {
			last_log = now;
			const bool chatty = round < 4000 || (round % 1250) < 125;
			uint64_t w = cpu_dom[0].windows ? cpu_dom[0].windows : 1;
			if (chatty) {
				printf("NEZHA CPU: busy per cpu %llu %llu %llu %llu %llu %llu | %llu %llu %%\n",
				    cpu_busy_sum[0] / w / 10, cpu_busy_sum[1] / w / 10, cpu_busy_sum[2] / w / 10, cpu_busy_sum[3] / w / 10,
				    cpu_busy_sum[4] / w / 10, cpu_busy_sum[5] / w / 10, cpu_busy_sum[6] / w / 10, cpu_busy_sum[7] / w / 10);
				cpu_temps(false);
			} else if (++tlog % 5 == 0) {
				cpu_t_log();   /* raw sensors every 10 s after the chatty phase */
			}
			for (int c = 0; c < 8; c++) {
				cpu_busy_sum[c] = 0;
			}
			for (uint32_t d = 0; d < 2; d++) {
				struct cpu_domain *dm = &cpu_dom[d];
				const uint32_t fw = cpu_get(d, 0x8, 1);
				const bool taken = dm->fixed == ~0u && fw != ~0u && fw < dm->levels && fw != dm->level;
				nz_busy_pct[d] = dm->windows ? (uint32_t)(dm->busy_sum / dm->windows / 10) : 0;
				if (chatty) {
					printf("NEZHA CPU: domain %u busy %u%% avg %llu MHz, level %u (%u MHz), %u changes, firmware %d%s\n",
					    d, nz_busy_pct[d], dm->windows ? dm->khz_sum / dm->windows / 1000 : 0, dm->level,
					    dm->khz[lv(d, dm->level)] / 1000, dm->changes, (int)fw, taken ? " (taken)" : "");
				}
				if (taken) {
					dm->level = fw;   /* pandora 0204: follow the firmware's own record */
				} else if (dm->fixed != ~0u && fw != ~0u && fw != dm->level && dm->method >= 0) {
					(void)cpu_set_level(d, dm->level);   /* fixed: put it back */
				}
				dm->busy_sum = dm->khz_sum = dm->windows = 0;
				dm->changes = 0;
			}
			nz_cpu_row();
		}
		if (now - last_ten >= ten) {
			last_ten = now;
#ifndef NEZHA_CPU_IOS27
			nezha_idle_log();
#endif
		}
	}
}

/* nezha-cpufreq=0/1 with nezha-thermal=1: temperatures (and the GPU cap) only, every 100 ms; row every 2 s. */
static void
cpu_thermal_only(void)
{
	for (uint64_t round = 0;; round++) {
#ifdef NEZHA_CPU_IOS27
        if(__atomic_load_n(&system_inshutdown,__ATOMIC_RELAXED))return;
#endif
		IOSleep(100);
		cpu_thermal();
		if (round % 20 == 0) {
			nz_cpu_row();
		}
		if (round % (round < 640 ? 20 : 100) == 0) {
			cpu_t_log();   /* raw sensors every 2 s, every 10 s after ~64 s */
		}
		if (round % 100 == 0) {
#ifndef NEZHA_CPU_IOS27
			nezha_idle_log();
#endif
		}
	}
}

static void
cpu_thread(__unused void *arg, __unused wait_result_t wr)
{
	int idle = 0, test = 0, thermal = -1;
	const ml_topology_info_t *t = ml_get_topology_info();
	native_cpufreq_stage = 1;
	IOSleep(3000);
#ifdef NEZHA_CPU_IOS27
    if(__atomic_load_n(&system_inshutdown,__ATOMIC_RELAXED))goto cpu27_done;
    if(!t || t->num_cpus!=8) {
        printf("NEZHA CPU27: unexpected topology; no frequency control\n");goto cpu27_done;
    }
#endif
	if (t) {
		printf("NEZHA CPU: %u CPUs in %u clusters (boot cpu %u)\n", t->num_cpus, t->num_clusters,
		    t->boot_cpu ? t->boot_cpu->cpu_id : 0);
		for (unsigned int i = 0; i < t->num_cpus; i++) {
			printf("NEZHA CPU: cpu %u phys 0x%x cluster %u type %d\n", t->cpus[i].cpu_id, t->cpus[i].phys_id,
			    t->cpus[i].cluster_id, (int)t->cpus[i].cluster_type);
		}
	}
#ifndef NEZHA_CPU_IOS27
	if (PE_parse_boot_argn("nezha-idle", &idle, sizeof(idle)) && idle == 1) {
		nezha_idle_start();
	}
#endif
	(void)PE_parse_boot_argn("nezha-thermal", &thermal, sizeof(thermal));
	nz_thermal_on = thermal != 0;
#ifdef NEZHA_CPU_IOS27
    if(nz_mode>=2)nz_thermal_on=true;
#endif
	if (PE_parse_boot_argn("nezha-thermal-test", &test, sizeof(test)) && test > 0 && test < 80) {
		cpu_t_test = test * 10;
		printf("NEZHA THERMAL: test: every threshold %d C lower\n", test);
	}
	cpu_domains_default();
	cpu_temps(true);
	native_cpufreq_stage = 2;
	nz_cpu_row();
	if (nz_mode >= 1) {
		cpu_clocks("at 3 s");
		if (!cpu_scmi_probe()) {
			nz_mode = nz_mode >= 2 ? -nz_mode : nz_mode;   /* shown on the row: no SCMI */
			nz_cpu_row();
		} else if (nz_mode >= 2) {
#ifdef NEZHA_CPU_IOS27
            if(cpu_temp_of(0,0,12)==INT32_MIN || cpu_temp_of(1,0,4)==INT32_MIN || cpu_temp_of(2,0,11)==INT32_MIN) {
                printf("NEZHA CPU27: temperature source unavailable; retain existing clocks\n");nz_mode=1;goto probe_only;
            }
            cpu_thermal();
            if(!cpu27_limit_levels()) {
                printf("NEZHA CPU27: no level within initial ceiling\n");nz_mode=1;goto probe_only;
            }
#endif
			native_cpufreq_stage = 3;
			cpu_check_levels();
			cpu_start_levels();
			cpu_clocks("after start");
			native_cpufreq_stage = 4;
			nz_cpu_row();
			cpu_loop();   /* does not return */
		}
	}
#ifdef NEZHA_CPU_IOS27
probe_only:
    if(__atomic_load_n(&system_inshutdown,__ATOMIC_RELAXED))goto cpu27_done;
#endif
	if (nz_thermal_on) {
		cpu_thermal_only();   /* does not return */
	}
#ifdef NEZHA_CPU_IOS27
    if(__atomic_load_n(&system_inshutdown,__ATOMIC_RELAXED))goto cpu27_done;
#endif
	IOSleep(17000);
	if (nz_mode == 1) {
		cpu_clocks("at 20 s");
	}
	nz_cpu_row();
#ifdef NEZHA_CPU_IOS27
cpu27_done:
#endif
	printf("NEZHA CPU: stopped\n");
#ifdef NEZHA_CPU_IOS27
    thread_terminate(current_thread());thread_block(THREAD_CONTINUE_NULL);
#endif
}

/* From nezha_input_start (bsd_autoconf, after IOKitBSDInit). */
void nezha_cpu_start(void);
void
nezha_cpu_start(void)
{
	static bool started;
	thread_t thread;
	if (started) {
		return;
	}
	started = true;
	(void)PE_parse_boot_argn("nezha-cpufreq", &nz_mode, sizeof(nz_mode));
#ifdef NEZHA_CPU_IOS27
    if(nz_mode<0 || nz_mode>3)nz_mode=0;
#endif
	int thermal = -1;
	(void)PE_parse_boot_argn("nezha-thermal", &thermal, sizeof(thermal));
	if (nz_mode <= 0 && thermal <= 0) {
		return;   /* nothing requested */
	}
	if (kernel_thread_start(cpu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
