/*
 * nezha (Xiaomi 17 Ultra, SM8850) touch and keys for UniXNU N4.
 *
 * Touch: ported unchanged in protocol and transport from data-a-live (ShueiTsuki, ios-pandora)
 * xnu/patches 0130-0135 bsd/dev/pandora_touch.c, proven on the Xiaomi 17 Pro with the same
 * controller, SPI engine, GPI DMA, RPMh LDO and pins. nezha differences (evidence/n0 live-props):
 * avdd enable is TLMM GPIO 215 (touch_avdd_vreg gpio <&tlmm 0xd7>), panel 1200 x 2608.
 * Contacts go to NativeHidInput.cpp (nezha_touch_next) instead of SweetHIDTouch, and to the
 * boot-progress touch row/pad (native_touch_*). Boot-arg names: pandora_* -> nezha_*.
 * Keys: data-a-live 0128/0130/0132 bsd/dev/fuxi_keys.c, PANDORA path (SPMI arbiter v8 PON,
 * TLMM GPIO 101), sent as HID report 2 bits (nezha_keys_next). Everything fails soft: a
 * failed step prints, records the stage on the boot-progress row and stops its thread.
 *
 * Original pandora description follows.
 *
 * PANDORA (Xiaomi 17 Pro, SM8850) main screen touch: Synaptics S3910P speaking
 * TouchComm (v1 or v2, detected) over SPI on QUPv3 wrapper 4 SE0 (0x01a80000),
 * polled through GPI DMA. Hardware from the live DT (device/DT_FINDINGS.md):
 *   IRQ    TLMM GPIO 158, active low        reset  GPIO 66, active low
 *   avdd   fixed regulator, GPIO 117 high   iovdd  RPMh LDO "L4I_E0" 1.8 V (pmh0110_i_l4)
 *   SPI    GPIO 48 MISO, 49 MOSI, 50 CLK, 51 CS, function 1 (qup4_se0), 6 mA, pull-down
 * TLMM pins are 0x1000 apart from 0x0f100000: the downstream pinctrl-canoe.ko adds
 * 0x100000 to the DT base 0x0f000000 (its pin table holds 0x100000 + 0x1000 * n), and
 * 0x0f0xxxxx hangs the bus. GCC offsets from gcc-kaanapali.c (wrapper 4: vote 0x52018
 * bits 22-26, SE0 RCG 0xa9014); GENI and SPI registers as Linux qcom-geni-se.c and
 * spi-geni-qcom.c; TouchComm as Synaptics' syna_c10 driver (tcm sources).
 *
 * Boot-arg pandora_touch (bit mask, default 0xf; 0 = off):
 *   bit 0  probe: print GCC, SE and pin state (votes the wrapper 4 clocks on)
 *   bit 1  vote L4I_E0 on (1.8 V, high power mode) through RPMh, apps RSC DRV 2
 *   bit 2  run: SPI set-up, avdd, reset, TouchComm identify, application info, touch
 *          report config, then report touches to SweetHIDTouch
 *   bit 3  also keep the firmware's THP frames (report 0xc0: 64-byte header, then the
 *          16 x 35 signed 16-bit capacitance deltas) and find fingers in them here, for
 *          a firmware that leaves touch positions to the host. Its own touch reports
 *          (0x11) win when they come. pandora_thp_thr (default 200) is the finger
 *          threshold, pandora_thp_swap=1 reads the deltas as 35 rows of 16,
 *          pandora_thp_flip bit 0 / bit 1 mirror x / y.
 * pandora_steps=MS: before the first access to each new register region print its
 * name and wait MS (default 0), so a bus hang leaves the name in the log file.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>
#include <vm/pmap.h>
#include <arm/caches_internal.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern kern_return_t thread_terminate(thread_t);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation,
    void *parameter, integer_t priority, thread_t *new_thread);
#include "NezhaInput.h"
#include "NezhaInputTelemetry.h"
static void pandora_touch_init(void);
static void nz_stage(int stage, int err);
static char nz_part[17];             /* identify part name, for the boot-progress row */

#define PT_GCC_PA     0x00100000u
#define PT_SE_PA      0x01a80000u
#define PT_TLMM_PA    0x0f100000u
#define PT_RSC_PA     0x18920000u      /* apps RSC "drv-2" */
#define PT_CMDDB_PA   0x81c60000u      /* reserved-memory aop_cmd_db_region */
#define PT_CMDDB_SIZE 0x20000u
#define PT_GPIO(n)    ((uint32_t)(n) * 0x1000u)
#define PT_IRQ        158u
#define PT_RESET      66u
#define PT_AVDD       215u            /* nezha: touch_avdd_vreg (pandora: 117) */
#define PT_MAX        10u
#define PT_PRIORITY   92               /* BASEPRI_PREEMPT, as fuxi_touch */

/* GCC */
#define GCC_VOTE      0x52018u
#define GCC_VOTE_BITS (0x1fu << 22)    /* m_ahb, s_ahb, core, core2x, s0 */
#define GCC_M_AHB     0x23548u
#define GCC_S_AHB     0x2354cu
#define GCC_CORE      0x23550u
#define GCC_CORE2X    0x23564u
#define GCC_S0        0xa9004u
#define GCC_S0_RCG    0xa9014u         /* CMD; CFG +4, DFSR +0x14, PERF_DFSR(l) +0x1c+4l */

/* GENI serial engine */
#define SE_FORCE_DEFAULT 0x020u
#define SE_OUTPUT_CTRL   0x024u
#define SE_CGC_CTRL      0x028u
#define SE_STATUS        0x040u
#define SE_M_CLK_CFG     0x048u
#define SE_IF_DISABLE    0x064u
#define SE_FW_REV        0x068u
#define SE_CLK_SEL       0x07cu
#define SE_SPI_CPHA      0x224u
#define SE_SPI_LOOPBACK  0x22cu
#define SE_SPI_CPOL      0x230u
#define SE_SPI_DEMUX_INV 0x24cu
#define SE_SPI_DEMUX_SEL 0x250u
#define SE_BYTE_GRAN     0x254u
#define SE_DMA_MODE      0x258u
#define SE_TX_PACK0      0x260u
#define SE_TX_PACK1      0x264u
#define SE_SPI_WORD_LEN  0x268u
#define SE_SPI_TX_LEN    0x26cu
#define SE_SPI_RX_LEN    0x270u
#define SE_RX_PACK0      0x284u
#define SE_RX_PACK1      0x288u
#define SE_M_CMD0        0x600u
#define SE_M_CMD_CTRL    0x604u
#define SE_M_IRQ         0x610u
#define SE_M_IRQ_EN      0x614u
#define SE_M_IRQ_CLR     0x618u
#define SE_S_IRQ_CLR     0x648u
#define SE_TX_FIFO       0x700u
#define SE_RX_FIFO       0x780u
#define SE_TX_STATUS     0x800u
#define SE_RX_STATUS     0x804u
#define SE_TX_WM         0x80cu
#define SE_RX_WM         0x810u
#define SE_RX_RFR_WM     0x814u
#define SE_DMA_TX_CLR    0xc44u
#define SE_DMA_RX_CLR    0xd44u
#define SE_GSI_EVENT_EN  0xe18u
#define SE_IRQ_EN        0xe1cu
#define SE_HW_PARAM0     0xe24u
#define SE_HW_PARAM1     0xe28u
#define PT_SPI_DIV       2u            /* 19.2 MHz XO / 2 = 9.6 MHz (IC allows 15 MHz) */
#define PT_XFER_MAX      4096u        /* DMA TX 0x2000-0x2fff, RX 0x3000-0x3fff */

/* TouchComm */
#define TC_MARKER        0xa5u
#define TC_PAD           0x5au
#define TC_CONTINUED     0x03u
#define TC_OK            0x01u
#define TC_NO_REPORT     0x04u
#define TC_ACK           0x07u
#define TC_IDENTIFY      0x10u
#define TC_TOUCH         0x11u
#define CMD_IDENTIFY     0x02u
#define CMD_ENABLE_REPORT 0x05u
#define CMD_DISABLE_REPORT 0x06u
#define TC_THP_FRAME     0xc0u        /* Xiaomi THP raw frame, streamed after reset */
#define CMD_V2_ACK       0x07u
#define CMD_V2_RETRY     0x08u
#define CMD_V2_GET_REPORT 0x0au
#define CMD_APP_INFO     0x20u
#define CMD_GET_TOUCH_CONFIG 0x25u
#define PT_MSG_MAX       4000u

static struct {
	vm_offset_t gcc, se, tlmm;
	bool started, dead;
	unsigned steps_ms, level, version;     /* version: TouchComm 1 or 2 */
	bool crc;                              /* v1: CRC-16 after the payload */
	unsigned seq;                          /* v2: header sequence bit */
	unsigned max_rd, max_wr;
	unsigned max_x, max_y, max_objects;
	bool fill;
	bool thp;                              /* bit 3: fingers from THP frames too */
	unsigned rows, cols;                   /* THP image: 16 x 35 */
	unsigned thp_thr, thp_swap, thp_flip, thp_logs;
	uint64_t last_ic_touch;                /* last 0x11 report with a finger */
	uint8_t msg[PT_MSG_MAX];
	unsigned msg_len;
	uint8_t config[256];
	unsigned config_len;
	uint32_t active;
	int x[PT_MAX], y[PT_MAX];               /* last delivered, presented display */
	int px[PT_MAX], py[PT_MAX];             /* the same in panel pixels */
	uint32_t codes_seen[8];                /* bitmap of report codes already logged */
	unsigned reports, touch_reports, superres;
} pt;
static uint8_t pt_buf[PT_XFER_MAX + 8];
static int pt_chan;

static uint32_t
pt_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
pt_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
pt_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&pt_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
pt_sleep_until(uint64_t deadline)
{
	(void)assert_wait_deadline_with_leeway(&pt_chan, THREAD_UNINT,
	    TIMEOUT_URGENCY_SYS_CRITICAL, deadline, 0);
	thread_block(THREAD_CONTINUE_NULL);
}

static const char *volatile nz_step = "-";
static void nz_row(void);
static void
pt_step(const char *what, uint32_t pa)
{
	nz_step = what;
	nz_row();
	if (pt.steps_ms) {
		printf("NEZHA TOUCH: next: first access to %s at %#x\n", what, pa);
		pt_sleep(pt.steps_ms);
	}
}

