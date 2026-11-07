/* SPDX-License-Identifier: APSL-2.0 */
/* Derived from ShueiTsuki/data-a-live d2c7089b, wsl/pandora_gpi.c. */
/*
 * Qualcomm GPI DMA engine for one GPII, polled (NezhaPowerGpi.h). Sequences as Linux 6.6 gpi.c:
 * gpi_ch_init (event ring allocated and programmed, then both channels allocated, programmed and
 * started), gpi_issue_pending (TREs copied to the ring, doorbell = next free element),
 * gpi_process_events (the event ring's hardware pointer in its context word 4, events consumed,
 * doorbell = last element handed back), and the I2C split of i2c-qcom-geni.c geni_i2c_gpi_xfer.
 * Command completion requires its control status bit; context state alone is insufficient.
 */
#if defined(KERNEL)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include <arm/caches_internal.h>
#include "NezhaPowerGpi.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, uint32_t);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);

/* Registers (GPII n, channel k), relative to block - ee_offset. */
#define CH_CNTXT(n, k)     (0x20000u + 0x4000u * (n) + 0x80u * (k))
#define CH_QOS(n, k)       (0x2005cu + 0x4000u * (n) + 0x80u * (k))
#define CH_SCRATCH(n, k, i) (0x20060u + 0x4000u * (n) + 0x80u * (k) + 4u * (i))
#define EV_CNTXT(n)        (0x21000u + 0x4000u * (n))
#define CH_DB(n, k)        (0x22000u + 0x4000u * (n) + 0x8u * (k))
#define EV_DB(n)           (0x22100u + 0x4000u * (n))
#define CH_CMD(n)          (0x23008u + 0x4000u * (n))
#define EV_CMD(n)          (0x23010u + 0x4000u * (n))
#define TYPE_IRQ(n)        (0x23080u + 0x4000u * (n))
#define TYPE_IRQ_MSK(n)    (0x23088u + 0x4000u * (n))
#define SRC_CH_IRQ(n)      (0x23090u + 0x4000u * (n))
#define SRC_EV_IRQ(n)      (0x23094u + 0x4000u * (n))
#define SRC_CH_IRQ_MSK(n)  (0x23098u + 0x4000u * (n))
#define SRC_EV_IRQ_MSK(n)  (0x2309cu + 0x4000u * (n))
#define SRC_CH_IRQ_CLR(n)  (0x230a0u + 0x4000u * (n))
#define SRC_EV_IRQ_CLR(n)  (0x230a4u + 0x4000u * (n))
#define IEOB_IRQ_MSK(n)    (0x230b8u + 0x4000u * (n))
#define IEOB_IRQ_CLR(n)    (0x230c0u + 0x4000u * (n))
#define GLOB_IRQ_STTS(n)   (0x23100u + 0x4000u * (n))
#define GLOB_IRQ_EN(n)     (0x23108u + 0x4000u * (n))
#define GLOB_IRQ_CLR(n)    (0x23110u + 0x4000u * (n))
#define GPII_IRQ_STTS(n)   (0x23118u + 0x4000u * (n))
#define GPII_IRQ_EN(n)     (0x23120u + 0x4000u * (n))
#define GPII_IRQ_CLR(n)    (0x23128u + 0x4000u * (n))
#define INTSET(n)          (0x23180u + 0x4000u * (n))
#define MSI_LSB(n)         (0x23188u + 0x4000u * (n))
#define MSI_MSB(n)         (0x2318cu + 0x4000u * (n))
#define ERROR_LOG(n)       (0x23200u + 0x4000u * (n))
#define CNTXT_SCRATCH(n, i) (0x23400u + 0x4000u * (n) + 4u * (i))

/* Context words. */
#define CX_CONFIG     0x00u
#define CX_LENGTH     0x04u
#define CX_BASE_LSB   0x08u
#define CX_BASE_MSB   0x0cu
#define CX_RP_LSB     0x10u
#define CX_INT_MOD    0x20u
#define CX_MSI_LSB    0x28u
#define CX_MSI_MSB    0x2cu
#define CX_RPU_LSB    0x30u
#define CX_RPU_MSB    0x34u

