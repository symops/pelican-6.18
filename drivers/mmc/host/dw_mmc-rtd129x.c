// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek RTD1295/RTD1296 DesignWare MMC (eMMC) glue
 *
 * The RTD129x eMMC controller at 0x98012000 is a Synopsys DesignWare
 * Mobile Storage Host (dw_mmc) IP -- confirmed register-for-register
 * against the vendor 4.9.330 tree's drivers/mmc/host/reg_mmc.h: every
 * offset the vendor driver uses in the 0x00-0x0f0 range (CTRL, PWREN,
 * CLKDIV, CLKSRC, CLKENA, TMOUT, CTYPE, BLKSIZE, BYTCNT, INTMASK, CMDARG,
 * CMD, RESP0-3, MINTSTS, RINTSTS, STATUS, FIFOTH, TBBCNT, UHSREG, BMOD,
 * DBADDR, IDINTEN, IDSTS, CARD_THR_CTL, DDR_REG) matches mainline
 * dw_mmc.h's SDMMC_* offsets exactly, and the vendor's own hand-rolled
 * IDMAC descriptor builder (make_sg_des()/make_ip_des() in rtkemmc.c)
 * writes the same 4-word OWN/CH/FD/LD descriptor format dw_mci_idmac.c
 * already produces. So instead of forward-porting the vendor's ~5400
 * line bespoke command-dispatch/DMA driver, this is a thin glue driver
 * on top of the mainline dw_mmc core (same pattern as dw_mmc-rockchip.c
 * etc.) -- the core handles command dispatch, IDMAC, and interrupts
 * generically and correctly; this file only handles what's genuinely
 * SoC-specific and has no mainline provider: the CRT clock-gate/reset
 * bits, pin mux, pad drive strength, and the eMMC PLL (frequency +
 * phase) that a mainline `clocks =` binding can't reach because no
 * mainline clock/reset driver exists yet for these CRT bits (same
 * situation as drivers/ata/ahci_rtd1295.c).
 *
 * Phase 1 scope: MMC_TIMING_MMC_HS (52MHz-class "high speed", no UHS/
 * DDR/HS200/HS400), fixed phase, no tuning -- this matches how the
 * vendor's own production board DTS configures this exact board
 * (rtd-1296-pelican-1GB.dts: `speed-step = <0>;` i.e. SDR, and
 * `phase_tuning = <0 0>;` i.e. tuning disabled, fixed phase). The eMMC
 * PLL is programmed once, in .init(), to the vendor's own "100MHz"
 * MMC_TIMING_MMC_HS setting (freq code 0x57 from rtkemmc_set_ios()'s
 * MMC_TIMING_MMC_HS case) and left there; `clock-frequency = <100000000>`
 * in the board DT tells the dw_mmc core to treat that as host->bus_hz,
 * so its own generic CLKDIV math (dw_mci_setup_bus()) derives both the
 * ~400kHz identification clock and the ~50MHz operating clock from it
 * without this glue driver having to reimplement per-mode PLL
 * switching -- 100MHz/CLKDIV reproduces the vendor's own "100MHz/2 =
 * 50MHz" MMC_TIMING_MMC_HS result exactly.
 *
 * Phase 2 adds MMC_TIMING_MMC_HS200 (raw ~200MHz PLL out, freq code
 * 0xa6 from rtkemmc_set_ios()'s MMC_TIMING_MMC_HS200 case, ip_div
 * bypassed so card clock == PLL out, matching the vendor's
 * EMMC_CLOCK_DIV_NON) plus RX-only tuning (VP1/RX phase swept 0-31 via
 * the generic mmc_send_tuning(), same CMD21-based mechanism every
 * other dw_mmc glue driver uses -- see dw_mci_rtd129x_execute_tuning()).
 * TX (VP0/write) phase is left at the fixed value that was safe at
 * probe time and is *not* independently tuned: the vendor's own
 * rtkemmc_phase_tuning() does a second, proprietary TX sweep using
 * CMD13/CMD25-based write validation that has no mainline equivalent
 * (CMD21 tuning is a read-only, spec-defined mechanism). Since this
 * board's own vendor DTS default (rtd-1296.dtsi: `speed-step = <0>`,
 * `phase_tuning = <0 0>`) doesn't use HS200 either -- only U-Boot's own
 * loader does, for its own purposes -- there's no known-good reference
 * TX phase to inherit, and porting the vendor's full dual-direction
 * retry state machine (with escalating pad-drive-strength fallback) was
 * judged out of scope for this phase. If HS200 proves unstable on
 * writes in practice, that TX-tuning gap is the first place to look.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"

