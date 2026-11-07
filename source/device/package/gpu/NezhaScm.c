/*
 * nezha N5: TrustZone (SCM) calls, the PAS loader and the firmware pack for NezhaGpu.c.
 *
 * Port of data-a-live (ShueiTsuki, ios-pandora) wsl/pandora_adsp.c at
 * 91f1f2468f30841c39123c7ba20ae520ecd82c84, only the parts pandora_gpu.c calls: scm_smc_raw/scm_smc
 * (Linux qcom_scm-smc.c, SMCCC yielding call, the WAITQ resume loop), pandora_scm_call, and
 * pandora_pas_load_auth with pas_load/pas_auth (qcom_mdt_loader, a whole .mbn image or .mdt + .bNN).
 * Unchanged in behaviour; pandora's firmware partition (FAT on modem_a/b) is not read: files come only
 * from the pack.
 *
 * The pack (tools/mkfwpack.py layout: "PANDFW01", u32 count, entries {char name[48]; u64 off; u64 size},
 * files on 4 KiB boundaries) is linked into the kernel by scripts/nezha-gpu.py as nezha_gpu_fwpack
 * (NezhaGpuFw.c, generated; firmware never enters Git). pandora_fw_read reads from it.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#if NEZHA_GPU_SHARED_BACKEND
#include <vm/vm_kern_xnu.h>
#endif
#include <vm/pmap.h>
#include <arm/caches_internal.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);

extern const uint8_t nezha_gpu_fwpack[];
extern const uint64_t nezha_gpu_fwpack_size;

int pandora_fw_read(uint64_t off, void *dst, uint64_t len);
int
pandora_fw_read(uint64_t off, void *dst, uint64_t len)
{
	if (!nezha_gpu_fwpack_size || off + len < off || off + len > nezha_gpu_fwpack_size) {
		return EIO;
	}
	memcpy(dst, nezha_gpu_fwpack + off, (size_t)len);
	return 0;
}

static vm_offset_t
ad_map(uint64_t pa, uint64_t len, uint32_t wimg)
{
	return ml_io_map_unmappable((vm_offset_t)pa, (vm_size_t)((len + 0xfff) & ~0xfffull), wimg);
}

/* ---- SCM ------------------------------------------------------------------------ */

struct scm_res {
	uint64_t a0, a1, a2, a3;
};

static void
scm_smc_raw(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5,
    struct scm_res *res)
{
	uint64_t a0 = fn, a6 = 0;
	for (int tries = 0; tries < 100000; tries++) {
		register uint64_t r0 __asm__("x0") = a0;
		register uint64_t r1 __asm__("x1") = arginfo;
		register uint64_t r2 __asm__("x2") = x2;
		register uint64_t r3 __asm__("x3") = x3;
		register uint64_t r4 __asm__("x4") = x4;
		register uint64_t r5 __asm__("x5") = x5;
		register uint64_t r6 __asm__("x6") = a6;
		register uint64_t r7 __asm__("x7") = 0;
		__asm__ volatile ("smc #0"
		    : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7)
		    :
		    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		res->a0 = r0;
		res->a1 = r1;
		res->a2 = r2;
		res->a3 = r3;
		if (r0 != 1) {           /* QCOM_SCM_INTERRUPTED */
			return;
		}
		a0 = 1;
		a6 = r6;
	}
}

#define SCM_FN(svc, cmd)  ((1ULL << 30) | (2ULL << 24) | (((uint64_t)(svc) & 0xff) << 8) | ((cmd) & 0xff))
#define SCM_ARGS(n)       ((uint64_t)(n) & 0xf)
#define SCM_RW            2u
#define SCM_WAITQ_SLEEP   2u
#define SCM_SVC_WAITQ     0x24u
#define SCM_WAITQ_RESUME  0x02u
#define SCM_WAITQ_GET_CTX 0x03u
#define SCM_WAKE_ONE      1u

LCK_GRP_DECLARE(nz_scm_grp, "nezha_scm");
LCK_MTX_DECLARE(nz_scm_lock, &nz_scm_grp);