static bool
pt_wait_clear(vm_offset_t base, uint32_t off, uint32_t mask, unsigned us)
{
	for (unsigned i = 0; i < us; i++) {
		if (!(pt_rd(base, off) & mask)) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* ---- pins ---------------------------------------------------------------------- */

/* TLMM CTL: pull [1:0] (1 down, 3 up), func [5:2], drive [8:6] = mA/2-1, OE [9],
 * EGPIO_PRESENT [11], EGPIO_ENABLE [12] (set: the pin stays with the TLMM). */
static void
pt_gpio(unsigned n, unsigned func, unsigned pull, unsigned ma, int out)
{
	if (out >= 0) {
		pt_wr(pt.tlmm, PT_GPIO(n) + 4, out ? 2u : 0u);
	}
	uint32_t ctl = pt_rd(pt.tlmm, PT_GPIO(n)) & ~0x3ffu;
	ctl |= pull | (func << 2) | ((ma / 2 - 1) << 6) | (out >= 0 ? 0x200u : 0u);
	if (ctl & (1u << 11)) {
		ctl |= 1u << 12;
	}
	pt_wr(pt.tlmm, PT_GPIO(n), ctl);
}

static void __attribute__((unused))
pt_pins_dump(const char *when)
{
	static const uint8_t pins[] = {48, 49, 50, 51, PT_RESET, PT_AVDD, PT_IRQ};
	printf("NEZHA TOUCH: pins %s:", when);
	for (unsigned i = 0; i < sizeof(pins); i++) {
		printf(" %u=%#x/%u", pins[i], pt_rd(pt.tlmm, PT_GPIO(pins[i])),
		    pt_rd(pt.tlmm, PT_GPIO(pins[i]) + 4) & 3u);
	}
	printf("\n");
}

/* ---- clocks and serial engine -------------------------------------------------- */

static bool
pt_on(uint32_t cbcr)
{
	return !(pt_rd(pt.gcc, cbcr) & 0x80000000u);
}

static void
pt_clock_dump(const char *when)
{
	uint32_t r = GCC_S0_RCG;
	printf("NEZHA TOUCH: gcc %s: vote=%#x m_ahb=%#x s_ahb=%#x core=%#x core2x=%#x s0=%#x "
	    "rcg=%#x cfg=%#x dfs=%#x perf0..3=%#x %#x %#x %#x\n", when,
	    pt_rd(pt.gcc, GCC_VOTE), pt_rd(pt.gcc, GCC_M_AHB), pt_rd(pt.gcc, GCC_S_AHB),
	    pt_rd(pt.gcc, GCC_CORE), pt_rd(pt.gcc, GCC_CORE2X), pt_rd(pt.gcc, GCC_S0),
	    pt_rd(pt.gcc, r), pt_rd(pt.gcc, r + 4), pt_rd(pt.gcc, r + 0x14),
	    pt_rd(pt.gcc, r + 0x1c), pt_rd(pt.gcc, r + 0x20), pt_rd(pt.gcc, r + 0x24),
	    pt_rd(pt.gcc, r + 0x28));
}

static int
pt_clocks(void)
{
	pt_wr(pt.gcc, GCC_VOTE, pt_rd(pt.gcc, GCC_VOTE) | GCC_VOTE_BITS);
	static const uint32_t branches[] = {GCC_M_AHB, GCC_S_AHB, GCC_CORE, GCC_CORE2X};
	for (unsigned i = 0; i < 4; i++) {
		for (unsigned t = 0; t < 10000 && !pt_on(branches[i]); t++) {
			delay(1);
		}
		if (!pt_on(branches[i])) {
			pt_clock_dump("branch off");
			return EIO;
		}
	}
	/* SE0 source: XO undivided (19.2 MHz). With DFS the SE picks a performance level
	 * (SE_CLK_SEL); without, set the RCG to XO/1 (no PLL or voltage change). */
	uint32_t r = GCC_S0_RCG;
	unsigned level = 0;
	if (pt_rd(pt.gcc, r + 0x14) & 1u) {
		while (level < 8 && (pt_rd(pt.gcc, r + 0x1c + 4 * level) & 0x371fu) != 1u) {
			level++;
		}
		if (level == 8) {
			pt_clock_dump("no XO DFS level");
			return EIO;
		}
	} else {
		uint32_t cfg = (pt_rd(pt.gcc, r + 4) & ~(0x371fu | (1u << 20))) | 1u;
		pt_wr(pt.gcc, r + 4, cfg);
		pt_wr(pt.gcc, r, pt_rd(pt.gcc, r) | 1u);
		if (!pt_wait_clear(pt.gcc, r, 1u, 5000)) {
			pt_clock_dump("RCG update timeout");
			return EIO;
		}
	}
	pt.level = level;
	return 0;
}

static void __attribute__((unused))
pt_se_dump(const char *when)
{
	printf("NEZHA TOUCH: se %s: fw=%#x if_disable=%#x status=%#x hw0=%#x hw1=%#x "
	    "clk_sel=%#x m_clk=%#x dma=%#x gsi_ev=%#x irq_en=%#x m_irq=%#x m_irq_en=%#x "
	    "word=%#x tx=%#x rx=%#x\n", when,
	    pt_rd(pt.se, SE_FW_REV), pt_rd(pt.se, SE_IF_DISABLE), pt_rd(pt.se, SE_STATUS),
	    pt_rd(pt.se, SE_HW_PARAM0), pt_rd(pt.se, SE_HW_PARAM1), pt_rd(pt.se, SE_CLK_SEL),
	    pt_rd(pt.se, SE_M_CLK_CFG), pt_rd(pt.se, SE_DMA_MODE), pt_rd(pt.se, SE_GSI_EVENT_EN),
	    pt_rd(pt.se, SE_IRQ_EN), pt_rd(pt.se, SE_M_IRQ), pt_rd(pt.se, SE_M_IRQ_EN),
	    pt_rd(pt.se, SE_SPI_WORD_LEN), pt_rd(pt.se, SE_TX_STATUS), pt_rd(pt.se, SE_RX_STATUS));
}

/*
 * SE0's firmware has the FIFO interface disabled (GENI_IF_DISABLE_RO bit 0): transfers go
 * through the wrapper 4 GPI DMA engine (qcom,gpi-dma@1a00000, ee-offset 0x10000), as
 * fuxi_touch.c does on the Xiaomi 13. GPII 1 (first of the DT's HLOS gpii-mask 0x1e):
 * TX channel 0, RX channel 1, one event ring. Per-GPII window = ee base (0x1a00000 -
 * 0x10000) + 0x20000 + 0x4000 * n: channel contexts +0, event context +0x1000, channel
 * doorbells +0x2000, event doorbell +0x2100, GPII registers +0x3000 (Linux qcom gpi.c).
 * The GPI (stream 0x536) and QUP (0x523) need apps SMMU stream matches: each goes to the
 * context bank that UFS's stream 0x60 already uses (stage 1 off, sweet_ufs.c).
 */
#define PT_GPII       1u
#define PT_GPI_PA     (0x01a00000u - 0x10000u + 0x20000u + 0x4000u * PT_GPII)
#define PT_SMMU_PA    0x15000000u
#define PT_DMA_SIZE   0x4000u
#define FT_EV_OFF     0x0000u
#define FT_RING_OFF(k) (0x1000u + 0x400u * (k))
#define FT_TXB_OFF    0x2000u          /* up to 0x800 */
#define FT_RXB_OFF    0x3000u          /* up to 0x1000 */
#define FT_RING       64u
#define FT_EVRING     256u
#define GPI_CH(k)     (0x0000u + 0x80u * (k))
#define GPI_EV        0x1000u
#define GPI_CHDB(k)   (0x2000u + 8u * (k))
#define GPI_EVDB      0x2100u
#define GPI_GPII      0x3000u

static vm_offset_t pt_gpi, pt_smmu, pt_dma;
static uint64_t pt_dma_pa;
static unsigned pt_wp[2], pt_evrp;

static void
pt_dma_sync(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(pt_dma_pa + off), len, TRUE);
}

static uint32_t
pt_gstate(uint32_t ctx)
{
	return (pt_rd(pt_gpi, ctx) >> 20) & 0xf;
}

static bool
pt_gpi_cmd(uint32_t reg, uint32_t v, uint32_t ctx, uint32_t want)
{
	pt_wr(pt_gpi, GPI_GPII + reg, v);
	for (unsigned i = 0; i < 50000; i++) {
		uint32_t st = pt_gstate(ctx);
		if (st == want) {
			pt_wr(pt_gpi, GPI_GPII + 0xa0, pt_rd(pt_gpi, GPI_GPII + 0x90));
			pt_wr(pt_gpi, GPI_GPII + 0xa4, pt_rd(pt_gpi, GPI_GPII + 0x94));
			return true;
		}
		if (st == 0xf) {
			break;
		}
		delay(1);
	}
	printf("NEZHA TOUCH: GPI cmd %#x -> state %#x (want %u), error log %#x\n",
	    v, pt_gstate(ctx), want, pt_rd(pt_gpi, GPI_GPII + 0x200));
	return false;
}

/* Reuse the proven17 GPI/QUP transport, but verify the firmware bank before
 * copying its routes. Geometry follows QcomUsbDmaDomain (ID1 pages/stride).
 * Never alter a context bank, UFS route, global translation or USB lease. */
static int
pt_smmu_setup(void)
{
 static const uint16_t sid[] = {0x536, 0x523};
 uint32_t id1=pt_rd(pt_smmu,0x24),nsmr=pt_rd(pt_smmu,0x20)&255;
 unsigned banks=id1&255,shift=(id1>>31)?16:12,pages=2u<<((id1>>28)&7);
 unsigned source=~0u,slot[2]={~0u,~0u},need=0;
 uint32_t route=0,source_smr=0,old_route[2]={0,0};
 if(!nsmr||nsmr>128||!banks||banks>128)return ENODEV;
 for(unsigned i=0;i<nsmr;i++) {
  uint32_t smr=pt_rd(pt_smmu,0x800+4*i);
  if((smr&0x80000000u)&&(((0x60u^smr)&~(smr>>16)&0x7fff)==0)) {
   if(source!=~0u)return EBUSY;
   source=i;source_smr=smr;route=pt_rd(pt_smmu,0xc00+4*i);
  }
 }
 unsigned bank=route&255;
 uint64_t cb=((uint64_t)pages+bank)<<shift;
 if(source==~0u||((route>>16)&3)!=0||bank>=banks||cb+4>0x100000)return ENODEV;
 uint32_t sctlr=pt_rd(pt_smmu,(uint32_t)cb);
 if(sctlr&1){printf("NEZHA TOUCH: firmware bank %u translated; stop, no DMA route changed\n",bank);return EBUSY;}
 for(unsigned j=0;j<2;j++) {
  for(unsigned i=0;i<nsmr;i++) {
   uint32_t smr=pt_rd(pt_smmu,0x800+4*i);
   if((smr&0x80000000u)&&(((sid[j]^smr)&~(smr>>16)&0x7fff)==0)) {
    if(slot[j]!=~0u||pt_rd(pt_smmu,0xc00+4*i)!=route)return EBUSY;
    slot[j]=i;
   }
  }
  if(slot[j]==~0u)need|=1u<<j;
 }
 /* Reserve both new exact matches before the first register write. */
 for(unsigned j=0;j<2;j++)if(need&(1u<<j)) {
  for(unsigned i=0;i<nsmr;i++)if(!(pt_rd(pt_smmu,0x800+4*i)&0x80000000u)&&i!=slot[1-j]){slot[j]=i;break;}
  if(slot[j]==~0u)return ENOSPC;
  old_route[j]=pt_rd(pt_smmu,0xc00+4*slot[j]);
 }
 if(pt_rd(pt_smmu,0x800+4*source)!=source_smr||pt_rd(pt_smmu,0xc00+4*source)!=route||pt_rd(pt_smmu,(uint32_t)cb)!=sctlr)return EBUSY;
 for(unsigned j=0;j<2;j++)if(need&(1u<<j)) {
  if(pt_rd(pt_smmu,0x800+4*slot[j])&0x80000000u)return EBUSY;
  pt_wr(pt_smmu,0xc00+4*slot[j],route);
  pt_wr(pt_smmu,0x800+4*slot[j],0x80000000u|sid[j]);
  if(pt_rd(pt_smmu,0xc00+4*slot[j])!=route||pt_rd(pt_smmu,0x800+4*slot[j])!=(0x80000000u|sid[j])) {
   /* No GPI DMA started yet. Revert only our still-identical exact entries. */
   for(unsigned k=0;k<=j;k++)if((need&(1u<<k))&&pt_rd(pt_smmu,0x800+4*slot[k])==(0x80000000u|sid[k])&&pt_rd(pt_smmu,0xc00+4*slot[k])==route) {
    pt_wr(pt_smmu,0x800+4*slot[k],0);pt_wr(pt_smmu,0xc00+4*slot[k],old_route[k]);
   }
   return EIO;
  }
 }
 printf("NEZHA TOUCH: verified firmware bank %u SCTLR %#x;GPI/QUP routes %u/%u only;USB bank untouched\n",bank,sctlr,slot[0],slot[1]);
 return 0;
}

static int
pt_gpi_init(void)
{
	uint32_t lo = (uint32_t)pt_dma_pa, hi = (uint32_t)(pt_dma_pa >> 32);
	for (unsigned k = 0; k < 2; k++) {
		if (pt_gstate(GPI_CH(k)) == 2) {
			(void)pt_gpi_cmd(0x08, (2u << 24) | k, GPI_CH(k), 3);
		}
		if (pt_gstate(GPI_CH(k))) {
			(void)pt_gpi_cmd(0x08, (9u << 24) | k, GPI_CH(k), 1);
			pt_wr(pt_gpi, GPI_GPII + 0x08, (10u << 24) | k);
			delay(1000);
		}
	}
	if (pt_gstate(GPI_EV)) {
		(void)pt_gpi_cmd(0x10, 9u << 24, GPI_EV, 1);
		pt_wr(pt_gpi, GPI_GPII + 0x10, 10u << 24);
		delay(1000);
	}
	bzero((void *)pt_dma, PT_DMA_SIZE);
	pt_dma_sync(0, PT_DMA_SIZE);
	pt_wr(pt_gpi, GPI_GPII + 0x88, 0x4f);
	pt_wr(pt_gpi, GPI_GPII + 0xb8, 1);
	pt_wr(pt_gpi, GPI_GPII + 0x98, 3);
	pt_wr(pt_gpi, GPI_GPII + 0x9c, 1);
	pt_wr(pt_gpi, GPI_GPII + 0x108, 1);
	pt_wr(pt_gpi, GPI_GPII + 0x120, 0xf);
	pt_wr(pt_gpi, GPI_GPII + 0x188, 0);
	pt_wr(pt_gpi, GPI_GPII + 0x18c, 0);
	pt_wr(pt_gpi, GPI_GPII + 0x400, 0);
	pt_wr(pt_gpi, GPI_GPII + 0x404, 0);
	pt_wr(pt_gpi, GPI_GPII + 0x180, 1);
	pt_wr(pt_gpi, GPI_GPII + 0x200, 0);
	if (!pt_gpi_cmd(0x10, 0, GPI_EV, 1)) {
		return EIO;
	}
	pt_wr(pt_gpi, GPI_EV + 0x00, 0x10010002u);
	pt_wr(pt_gpi, GPI_EV + 0x04, FT_EVRING * 16);
	pt_wr(pt_gpi, GPI_EV + 0x08, lo + FT_EV_OFF);
	pt_wr(pt_gpi, GPI_EV + 0x0c, hi);
	pt_wr(pt_gpi, GPI_EVDB + 4, hi);
	for (uint32_t o = 0x20; o <= 0x34; o += 4) {
		pt_wr(pt_gpi, GPI_EV + o, 0);
	}
	pt_evrp = 0;
	pt_wr(pt_gpi, GPI_EVDB, lo + FT_EV_OFF + FT_EVRING * 16 - 16);
	for (unsigned k = 0; k < 2; k++) {
		if (!pt_gpi_cmd(0x08, k, GPI_CH(k), 1)) {
			return EIO;
		}
		pt_wr(pt_gpi, GPI_CH(k) + 0x00, k ? 0x10000002u : 0x1000000au);
		pt_wr(pt_gpi, GPI_CH(k) + 0x04, FT_RING * 16);
		pt_wr(pt_gpi, GPI_CH(k) + 0x08, lo + FT_RING_OFF(k));
		pt_wr(pt_gpi, GPI_CH(k) + 0x0c, hi);
		pt_wr(pt_gpi, GPI_CHDB(k) + 4, hi);
		/* SCRATCH_0: pair channel, protocol 1 (SPI), SE 0. */
		pt_wr(pt_gpi, GPI_CH(k) + 0x60, k ? 0x00000010u : 0x00010010u);
		pt_wr(pt_gpi, GPI_CH(k) + 0x64, 0);
		pt_wr(pt_gpi, GPI_CH(k) + 0x68, 0);
		pt_wr(pt_gpi, GPI_CH(k) + 0x6c, 0);
		pt_wr(pt_gpi, GPI_CH(k) + 0x5c, 1);
		pt_wp[k] = 0;
	}
	for (unsigned k = 0; k < 2; k++) {
		if (!pt_gpi_cmd(0x08, (1u << 24) | k, GPI_CH(k), 2)) {
			return EIO;
		}
	}
	return 0;
}

/* geni_se_select_mode(GENI_GPI_DMA); the CONFIG0 TRE sets word size, packing and clock. */
static int
pt_se_init(void)
{
	uint32_t fw = pt_rd(pt.se, SE_FW_REV);
	if (((fw >> 8) & 0xff) != 1) {
		printf("NEZHA TOUCH: SE0 firmware %#x is not SPI\n", fw);
		return ENODEV;
	}
	if (pt_rd(pt.se, SE_STATUS) & 1u) {
		printf("NEZHA TOUCH: SE0 busy (status %#x)\n", pt_rd(pt.se, SE_STATUS));
		return EBUSY;
	}
	pt_wr(pt.se, SE_GSI_EVENT_EN, 0);
	pt_wr(pt.se, SE_M_IRQ_CLR, 0xffffffffu);
	pt_wr(pt.se, SE_S_IRQ_CLR, 0xffffffffu);
	pt_wr(pt.se, SE_DMA_TX_CLR, 0xffffffffu);
	pt_wr(pt.se, SE_DMA_RX_CLR, 0xffffffffu);
	pt_wr(pt.se, SE_CGC_CTRL, pt_rd(pt.se, SE_CGC_CTRL) | 0x7fu);
	pt_wr(pt.se, 0xe30, pt_rd(pt.se, 0xe30) | 0xfu);
	pt_wr(pt.se, SE_OUTPUT_CTRL, 0x7fu);
	pt_wr(pt.se, SE_FORCE_DEFAULT, 1);
	pt_wr(pt.se, SE_IRQ_EN, 0);
	pt_wr(pt.se, SE_M_IRQ_EN, pt_rd(pt.se, SE_M_IRQ_EN) &
	    ~((1u << 0) | (1u << 26) | (1u << 27) | (1u << 30)));
	pt_wr(pt.se, 0x644, pt_rd(pt.se, 0x644) & ~1u);
	pt_wr(pt.se, SE_DMA_MODE, 1);
	pt_wr(pt.se, SE_GSI_EVENT_EN, pt_rd(pt.se, SE_GSI_EVENT_EN) | 0xfu);
	pt_step("apps SMMU", PT_SMMU_PA);
	pt_smmu = ml_io_map(PT_SMMU_PA, 0x100000);
	pt_gpi = ml_io_map(PT_GPI_PA, 0x4000);
	if (!pt_smmu || !pt_gpi) {
		return ENOMEM;
	}
	int err;
	if ((err = pt_smmu_setup())) {
		return err;
	}
	if (kmem_alloc_contig(kernel_map, &pt_dma, PT_DMA_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("NEZHA TOUCH: no DMA memory\n");
		return ENOMEM;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)pt_dma);
	if (!pn) {
		return ENOMEM;
	}
	pt_dma_pa = ptoa_64(pn);
	if (pt_dma_pa + PT_DMA_SIZE > (1ULL << 36)) {
		printf("NEZHA TOUCH: DMA at %#llx is outside the SMMU's 36-bit range\n", pt_dma_pa);
		return ENOMEM;
	}
	pt_step("GPII 1 registers", PT_GPI_PA);
	printf("NEZHA TOUCH: GPII %u before: ch0 %#x ch1 %#x ev %#x\n", PT_GPII,
	    pt_rd(pt_gpi, GPI_CH(0)), pt_rd(pt_gpi, GPI_CH(1)), pt_rd(pt_gpi, GPI_EV));
	if ((err = pt_gpi_init())) {
		return err;
	}
	printf("NEZHA TOUCH: SE0 SPI over GPII %u, DMA page %#llx, DFS level %u, %u kHz\n",
	    PT_GPII, pt_dma_pa, pt.level, 19200u / PT_SPI_DIV);
	return 0;
}

static void
pt_tre(unsigned k, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
	uint32_t off = FT_RING_OFF(k) + 16 * pt_wp[k];
	volatile uint32_t *t = (volatile uint32_t *)(pt_dma + off);
	t[0] = d0;
	t[1] = d1;
	t[2] = d2;
	t[3] = d3;
	pt_dma_sync(off, 16);
	pt_wp[k] = (pt_wp[k] + 1) % FT_RING;
}

static void
pt_ring_db(unsigned k)
{
	__asm__ volatile("dsb sy" ::: "memory");
	pt_wr(pt_gpi, GPI_CHDB(k), (uint32_t)pt_dma_pa + FT_RING_OFF(k) + 16 * pt_wp[k]);
}

/* Drain events until end-of-transfer (code 2) has come from every channel in want. */
static int
pt_gpi_wait(unsigned want)
{
	unsigned seen = 0;
	for (unsigned us = 0; us < 50000; us++) {
		uint32_t hw = pt_rd(pt_gpi, GPI_EV + 0x10);
		bool bad = false;
		while (hw != (uint32_t)pt_dma_pa + FT_EV_OFF + 16 * pt_evrp) {
			uint32_t off = FT_EV_OFF + 16 * pt_evrp;
			pt_dma_sync(off, 16);
			volatile uint32_t *e = (volatile uint32_t *)(pt_dma + off);
			uint32_t d2 = e[2], d3 = e[3];
			pt_evrp = (pt_evrp + 1) % FT_EVRING;
			unsigned code = d2 >> 24, type = (d3 >> 16) & 0xff, ch = d3 >> 24;
			if (code == 16 || type == 0xff || ch >= 2) {
				bad = true;
				printf("NEZHA TOUCH: GPI event %#x %#x\n", d2, d3);
			} else if (type == 0x22 && code == 2) {
				seen |= 1u << ch;
			}
		}
		pt_wr(pt_gpi, GPI_EVDB, (uint32_t)pt_dma_pa + FT_EV_OFF +
		    16 * ((pt_evrp + FT_EVRING - 1) % FT_EVRING));
		pt_wr(pt_gpi, GPI_GPII + 0xc0, 1);
		if (bad) {
			return EIO;
		}
		if ((seen & want) == want) {
			return 0;
		}
		delay(1);
	}
	return ETIMEDOUT;
}

/* One full-duplex transfer with chip select held for its length. tx NULL sends 0xff
 * (TouchComm reads), rx NULL drops what comes back. */
static int
pt_spi(const uint8_t *tx, uint8_t *rx, unsigned n)
{
	if (pt.dead || n == 0 || n > PT_XFER_MAX) {
		return EIO;
	}
	uint8_t *txb = (uint8_t *)(pt_dma + FT_TXB_OFF), *rxb = (uint8_t *)(pt_dma + FT_RXB_OFF);
	if (tx) {
		memcpy(txb, tx, n);
	} else {
		memset(txb, 0xff, n);
	}
	pt_dma_sync(FT_TXB_OFF, n);
	pt_dma_sync(FT_RXB_OFF, n);
	uint32_t hi = (uint32_t)(pt_dma_pa >> 32);
	pt_tre(1, (uint32_t)pt_dma_pa + FT_RXB_OFF, hi, n, 0x00100200u);
	/* CONFIG0: 8-bit words (8 - 4), TX/RX packing, mode 0; clock level and divider. */
	pt_tre(0, 0x03000004u, 0, (pt.level << 16) | PT_SPI_DIV, 0x00220001u);
	/* GO: full duplex (3), RX length; chain + link. */
	pt_tre(0, 3u, 0, n, 0x00200801u);
	pt_tre(0, (uint32_t)pt_dma_pa + FT_TXB_OFF, hi, n, 0x00100200u);
	pt_ring_db(1);
	pt_ring_db(0);
	int err = pt_gpi_wait(3u);
	if (err) {
		static unsigned fails;
		if (fails++ < 10) {
			printf("NEZHA TOUCH: SPI over GPI failed %d (n %u), ch0 %#x ch1 %#x err log %#x\n",
			    err, n, pt_rd(pt_gpi, GPI_CH(0)), pt_rd(pt_gpi, GPI_CH(1)),
			    pt_rd(pt_gpi, GPI_GPII + 0x200));
		}
		if (pt_gpi_init()) {
			pt.dead = true;
		}
		return err;
	}
	pt_dma_sync(FT_RXB_OFF, n);
	if (rx) {
		memcpy(rx, rxb, n);
	}
	return 0;
}

/* ---- RPMh: L4I_E0 on ----------------------------------------------------------- */

/* Register layout per RSC major version (Linux rpmh-rsc.c ver_2_7 / ver_3_0). */
struct rh_layout {
	uint32_t wait, control, status, enable, msgid, addr, data, cmd_status, cmd_stride;
};
static const struct rh_layout rh_v2 = {0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c, 20};
static const struct rh_layout rh_v3 = {0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c, 0x40, 24};
#define RH_TCS_OFF     0xd00u          /* DT drv@2 qcom,tcs-offset */
#define RH_TCS_STRIDE  0x2a0u          /* qcom,tcs-distance */
#define RH_ACTIVE_N    3u              /* qcom,tcs-config: ACTIVE 3 first */
#define RH_DRV         2u
#define RH_WRITE_ACK   (8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC    0x0c0330dbu
#define RH_DB_HEADER   144u
#define RH_DB_ENTRY    24u

static vm_offset_t rh_drv, rh_db;

static uint16_t
rh_db16(uint32_t off)
{
	return (uint16_t)(pt_rd(rh_db, off & ~3u) >> (8u * (off & 3u)));
}

static int
rh_lookup(const char *res, uint32_t *address)
{
	uint32_t key[2] = {0, 0};
	for (unsigned i = 0; i < 8 && res[i]; i++) {
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	}
	unsigned matches = 0;
	for (unsigned i = 0; i < 8; i++) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave) {
			break;
		}
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		unsigned count = rh_db16(h + 6);
		if ((entries & 3u) || entries + count * RH_DB_ENTRY > PT_CMDDB_SIZE) {
			printf("NEZHA TOUCH: cmd-db slave header %u malformed\n", i);
			return EINVAL;
		}
		for (unsigned j = 0; j < count; j++) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (pt_rd(rh_db, e) != key[0] || pt_rd(rh_db, e + 4) != key[1]) {
				continue;
			}
			uint32_t addr = pt_rd(rh_db, e + 16);
			printf("NEZHA TOUCH: cmd-db %s addr=%#x slave=%u\n", res, addr, slave);
			if (slave == 4 && ((addr >> 16) & 7u) == 4 && !(addr & 15u)) {
				*address = addr;
				matches++;
			}
		}
	}
	if (matches != 1) {
		printf("NEZHA TOUCH: cmd-db %s: %u VRM matches; not voting\n", res, matches);
		return ENODEV;
	}
	return 0;
}