/* CRT registers (0x98000000+), shared with other CRT reset/clock-gate
 * consumers (see ahci_rtd1295.c) -- not exposed via a mainline clock/
 * reset provider, so poked directly the same way.
 */
#define RTD129X_CRT_BASE		0x98000000

#define RTD129X_CRT_RSTN_BANK2		0x04	/* CR bit10, EMMC bit11 */
#define RTD129X_CRT_RSTN_CR_BIT		10
#define RTD129X_CRT_RSTN_EMMC_BIT	11

#define RTD129X_CRT_CLKEN		0x0C	/* EMMC bit24, CR bit25, EMMC_IP bit28 */
#define RTD129X_CRT_CLKEN_EMMC_BIT	24
#define RTD129X_CRT_CLKEN_CR_BIT	25
#define RTD129X_CRT_CLKEN_EMMC_IP_BIT	28

#define RTD129X_CRT_PLL_EMMC_BASE	0x1F0	/* SYS_PLL_EMMC1..4 */
#define RTD129X_SYS_PLL_EMMC1		0x00	/* phase: VP0 [7:3], VP1 [12:8], PLL reset bit1 */
#define RTD129X_SYS_PLL_EMMC3		0x08	/* freq code [23:16] */
#define RTD129X_SYS_PLL_EMMC4		0x0C	/* bit0: freq code latch */

#define RTD129X_EMMC_HS_FREQ_CODE	0x57	/* vendor MMC_TIMING_MMC_HS setting, ~100MHz raw PLL out */
#define RTD129X_EMMC_HS200_FREQ_CODE	0xa6	/* vendor MMC_TIMING_MMC_HS200 setting, ~200MHz raw PLL out */

#define RTD129X_EMMC_HS_BUS_HZ		100000000
#define RTD129X_EMMC_HS200_BUS_HZ	200000000

/* SYS_PLL_EMMC1 phase fields: VP0 (TX) [7:3], VP1 (RX) [12:8], each 0-0x1f */
#define RTD129X_PHASE_VP0_SHIFT		3
#define RTD129X_PHASE_VP1_SHIFT		8
#define RTD129X_PHASE_MASK		0x1f
#define RTD129X_PHASE_RANGE		32

/* eMMC-block registers beyond the standard dw_mmc window (all still
 * inside the same 0x98012000 4K region as host->regs, so no separate
 * ioremap needed -- vendor reg_mmc.h "1295 emmc wrapper register" /
 * pin-mux / pad-drive block).
 */
#define RTD129X_EMMC_CP			0x41C
#define RTD129X_EMMC_DUMMY_SYS		0x42C
#define RTD129X_EMMC_CKGEN_CTL		0x478
#define RTD129X_EMMC_PAD_CTL		0x474
#define RTD129X_EMMC_SWC_SEL		0x4D4
#define RTD129X_EMMC_MUXPAD0		0x600
#define RTD129X_EMMC_MUXPAD1		0x604
#define RTD129X_EMMC_PFUNC_NF1		0x60C
#define RTD129X_EMMC_PDRIVE_NF1		0x624
#define RTD129X_EMMC_PDRIVE_NF2		0x628
#define RTD129X_EMMC_PDRIVE_NF3		0x62C
#define RTD129X_EMMC_PDRIVE_NF4		0x630

struct dw_mci_rtd129x_priv {
	void __iomem *crt;		/* CRT_BASE + 0x00, 16 bytes: reset bank2 + clken */
	void __iomem *crt_pll;		/* CRT_BASE + 0x1F0, 16 bytes: SYS_PLL_EMMC1-4 */

	int cur_timing;			/* last ios->timing applied by .set_ios, -1 = none yet */
	u32 vp0, vp1;			/* current TX/RX phase, 0-0x1f each */

	/* pad-driving tables: clk/cmd/data/ds, index order matches
	 * rtd129x_emmc_pad_driving()'s args. [0] is SDR/HS (pddrive_nf_s0),
	 * [1] is HS200 (pddrive_nf_s2).
	 */
	u32 pad_drv[2][4];
};