static void
scm_smc(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5,
    struct scm_res *res)
{
	static unsigned logged;
	lck_mtx_lock(&nz_scm_lock);
	scm_smc_raw(fn, arginfo, x2, x3, x4, x5, res);
	for (unsigned n = 0; res->a0 == SCM_WAITQ_SLEEP && n < 16; n++) {
		uint64_t queue = res->a1, call = res->a2;
		struct scm_res w = {0, 0, 0, 0};
		unsigned ms = 0;
		for (; ms < 5000; ms++) {
			scm_smc_raw(SCM_FN(SCM_SVC_WAITQ, SCM_WAITQ_GET_CTX), SCM_ARGS(0), 0, 0, 0, 0, &w);
			if (w.a0 == 0 && w.a2 == SCM_WAKE_ONE) {
				break;
			}
			delay(1000);
		}
		if (logged < 16) {
			logged++;
			printf("NEZHA SCM: fn %#llx queued (queue %#llx, call %#llx); GET_WQ_CTX after %u ms -> a0 %#llx "
			    "ctx %#llx flags %#llx more %#llx\n", fn, queue, call, ms, w.a0, w.a1, w.a2, w.a3);
		}
		scm_smc_raw(SCM_FN(SCM_SVC_WAITQ, SCM_WAITQ_RESUME), SCM_ARGS(1), call, 0, 0, 0, res);
	}
	lck_mtx_unlock(&nz_scm_lock);
}

void pandora_scm_call(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t *a0,
    uint64_t *a1);
void
pandora_scm_call(uint64_t fn, uint64_t arginfo, uint64_t x2, uint64_t x3, uint64_t *a0,
    uint64_t *a1)
{
	struct scm_res r;
	scm_smc(fn, arginfo, x2, x3, 0, 0, &r);
	*a0 = r.a0;
	*a1 = r.a1;
}

/* Audio PAS operations use the same global SCM/WAITQ serialization as GPU.
 * Only the ADSP/ADSP-DTB PAS identifiers and required PIL commands are exposed.
 * No independent SMC implementation or second lock is introduced. */
int nezha_audio_pas_call(unsigned command, unsigned pas, uint64_t arg1, uint64_t arg2);
int
nezha_audio_pas_call(unsigned command, unsigned pas, uint64_t arg1, uint64_t arg2)
{
    if (pas != 1 && pas != 36) return EINVAL;
    uint64_t info;
    switch (command) {
    case 1:
        if (!arg1 || (arg1 & 4095) || arg2) return EINVAL;
        info = SCM_ARGS(2) | (SCM_RW << 6);
        break;
    case 2:
        if (!arg1 || !arg2 || ((arg1 | arg2) & 4095)) return EINVAL;
        info = SCM_ARGS(3);
        break;
    case 5:
    case 6:
        if (arg1 || arg2) return EINVAL;
        info = SCM_ARGS(1);
        break;
    default: return EINVAL;
    }
    struct scm_res r = {0, 0, 0, 0};
    scm_smc(SCM_FN(2, command), info, pas, arg1, arg2, 0, &r);
    /* Preserve the secure firmware's answer before normalizing it to EIO.
     * This path runs once per lifecycle step; no periodic tracing or retry. */
    printf("NEZHA AUDIO27 PAS: command %u pas %u -> a0 %#llx a1 %#llx a2 %#llx a3 %#llx\n",
        command, pas, r.a0, r.a1, r.a2, r.a3);
    return (r.a0 || r.a1) ? EIO : 0;
}

/* ---- the pack ------------------------------------------------------------------- */

struct fw_ent {
	char name[48];
	uint64_t off, size;
};
static struct fw_ent ad_dir[63];
static unsigned ad_nfiles;

struct fw_file {
	uint64_t size, pack_off;
};

static int
fw_dir(void)
{
	uint8_t blk[4096];
	int err = pandora_fw_read(0, blk, sizeof(blk));
	if (err) {
		printf("NEZHA SCM: no firmware pack linked (%d)\n", err);
		return ENOENT;
	}
	uint32_t n;
	memcpy(&n, blk + 8, 4);
	if (memcmp(blk, "PANDFW01", 8) || n == 0 || n > 63) {
		printf("NEZHA SCM: no firmware pack header\n");
		return ENOENT;
	}
	memcpy(ad_dir, blk + 16, n * sizeof(struct fw_ent));
	ad_nfiles = n;
	return 0;
}

static struct fw_ent *
fw_find(const char *name)
{
	for (unsigned i = 0; i < ad_nfiles; i++) {
		ad_dir[i].name[47] = 0;
		if (!strcmp(ad_dir[i].name, name)) {
			return &ad_dir[i];
		}
	}
	return NULL;
}