static bool
rh_sync(uint32_t off, uint32_t v)
{
	pt_wr(rh_drv, off, v);
	for (unsigned i = 0; i < 1000; i++) {
		if (pt_rd(rh_drv, off) == v) {
			return true;
		}
		delay(1);
	}
	printf("NEZHA TOUCH: RSC readback %#x want %#x got %#x\n", off, v, pt_rd(rh_drv, off));
	return false;
}

/* One ACTIVE TCS with three commands (voltage, mode, enable), as sweet_rpmh.c. */
static int
pt_rpmh_vote(void)
{
	pt_step("apps RSC DRV 2", PT_RSC_PA);
	rh_drv = ml_io_map(PT_RSC_PA, 0x10000);
	rh_db = ml_io_map(PT_CMDDB_PA, PT_CMDDB_SIZE);
	if (!rh_drv || !rh_db) {
		return ENOMEM;
	}
	uint32_t id = pt_rd(rh_drv, 0), config = pt_rd(rh_drv, 0x0c);
	unsigned major = (id >> 16) & 0xffu, minor = (id >> 8) & 0xffu;
	unsigned ntcs = (config >> (6u * RH_DRV)) & 0x3fu, ncpt = (config >> 27) & 0x1fu;
	printf("NEZHA TOUCH: RSC id=%#x (v%u.%u) solver=%#x config=%#x tcs=%u ncpt=%u\n",
	    id, major, minor, pt_rd(rh_drv, 4), config, ntcs, ncpt);
	/* Linux uses the 3.0 layout for every major >= 3 (this RSC reads v4.3). */
	const struct rh_layout *L = major >= 3 ? &rh_v3 : (major == 2 ? &rh_v2 : NULL);
	if (!L || ntcs < RH_ACTIVE_N || ntcs > 16 || ncpt < 3 || ncpt > 16) {
		printf("NEZHA TOUCH: RSC layout not known; not voting\n");
		return ENODEV;
	}
	pt_step("cmd-db", PT_CMDDB_PA);
	uint32_t magic = pt_rd(rh_db, 4);
	printf("NEZHA TOUCH: cmd-db version=%#x magic=%#x\n", pt_rd(rh_db, 0), magic);
	uint32_t address = 0;
	if (magic != RH_DB_MAGIC || rh_lookup("L4I_E0", &address)) {
		return ENODEV;
	}
	uint32_t tcs = RH_TCS_OFF;
	int chosen = -1;
	for (unsigned t = 0; t < RH_ACTIVE_N; t++) {
		uint32_t b = tcs + t * RH_TCS_STRIDE;
		uint32_t st = pt_rd(rh_drv, b + L->status), ctl = pt_rd(rh_drv, b + L->control);
		uint32_t en = pt_rd(rh_drv, b + L->enable), wt = pt_rd(rh_drv, b + L->wait);
		printf("NEZHA TOUCH: ACTIVE TCS%u idle=%#x control=%#x enable=%#x wait=%#x\n",
		    t, st, ctl, en, wt);
		if (chosen < 0 && st != 0 && st != 0xffffffffu && ctl == 0 && en == 0 && wt == 0 &&
		    !(pt_rd(rh_drv, tcs + 4) & (1u << t))) {
			chosen = (int)t;
		}
	}
	if (chosen < 0) {
		printf("NEZHA TOUCH: no free ACTIVE TCS; not voting\n");
		return EBUSY;
	}
	unsigned t = (unsigned)chosen;
	uint32_t b = tcs + t * RH_TCS_STRIDE, bit = 1u << t;
	uint32_t irq_en = pt_rd(rh_drv, tcs + 0);
	if (!rh_sync(tcs + 0, irq_en & ~bit)) {
		return EIO;
	}
	static const uint32_t offs[3] = {0, 8, 4}, vals[3] = {1800, 7, 1};
	for (unsigned c = 0; c < 3; c++) {
		uint32_t cmd = b + c * L->cmd_stride;
		if (!rh_sync(cmd + L->msgid, RH_WRITE_ACK) || !rh_sync(cmd + L->addr, address + offs[c]) ||
		    !rh_sync(cmd + L->data, vals[c])) {
			return EIO;
		}
	}
	if (!rh_sync(b + L->wait, 7) || !rh_sync(b + L->enable, 7) ||
	    !rh_sync(b + L->control, 0) || !rh_sync(b + L->control, 1u << 16)) {
		return EIO;
	}
	pt_wr(rh_drv, b + L->control, (1u << 16) | (1u << 24));
	bool done = false;
	uint32_t cs[3] = {0, 0, 0};
	for (unsigned us = 0; us < 100000 && !done; us++) {
		bool all = true;
		for (unsigned c = 0; c < 3; c++) {
			cs[c] = pt_rd(rh_drv, b + L->cmd_status + c * L->cmd_stride);
			all &= cs[c] != 0xffffffffu && (cs[c] & 0x10100u) == 0x10100u;
		}
		done = all && pt_rd(rh_drv, b + L->status) != 0;
		if (!done) {
			delay(1);
		}
	}
	printf("NEZHA TOUCH: L4I_E0 1800 mV vote TCS%u done=%d irq=%#x cmd-status=%#x/%#x/%#x\n",
	    t, done, pt_rd(rh_drv, tcs + 4), cs[0], cs[1], cs[2]);
	if (!done) {
		return ETIMEDOUT;
	}
	(void)rh_sync(b + L->control, 1u << 16);
	(void)rh_sync(b + L->control, 0);
	(void)rh_sync(b + L->enable, 0);
	(void)rh_sync(b + L->wait, 0);
	pt_wr(rh_drv, tcs + 8, bit);
	(void)rh_sync(tcs + 0, irq_en);
	return 0;
}