/*
 * CRT+0x0C (RTD129X_CRT_CLKEN) is the *same physical register*
 * drivers/ata/ahci_rtd1295.c's port-0 clock-gate enable writes (different
 * bits: SATA there, EMMC/CR/EMMC_IP here). That driver's probe can be
 * invoked repeatedly (deferred-probe retries) well after this driver's own
 * one-shot .init() has already run, with no ordering guarantee between the
 * two. An unlocked read-modify-write here can race with theirs and silently
 * drop whichever side wrote second based on a stale read -- observed on
 * real Duo hardware as AHCI's SATA clock going dark by the time its
 * ~1-11s-delayed deferred host-init work actually touches the controller
 * again, hanging deep in a register-poll loop. See ahci_rtd1295.c's
 * rtd129x_crt_lock comment for the full story; shared here via
 * EXPORT_SYMBOL_GPL from that driver.
 */
extern spinlock_t rtd129x_crt_lock;

static void rtd129x_emmc_crt_enable(struct dw_mci_rtd129x_priv *priv)
{
	unsigned long flags;
	u32 val;

	/* clock gates: EMMC, CR (shared SD/SDIO/eMMC DMA bus clock -- the
	 * vendor driver keeps this on unconditionally too, see rtkemmc_probe()
	 * comment "1295 uses the same DMA bus between SD, SDIO, and EMMC"),
	 * EMMC_IP.
	 */
	spin_lock_irqsave(&rtd129x_crt_lock, flags);
	val = readl(priv->crt + RTD129X_CRT_CLKEN);
	val |= BIT(RTD129X_CRT_CLKEN_EMMC_BIT) | BIT(RTD129X_CRT_CLKEN_CR_BIT) |
	       BIT(RTD129X_CRT_CLKEN_EMMC_IP_BIT);
	writel(val, priv->crt + RTD129X_CRT_CLKEN);
	spin_unlock_irqrestore(&rtd129x_crt_lock, flags);

	/* reset deassert: CR, EMMC -- different CRT bank (bank2, CRT+0x04)
	 * from anything ahci_rtd1295.c touches, but locked too for
	 * consistency/simplicity (cheap, one-shot, not a hot path).
	 */
	spin_lock_irqsave(&rtd129x_crt_lock, flags);
	val = readl(priv->crt + RTD129X_CRT_RSTN_BANK2);
	val |= BIT(RTD129X_CRT_RSTN_CR_BIT) | BIT(RTD129X_CRT_RSTN_EMMC_BIT);
	writel(val, priv->crt + RTD129X_CRT_RSTN_BANK2);
	spin_unlock_irqrestore(&rtd129x_crt_lock, flags);
}

/* Program the eMMC PLL's N/F (frequency) code -- rtkemmc_set_freq()
 * register-for-register, including the EMMC_IP clock gate-off/on around
 * the code write (avoids glitching a live clock tree while the PLL
 * relocks) and the ECO DUMMY_SYS bit30 toggle whenever the code changes.
 * Called both once at probe (Phase 1 HS baseline) and again from
 * .set_ios() on every HS<->HS200 transition (Phase 2).
 */
static void rtd129x_emmc_set_freq(struct dw_mci *host, struct dw_mci_rtd129x_priv *priv,
				   u32 freq_code)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&rtd129x_crt_lock, flags);
	val = readl(priv->crt + RTD129X_CRT_CLKEN) & ~BIT(RTD129X_CRT_CLKEN_EMMC_IP_BIT);
	writel(val, priv->crt + RTD129X_CRT_CLKEN);
	spin_unlock_irqrestore(&rtd129x_crt_lock, flags);

	val = (readl(priv->crt_pll + RTD129X_SYS_PLL_EMMC3) & 0xffff) |
	      (freq_code << 16);
	writel(val, priv->crt_pll + RTD129X_SYS_PLL_EMMC3);

	val = readl(priv->crt_pll + RTD129X_SYS_PLL_EMMC4) | BIT(0);
	writel(val, priv->crt_pll + RTD129X_SYS_PLL_EMMC4);

	/* ECO: toggle EMMC_DUMMY_SYS bit30 whenever the N/F code changes */
	writel(readl(host->regs + RTD129X_EMMC_DUMMY_SYS) ^ BIT(30),
	       host->regs + RTD129X_EMMC_DUMMY_SYS);
	udelay(400);

	spin_lock_irqsave(&rtd129x_crt_lock, flags);
	val = readl(priv->crt + RTD129X_CRT_CLKEN) | BIT(RTD129X_CRT_CLKEN_EMMC_IP_BIT);
	writel(val, priv->crt + RTD129X_CRT_CLKEN);
	spin_unlock_irqrestore(&rtd129x_crt_lock, flags);
}