/* Commands: opcode << 24 | channel. */
#define OP_ALLOCATE   0u
#define OP_START      1u
#define OP_STOP       2u
#define OP_RESET      9u
#define OP_DEALLOC    10u
#define ST_NOT_ALLOC  0u
#define ST_ALLOCATED  1u
#define ST_STARTED    2u
#define ST_STOPPED    3u

/* Memory: TX ring, RX ring (64 TREs each), event ring (128 events, ev-factor 1), bounce buffers.
 * Each ring sits at a multiple of its own (power of two) length, as gpi_alloc_ring aligns them. */
#define RING_LEN      (64u * PGPI_EL_SIZE)
#define EV_LEN        (128u * PGPI_EL_SIZE)
#define TX_RING       0x0000u
#define RX_RING       0x0400u
#define EV_RING       0x0800u
#define TX_BUF        0x1000u
#define RX_BUF        0x1800u
#define BUF_MAX       0x800u
#define MEM_LEN       0x2000u

#define PROTO_GPI     2u      /* channel context protocol: GPI */
#define CMD_TIMEOUT_MS 250u
#define XFER_TIMEOUT_MS 100u

static uint32_t
rd(const NpGpi *g, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(g->regs + off - g->ee_offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
wr(const NpGpi *g, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(g->regs + off - g->ee_offset) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* A command, then its channel (or the event ring) in STATE; the control interrupt status cleared. */
static int
command(NpGpi *g, bool ev, uint32_t op, uint32_t chid, uint32_t state)
{
	const uint32_t n = g->gpii, cx = ev ? EV_CNTXT(n) : CH_CNTXT(n, chid);
	const uint32_t bit = ev ? 1u : 1u << chid;
	wr(g, ev ? SRC_EV_IRQ_CLR(n) : SRC_CH_IRQ_CLR(n), bit);
	wr(g, ev ? EV_CMD(n) : CH_CMD(n), op << 24 | (ev ? 0u : chid));
	uint32_t now = 0, completed = 0;
	for (unsigned t = 0; t < CMD_TIMEOUT_MS * 10u; t++) {
		now = (rd(g, cx + CX_CONFIG) >> 20) & 0xfu;
		completed = rd(g, ev ? SRC_EV_IRQ(n) : SRC_CH_IRQ(n)) & bit;
		if (completed && (op == OP_DEALLOC || now == state)) {
			break;
		}
		delay(100);
	}
	wr(g, ev ? SRC_EV_IRQ_CLR(n) : SRC_CH_IRQ_CLR(n), ev ? 1u : 1u << chid);
	if (!completed || (op != OP_DEALLOC && now != state)) {
		printf("NEZHA POWER GPI: gpii %u %s %u opcode %u: state %u, wanted %u (type irq %#x glob %#x gpii %#x log %#x)\n", n,
		    ev ? "event ring" : "channel", chid, op, now, state, rd(g, TYPE_IRQ(n)), rd(g, GLOB_IRQ_STTS(n)),
		    rd(g, GPII_IRQ_STTS(n)), rd(g, ERROR_LOG(n)));
		g->poisoned = true; /* late completion cannot qualify a later command */
		return ETIMEDOUT;
	}
	return 0;
}

static void
put_tre(NpGpi *g, bool rx, const PgpiTre *t)
{
	uint32_t *wp = rx ? &g->rx_wp : &g->tx_wp;
	memcpy((void *)(g->mem + (rx ? RX_RING : TX_RING) + *wp), t, sizeof(*t));
	*wp = (*wp + PGPI_EL_SIZE) % RING_LEN;
}

static void
doorbell(NpGpi *g, bool rx)
{
	const uint32_t base = rx ? RX_RING : TX_RING, wp = rx ? g->rx_wp : g->tx_wp;
	g->last_tre[rx ? 1 : 0] = g->mem_pa + base + (wp + RING_LEN - PGPI_EL_SIZE) % RING_LEN;
	g->completed[rx ? 1 : 0] = false;
	clean_dcache64((addr64_t)(g->mem_pa + base), RING_LEN, TRUE);
	wr(g, CH_DB(g->gpii, rx ? 1u : 0u), (uint32_t)(g->mem_pa + base + wp));
}

/* Events until one completes channel CHID's transfer (IEOT); 0, EIO on an error code, ETIMEDOUT. */
static int
wait_done(NpGpi *g, uint32_t chid, uint32_t *len)
{
	const uint32_t n = g->gpii;
	for (unsigned t = 0; t < XFER_TIMEOUT_MS * 10u; t++) {
		if (g->completed[chid]) {
			*len = g->completion_length[chid];
			const uint32_t code = g->completion_code[chid];
			const PgpiTre *last = (const PgpiTre *)(g->mem + g->last_tre[chid] - g->mem_pa);
			const bool unlock = (last->w[3] >> 16 & 0xff) == 0x31;
			return code == PGPI_CODE_SUCCESS || code == PGPI_CODE_EOT ||
			    (unlock && code == PGPI_CODE_EOB) ? 0 : EIO;
		}
		const uint32_t hw = (uint32_t)(rd(g, EV_CNTXT(n) + CX_RP_LSB) - (uint32_t)(g->mem_pa + EV_RING));
		if (hw >= EV_LEN || (hw & (PGPI_EL_SIZE - 1))) { g->poisoned = true; return EIO; }
		if (hw == g->ev_rp) {
			delay(100);
			continue;
		}
		flush_dcache64((addr64_t)(g->mem_pa + EV_RING), EV_LEN, TRUE);
		while (g->ev_rp != hw) {
			const uint32_t *raw = (const uint32_t *)(g->mem + EV_RING + g->ev_rp);
			PgpiEvent e = pgpi_event(raw);
			if (e.type == PGPI_EV_IMMEDIATE && e.chid < 2) {
				/* Immediate events carry data, not a pointer, in words0/1. */
				const uint32_t index = (raw[2] >> 8) & 0xffff;
				if (index >= RING_LEN / PGPI_EL_SIZE) { g->poisoned = true; return EIO; }
				e.tre = g->mem_pa + (e.chid ? RX_RING : TX_RING) + index * PGPI_EL_SIZE;
				e.length = raw[2] & 0xf;
			}
			if (!g->quiet || e.type != PGPI_EV_XFER_DONE || e.code == PGPI_CODE_ERROR) {
				printf("NEZHA POWER GPI: event type %#x channel %u code %u status %#x length %u tre %#llx\n", e.type,
				    e.chid, e.code, e.status, e.length, e.tre);
			}
			if (e.code == PGPI_CODE_ERROR) { g->poisoned = true; return EIO; }
			if ((e.type == PGPI_EV_XFER_DONE || e.type == PGPI_EV_IMMEDIATE) && e.chid < 2 &&
			    e.tre == g->last_tre[e.chid] && !g->completed[e.chid]) {
				/* RX data and TX unlock may arrive in either order. Retain both. */
				g->completed[e.chid] = true;
				g->completion_code[e.chid] = e.code;
				g->completion_length[e.chid] = e.length;
			}
			g->ev_rp = (g->ev_rp + PGPI_EL_SIZE) % EV_LEN;
			g->ev_wp = (g->ev_wp + PGPI_EL_SIZE) % EV_LEN;
		}
		wr(g, EV_DB(n), (uint32_t)(g->mem_pa + EV_RING + g->ev_wp));
		wr(g, IEOB_IRQ_CLR(n), 1);
	}
	printf("NEZHA POWER GPI: gpii %u channel %u: no completion in %u ms (type irq %#x glob %#x gpii %#x log %#x, "
	    "tx state %u rx state %u)\n", n, chid, XFER_TIMEOUT_MS, rd(g, TYPE_IRQ(n)), rd(g, GLOB_IRQ_STTS(n)),
	    rd(g, GPII_IRQ_STTS(n)), rd(g, ERROR_LOG(n)), (rd(g, CH_CNTXT(n, 0)) >> 20) & 0xfu,
	    (rd(g, CH_CNTXT(n, 1)) >> 20) & 0xfu);
	return ETIMEDOUT;
}

int
np_gpi_open(NpGpi *g, uint64_t block_pa, uint32_t block_len, uint32_t ee_offset, uint32_t gpii,
    uint32_t seid, uint32_t proto)
{
	if (!g || !g->authorize_dma || g->poisoned || g->owned || gpii >= 5 || seid != 5 || proto != 3 ||
	    block_pa != 0xa00000 || (g->regs && (g->gpii != gpii || g->seid != seid)) ||
	    block_len < 0x60000 || ee_offset != 0x10000) return EINVAL;
	const vm_offset_t regs = g->regs, mem = g->mem;
	const uint64_t mem_pa = g->mem_pa;
	const bool quiet = g->quiet;
	const bool deallocated = g->deallocated;
	void *dma_context = g->dma_context;
	int (*authorize_dma)(void *, uint64_t, uint32_t) = g->authorize_dma;
	bzero(g, sizeof(*g));
	g->ee_offset = ee_offset;
	g->gpii = gpii;
	g->seid = seid;
	g->proto = proto;
	g->quiet = quiet;
	g->deallocated = deallocated;
	g->dma_context = dma_context; g->authorize_dma = authorize_dma;
	g->regs = regs ? regs : ml_io_map_unmappable((vm_offset_t)block_pa, block_len, VM_WIMG_IO);
	if (!g->regs) {
		return ENOMEM;
	}
	g->mem = mem;
	g->mem_pa = mem_pa;
	const uint32_t n = gpii;
	if (!quiet) {
		printf("NEZHA POWER GPI: gpii %u before: tx %#x rx %#x event %#x, type irq %#x, masks %#x/%#x/%#x\n", n,
		    rd(g, CH_CNTXT(n, 0)), rd(g, CH_CNTXT(n, 1)), rd(g, EV_CNTXT(n)), rd(g, TYPE_IRQ(n)),
		    rd(g, TYPE_IRQ_MSK(n)), rd(g, SRC_CH_IRQ_MSK(n)), rd(g, IEOB_IRQ_MSK(n)));
	}
	if (!deallocated && (((rd(g, CH_CNTXT(n, 0)) | rd(g, CH_CNTXT(n, 1)) | rd(g, EV_CNTXT(n))) >> 20) & 0xfu)) {
		if (!quiet) {
			printf("NEZHA POWER GPI: gpii %u is in use; left alone\n", n);
		}
		return EBUSY;
	}
	g->owned = true;
	if (!g->mem) {
		if (kmem_alloc_contig(kernel_map, &g->mem, MEM_LEN, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO | KMA_PERMANENT,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			g->mem = 0;
			return ENOMEM;
		}
		g->mem_pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)g->mem));
	}
	/* Validate this allocation, not merely the existence of an SMMU context.
	 * No GPI command or doorbell has been issued yet. */
	if (!g->mem_pa || g->authorize_dma(g->dma_context, g->mem_pa, MEM_LEN)) return EACCES;
	bzero((void *)g->mem, MEM_LEN);
	clean_dcache64((addr64_t)g->mem_pa, MEM_LEN, TRUE);

	/* interrupts as gpi_config_interrupts; nothing takes them, the status is polled */
	wr(g, TYPE_IRQ_MSK(n), 0x4fu);          /* general, IEOB, global, event and channel control */
	wr(g, IEOB_IRQ_MSK(n), 1);
	wr(g, SRC_CH_IRQ_MSK(n), 3);
	wr(g, SRC_EV_IRQ_MSK(n), 1);
	wr(g, GLOB_IRQ_EN(n), 0xf);
	wr(g, GPII_IRQ_EN(n), 0xf);
	wr(g, MSI_LSB(n), 0);
	wr(g, MSI_MSB(n), 0);
	wr(g, CNTXT_SCRATCH(n, 0), 0);
	wr(g, CNTXT_SCRATCH(n, 1), 0);
	wr(g, INTSET(n), 1);
	wr(g, ERROR_LOG(n), 0);

	/* event ring (gpi_alloc_ev_chan) */
	g->event_attempted = true;
	int e = command(g, true, OP_ALLOCATE, 0, ST_ALLOCATED);
	if (e) {
		return e;
	}
	const uint64_t ev = g->mem_pa + EV_RING;
	wr(g, EV_CNTXT(n) + CX_CONFIG, PGPI_EL_SIZE << 24 | 1u << 16 | 2u);   /* IRQ interrupt type, GPI event ring */
	wr(g, EV_CNTXT(n) + CX_LENGTH, EV_LEN);
	wr(g, EV_CNTXT(n) + CX_BASE_LSB, (uint32_t)ev);
	wr(g, EV_CNTXT(n) + CX_BASE_MSB, (uint32_t)(ev >> 32));
	wr(g, EV_DB(n) + 4, (uint32_t)(ev >> 32));
	wr(g, EV_CNTXT(n) + CX_INT_MOD, 0);
	wr(g, EV_CNTXT(n) + CX_MSI_LSB, 0);
	wr(g, EV_CNTXT(n) + CX_MSI_MSB, 0);
	wr(g, EV_CNTXT(n) + CX_RPU_LSB, 0);
	wr(g, EV_CNTXT(n) + CX_RPU_MSB, 0);
	g->ev_wp = EV_LEN - PGPI_EL_SIZE;   /* every element handed to the hardware */
	wr(g, EV_DB(n), (uint32_t)(ev + g->ev_wp));

	/* channels (gpi_alloc_chan): 0 = TX (direction out), 1 = RX */
	for (uint32_t k = 0; k < 2 && !e; k++) {
		const uint64_t ring = g->mem_pa + (k ? RX_RING : TX_RING);
		g->channel_attempted[k] = true;
		e = command(g, false, OP_ALLOCATE, k, ST_ALLOCATED);
		if (e) {
			break;
		}
		wr(g, CH_CNTXT(n, k) + CX_CONFIG, PGPI_EL_SIZE << 24 | 0u << 14 | (k ? 0u : 1u) << 3 | PROTO_GPI);
		wr(g, CH_CNTXT(n, k) + CX_LENGTH, RING_LEN);
		wr(g, CH_CNTXT(n, k) + CX_BASE_LSB, (uint32_t)ring);
		wr(g, CH_CNTXT(n, k) + CX_BASE_MSB, (uint32_t)(ring >> 32));
		wr(g, CH_DB(n, k) + 4, (uint32_t)(ring >> 32));
		wr(g, CH_SCRATCH(n, k, 0), (1u - k) << 16 | (proto & 0xfu) << 4 | (seid & 7u));
		wr(g, CH_SCRATCH(n, k, 1), 0);
		wr(g, CH_SCRATCH(n, k, 2), 0);
		wr(g, CH_SCRATCH(n, k, 3), 0);
		wr(g, CH_QOS(n, k), 1);
	}
	for (uint32_t k = 0; k < 2 && !e; k++) {
		e = command(g, false, OP_START, k, ST_STARTED);
	}
	if (!quiet || e) {
		printf("NEZHA POWER GPI: gpii %u for SE %u: rings at %#llx, tx %#x rx %#x event %#x, err %d\n", n, seid,
		    g->mem_pa, rd(g, CH_CNTXT(n, 0)), rd(g, CH_CNTXT(n, 1)), rd(g, EV_CNTXT(n)), e);
	}
	g->open = e == 0;
	return e;
}

int
np_gpi_i2c(NpGpi *g, const PgpiI2cClock *clk, uint8_t addr, const uint8_t *w, unsigned nw, uint8_t *r,
    unsigned nr)
{
	if (!g || !g->open || g->poisoned || addr > 0x7f || !clk || clk->dfs_index > 15 ||
	    nw > BUF_MAX || nr > BUF_MAX || (!nw && !nr) || (nw && !w) || (nr && !r)) {
		return EINVAL;
	}
	uint32_t len = 0;
	int e = 0;
	bool config = true;
	/* Stock qcom,shared protocol: LOCK once before the combined transaction,
	 * UNLOCK with EOB after its final GO. An AP mutex alone cannot arbitrate
	 * against SOCCP. These are GPI TREs, not writes to the battery/charger. */
	PgpiTre lock = {{0, 0, 0, 0x00300001}};
	put_tre(g, false, &lock);
	g->bus_lock_pending = true;
	if (nw) {
		/* the write: config, GO (write, no STOP when a read follows), the bytes; up to 8 go in an
		 * immediate TRE as Android sends them, longer writes as DMA from the TX buffer */
		PgpiTre t = pgpi_i2c_config0(clk);
		put_tre(g, false, &t);
		t = pgpi_i2c_go(PGPI_I2C_WRITE, addr, nr != 0, 0, 0);
		put_tre(g, false, &t);
		if (nw <= 8) {
			t = pgpi_imm(w, nw);
		} else {
			memcpy((void *)(g->mem + TX_BUF), w, nw);
			clean_dcache64((addr64_t)(g->mem_pa + TX_BUF), BUF_MAX, TRUE);
			t = pgpi_dma(g->mem_pa + TX_BUF, nw);
		}
		put_tre(g, false, &t);
		doorbell(g, false);
		e = wait_done(g, 0, &len);
		if (!e && len != nw) e = EIO;
		config = false;
	}
	if (!e && nr) {
		/* the read: (config,) GO (read, linked to RX) on TX, DMA into the RX buffer on RX; RX first */
		flush_dcache64((addr64_t)(g->mem_pa + RX_BUF), BUF_MAX, TRUE);
		PgpiTre t = pgpi_dma(g->mem_pa + RX_BUF, nr);
		put_tre(g, true, &t);
		if (config) {
			t = pgpi_i2c_config0(clk);
			put_tre(g, false, &t);
		}
		t = pgpi_i2c_go(PGPI_I2C_READ, addr, 0, nr, 1);
		put_tre(g, false, &t);
		t = (PgpiTre){{0, 0, 0, 0x00310100}};
		put_tre(g, false, &t);
		doorbell(g, true);
		doorbell(g, false);
		e = wait_done(g, 1, &len);
		flush_dcache64((addr64_t)(g->mem_pa + RX_BUF), BUF_MAX, TRUE);
		if (!e && len != nr) e = EIO;
		if (!e) memcpy(r, (const void *)(g->mem + RX_BUF), nr);
		if (!e) e = wait_done(g, 0, &len);
	} else if (!e) {
		PgpiTre unlock = {{0, 0, 0, 0x00310100}};
		put_tre(g, false, &unlock); doorbell(g, false);
		e = wait_done(g, 0, &len);
	}
	if (!e) g->bus_lock_pending = false;
	else g->poisoned = true; /* an unconfirmed LOCK/UNLOCK is not retried */
	return e;
}

/* A stop timeout quarantines the permanent DMA allocation and all its mappings.
 * Never recycle it or restore clocks while the engine may still own memory. */
int
np_gpi_close(NpGpi *g)
{
    if (!g || !g->regs || !g->owned) return 0;
    if (g->poisoned) return EIO; /* no command after an ambiguous completion */
    if (!g->event_attempted) { g->open = g->owned = false; return 0; }
    const uint32_t n = g->gpii;
    for (uint32_t k = 0; k < 2; ++k) {
        if (!g->channel_attempted[k]) continue;
        uint32_t state = (rd(g, CH_CNTXT(n, k)) >> 20) & 0xf;
        if (state == ST_STARTED && command(g, false, OP_STOP, k, ST_STOPPED)) goto uncertain;
        state = (rd(g, CH_CNTXT(n, k)) >> 20) & 0xf;
        if (state != ST_NOT_ALLOC && state != ST_ALLOCATED && state != ST_STOPPED) goto uncertain;
        if (state == ST_STOPPED && command(g, false, OP_RESET, k, ST_ALLOCATED)) goto uncertain;
    }
    /* Deallocation has a completion interrupt but does not update context
     * state. Wait for that completion, not a fixed delay or a guessed state. */
    for (uint32_t k = 0; k < 2; ++k) {
        if (g->channel_attempted[k] && command(g, false, OP_DEALLOC, k, 0)) goto uncertain;
    }
    if (g->event_attempted) {
        if (command(g, true, OP_RESET, 0, ST_ALLOCATED) ||
            command(g, true, OP_DEALLOC, 0, 0)) goto uncertain;
    }
    wr(g, TYPE_IRQ_MSK(n), 0); wr(g, IEOB_IRQ_MSK(n), 0);
    wr(g, SRC_CH_IRQ_MSK(n), 0); wr(g, SRC_EV_IRQ_MSK(n), 0);
    wr(g, GLOB_IRQ_EN(n), 0); wr(g, GPII_IRQ_EN(n), 0); wr(g, INTSET(n), 0);
    if (g->bus_lock_pending) goto uncertain;
    g->open = g->owned = false;
    g->deallocated = true;
    return 0;
uncertain:
    g->open = false; g->poisoned = true;
    printf("NEZHA POWER GPI: quarantine gpii %u; stop unconfirmed\n", n);
    return EIO;
}
#endif /* KERNEL */