/* ---- TouchComm ----------------------------------------------------------------- */

static uint8_t
tc_crc6(const uint8_t *p, unsigned bits)
{
	static const uint16_t table[16] = {
		0, 268, 536, 788, 1072, 1340, 1576, 1828,
		2144, 2412, 2680, 2932, 3152, 3420, 3656, 3908
	};
	uint16_t r = 0x3f << 2;
	for (; bits > 8; bits -= 8) {
		r ^= *p++;
		r = (uint16_t)((r << 4) ^ table[(r >> 4) & 0xf]);
		r = (uint16_t)((r << 4) ^ table[(r >> 4) & 0xf]);
	}
	if (bits > 0) {
		uint16_t x = *p;
		while (bits--) {
			if (x & 0x80) {
				r ^= 0x80;
			}
			x <<= 1;
			r <<= 1;
			if (r & 0x100) {
				r ^= 0x03 << 2;
			}
		}
	}
	return (uint8_t)((r >> 2) & 0x3f);
}

/* CRC-16/CCITT, MSB first: Synaptics' table version computes the same. */
static uint16_t
tc_crc16(const uint8_t *p, unsigned n, uint16_t r)
{
	while (n--) {
		r ^= (uint16_t)(*p++ << 8);
		for (int i = 0; i < 8; i++) {
			r = (r & 0x8000) ? (uint16_t)((r << 1) ^ 0x1021) : (uint16_t)(r << 1);
		}
	}
	return r;
}

static void
tc_hex(const char *what, const uint8_t *p, unsigned n)
{
	printf("NEZHA TOUCH: %s (%u):", what, n);
	for (unsigned i = 0; i < n && i < 64; i++) {
		printf(" %02x", p[i]);
	}
	printf("%s\n", n > 64 ? " ..." : "");
}

/* v1: one message (header, then a continued read for the payload). */
static int
tc1_read(uint8_t *code)
{
	uint8_t h[4];
	int err = pt_spi(NULL, h, 4);
	if (err) {
		return err;
	}
	if (h[0] != TC_MARKER) {
		tc_hex("v1 bad header", h, 4);
		return EIO;
	}
	unsigned len = (unsigned)h[2] | ((unsigned)h[3] << 8);
	*code = h[1];
	pt.msg_len = 0;
	if (len == 0) {
		return 0;
	}
	if (len > PT_MSG_MAX || len + 6 > PT_XFER_MAX) {
		printf("NEZHA TOUCH: v1 code %#x length %u too long\n", h[1], len);
		return EIO;
	}
	unsigned n = 2 + len + 1 + (pt.crc ? 2 : 0);
	if ((err = pt_spi(NULL, pt_buf, n))) {
		return err;
	}
	if (pt_buf[0] != TC_MARKER || pt_buf[1] != TC_CONTINUED) {
		tc_hex("v1 bad continued read", pt_buf, n);
		return EIO;
	}
	memcpy(pt.msg, pt_buf + 2, len);
	pt.msg_len = len;
	return 0;
}

static int
tc1_write(uint8_t cmd, const uint8_t *pl, unsigned n)
{
	uint8_t b[64];
	if (n + 5 > sizeof(b)) {
		return EINVAL;
	}
	b[0] = cmd;
	b[1] = (uint8_t)n;
	b[2] = (uint8_t)(n >> 8);
	if (n) {
		memcpy(b + 3, pl, n);
	}
	unsigned size = n + 3;
	if (pt.crc) {
		uint16_t c = tc_crc16(b, size, 0xffff);
		b[size++] = (uint8_t)c;
		b[size++] = (uint8_t)(c >> 8);
	}
	return pt_spi(b, NULL, size);
}