/* Program the eMMC PLL's TX/RX phase (VP0/VP1) -- phase() register-for-
 * register: switch to the slow reference clock while the PLL is reset
 * (avoids glitching a live card clock), write the new VP0/VP1 fields,
 * release reset, switch back to the PLL. Called at probe (fixed 0,0)
 * and repeatedly during HS200 tuning (VP1 swept 0-0x1f).
 */
static void rtd129x_emmc_set_phase(struct dw_mci *host, struct dw_mci_rtd129x_priv *priv,
				    u32 vp0, u32 vp1)
{
	u32 val;

	writel(readl(host->regs + RTD129X_EMMC_CKGEN_CTL) | 0x70000,
	       host->regs + RTD129X_EMMC_CKGEN_CTL);

	val = readl(priv->crt_pll + RTD129X_SYS_PLL_EMMC1) & ~BIT(1);
	writel(val, priv->crt_pll + RTD129X_SYS_PLL_EMMC1);	/* assert PLL reset */
	val = readl(priv->crt_pll + RTD129X_SYS_PLL_EMMC1) & 0xffffe0f8; /* clear VP0/VP1 */
	val |= ((vp0 & RTD129X_PHASE_MASK) << RTD129X_PHASE_VP0_SHIFT) |
	       ((vp1 & RTD129X_PHASE_MASK) << RTD129X_PHASE_VP1_SHIFT);
	writel(val, priv->crt_pll + RTD129X_SYS_PLL_EMMC1);
	val = readl(priv->crt_pll + RTD129X_SYS_PLL_EMMC1) | BIT(1);
	writel(val, priv->crt_pll + RTD129X_SYS_PLL_EMMC1);	/* release PLL reset */
	udelay(200);

	writel(readl(host->regs + RTD129X_EMMC_CKGEN_CTL) & 0xfff8ffff,
	       host->regs + RTD129X_EMMC_CKGEN_CTL);

	priv->vp0 = vp0;
	priv->vp1 = vp1;
}

/*
 * Given a 0x20-bit window of tuning candidates (bit i set == phase i
 * passed), find the widest contiguous run (circularly, since phase
 * wraps) and return its midpoint. Same approach as the vendor's
 * search_best() and mainline dw_mmc-exynos's get_best_clksmpl(), just
 * generalized from 8 to RTD129X_PHASE_RANGE (32) candidates. Returns
 * -EIO if no candidate passed at all.
 */
static int rtd129x_emmc_search_best_phase(u32 window)
{
	int i, start, run_start = 0, best_start = 0, best_len = 0, len;

	if (window == 0)
		return -EIO;
	if (window == 0xffffffff)
		return RTD129X_PHASE_RANGE / 2;

	/* find a 0->1 edge to use as a scan start, so a run wrapping past
	 * bit 31 back to bit 0 is still seen as one contiguous run
	 */
	for (i = 0; i < RTD129X_PHASE_RANGE; i++) {
		if ((window & BIT(i)) && !(window & BIT((i - 1) & (RTD129X_PHASE_RANGE - 1))))
			break;
	}
	start = i;

	len = 0;
	for (i = 0; i < RTD129X_PHASE_RANGE; i++) {
		int bit = (start + i) & (RTD129X_PHASE_RANGE - 1);

		if (window & BIT(bit)) {
			if (len == 0)
				run_start = i;
			len++;
			if (len > best_len) {
				best_len = len;
				best_start = run_start;
			}
		} else {
			len = 0;
		}
	}

	return (start + best_start + best_len / 2) & (RTD129X_PHASE_RANGE - 1);
}

static void rtd129x_emmc_pinmux_init(struct dw_mci *host)
{
	u32 val;

	val = readl(host->regs + RTD129X_EMMC_MUXPAD0);
	val &= ~0xFFFF0C3CU;
	val |= 0xaaaa0824;
	writel(val, host->regs + RTD129X_EMMC_MUXPAD0);

	val = (readl(host->regs + RTD129X_EMMC_MUXPAD1) & 0xffffcfff) | 0x2000;
	writel(val, host->regs + RTD129X_EMMC_MUXPAD1);

	writel(0x33333333, host->regs + RTD129X_EMMC_PFUNC_NF1);
	writel(0, host->regs + RTD129X_EMMC_PAD_CTL); /* PAD to 1.8V */
}