static int
fw_open(const char *name, struct fw_file *f)
{
	bzero(f, sizeof(*f));
	struct fw_ent *e = fw_find(name);
	if (!e) {
		return ENOENT;
	}
	f->size = e->size;
	f->pack_off = e->off;
	return 0;
}

static int
fw_pread(const struct fw_file *f, uint64_t off, void *dst, uint64_t len)
{
	if (off + len < off || off + len > f->size) {
		return EINVAL;
	}
	return pandora_fw_read(f->pack_off + off, dst, len);
}

/* ---- PAS (pandora_adsp.c pas_load / pas_auth) ----------------------------------- */

struct elf32_phdr {
	uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};
#define MDT_TYPE_MASK    (7u << 24)
#define MDT_TYPE_HASH    (2u << 24)
#define MDT_RELOCATABLE  (1u << 27)

static bool
mdt_loadable(const struct elf32_phdr *p)
{
	return p->p_type == 1 && (p->p_flags & MDT_TYPE_MASK) != MDT_TYPE_HASH && p->p_memsz;
}

static int
mdt_segment(const char *base, const struct fw_file *mdt, const struct elf32_phdr *p, unsigned i, void *dst)
{
	char name[48];
	struct fw_file seg;
	snprintf(name, sizeof(name), "%s.b%02u", base, i);
	if (!fw_open(name, &seg)) {
		if (seg.size != p->p_filesz) {
			printf("NEZHA SCM: %s is %llu bytes, segment wants %u\n", name, seg.size, p->p_filesz);
			return EINVAL;
		}
		return fw_pread(&seg, 0, dst, p->p_filesz);
	}
	if ((uint64_t)p->p_offset + p->p_filesz <= mdt->size) {
		return fw_pread(mdt, p->p_offset, dst, p->p_filesz);
	}
	printf("NEZHA SCM: segment %u of %s missing\n", i, base);
	return ENOENT;
}