static int
tc2_write(uint8_t cmd, const uint8_t *pl, unsigned n, bool resend)
{
	uint8_t b[64];
	if (n + 6 > sizeof(b)) {
		return EINVAL;
	}
	if (resend) {
		pt.seq--;
	}
	b[0] = cmd;
	b[1] = (uint8_t)n;
	b[2] = (uint8_t)(n >> 8);
	b[3] = (uint8_t)((pt.seq++ & 1u) << 6);
	b[3] |= tc_crc6(b, 26);
	unsigned size = 4;
	if (n) {
		memcpy(b + 4, pl, n);
		size += n;
		uint16_t c = tc_crc16(b, size, 0xffff);
		b[size++] = (uint8_t)(c >> 8);
		b[size++] = (uint8_t)c;
	}
	return pt_spi(b, NULL, size);
}

/* v2: the response to the command just written: header, then ACK + read per chunk. */
static int
tc2_response(uint8_t *code)
{
	uint8_t h[4];
	int err = pt_spi(NULL, h, 4);
	if (err) {
		return err;
	}
	if (tc_crc6(h, 32) != 0) {
		tc_hex("v2 header CRC bad", h, 4);
		return EIO;
	}
	unsigned len = (unsigned)h[1] | ((unsigned)h[2] << 8), off = 0, retries = 0;
	*code = h[0];
	pt.msg_len = 0;
	if (len > PT_MSG_MAX) {
		printf("NEZHA TOUCH: v2 code %#x length %u too long\n", h[0], len);
		return EIO;
	}
	bool retry = false;
	while (off < len) {
		unsigned room = (pt.max_rd ? pt.max_rd : 256) - 6, chunk = len - off;
		chunk = chunk > room ? room : chunk;
		if ((err = tc2_write(retry ? CMD_V2_RETRY : CMD_V2_ACK, NULL, 0, false)) ||
		    (err = pt_spi(NULL, pt_buf, chunk + 6))) {
			return err;
		}
		if (tc_crc6(pt_buf, 32) != 0 || tc_crc16(pt_buf, chunk + 6, 0xffff) != 0) {
			if (++retries > 5) {
				tc_hex("v2 continued read CRC bad", pt_buf, chunk + 6);
				return EIO;
			}
			retry = true;
			continue;
		}
		retry = false;
		retries = 0;
		memcpy(pt.msg + off, pt_buf + 4, chunk);
		off += chunk;
	}
	pt.msg_len = len;
	return 0;
}

static int
tc2_exchange(uint8_t cmd, const uint8_t *pl, unsigned n, uint8_t *code)
{
	int err = EIO;
	for (unsigned tries = 0; tries < 5; tries++) {
		if ((err = tc2_write(cmd, pl, n, tries > 0))) {
			return err;
		}
		delay(200);
		err = tc2_response(code);
		if (err == 0 && *code != 0x08 && *code != 0xfe) {   /* retry requested, corrupted */
			return 0;
		}
		delay(1000);
	}
	return err ? err : EIO;
}

/* Next message from the IC: v1 reads one, v2 asks for one (GET_REPORT). */
static int
tc_read(uint8_t *code)
{
	return pt.version == 1 ? tc1_read(code) : tc2_exchange(CMD_V2_GET_REPORT, NULL, 0, code);
}

static void tc_touch_report(const uint8_t *r, unsigned n);
static void tc_thp_frame(const uint8_t *f, unsigned n);

static void
tc_report(uint8_t code)
{
	pt.reports++;
	if (code == TC_TOUCH) {
		pt.touch_reports++;
		tc_touch_report(pt.msg, pt.msg_len);
		return;
	}
	if (code == TC_THP_FRAME && pt.thp) {
		tc_thp_frame(pt.msg, pt.msg_len);
	}
	if (!(pt.codes_seen[code >> 5] & (1u << (code & 31)))) {
		pt.codes_seen[code >> 5] |= 1u << (code & 31);
		printf("NEZHA TOUCH: first report %#x\n", code);
		/* The whole first THP frame, for its layout; other reports: the start. */
		if (!pt.msg_len) {
			tc_hex("report", pt.msg, 0);
		}
		for (unsigned o = 0; o < pt.msg_len; o += 64) {
			tc_hex("report", pt.msg + o, pt.msg_len - o < 64 ? pt.msg_len - o : 64);
			if (code != TC_THP_FRAME) {
				break;
			}
		}
	}
}

/* Write a command and wait (ATTN or 10 ms polls, 3 s) for its response; reports that
 * arrive meanwhile are handled. The response payload is left in pt.msg. */
static int
tc_command(uint8_t cmd, const uint8_t *pl, unsigned n, uint8_t *resp)
{
	uint8_t code = 0;
	int err;
	if (pt.version == 1) {
		err = tc1_write(cmd, pl, n);
	} else {
		err = tc2_exchange(cmd, pl, n, &code);
		if (!err && code != TC_ACK && code < TC_IDENTIFY) {
			*resp = code;
			return code == TC_OK ? 0 : EIO;
		}
		if (!err && code == TC_IDENTIFY && cmd == CMD_IDENTIFY) {
			*resp = code;
			return 0;
		}
		if (!err && code >= TC_IDENTIFY) {
			tc_report(code);
		}
	}
	if (err) {
		return err;
	}
	for (unsigned t = 0; t < 300; t++) {
		pt_sleep(10);
		if ((err = tc_read(&code))) {
			continue;
		}
		if (code == 0 || code == TC_NO_REPORT || code == TC_ACK) {
			continue;
		}
		if (code >= TC_IDENTIFY) {
			if (code == TC_IDENTIFY && cmd == CMD_IDENTIFY) {
				*resp = code;
				return 0;
			}
			tc_report(code);
			continue;
		}
		*resp = code;
		return code == TC_OK ? 0 : EIO;
	}
	printf("NEZHA TOUCH: command %#x: no response\n", cmd);
	return ETIMEDOUT;
}