/* Pad drive strength -- clk/cmd/data/ds, from the board DT's
 * pddrive_nf_s0 (index 0 is a "calibrated" flag, 1-4 are clk/cmd/data/
 * ds, matching the vendor binding exactly). Falls back to the vendor's
 * own SDR default (0x33 all round) if the board DT doesn't supply one.
 */
static void rtd129x_emmc_pad_driving(struct dw_mci *host, u32 clk_drv, u32 cmd_drv,
				      u32 data_drv, u32 ds_drv)
{
	u32 val;

	writel(data_drv | (data_drv << 8) | (data_drv << 16) | (data_drv << 24),
	       host->regs + RTD129X_EMMC_PDRIVE_NF1);
	writel(data_drv | (data_drv << 8) | (data_drv << 16) | (data_drv << 24),
	       host->regs + RTD129X_EMMC_PDRIVE_NF2);

	val = (readl(host->regs + RTD129X_EMMC_PDRIVE_NF3) & 0x00ff00ff) |
	      (clk_drv << 8) | (cmd_drv << 24);
	writel(val, host->regs + RTD129X_EMMC_PDRIVE_NF3);

	writel(ds_drv, host->regs + RTD129X_EMMC_PDRIVE_NF4);
}

/* vendor's pddrive_nf_s0 (SDR/HS) and pddrive_nf_s2 (HS200) defaults,
 * from rtd-1296.dtsi -- used when the board DT doesn't override.
 */
static const u32 rtd129x_pad_drv_default[2][4] = {
	[0] = { 0x77, 0x77, 0x77, 0x33 },	/* SDR/HS: clk, cmd, data, ds */
	[1] = { 0xbb, 0xbb, 0xbb, 0x33 },	/* HS200 */
};

static void rtd129x_emmc_parse_pad_driving(struct device_node *np, const char *prop,
					    u32 out[4], const u32 fallback[4])
{
	u32 pd[5];

	if (of_property_read_variable_u32_array(np, prop, pd, 5, 5) == 5 && pd[0]) {
		out[0] = pd[1];
		out[1] = pd[2];
		out[2] = pd[3];
		out[3] = pd[4];
	} else {
		memcpy(out, fallback, 4 * sizeof(u32));
	}
}

