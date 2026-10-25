/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * CPU0 continuous observations in the already mapped 4 KiB cas mailbox.
 * Does not start a driver, unmask IRQs, allocate memory or stop the kernel.
 */
extern unsigned long cas_vm_trace_address;
extern void cas_runtime_vectors(void);
void cas_runtime_step(unsigned long phase, unsigned long detail);
void cas_irq_publish(volatile uint64_t const *values);
void cas_runtime_panic(unsigned long caller, unsigned int reason, char const *format);

#ifndef CAS_CONTINUOUS_HOST_TEST
static uint64_t cas_observe_mask(void) {
	uint64_t value;
	__asm__ volatile ("mrs %0, daif\n\tmsr daifset, #3" : "=r" (value) :: "memory");
	return value;
}
static void cas_observe_restore(uint64_t value) {
	__asm__ volatile ("msr daif, %0" :: "r" (value) : "memory");
}
static void cas_observe_barrier(void) { __asm__ volatile ("dsb sy" ::: "memory"); }
#define CAS_READ_SYS(name, output) __asm__ volatile ("mrs %0, " #name : "=r" (output))
#endif

static volatile uint64_t *cas_observe_base(void) {
	if (cas_vm_trace_address <= 1) { return 0; }
	volatile uint64_t *base = (void *)cas_vm_trace_address;
	if (base[0] != 0x363048534143ULL || base[1] != 1 || base[2] != 256 || base[3] == 0) { return 0; }
	return base;
}

static void cas_observe_commit(volatile uint64_t *out, uint64_t *record, unsigned int words) {
	record[words - 2] = 0;
	for (unsigned int i = 1; i < words - 2; ++i) { record[words - 2] ^= record[i]; }
	record[words - 1] = ~record[0];
	out[0] = 0;
	for (unsigned int i = 1; i < words; ++i) { out[i] = record[i]; }
	cas_observe_barrier();
	out[0] = record[0];
	cas_observe_barrier();
}

void cas_irq_publish(volatile uint64_t const *values) {
	uint64_t daif = cas_observe_mask();
	volatile uint64_t *base = cas_observe_base();
	if (base && values[0] == 1) {
		volatile uint64_t *out = base + 64; /* offset512, bytes320 */
		uint64_t record[40] = {0};
		record[0] = 0x31515249534143ULL; /* CASIRQ1 */
		record[1] = base[3]; record[2] = base[19];
		record[3] = out[0] == record[0] && out[1] == record[1] && out[2] == record[2] ? out[3] + 1 : 1;
		record[4] = 32;
		for (unsigned int i = 0; i < 32; ++i) { record[5 + i] = values[i]; }
		cas_observe_commit(out, record, 40);
	}
	cas_observe_restore(daif);
}

void cas_runtime_step(unsigned long phase, unsigned long detail) {
	uint64_t daif = cas_observe_mask();
	volatile uint64_t *base = cas_observe_base();
	if (base) {
		volatile uint64_t *out = base + 104; /* offset832, bytes128 */
		uint64_t record[16] = {0};
		record[0] = 0x314e5552534143ULL; /* CASRUN1 */
		record[1] = base[3]; record[2] = base[19];
		if (out[0] == record[0] && out[1] == record[1] && out[2] == record[2]) {
			record[3] = out[3]; record[5] = out[5];
		}
		++record[3]; record[4] = phase; record[6] = detail;
		if (phase >= 0x70 && phase <= 0x7f) { record[5] |= 1ULL << (phase - 0x70); }
		if (phase == 0x90) { record[5] |= 1ULL << 31; }
		CAS_READ_SYS(vbar_el1, record[7]);
		record[8] = daif;
		CAS_READ_SYS(cntvct_el0, record[9]);
		cas_observe_commit(out, record, 16);
		cas_vm_trace(phase, (unsigned long)base);
	}
	cas_observe_restore(daif);
}

void cas_runtime_panic(unsigned long caller, unsigned int reason, char const *format) {
	uint64_t daif = cas_observe_mask();
	volatile uint64_t *base = cas_observe_base();
	if (base) {
		volatile uint64_t *out = base + 128; /* offset1024, bytes256 */
		uint64_t record[32] = {0};
		record[0] = 0x31434e50534143ULL; /* CASPNC1 */
		record[1] = base[3]; record[2] = base[19];
		/* Preserve the first committed panic of this boot during nested panic. */
		if (out[0] != record[0] || out[1] != record[1] || out[2] != record[2] || out[31] != ~record[0]) {
			record[3] = caller; record[4] = reason;
			CAS_READ_SYS(esr_el1, record[5]); CAS_READ_SYS(far_el1, record[6]);
			CAS_READ_SYS(elr_el1, record[7]); CAS_READ_SYS(spsr_el1, record[8]);
			record[9] = daif; record[10] = base[30];
			CAS_READ_SYS(cntvct_el0, record[11]);
			unsigned char *text = (void *)&record[12];
			unsigned int length = 0;
			if (format) {
				while (length < 111 && format[length]) { text[length] = (unsigned char)format[length]; ++length; }
			}
			record[26] = length; /* Literal format, not printf-expanded arguments. */
			cas_observe_commit(out, record, 32);
			cas_runtime_step(0x90, caller);
		}
	}
	cas_observe_restore(daif);
}