static int
pas_load(const char *base, unsigned pas, uint64_t mem_pa, uint64_t mem_size)
{
	char name[48];
	if (strchr(base, '.')) {
		strlcpy(name, base, sizeof(name));
	} else {
		snprintf(name, sizeof(name), "%s.mdt", base);
	}
	struct fw_file mdt;
	if (fw_open(name, &mdt)) {
		printf("NEZHA SCM: %s not in the pack\n", name);
		return ENOENT;
	}
	if (mdt.size < 52 || mdt.size > 0x10000) {
		printf("NEZHA SCM: %s: size %llu\n", name, mdt.size);
		return EINVAL;
	}
	static uint8_t hdr[0x10000];
	int err = fw_pread(&mdt, 0, hdr, mdt.size);
	if (err) {
		return err;
	}
	uint32_t phoff, phnum;
	memcpy(&phoff, hdr + 28, 4);
	phnum = hdr[44] | (hdr[45] << 8);
	if (memcmp(hdr, "\177ELF", 4) || hdr[4] != 1 || phnum < 2 || phnum > 128 ||
	    phoff + phnum * sizeof(struct elf32_phdr) > mdt.size) {
		printf("NEZHA SCM: %s is not a 32-bit ELF header\n", name);
		return EINVAL;
	}
	const struct elf32_phdr *ph = (const struct elf32_phdr *)(hdr + phoff);
	unsigned hash = 0;
	uint64_t min = ~0ull, max = 0;
	bool reloc = false;
	for (unsigned i = 0; i < phnum; i++) {
		if (!hash && i && (ph[i].p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
			hash = i;
		}
		if (!mdt_loadable(&ph[i])) {
			continue;
		}
		reloc |= (ph[i].p_flags & MDT_RELOCATABLE) != 0;
		min = MIN(min, (uint64_t)ph[i].p_paddr);
		max = MAX(max, ((uint64_t)ph[i].p_paddr + ph[i].p_memsz + 0xfff) & ~0xfffull);
	}
	if (!hash || ph[0].p_type == 1 || min == ~0ull) {
		printf("NEZHA SCM: %s: no hash segment\n", name);
		return EINVAL;
	}
	uint32_t ehdr_size = ph[0].p_filesz, hash_size = ph[hash].p_filesz;
	uint64_t md_size = ((uint64_t)ehdr_size + hash_size + 0xfff) & ~0xfffull;
	printf("NEZHA SCM: %s: %u segments, hash %u (%u bytes), load %#llx-%#llx%s\n", name, phnum, hash,
	    hash_size, min, max, reloc ? " relocatable" : "");
	if (max - min > mem_size) {
		printf("NEZHA SCM: %s does not fit its carveout\n", name);
		return EINVAL;
	}
	vm_offset_t md = 0;
	if (kmem_alloc_contig(kernel_map, &md, (vm_size_t)md_size, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return ENOMEM;
	}
	uint64_t md_pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)md));
	memcpy((void *)md, hdr, ehdr_size);
	if (ehdr_size + hash_size == mdt.size) {
		memcpy((void *)(md + ehdr_size), hdr + ehdr_size, hash_size);
	} else if ((uint64_t)ph[hash].p_offset + hash_size <= mdt.size) {
		memcpy((void *)(md + ehdr_size), hdr + ph[hash].p_offset, hash_size);
	} else if ((err = mdt_segment(base, &mdt, &ph[hash], hash, (void *)(md + ehdr_size)))) {
		return err;
	}
	flush_dcache64((addr64_t)md_pa, (unsigned)md_size, TRUE);
	struct scm_res r;
	scm_smc(SCM_FN(2, 1), SCM_ARGS(2) | (SCM_RW << 6), pas, md_pa, 0, 0, &r);
	printf("NEZHA SCM: pas %u init_image (%u bytes) -> a0 %#llx (%d) a1 %#llx\n", pas,
	    ehdr_size + hash_size, r.a0, (int)(int32_t)r.a0, r.a1);
	if (r.a0 || r.a1) {
		return EIO;
	}
	if (reloc) {
		scm_smc(SCM_FN(2, 2), SCM_ARGS(3), pas, mem_pa, max - min, 0, &r);
		printf("NEZHA SCM: pas %u mem_setup %#llx+%#llx -> a0 %#llx a1 %#llx\n", pas, mem_pa,
		    max - min, r.a0, r.a1);
		if (r.a0 || r.a1) {
			return EIO;
		}
	}
	vm_offset_t mem = ad_map(mem_pa, mem_size, VM_WIMG_WCOMB);
	if (!mem) {
		printf("NEZHA SCM: %s: carveout %#llx+%#llx not mapped\n", name, mem_pa, mem_size);
		return ENOMEM;
	}
	uint64_t reloc_base = reloc ? min : mem_pa, total = 0;
	for (unsigned i = 0; i < phnum && !err; i++) {
		if (!mdt_loadable(&ph[i])) {
			continue;
		}
		uint64_t off = (uint64_t)ph[i].p_paddr - reloc_base;
		if ((int64_t)off < 0 || off + ph[i].p_memsz > mem_size || ph[i].p_filesz > ph[i].p_memsz) {
			printf("NEZHA SCM: %s segment %u outside the carveout\n", name, i);
			err = EINVAL;
			break;
		}
		if (ph[i].p_filesz && (err = mdt_segment(base, &mdt, &ph[i], i, (void *)(mem + off)))) {
			break;
		}
		if (ph[i].p_memsz > ph[i].p_filesz) {
			bzero((void *)(mem + off + ph[i].p_filesz), ph[i].p_memsz - ph[i].p_filesz);
		}
		total += ph[i].p_filesz;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	ml_io_unmap(mem, (vm_size_t)((mem_size + 0xfff) & ~0xfffull));
	if (!err) {
		printf("NEZHA SCM: %s: %llu bytes loaded at %#llx\n", name, total, mem_pa);
	}
	return err;
}

static int
pas_auth(unsigned pas)
{
	struct scm_res r;
	uint64_t t0 = mach_absolute_time(), ns;
	scm_smc(SCM_FN(2, 5), SCM_ARGS(1), pas, 0, 0, 0, &r);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	printf("NEZHA SCM: pas %u auth_and_reset -> a0 %#llx a1 %#llx (%llu ms)\n", pas, r.a0, r.a1,
	    ns / 1000000);
	return (r.a0 || r.a1) ? EIO : 0;
}

/* Load FILE from the pack as PAS image PAS into its carveout, then authenticate and reset it.
 * ENOENT when the pack or the file is absent (NezhaGpu.c then skips the zap). */
int pandora_pas_load_auth(const char *file, unsigned pas, uint64_t mem_pa, uint64_t mem_size);
int
pandora_pas_load_auth(const char *file, unsigned pas, uint64_t mem_pa, uint64_t mem_size)
{
	int err = ad_nfiles ? 0 : fw_dir();
	if (!err) {
		err = pas_load(file, pas, mem_pa, mem_size);
	}
	if (!err) {
		err = pas_auth(pas);
	}
	return err;
}