static int dw_mci_rtd129x_init(struct dw_mci *host)
{
	struct dw_mci_rtd129x_priv *priv;
	struct device_node *np = host->dev->of_node;

	priv = devm_kzalloc(host->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->crt = devm_ioremap(host->dev, RTD129X_CRT_BASE, 0x10);
	if (!priv->crt)
		return -ENOMEM;

	priv->crt_pll = devm_ioremap(host->dev, RTD129X_CRT_BASE + RTD129X_CRT_PLL_EMMC_BASE, 0x10);
	if (!priv->crt_pll)
		return -ENOMEM;

	priv->cur_timing = -1;

	host->priv = priv;

	rtd129x_emmc_crt_enable(priv);
	rtd129x_emmc_pinmux_init(host);

	rtd129x_emmc_parse_pad_driving(np, "realtek,pad-driving", priv->pad_drv[0],
					rtd129x_pad_drv_default[0]);
	rtd129x_emmc_parse_pad_driving(np, "realtek,pad-driving-hs200", priv->pad_drv[1],
					rtd129x_pad_drv_default[1]);

	rtd129x_emmc_pad_driving(host, priv->pad_drv[0][0], priv->pad_drv[0][1],
				  priv->pad_drv[0][2], priv->pad_drv[0][3]);

	rtd129x_emmc_set_freq(host, priv, RTD129X_EMMC_HS_FREQ_CODE);
	rtd129x_emmc_set_phase(host, priv, 0, 0);
	priv->cur_timing = MMC_TIMING_MMC_HS;

	/* This is a hardwired eMMC, never SD/SDIO -- match the vendor
	 * driver's own `mmc->caps2 = (MMC_CAP2_NO_SDIO | MMC_CAP2_NO_SD);`.
	 * Without this, mmc_rescan() probes SDIO (CMD5) and SD (ACMD41/CMD8,
	 * including voltage-switch handling) before ever trying plain MMC
	 * (CMD1) -- there's no mainline DT binding for this, so it has to be
	 * set in code. host->pdata already exists at .init() time (set from
	 * dw_mci_parse_dt() before this callback runs), and
	 * dw_mci_init_slot_caps() reads it into mmc->caps2 later.
	 *
	 * MMC_CAP2_HS200_1_8V_SDR (Phase 2) has to be set here too rather
	 * than via the standard `mmc-hs200-1_8v` DT property:
	 * dw_mci_init_slot_caps() does `mmc->caps2 = host->pdata->caps2`
	 * (an overwrite, not `|=`) whenever pdata->caps2 is non-zero, which
	 * it already is because of the NO_SDIO/NO_SD bits above -- so a
	 * DT-set cap here would just get silently clobbered.
	 */
	host->pdata->caps2 |= MMC_CAP2_NO_SDIO | MMC_CAP2_NO_SD | MMC_CAP2_HS200_1_8V_SDR;

	/* vendor's constant (non-DDR) UHSREG bit0; the dw_mmc core already
	 * manages the DDR bit (bit16) generically in dw_mci_set_ios() but
	 * doesn't know about this SoC-specific bit0.
	 */
	writel(readl(host->regs + SDMMC_UHS_REG) | BIT(0), host->regs + SDMMC_UHS_REG);

	/* constant zero pokes the vendor driver does before every command
	 * (SD_Stream_Cmd()) -- harmless and apparently just a one-time mode
	 * select, so done once here instead of per-command.
	 */
	writel(0, host->regs + RTD129X_EMMC_SWC_SEL);
	writel(0, host->regs + RTD129X_EMMC_CP);

	/*
	 * IDMAC (internal DMA) never completes a real data transfer on this
	 * hardware -- confirmed on a real Duo unit: command-only completions
	 * (CMD0/1/2/3/9/7...) all work fine over IRQ, but the very first
	 * actual DMA data phase (CMD8/SEND_EXT_CSD, 512 bytes) hangs forever
	 * -- no completion, no data-timeout error either, despite TMOUT being
	 * programmed correctly. The IDMAC descriptor format itself is right
	 * (verified bit-for-bit against the vendor driver's own
	 * make_sg_des()/make_ip_des()), so this looks like a genuine
	 * DMA-master/bus-level quirk specific to this SoC's eMMC instance
	 * rather than a descriptor-programming bug. Forcing PIO instead
	 * fixes it outright -- confirmed end to end on real hardware (card
	 * identifies, mmcblk0 registers). Slower, but correct; DMA can be
	 * revisited later if ever needed.
	 */
	host->quirks |= DW_MMC_QUIRK_NO_DMA;

	/*
	 * HCON reports a 64-bit-wide FIFO on this instance ("64 bit host
	 * data width" in dmesg), and the mainline default for that width is
	 * a genuine 64-bit __raw_readq()/__raw_writeq() PIO access
	 * (dw_mci_pull_data64()/push_data64() -> mci_fifo_readq()). That
	 * silently returns all-zero data on this SoC instead of erroring:
	 * confirmed on real hardware -- CID/CSD (which come back over the
	 * 32-bit RESP0-3 registers, not the FIFO) decode fine and the card
	 * identifies correctly, but SEND_EXT_CSD (CMD8, the first real FIFO
	 * data read) "succeeds" with a fully-zeroed 512-byte buffer, so
	 * EXT_CSD's SEC_COUNT reads back 0 and mmcblk0 registers at 0 B.
	 * DW_MMC_QUIRK_FIFO64_32 (added for the same class of bug on other
	 * SoCs, see its doc comment in dw_mmc.h) switches to
	 * pull_data64_32()/push_data64_32(), which do two paired 32-bit
	 * accesses instead of one 64-bit one -- exactly what this bus can
	 * actually service.
	 */
	host->quirks |= DW_MMC_QUIRK_FIFO64_32;

	return 0;
}

/* Switch PLL freq code + pad-driving table on HS<->HS200 transitions.
 * Called from dw_mci_set_ios() *before* the core's own dw_mci_setup_bus(),
 * so setting host->bus_hz here is picked up by the same call's generic
 * CLKDIV math -- same pattern as dw_mmc-exynos.c/dw_mmc-rockchip.c.
 *
 * Phase is deliberately left untouched here: on entry into HS200 it keeps
 * whatever fixed (0,0) value .init() set, and .execute_tuning() below
 * reprograms VP1 (RX) right afterwards, per the mmc core's own HS200
 * sequence (mmc_select_hs200() switches timing+clock, *then* tuning
 * runs). Leaving HS200 resets phase back to (0,0), matching .init()'s
 * baseline and the Phase 1 fixed-phase assumption for every other timing.
 */
static void dw_mci_rtd129x_set_ios(struct dw_mci *host, struct mmc_ios *ios)
{
	struct dw_mci_rtd129x_priv *priv = host->priv;

	if (ios->timing == priv->cur_timing)
		return;

	if (ios->timing == MMC_TIMING_MMC_HS200) {
		rtd129x_emmc_set_freq(host, priv, RTD129X_EMMC_HS200_FREQ_CODE);
		host->bus_hz = RTD129X_EMMC_HS200_BUS_HZ;
		rtd129x_emmc_pad_driving(host, priv->pad_drv[1][0], priv->pad_drv[1][1],
					  priv->pad_drv[1][2], priv->pad_drv[1][3]);
	} else {
		rtd129x_emmc_set_freq(host, priv, RTD129X_EMMC_HS_FREQ_CODE);
		host->bus_hz = RTD129X_EMMC_HS_BUS_HZ;
		rtd129x_emmc_set_phase(host, priv, 0, 0);
		rtd129x_emmc_pad_driving(host, priv->pad_drv[0][0], priv->pad_drv[0][1],
					  priv->pad_drv[0][2], priv->pad_drv[0][3]);
	}

	priv->cur_timing = ios->timing;
}

/* HS200 tuning: sweep RX phase (VP1) 0-0x1f, sending the spec-defined
 * CMD21 tuning block at each candidate via the generic mmc_send_tuning()
 * (handles block size/pattern/CRC check for us -- same call every other
 * dw_mmc glue driver's execute_tuning() makes), then settle on the
 * widest passing window's midpoint. TX (VP0) is not tuned -- see the
 * file header comment for why.
 */
static int dw_mci_rtd129x_execute_tuning(struct dw_mci_slot *slot, u32 opcode)
{
	struct dw_mci *host = slot->host;
	struct dw_mci_rtd129x_priv *priv = host->priv;
	struct mmc_host *mmc = slot->mmc;
	u32 window = 0;
	int i, best;

	for (i = 0; i < RTD129X_PHASE_RANGE; i++) {
		rtd129x_emmc_set_phase(host, priv, priv->vp0, i);
		if (!mmc_send_tuning(mmc, opcode, NULL))
			window |= BIT(i);
	}

	best = rtd129x_emmc_search_best_phase(window);
	if (best < 0) {
		dev_warn(host->dev, "HS200 tuning failed: no RX phase candidate passed\n");
		rtd129x_emmc_set_phase(host, priv, priv->vp0, 0);
		return best;
	}

	rtd129x_emmc_set_phase(host, priv, priv->vp0, best);
	dev_info(host->dev, "HS200 tuning: RX window=0x%08x, phase=0x%x\n", window, best);

	return 0;
}

static const struct dw_mci_drv_data rtd129x_drv_data = {
	.init = dw_mci_rtd129x_init,
	.set_ios = dw_mci_rtd129x_set_ios,
	.execute_tuning = dw_mci_rtd129x_execute_tuning,
};

static const struct of_device_id dw_mci_rtd129x_match[] = {
	{ .compatible = "realtek,rtd1295-dw-mshc", .data = &rtd129x_drv_data },
	{},
};
MODULE_DEVICE_TABLE(of, dw_mci_rtd129x_match);

static int dw_mci_rtd129x_probe(struct platform_device *pdev)
{
	const struct dw_mci_drv_data *drv_data;
	const struct of_device_id *match;

	if (!pdev->dev.of_node)
		return -ENODEV;

	match = of_match_node(dw_mci_rtd129x_match, pdev->dev.of_node);
	drv_data = match->data;

	return dw_mci_pltfm_register(pdev, drv_data);
}

static struct platform_driver dw_mci_rtd129x_pltfm_driver = {
	.probe = dw_mci_rtd129x_probe,
	.remove = dw_mci_pltfm_remove,
	.driver = {
		.name = "dwmmc_rtd129x",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = dw_mci_rtd129x_match,
		.pm = &dw_mci_pltfm_pmops,
	},
};
module_platform_driver(dw_mci_rtd129x_pltfm_driver);

MODULE_DESCRIPTION("Realtek RTD1295/RTD1296 Specific DW-MSHC Driver Extension");
MODULE_LICENSE("GPL v2");