static unsigned
le16(const uint8_t *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static int
tc_identify_parse(void)
{
	if (pt.msg_len < 24) {
		tc_hex("identify too short", pt.msg, pt.msg_len);
		return EIO;
	}
	char part[17];
	memcpy(part, pt.msg + 2, 16);
	part[16] = 0;
	for (unsigned i = 0; i < 16; i++) {
		if (part[i] && (part[i] < 0x20 || part[i] > 0x7e)) {
			part[i] = '?';
		}
	}
	memcpy(nz_part, part, sizeof(nz_part));
	pt.max_wr = le16(pt.msg + 22);
	pt.max_rd = pt.msg_len >= 26 ? le16(pt.msg + 24) : 0;
	if (pt.max_rd && pt.max_rd < 16) {
		pt.max_rd = 0;
	}
	if (pt.max_rd > PT_XFER_MAX) {
		pt.max_rd = PT_XFER_MAX;
	}
	printf("NEZHA TOUCH: identify: TouchComm v%u version %u mode %#x part \"%s\" build %u "
	    "max write %u read %u\n", pt.version, pt.msg[0], pt.msg[1], part,
	    (uint32_t)pt.msg[18] | ((uint32_t)pt.msg[19] << 8) | ((uint32_t)pt.msg[20] << 16) | ((uint32_t)pt.msg[21] << 24),
	    pt.max_wr, pt.max_rd);
	if (pt.msg[1] != 0x01) {
		printf("NEZHA TOUCH: not in application firmware (mode %#x); stopping\n", pt.msg[1]);
		return ENODEV;
	}
	return 0;
}

/* After reset: find the protocol from the first header and get the identify info. */
static int
tc_detect(void)
{
	uint8_t h[4], code = 0;
	int err = pt_spi(NULL, h, 4);
	if (err) {
		return err;
	}
	tc_hex("first header after reset", h, 4);
	if (h[0] == TC_MARKER) {
		pt.version = 1;
		pt.crc = true;              /* found out from the identify packet's tail */
		if (h[1] == TC_IDENTIFY) {
			unsigned len = le16(h + 2);
			unsigned n = 2 + len + 3;
			if (len < 24 || len > 64) {
				tc_hex("identify header", h, 4);
				return EIO;
			}
			if ((err = pt_spi(NULL, pt_buf, n))) {
				return err;
			}
			tc_hex("identify continued read", pt_buf, n);
			if (pt_buf[0] != TC_MARKER || pt_buf[1] != TC_CONTINUED) {
				return EIO;
			}
			memcpy(pt.msg, pt_buf + 2, len);
			pt.msg_len = len;
			pt.crc = !(pt_buf[2 + len + 1] == TC_PAD && pt_buf[2 + len + 2] == TC_PAD);
		} else {
			pt.crc = false;
			if ((err = tc_command(CMD_IDENTIFY, NULL, 0, &code))) {
				return err;
			}
		}
		printf("NEZHA TOUCH: TouchComm v1, CRC %s\n", pt.crc ? "on" : "off");
	} else if (tc_crc6(h, 32) == 0 && !(h[0] == 0 && h[1] == 0 && h[2] == 0)) {
		pt.version = 2;
		if ((err = tc_command(CMD_IDENTIFY, NULL, 0, &code))) {
			return err;
		}
		printf("NEZHA TOUCH: TouchComm v2 (identify answered %#x)\n", code);
	} else {
		printf("NEZHA TOUCH: no TouchComm header (IC unpowered or SPI wrong)\n");
		return ENODEV;
	}
	return tc_identify_parse();
}

static int
tc_setup(void)
{
	uint8_t code = 0;
	int err;
	if ((err = tc_command(CMD_APP_INFO, NULL, 0, &code))) {
		printf("NEZHA TOUCH: application info failed %d (code %#x)\n", err, code);
		return err;
	}
	if (pt.msg_len < 38) {
		tc_hex("application info too short", pt.msg, pt.msg_len);
		return EIO;
	}
	pt.max_x = le16(pt.msg + 32);
	pt.max_y = le16(pt.msg + 34);
	pt.max_objects = le16(pt.msg + 36);
	/* app info may already carry the super-resolution range (x100): fall back to the panel / DT size. */
	if (pt.max_x > 10 * NEZHA_PANEL_W || pt.max_y > 10 * NEZHA_PANEL_H) {
		pt.max_x /= 100;
		pt.max_y /= 100;
	}
	if (pt.max_x == 0 || pt.max_y == 0) {
		pt.max_x = NEZHA_PANEL_W - 1;
		pt.max_y = NEZHA_PANEL_H - 1;
	}
	printf("NEZHA TOUCH: app info: version %u status %u max %u x %u objects %u image %u x %u\n",
	    le16(pt.msg), le16(pt.msg + 2), pt.max_x, pt.max_y, pt.max_objects,
	    pt.msg_len >= 44 ? le16(pt.msg + 40) : 0, pt.msg_len >= 44 ? le16(pt.msg + 42) : 0);
	pt.rows = pt.msg_len >= 44 ? le16(pt.msg + 40) : 16;
	pt.cols = pt.msg_len >= 44 ? le16(pt.msg + 42) : 35;
	if (pt.max_objects == 0 || pt.max_objects > PT_MAX) {
		pt.max_objects = PT_MAX;
	}
	if (pt.max_x == 0 || pt.max_y == 0) {
		return EIO;
	}
	if ((err = tc_command(CMD_GET_TOUCH_CONFIG, NULL, 0, &code))) {
		printf("NEZHA TOUCH: touch report config failed %d (code %#x)\n", err, code);
		return err;
	}
	pt.config_len = pt.msg_len < sizeof(pt.config) ? pt.msg_len : sizeof(pt.config);
	memcpy(pt.config, pt.msg, pt.config_len);
	tc_hex("touch report config", pt.config, pt.config_len);
	if (!pt.config_len) {
		return EIO;
	}
	/* After reset the firmware streams Xiaomi THP frames (report 0xc0, about 1.5 KB each)
	 * for the host's touch algorithm. Stop those and ask for its own touch reports. */
	uint8_t r = TC_THP_FRAME;
	if (pt.thp) {
		printf("NEZHA TOUCH: keeping THP frames (fingers found here too)\n");
	} else {
		err = tc_command(CMD_DISABLE_REPORT, &r, 1, &code);
		printf("NEZHA TOUCH: disable report %#x -> %d (code %#x)\n", r, err, code);
	}
	r = TC_TOUCH;
	err = tc_command(CMD_ENABLE_REPORT, &r, 1, &code);
	printf("NEZHA TOUCH: enable report %#x -> %d (code %#x)\n", r, err, code);
	return 0;
}

/* syna_tcm_get_touch_data: little-endian bit field at a bit offset. */
static unsigned
tc_bits(const uint8_t *r, unsigned n, unsigned off, unsigned bits)
{
	if (bits == 0 || bits > 32 || off + bits > n * 8) {
		return 0;
	}
	unsigned out = 0, done = 0;
	while (done < bits) {
		unsigned bo = off % 8, take = 8 - bo;
		take = take < bits - done ? take : bits - done;
		out |= ((unsigned)(r[off / 8] >> bo) & (0xffu >> (8 - take))) << done;
		off += take;
		done += take;
	}
	return out;
}

/* ---- publication: HID frame queue and boot-progress state ----------------------- */
volatile unsigned native_touch_stage_req;            /* boot-progress: show the touch rows and pad */
char native_touch_rows[4][120];
volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports, native_touch_loop_step;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
volatile int nezha_touch_stage, nezha_touch_err;     /* stage reached / last error, for the row */

static struct nezha_touch_frame nz_frames[64], nz_state;
static unsigned nz_producer, nz_consumer, nz_drops;

volatile struct nzi_frame nezha_raw_input_diag,nezha_hid_input_diag;
static unsigned nz_trace_down,nz_trace_up,nz_trace_id,nz_trace_errors;
static void nz_publish_raw(void) {
    volatile struct nzi_frame *d=&nezha_raw_input_diag;uint32_t stamp=nzi_begin(d);
    nzi_put(d,R_VERSION,1);nzi_put(d,R_PACKETS,pt.reports);nzi_put(d,R_IC_REPORTS,pt.touch_reports);
    nzi_put(d,R_ERRORS,nz_trace_errors);nzi_put(d,R_FRAME,nz_state.sequence);nzi_put(d,R_MASK,pt.active);
    nzi_put(d,R_DOWN,nz_trace_down);nzi_put(d,R_UP,nz_trace_up);
    nzi_put(d,R_PRODUCER,__atomic_load_n(&nz_producer,__ATOMIC_ACQUIRE));nzi_put(d,R_CONSUMER,__atomic_load_n(&nz_consumer,__ATOMIC_ACQUIRE));
    nzi_put(d,R_DROP,nz_drops);nzi_put(d,R_ID,nz_trace_id);nzi_put(d,R_X,pt.px[nz_trace_id]);nzi_put(d,R_Y,pt.py[nz_trace_id]);
    nzi_time(d,R_TICKLO,mach_absolute_time());nzi_put(d,R_STAGE,(uint32_t)nezha_touch_stage);nzi_put(d,R_THRESHOLD,pt.thp_thr);nzi_end(d,stamp);
}
#include "InputChainTrace.h"

static void nz_trace_thread(void *arg,wait_result_t result) {
    (void)arg;(void)result;
    for(;;) {
        uint32_t r[NZI_WORDS],h[NZI_WORDS];bool rv=nzi_copy(&nezha_raw_input_diag,r),hv=nzi_copy(&nezha_hid_input_diag,h);
        if(rv)printf("NEZHA INPUT RAW: v=%u st=%d packets=%u ic=%u err=%u frame=%u mask=%x down=%u up=%u q=%u/%u drop=%u id=%u panel=%u,%u thr=%u rawticks=%llu\n",
            r[R_VERSION],(int32_t)r[R_STAGE],r[R_PACKETS],r[R_IC_REPORTS],r[R_ERRORS],r[R_FRAME],r[R_MASK],r[R_DOWN],r[R_UP],
            r[R_PRODUCER],r[R_CONSUMER],r[R_DROP],r[R_ID],r[R_X],r[R_Y],r[R_THRESHOLD],(unsigned long long)nzi_u64(r,R_TICKLO));
        else printf("NEZHA INPUT RAW: snapshot busy\n");
        if(hv)printf("NEZHA INPUT HID: v=%u tick=%u ready=%u stop=%u in=%u seen=%u frame=%u mask=%x ack=%x down=%u up=%u call=%u ret=%u ok=%u fail=%u id=%u panel=%u,%u wire=%u,%u count=%u result=%d coalesced=%llu ticktime=%llu started=%llu finished=%llu\n",
            h[H_VERSION],h[H_TICKS],h[H_READY],h[H_STOP],h[H_IN_CALL],h[H_SEEN],h[H_FRAME],h[H_MASK],h[H_ACK_MASK],h[H_DOWN],h[H_UP],
            h[H_ATTEMPT],h[H_RETURN],h[H_OK],h[H_ERROR],h[H_ID],h[H_X],h[H_Y],h[H_WIRE_X],h[H_WIRE_Y],h[H_CONTACTS],(int32_t)h[H_RESULT],(unsigned long long)nzi_u64(h,H_COALLO),(unsigned long long)nzi_u64(h,H_TICKLO),(unsigned long long)nzi_u64(h,H_STARTLO),(unsigned long long)nzi_u64(h,H_ENDLO));
        else printf("NEZHA INPUT HID: snapshot busy\n");
        nezha_input_chain_flush();
        pt_sleep(1000);
    }
}


#include "NezhaTouchSelfTest.h"
extern bool nezha_touch_selftest_due(void);
static struct nezha_touch_test nz_settings_test;

bool
nezha_touch_next(struct nezha_touch_frame *frame)
{
	unsigned c = __atomic_load_n(&nz_consumer, __ATOMIC_RELAXED);
	unsigned p = __atomic_load_n(&nz_producer, __ATOMIC_ACQUIRE);
    if(nz_settings_test.phase!=2 && nezha_touch_selftest_due()) {
        uint32_t raw[NZI_WORDS], hid[NZI_WORDS];
        bool have_raw=nzi_copy(&nezha_raw_input_diag,raw);
        bool have_hid=nzi_copy(&nezha_hid_input_diag,hid);
        bool physical=have_raw && raw[R_MASK]!=0;
        bool idle=have_raw && have_hid && raw[R_MASK]==0 && hid[H_ACK_MASK]==0 &&
            hid[H_IN_CALL]==0 && hid[H_READY]==1 && hid[H_STOP]==0 && c==p;
        uint64_t hold;
        nanoseconds_to_absolutetime(100*NSEC_PER_MSEC,&hold);
        enum nezha_test_result result=nezha_test_next(&nz_settings_test,
            mach_absolute_time(),hold,nezha_touch_selftest_due(),physical,idle,frame);
        if(result==NZ_TEST_FRAME) {
            printf("NEZHA INPUT SELFTEST: fixed_settings_tap phase=%u active=%x panel=444,1872 sequence=%u\n",
                nz_settings_test.phase,frame->active,frame->sequence);
            return true;
        }
        if(result==NZ_TEST_HOLD)return false;
    }

	if (c == p) {
		return false;
	}
	*frame = nz_frames[c % 64];
	__atomic_store_n(&nz_consumer, c + 1, __ATOMIC_RELEASE);
	return true;
}

static void nz_row(void);

/* Fingers in panel pixels (1200 x 2608): one frame to the HID queue, the same set to the
 * boot-progress pad (its 30480 x 20320 units). */
static void
pt_deliver(uint32_t mask, const int *px, const int *py)
{
	for (unsigned id = 0; id < PT_MAX; id++) {
		if (!(mask & (1u << id))) {
			continue;
		}
		int x = px[id], y = py[id];
		x = x < 0 ? 0 : (x >= NEZHA_PANEL_W ? NEZHA_PANEL_W - 1 : x);
		y = y < 0 ? 0 : (y >= NEZHA_PANEL_H ? NEZHA_PANEL_H - 1 : y);
		pt.px[id] = x;
		pt.py[id] = y;
		pt.x[id] = x;
		pt.y[id] = y;
	}
	if (mask == pt.active && mask == 0) {
		return;
	}
	nz_state.sequence++;
	nz_state.active = mask;
	for (unsigned id = 0; id < PT_MAX; id++) {
		if (mask & (1u << id)) {
			nz_state.x[id] = (uint16_t)pt.x[id];
			nz_state.y[id] = (uint16_t)pt.y[id];
		}
	}
	unsigned p = __atomic_load_n(&nz_producer, __ATOMIC_RELAXED);
	unsigned c = __atomic_load_n(&nz_consumer, __ATOMIC_ACQUIRE);
	if (p - c < 64) {
		nz_frames[p % 64] = nz_state;
		__atomic_store_n(&nz_producer, p + 1, __ATOMIC_RELEASE);
	} else {
		nz_drops++;
	}
	/* boot-progress pad: seqlock, odd while writing */
	uint32_t seq = native_touch_seq;
	__atomic_store_n(&native_touch_seq, seq | 1u, __ATOMIC_RELEASE);
	__asm__ volatile("dmb ish" ::: "memory");
	unsigned n = 0;
	for (unsigned id = 0; id < PT_MAX; id++) {
		if (mask & (1u << id)) {
			native_touch_xs[n] = pt.x[id] * 30480 / NEZHA_PANEL_W;
			native_touch_ys[n] = pt.y[id] * 20320 / NEZHA_PANEL_H;
			native_touch_ids[n] = (int)id;
			n++;
		}
	}
	native_touch_count = n;
	__asm__ volatile("dmb ish" ::: "memory");
	__atomic_store_n(&native_touch_seq, (seq | 1u) + 1u, __ATOMIC_RELEASE);
	nz_trace_down+=(unsigned)__builtin_popcount(mask & ~pt.active);
	nz_trace_up+=(unsigned)__builtin_popcount(pt.active & ~mask);
	if(mask)nz_trace_id=(unsigned)__builtin_ctz(mask);
	pt.active = mask;
	nz_publish_raw();
	nz_row();
}

/* Touch report, decoded with the IC's own report config (syna_tcm_parse_touch_report). */
static void
tc_touch_report(const uint8_t *r, unsigned n)
{
	unsigned status[PT_MAX] = {0}, xs[PT_MAX] = {0}, ys[PT_MAX] = {0};
	unsigned idx = 0, off = 0, obj = 0, next = 0, objects = 0, active_objects = 0;
	bool active_only = false, have_count = false;
	static unsigned end_of_foreach;
	while (idx < pt.config_len) {
		uint8_t c = pt.config[idx++];
		unsigned bits, v;
		switch (c) {
		case 0x00:
			goto done;
		case 0x01:          /* for each active object */
			obj = 0;
			next = idx;
			active_only = true;
			break;
		case 0x02:          /* for each object */
			obj = 0;
			next = idx;
			active_only = false;
			break;
		case 0x03:          /* end for each */
			end_of_foreach = idx;
			if (active_only) {
				if (have_count) {
					objects++;
					obj++;
					if (objects < active_objects) {
						idx = next;
					}
				} else if (off < n * 8) {
					obj++;
					idx = next;
				}
			} else {
				obj++;
				if (obj < pt.max_objects) {
					idx = next;
				}
			}
			break;
		case 0x04:          /* pad to next byte */
			off = (off + 7) / 8 * 8;
			break;
		default:
			if (idx >= pt.config_len) {
				goto done;
			}
			bits = pt.config[idx++];
			v = tc_bits(r, n, off, bits);
			off += bits;
			if (c == 0x06) {
				obj = v;
			} else if (obj < PT_MAX && c == 0x07) {
				status[obj] = v;
			} else if (obj < PT_MAX && c == 0x08) {
				xs[obj] = v;
			} else if (obj < PT_MAX && c == 0x09) {
				ys[obj] = v;
			} else if (c == 0x18) {
				active_objects = v;
				have_count = true;
				if (v == 0) {
					if (end_of_foreach == 0) {
						goto done;
					}
					idx = end_of_foreach;
				}
			}
			break;
		}
		if (obj > 64) {
			break;
		}
	}
done:
	;
	uint32_t mask = 0;
	int px[PT_MAX], py[PT_MAX];
	for (unsigned id = 0; id < PT_MAX; id++) {
		/* finger, glove, stylus, eraser, small object */
		if (status[id] < 1 || status[id] > 5) {
			continue;
		}
		mask |= 1u << id;
		/* nezha: synaptics,super-resolution-factor = 100 (Android reports 0..119999 x 0..260799):
		 * coordinates beyond the app-info maximum are 100x, bring them back first. */
		if (xs[id] > pt.max_x || ys[id] > pt.max_y) {
			xs[id] /= 100;
			ys[id] /= 100;
			pt.superres++;
		}
		/* IC units (0..max) -> panel pixels, 1200 x 2608 */
		px[id] = (int)((uint64_t)xs[id] * NEZHA_PANEL_W / (pt.max_x + 1));
		py[id] = (int)((uint64_t)ys[id] * NEZHA_PANEL_H / (pt.max_y + 1));
	}
	if (mask) {
		pt.last_ic_touch = mach_absolute_time();
	}
	if (mask != pt.active && pt.touch_reports <= 20) {
		printf("NEZHA TOUCH: report %u: fingers %#x (first %u,%u raw)\n", pt.touch_reports,
		    mask, xs[0], ys[0]);
	}
	pt_deliver(mask, px, py);
}

/*
 * Fingers from a THP frame: cells at or above pandora_thp_thr that are the largest in
 * their 3 x 3 neighbourhood, position = weighted centroid of that neighbourhood, ids kept
 * by nearest previous position. Panel pixels as tc_touch_report.
 */
static void
tc_thp_frame(const uint8_t *f, unsigned n)
{
	unsigned rows = pt.rows, cols = pt.cols, cells = rows * cols;
	if (rows == 0 || cols == 0 || n < 64 + 2 * cells) {
		return;
	}
	const uint8_t *d = f + 64;
#define THP(r, c) ((int)(int16_t)(d[2 * ((r) * cols + (c))] | (d[2 * ((r) * cols + (c)) + 1] << 8)))
	int thr = (int)pt.thp_thr, found = 0, fx[PT_MAX], fy[PT_MAX], fv[PT_MAX];
	for (unsigned r = 0; r < rows && found < (int)PT_MAX; r++) {
		for (unsigned c = 0; c < cols && found < (int)PT_MAX; c++) {
			int v = THP(r, c);
			if (v < thr) {
				continue;
			}
			bool peak = true;
			long sw = 0, sr = 0, sc = 0;
			for (int dr = -1; dr <= 1; dr++) {
				for (int dc = -1; dc <= 1; dc++) {
					int rr = (int)r + dr, cc = (int)c + dc;
					if (rr < 0 || cc < 0 || rr >= (int)rows || cc >= (int)cols) {
						continue;
					}
					int w = THP((unsigned)rr, (unsigned)cc);
					/* ties: the first cell in scan order is the peak */
					if (w > v || (w == v && (dr < 0 || (dr == 0 && dc < 0)))) {
						peak = false;
					}
					w -= thr / 2;
					if (w > 0) {
						sw += w;
						sr += (long)w * (2 * rr + 1);
						sc += (long)w * (2 * cc + 1);
					}
				}
			}
			if (!peak || sw == 0) {
				continue;
			}
			/* centre of the cell, in units of 1/16 cell: (2i+1)/2 */
			long rpos = sr * 8 / sw, cpos = sc * 8 / sw;     /* 1/16 cell */
			long xc = pt.thp_swap ? cpos : rpos, yc = pt.thp_swap ? rpos : cpos;
			unsigned xn = pt.thp_swap ? cols : rows, yn = pt.thp_swap ? rows : cols;
			if (pt.thp_flip & 1u) {
				xc = (long)xn * 16 - xc;
			}
			if (pt.thp_flip & 2u) {
				yc = (long)yn * 16 - yc;
			}
			fx[found] = (int)(xc * NEZHA_PANEL_W / ((long)xn * 16));
			fy[found] = (int)(yc * NEZHA_PANEL_H / ((long)yn * 16));
			fv[found] = v;
			if (pt.thp_logs < 40) {
				printf("NEZHA TOUCH: THP peak cell %u,%u value %d -> panel %d,%d\n", r, c, v,
				    fx[found], fy[found]);
			}
			found++;
		}
	}
#undef THP
	if (pt.thp_logs < 40 && (found || pt.active)) {
		pt.thp_logs++;
	}
	/* The firmware's own reports win while they come. */
	uint64_t recent;
	nanoseconds_to_absolutetime(300 * NSEC_PER_MSEC, &recent);
	if (pt.last_ic_touch && mach_absolute_time() - pt.last_ic_touch < recent) {
		return;
	}
	/* Keep ids: each previous finger takes its nearest peak (within 200 px). */
	uint32_t mask = 0, used = 0;
	int px[PT_MAX], py[PT_MAX];
	for (unsigned id = 0; id < PT_MAX; id++) {
		if (!(pt.active & (1u << id))) {
			continue;
		}
		int best = -1;
		long bd = 200L * 200L;
		for (int k = 0; k < found; k++) {
			if (used & (1u << k)) {
				continue;
			}
			long dx = fx[k] - pt.px[id], dy = fy[k] - pt.py[id], dd = dx * dx + dy * dy;
			if (dd < bd) {
				bd = dd;
				best = k;
			}
		}
		if (best >= 0) {
			used |= 1u << best;
			mask |= 1u << id;
			px[id] = fx[best];
			py[id] = fy[best];
		}
	}
	for (int k = 0; k < found; k++) {
		if (used & (1u << k)) {
			continue;
		}
		for (unsigned id = 0; id < PT_MAX; id++) {
			if (!(mask & (1u << id)) && !(pt.active & (1u << id))) {
				mask |= 1u << id;
				px[id] = fx[k];
				py[id] = fy[k];
				break;
			}
		}
	}
	(void)fv;
	pt_deliver(mask, px, py);
}

/* ---- thread -------------------------------------------------------------------- */

/* Hardware reset (reset low 10 ms, 200 ms before the first read), identify, set-up. */
static int
pt_ic_start(void)
{
	pt_gpio(PT_RESET, 0, 0, 2, 0);
	pt_sleep(10);
	pt_gpio(PT_RESET, 0, 0, 2, 1);
	pt_sleep(200);
	pt.active = 0;
	int err = tc_detect();
	return err ? err : tc_setup();
}

static void
pt_thread(void *arg, wait_result_t result)
{
	unsigned mask = 0xf, delay_s = 25;   /* nezha: well after the HID device and the first UI frames */
	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("nezha_touch", &mask, sizeof(mask));
	(void)PE_parse_boot_argn("nezha_touchdelay", &delay_s, sizeof(delay_s));
	(void)PE_parse_boot_argn("nezha_steps", &pt.steps_ms, sizeof(pt.steps_ms));
	pt.thp = (mask & 8u) != 0;
	pt.thp_thr = 200;
	(void)PE_parse_boot_argn("nezha_thp_thr", &pt.thp_thr, sizeof(pt.thp_thr));
	(void)PE_parse_boot_argn("nezha_thp_swap", &pt.thp_swap, sizeof(pt.thp_swap));
	(void)PE_parse_boot_argn("nezha_thp_flip", &pt.thp_flip, sizeof(pt.thp_flip));
	printf("NEZHA TOUCH: nezha_touch=%#x, starting in %u s\n", mask, delay_s);
	nz_stage(1, 0);
	pt_sleep(delay_s * 1000u);
	pt.gcc = ml_io_map(PT_GCC_PA, 0x1f0000);
	pt.tlmm = ml_io_map(PT_TLMM_PA, 0x100000);   /* GPIO 0..255: avdd GPIO 215 is beyond pandora's 0xa0000 */
	pt.se = ml_io_map(PT_SE_PA, 0x4000);
	if (!pt.gcc || !pt.tlmm || !pt.se) {
		nz_stage(-2, ENOMEM);
		goto out;
	}
	nz_stage(2, 0);
	pt_step("GCC wrapper 4 clocks", PT_GCC_PA + GCC_VOTE);
	int err;
	if ((err = pt_clocks())) {
		printf("NEZHA TOUCH: clocks failed %d\n", err);
		nz_stage(-3, err);
		goto out;
	}
	pt_step("SE0 registers", PT_SE_PA);
	if (mask & 2u) {
		err = pt_rpmh_vote();
		printf("NEZHA TOUCH: L4I_E0 vote -> %d\n", err);
		nz_stage(4, err);
	}
	if (!(mask & 4u)) {
		goto out;
	}
	if ((err = pt_se_init())) {
		nz_stage(-5, err);
		goto out;
	}
	nz_stage(5, 0);
	pt_step("TLMM touch pins", PT_TLMM_PA + PT_GPIO(48));
	/* Pins, then power and reset (syna_spi_power_on / hw_reset: 50 ms power delay,
	 * reset low 10 ms, 200 ms before the first read). */
	pt_gpio(PT_RESET, 0, 0, 2, 0);
	pt_gpio(PT_IRQ, 0, 0, 2, -1);
	for (unsigned p = 48; p <= 51; p++) {
		pt_gpio(p, 1, 1, 6, -1);
	}
	pt_gpio(PT_AVDD, 0, 0, 2, 1);
	pt_sleep(50);
	pt_step("IC reset+identify (SPI)", PT_SE_PA);
	if ((err = pt_ic_start())) {
		printf("NEZHA TOUCH: TouchComm set-up failed %d\n", err);
		nz_stage(-6, err);
		goto out;
	}
	pt_step("running", 0);
	printf("NEZHA TOUCH: running, polling GPIO %u\n", PT_IRQ);
	nz_stage(7, 0);
	uint64_t active_abs, idle_abs, next = mach_absolute_time(), last_irq = 0, recent_abs;
	nanoseconds_to_absolutetime(4 * NSEC_PER_MSEC, &active_abs);
	nanoseconds_to_absolutetime(8 * NSEC_PER_MSEC, &idle_abs);
	nanoseconds_to_absolutetime(150 * NSEC_PER_MSEC, &recent_abs);
	unsigned errors = 0, polls = 0, bad = 0;
	for (;;) {
		uint64_t now = mach_absolute_time();
		if (++polls % 15000u == 0) {
			printf("NEZHA TOUCH: %u reports (%u touch), %u errors\n", pt.reports,
			    pt.touch_reports, errors);
		}
		for (unsigned k = 0; k < 4 && !(pt_rd(pt.tlmm, PT_GPIO(PT_IRQ) + 4) & 1u); k++) {
			uint8_t code = 0;
			last_irq = now;
			if ((err = tc_read(&code))) {
				if ((++errors % 100u) == 1) {
					printf("NEZHA TOUCH: read error %d (%u)\n", err, errors);
				}
				if (pt.active) {
					pt_deliver(0, NULL, NULL);
				}
				/* Out of step with the IC (a message read half): start it again. */
				if (++bad >= 20) {
					bad = 0;
					err = pt_ic_start();
					printf("NEZHA TOUCH: IC reset after read errors -> %d\n", err);
				nezha_touch_err = err ? err : nezha_touch_err;
				nz_row();
				}
				break;
			}
			bad = 0;
			if (code >= TC_IDENTIFY) {
				tc_report(code);
			} else if (code == 0 || code == TC_NO_REPORT) {
				break;
			}
		}
		if (pt.dead) {
			nz_stage(-8, EIO);
			break;
		}
		nz_trace_errors=errors;nz_publish_raw();
		uint64_t period = (pt.active || now - last_irq < recent_abs) ? active_abs : idle_abs;
		next += period;
		now = mach_absolute_time();
		if (next <= now) {
			next = now + period;
		}
		pt_sleep_until(next);
	}
out:
	nz_publish_raw();
	printf("NEZHA TOUCH: stopped\n");
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static void
nz_stage(int stage, int err)
{
	nezha_touch_stage = stage;
	if (err) {
		nezha_touch_err = err;
	}
	nz_row();
}

static void
pandora_touch_init(void)
{
	thread_t thread;
	unsigned mask = 0xf;
	if (pt.started) {
		return;
	}
	pt.started = true;
	(void)PE_parse_boot_argn("nezha_touch", &mask, sizeof(mask));
	if (!mask) {
		printf("NEZHA TOUCH: off (nezha_touch=0)\n");
		nz_stage(-1, 0);
		return;
	}
	if (kernel_thread_start_priority(pt_thread, NULL, PT_PRIORITY, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		nz_stage(-1, ENOMEM);
	}
}

/* ---- keys: data-a-live fuxi_keys.c, PANDORA path (17 Pro = nezha SoC/PMIC) ---------- */
/*
 * SPMI PMIC arbiter v8: core 0x0c400000 (FEATURES +4: bus 0 APID count, 13 bits), APID->PPID
 * map 0x0c403000 + 4n, bus 0 owner table 0x0c42a000 + 4n, observer channels 0x0c4c0000 +
 * 0x20 * apid (EE 0). PMK8850 (SID 0) pon_hlos@1300: RT_STS 0x1310 bit 7 power, bit 6 resin =
 * volume down. Volume up: TLMM GPIO 101 (active low), GPIO_IN_OUT at 0x0f165004 bit 0.
 * Read only: the only stores are observer EXT_READL commands. Same addresses as the nezha
 * UEFI NezhaKeysDxe (menu keys proven in n2-001).
 */
#define FK_PON_PPID    0x013u
#define FK_PON_RT      0x10u
#define FK_PON_PWR     0x80u
#define FK_PON_RESIN   0x40u
#define FK_NO_APID     0xffffu
#define PK_CORE_PA     0x0c400000u
#define PK_MAP_PA      0x0c403000u
#define PK_OWN_PA      0x0c42a000u
#define PK_OBS_PA      0x0c4c0000u
#define PK_TLMM_GPIO   0x0f165000u
#define PK_MAX_APIDS   8192u

static vm_offset_t fk_core, fk_obs, pk_map, pk_own, pk_tlmm;
static uint16_t fk_pon_apid = FK_NO_APID;
static uint32_t fk_last_status;
static bool fk_started;
static int fk_chan;
volatile int nezha_keys_stage;                       /* 1 started, 2 polling; negative: failed step */
volatile uint32_t nezha_keys_state, nezha_keys_events;
static uint32_t nz_key_ring[32];
static unsigned nz_key_producer, nz_key_consumer;

bool
nezha_keys_next(uint32_t *mask)
{
	unsigned c = __atomic_load_n(&nz_key_consumer, __ATOMIC_RELAXED);
	unsigned p = __atomic_load_n(&nz_key_producer, __ATOMIC_ACQUIRE);
	if (c == p) {
		return false;
	}
	*mask = nz_key_ring[c % 32];
	__atomic_store_n(&nz_key_consumer, c + 1, __ATOMIC_RELEASE);
	return true;
}

static uint32_t
fk_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static bool
fk_read(uint16_t apid, uint8_t reg, uint8_t *out)
{
	if (apid == FK_NO_APID) {
		return false;
	}
	uint32_t off = 0x20u * apid;
	uint32_t cmd = (1u << 27) | ((uint32_t)reg << 4); /* EXT_READL, 1 byte */
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(fk_obs + off) = cmd;
	__asm__ volatile("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		fk_last_status = fk_rd(fk_obs, off + 0x08u);
		if (fk_last_status & 1u) {
			if (fk_last_status & 0x0eu) {
				return false;
			}
			*out = (uint8_t)fk_rd(fk_obs, off + 0x18u);
			return true;
		}
		delay(1);
	}
	return false;
}

static void
fk_sleep(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&fk_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static uint16_t
pk_find_apid(uint16_t ppid, uint32_t count, unsigned *owner_out)
{
	uint16_t found = FK_NO_APID;
	unsigned found_owner = 7;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t map = fk_rd(pk_map, 4u * i);
		if (map == 0 || (map & 0x1fffu) != ppid) {
			continue;
		}
		unsigned owner = fk_rd(pk_own, 4u * i) & 7u;
		if (found == FK_NO_APID || owner == 0) {
			found = (uint16_t)i;
			found_owner = owner;
		}
	}
	*owner_out = found_owner;
	return found;
}

static bool
pk_setup(void)
{
	fk_core = ml_io_map(PK_CORE_PA, 0x1000);
	pk_map = ml_io_map(PK_MAP_PA, 0x8000);
	pk_own = ml_io_map(PK_OWN_PA, 0x8000);
	fk_obs = ml_io_map(PK_OBS_PA, 0x40000);
	pk_tlmm = ml_io_map(PK_TLMM_GPIO, 0x1000);
	if (!fk_core || !pk_map || !pk_own || !fk_obs || !pk_tlmm) {
		printf("NEZHA KEYS: MMIO mapping failed\n");
		nezha_keys_stage = -2;
		return false;
	}
	uint32_t arbiter_version = fk_rd(fk_core, 0);
	uint32_t count = fk_rd(fk_core, 4) & 0x1fffu;
	printf("NEZHA KEYS: arbiter version=%#x bus0 apids=%u\n", arbiter_version, count);
	if (arbiter_version < 0x80000000u || count == 0 || count > PK_MAX_APIDS) {
		printf("NEZHA KEYS: not an arbiter v8, PMIC keys off\n");
	} else {
		unsigned owner;
		fk_pon_apid = pk_find_apid(FK_PON_PPID, count, &owner);
		uint8_t ptype = 0, psub = 0, pon = 0;
		bool pok = fk_read(fk_pon_apid, 0x04, &ptype) &&
		    fk_read(fk_pon_apid, 0x05, &psub) &&
		    fk_read(fk_pon_apid, FK_PON_RT, &pon);
		printf("NEZHA KEYS: pon-apid=%u/ee%u ok=%d type=%#x subtype=%#x rt=%#x st=%#x\n",
		    fk_pon_apid, owner, pok, ptype, psub, pon, fk_last_status);
		if (!pok) {
			fk_pon_apid = FK_NO_APID;
		}
	}
	uint32_t ctl = fk_rd(pk_tlmm, 0x0), io = fk_rd(pk_tlmm, 0x4);
	printf("NEZHA KEYS: gpio101 ctl=%#x io=%#x\n", ctl, io);
	if ((ctl & 0x3cu) != 0 || (ctl & 0x200u)) {
		printf("NEZHA KEYS: gpio101 not a GPIO input; volume-up withheld\n");
		pk_tlmm = 0;
	}
	if (fk_pon_apid == FK_NO_APID && !pk_tlmm) {
		nezha_keys_stage = -3;
		return false;
	}
	printf("NEZHA KEYS: polling every 20 ms (power/volume-down=%d volume-up=%d)\n",
	    fk_pon_apid != FK_NO_APID, pk_tlmm != 0);
	return true;
}

static void
fk_thread(void *arg, wait_result_t result)
{
	static const char *names[3] = {"power", "volume-up", "volume-down"};
	int state[3] = {0, 0, 0}, last[3] = {0, 0, 0};
	unsigned errors = 0, volup_home = 1;
	(void)arg;
	(void)result;
	/* nezha-volup-home=1 (default): volume up is Home (the iPad7,1 Hello says "Press Home");
	 * 0: volume up is Volume Increment;
	 * 2 (N5): volume up / down are Volume Increment / Decrement, both together are Home (no volume sent then). */
	(void)PE_parse_boot_argn("nezha-volup-home", &volup_home, sizeof(volup_home));
	nezha_keys_stage = 1;
	if (!pk_setup()) {
		goto out;
	}
	nezha_keys_stage = 2;
	nz_row();
	for (;;) {
		uint8_t pon = 0;
		bool pok = fk_read(fk_pon_apid, FK_PON_RT, &pon);
		bool up = pk_tlmm != 0 && (fk_rd(pk_tlmm, 0x4) & 1u) == 0;
		bool gok = pk_tlmm != 0;
		bool ok[3] = {pok, gok, pok};
		int now[3] = {!!(pon & FK_PON_PWR), up, !!(pon & FK_PON_RESIN)};
		if (fk_pon_apid != FK_NO_APID && !pok) {
			if ((errors++ % 250u) == 0) {
				printf("NEZHA KEYS: read error st=%#x\n", fk_last_status);
			}
		} else {
			errors = 0;
		}
		bool changed = false;
		for (int k = 0; k < 3; ++k) {
			/* Two equal samples 20 ms apart before a change counts. */
			if (ok[k] && now[k] == last[k] && now[k] != state[k]) {
				state[k] = now[k];
				changed = true;
				printf("NEZHA KEYS: %s %s (pon=%#x)\n", names[k], state[k] ? "down" : "up", pon);
			}
			if (ok[k]) {
				last[k] = now[k];
			}
		}
		if (changed) {
			uint32_t mask = (state[0] ? NEZHA_KEY_POWER : 0) | (state[2] ? NEZHA_KEY_VOLDN : 0) |
			    (state[1] ? (volup_home == 1 ? NEZHA_KEY_HOME : NEZHA_KEY_VOLUP) : 0);
			if (volup_home == 2 && state[1] && state[2]) {
				mask = (mask & ~(NEZHA_KEY_VOLUP | NEZHA_KEY_VOLDN)) | NEZHA_KEY_HOME;   /* chord: Home */
			}
			nezha_keys_state = mask;
			nezha_keys_events++;
			unsigned p = __atomic_load_n(&nz_key_producer, __ATOMIC_RELAXED);
			unsigned c = __atomic_load_n(&nz_key_consumer, __ATOMIC_ACQUIRE);
			if (p - c < 32) {
				nz_key_ring[p % 32] = mask;
				__atomic_store_n(&nz_key_producer, p + 1, __ATOMIC_RELEASE);
			}
			nz_row();
		}
		fk_sleep(20);
	}
out:
	printf("NEZHA KEYS: stopped\n");
	nz_row();
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static void
nezha_keys_init(void)
{
	unsigned enabled = 1;
	thread_t thread;
	if (fk_started) {
		return;
	}
	fk_started = true;
	(void)PE_parse_boot_argn("nezha-keys", &enabled, sizeof(enabled));
	if (!enabled) {
		nezha_keys_stage = -1;
		return;
	}
	if (kernel_thread_start(fk_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		nezha_keys_stage = -1;
	}
}

/* boot-progress row 42 (replaces the TIMER row while touch is requested). */
static void
nz_row(void)
{
	snprintf(native_touch_rows[0], sizeof(native_touch_rows[0]),
	    "N4 TOUCH st=%d [%.24s] err=%d v%u \"%s\" max=%ux%u rep=%u touch=%u act=%#x drop=%u sr=%u | KEYS st=%d ev=%u now=%#x",
	    nezha_touch_stage, nz_step, nezha_touch_err, pt.version, nz_part, pt.max_x, pt.max_y,
	    pt.reports, pt.touch_reports, pt.active, nz_drops, pt.superres, nezha_keys_stage, nezha_keys_events,
	    nezha_keys_state);
}

/* Called once from bsd_autoconf after IOKitBSDInit (patches/xnu/nezha-input-probe.patch).
 * Boot-args: nezha-input=0 turns both off. */
void
nezha_input_start(void)
{
	/*27 input owns only touch/keys;GPU,USB,storage and power have separate owners. */
	extern void nezha_resource_probe_start(void);
	nezha_resource_probe_start();
	unsigned enabled = 1;
	(void)PE_parse_boot_argn("nezha-input", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	static bool trace_started;
	if(!trace_started){thread_t observer;if(kernel_thread_start(nz_trace_thread,NULL,&observer)==KERN_SUCCESS){trace_started=true;thread_deallocate(observer);}}
	native_touch_stage_req = 1;
	nz_row();
	nezha_keys_init();
	pandora_touch_init();
}
