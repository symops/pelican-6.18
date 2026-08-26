// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek RTD1295/RTD1296 AHCI SATA platform driver
 *
 * Minimal glue on top of the standard AHCI platform driver: the RTD129x
 * AHCI block needs two vendor-extension register writes (offsets 0xf20 and
 * 0xC in the AHCI MMIO window, both undocumented outside the vendor tree)
 * before the SATA PHY will ever report a link. Without them the port stays
 * at SStatus 0 forever, silently, and the AHCI IRQ never fires even on
 * physical hotplug.
 *
 * Forward-ported from the vendor 4.9.330 tree's drivers/ata/ahci_rtk.c,
 * the RTD129X-only code path in rtk_sata_init(). The rest of that 757-line
 * driver (RTD1619/RTD1319 variants, runtime power-save, sysfs presence
 * attributes) is not needed for RTD1295/RTD1296 bring-up.
 *
 * RTD1296 (WD My Cloud Home Duo) additionally wires up a second SATA lane
 * (port 1). Both ports need the same per-port "analog PHY power domain is
 * still in hardware reset" fix that port 0 needed on RTD1295 -- see
 * ahci_rtd1295_ports_1295[]/ahci_rtd1295_ports_1296[] below for the
 * per-compatible-string register tables. RTD1295 (single-bay, port 0 only)
 * keeps exactly the register pokes already proven on real hardware;
 * nothing here changes its behaviour.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/ahci_platform.h>
#include <linux/libata.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "ahci.h"

#define DRV_NAME "ahci_rtd1295"

/*
 * CRT (0x98000000+) is a shared SoC-wide register bank -- this driver isn't
 * its only consumer. drivers/mmc/host/dw_mmc-rtd129x.c's eMMC clock-gate
 * enable lands in the exact same physical register as this driver's port-0
 * clock-gate enable (CRT+0x0C, different bits), and this driver's own probe
 * gets invoked repeatedly (deferred-probe retries -- visible in the boot
 * log as several repeats of "sata clocks enabled..." before the AHCI host
 * actually comes up). Without a lock around the read-modify-write, one
 * driver's write can land between the other's read and write and get lost
 * (a torn/lost update on a real hardware register, not just a data race) --
 * observed on real Duo hardware as SATA clocks going mysteriously dark by
 * the time ahci_rtd1295_init_work() (which runs ~800ms-11s after probe, via
 * schedule_delayed_work()) actually touches the AHCI controller again,
 * hanging deep inside a register-poll loop. Exported so dw_mmc-rtd129x.c
 * can take the same lock around its own CRT pokes.
 */
DEFINE_SPINLOCK(rtd129x_crt_lock);
EXPORT_SYMBOL_GPL(rtd129x_crt_lock);

/*
 * Vendor-extension registers beyond the standard AHCI register block.
 * 0xf20 sits outside the DT "reg" window on purpose (kept narrow so it
 * doesn't overlap the sibling sata-phy node's own MMIO region), so it is
 * mapped separately and non-exclusively below, same as the vendor driver
 * does for its own one-off CRT register pokes.
 */
#define RTD1295_MASK_ERR_SEL		0xf20
#define RTD1295_MASK_ERR_SEL_VAL	0x3c300
#define RTD1295_PORT_MAP_FIX		0xC

/*
 * Drive-bay power enable GPIO, board-specific (from the vendor DTS's
 * ahci_sata/sata-port@N { gpios = <&rtk_misc_gpio N 1 1>; }). The vendor
 * driver drives this high in ahci_rtk_probe() before any link bring-up is
 * attempted; without it the SATA link stays down forever on a cold boot
 * (the bay has no power, so there is nothing to negotiate OOB signaling
 * with). No mainline gpio-rtd129x driver/DT binding exists yet, so this is
 * a raw MMIO poke, same pattern as the vbus gpio19 poke in the initramfs
 * init script for USB -- except this one has to happen here, in-kernel
 * before ahci_platform_init_host(), because unlike USB hotplug, this
 * controller does not appear to generate a hotplug IRQ for a drive that
 * shows up after the initial COMRESET attempt.
 *
 * rtk_misc_gpio bank registers (base 0x9801b100). Bank 0 (gpio 0-31) is
 * DIR/DATO at +0x00/+0x10; bank 1 (gpio 32-63) is +0x04/+0x14 (see the
 * vendor drivers/gpio/gpio-rtd129x.c GPIO_REG_OFST()/reg_dir_off[]/
 * reg_dato_off[] tables -- id>>5 selects the bank, id&0x1f the bit).
 */
#define RTD_MISC_GPIO_BASE		0x9801b100
#define RTD_MISC_GPIO_DIR(bank)		(RTD_MISC_GPIO_BASE + 0x00 + 4 * (bank))
#define RTD_MISC_GPIO_DATO(bank)	(RTD_MISC_GPIO_BASE + 0x10 + 4 * (bank))

/*
 * Per-port "analog PHY power domain is still in hardware reset" + "drive
 * bay has no power" fix. RTD1295 (Monarch, single bay) needs it for port 0
 * only; RTD1296 (WD My Cloud Home Duo, two bays) needs it for both ports,
 * at different CRT registers -- the second SATA lane lives in a separate
 * CRT reset bank entirely (see vendor include/dt-bindings/reset/
 * rtd1295-reset.h: RTD1295_CRT_RSTN_SATA_PHY_POW_0 is REG_BANK_1 (CRT+0x00)
 * bit 10; RTD1295_CRT_RSTN_SATA_PHY_POW_1 is REG_BANK_4 (CRT+0x50) bit 7 --
 * bank->register offsets from the vendor 4.9.330 clk-rtd1295-cc.c
 * cc_reset_banks[] table).
 *
 * Confirmed against a real WD My Cloud Home Duo (RTD1296) vendor 4.9.330
 * boot log with both bays populated: the vendor ahci_rtk driver's per-port
 * reset-deassert loop touches SATA_n/SATA_PHY_n (port0: CRT+0x00 bits 5,7;
 * port1: CRT+0x50 bits 10,9) -- crt register read back
 * 0xaf80e355 -> 0xaf80e375 -> 0xaf80e3f5 for port 0, and 0x81f -> 0xc1f ->
 * 0xe1f for port 1. The PHY_POW bit is left alone by that loop and only
 * gets deasserted later, lazily, from phy_power_on(): the same log shows
 * "power on phy1" flipping CRT+0x50 bit 7 (0xe1f -> 0xe9f) and "power on
 * phy0" flipping CRT+0x00 bit 10 (0xaf80e3f5 -> 0xaf80e7f5) -- both ports
 * then link at 6.0 Gbps.
 *
 * Initially only the POW bit was reproduced here (deasserted eagerly in
 * probe() instead of lazily in phy_power_on(), on the theory it was
 * equivalent). On real Duo hardware that was NOT enough: both ports
 * stalled with continuous "mdio busy" for their entire PHY init (~3.3s
 * each), then the shared AHCI register block itself read back as
 * bus-fault poison (0xdeadbeef). Root cause: this Duo unit's bootloader
 * boots from eMMC, not SATA, so unlike Monarch (which always boots from
 * SATA and deasserts these as a side effect of its own boot path), it
 * never deasserts SATA_n/SATA_PHY_n on either CRT bank at all -- both
 * lanes stay held in hardware reset from cold boot. The vendor DTS
 * (rtd-129x-sata.dtsi, per-port `resets =` on ahci_sata/sata-port@N) wires
 * these through the reset-controller framework; this driver instead pokes
 * them directly here, same pattern as the POW bit, since the DT binding
 * used for this board doesn't route per-port resets and phy-rtk-sata.c
 * doesn't consume a reset_control for them either.
 *
 * The drive-bay power GPIO is a defensive belt-and-suspenders poke, same
 * pattern as Monarch's gpio18: on the Duo hardware log the bay was already
 * powered by the FSBL ("[SATA] Hardisk exist! turn on power", well before
 * Linux boots), so this may be a no-op there, but costs nothing to keep
 * for boot paths where it isn't. Monarch's board wires the single bay to
 * misc_gpio 18; the WD My Cloud Home Duo's own board DT (vendor
 * rtd-1296-pelican-1GB.dts, ahci_sata/sata-port@{0,1}) wires its two bays
 * to misc_gpio 60 and 62 -- unrelated to the generic RTD1296 reference
 * board's gpio56/iso15 in rtd-1296-sata.dtsi, which this device doesn't
 * use.
 */
struct ahci_rtd1295_port_quirk {
	u32	crt_reg;	/* absolute CRT reset-bank physical address */
	u32	crt_pow_bit;	/* SATA_PHY_POW_n reset */
	u32	crt_sata_bit;	/* SATA_n reset */
	u32	crt_phy_bit;	/* SATA_PHY_n reset */
	u32	clk_reg;	/* absolute CRT clock-gate physical address */
	u32	clk_bit0;	/* SATA_n clock gate */
	u32	clk_bit1;	/* SATA_ALIVE_n clock gate */
	u32	gpio;		/* rtk_misc_gpio number, drive bay power */
};

/*
 * SATA_n/SATA_ALIVE_n clock gates (CRT clock-gate register, not the reset
 * bank above -- see vendor 4.9.330 drivers/clk/realtek/clk-rtd1295-cc.c
 * cc_gates[]: SATA_0 is CRT+0x0C bit 2, SATA_ALIVE_0 is CRT+0x0C bit 7;
 * SATA_1 is CRT+0x10 bit 25, SATA_ALIVE_1 is CRT+0x10 bit 26). On Monarch
 * these were already enabled by the bootloader (confirmed by devmem
 * readback), so the driver never had to touch them. On the WD My Cloud
 * Home Duo unit this was tested on, they were NOT: the AHCI HBA's own
 * registers read back as raw bus-fault garbage (0xdeadbeef) until these
 * are explicitly enabled here -- "masking port_map 0xdeadbeef -> 0x3" and
 * "Controller reset failed (0xdeadbeef)" in the boot log, plus an
 * ~9-second "mdio busy" stall during PHY init (consistent with the SATA
 * block running off some slow fallback path instead of its real clock).
 * Enabling both ports' gates unconditionally here is a no-op on hardware
 * where the bootloader already did it (OR of already-set bits), so this
 * is applied for both RTD1295 and RTD1296.
 */
static const struct ahci_rtd1295_port_quirk ahci_rtd1295_ports_1295[] = {
	{ .crt_reg = 0x98000000, .crt_pow_bit = 10, .crt_sata_bit = 5, .crt_phy_bit = 7,
	  .clk_reg = 0x9800000C, .clk_bit0 = 2, .clk_bit1 = 7, .gpio = 18 },
};

static const struct ahci_rtd1295_port_quirk ahci_rtd1295_ports_1296[] = {
	{ .crt_reg = 0x98000000, .crt_pow_bit = 10, .crt_sata_bit = 5, .crt_phy_bit = 7,
	  .clk_reg = 0x9800000C, .clk_bit0 = 2,  .clk_bit1 = 7,  .gpio = 60 },
	{ .crt_reg = 0x98000050, .crt_pow_bit = 7, .crt_sata_bit = 10, .crt_phy_bit = 9,
	  .clk_reg = 0x98000010, .clk_bit0 = 25, .clk_bit1 = 26, .gpio = 62 },
};

static void ahci_rtd1295_port_quirks_apply(struct device *dev,
				const struct ahci_rtd1295_port_quirk *ports,
				unsigned int nports)
{
	void __iomem *reg;
	unsigned long flags;
	u32 val;
	unsigned int i;

	for (i = 0; i < nports; i++) {
		const struct ahci_rtd1295_port_quirk *p = &ports[i];
		u32 bank = p->gpio >> 5;
		u32 bit = p->gpio & 0x1f;

		reg = ioremap(p->clk_reg, 4);
		if (!reg) {
			dev_warn(dev, "can't map crt clock-gate register for port %u\n", i);
		} else {
			spin_lock_irqsave(&rtd129x_crt_lock, flags);
			val = readl(reg);
			writel(val | BIT(p->clk_bit0) | BIT(p->clk_bit1), reg);
			spin_unlock_irqrestore(&rtd129x_crt_lock, flags);
			iounmap(reg);
		}

		reg = ioremap(p->crt_reg, 4);
		if (!reg) {
			dev_warn(dev, "can't map crt reset register for port %u\n", i);
		} else {
			spin_lock_irqsave(&rtd129x_crt_lock, flags);
			val = readl(reg);
			writel(val | BIT(p->crt_pow_bit) | BIT(p->crt_sata_bit) |
				     BIT(p->crt_phy_bit), reg);
			spin_unlock_irqrestore(&rtd129x_crt_lock, flags);
			iounmap(reg);
		}

		reg = ioremap(RTD_MISC_GPIO_DIR(bank), 4);
		if (!reg) {
			dev_warn(dev, "can't map misc-gpio dir register for port %u\n", i);
		} else {
			val = readl(reg);
			writel(val | BIT(bit), reg);
			iounmap(reg);
		}

		reg = ioremap(RTD_MISC_GPIO_DATO(bank), 4);
		if (!reg) {
			dev_warn(dev, "can't map misc-gpio dato register for port %u\n", i);
		} else {
			val = readl(reg);
			writel(val | BIT(bit), reg);
			iounmap(reg);
		}

		dev_info(dev, "port %u: sata clocks enabled, sata/phy/pow resets deasserted, drive bay gpio%u driven high\n",
			 i, p->gpio);
	}
}

static const struct ata_port_info ahci_rtd1295_port_info = {
	.flags		= AHCI_FLAG_COMMON,
	.pio_mask	= ATA_PIO4,
	.udma_mask	= ATA_UDMA6,
	.port_ops	= &ahci_platform_ops,
};

static const struct scsi_host_template ahci_platform_sht = {
	AHCI_SHT(DRV_NAME),
};

/*
 * The vendor driver never calls ahci_platform_init_host() synchronously in
 * probe(): unless a "hostinit-mode" DT property is set (the Monarch board
 * doesn't set it), it defers host init -- i.e. the first COMRESET attempt
 * -- by 800ms via schedule_delayed_work(). That gap is load-bearing: with
 * it removed (host init called synchronously, as ahci_platform normally
 * does), the SATA link only ever comes up if something else (U-Boot's own
 * AHCI probe on its normal boot path) already trained/powered the drive
 * beforehand. On a genuinely cold link -- e.g. the USB-rescue-button boot
 * path, which skips U-Boot's SATA probe entirely -- the drive bay and PHY
 * apparently need this settling time before the first hardreset can
 * succeed. Reproduced here for the same reason.
 */
#define RTD1295_HOSTINIT_DELAY_MS	800

struct ahci_rtd1295_data {
	struct platform_device *pdev;
	struct ahci_host_priv *hpriv;
	struct delayed_work init_work;
};

static void ahci_rtd1295_init_work(struct work_struct *work)
{
	struct ahci_rtd1295_data *data =
		container_of(work, struct ahci_rtd1295_data, init_work.work);
	int rc;

	rc = ahci_platform_init_host(data->pdev, data->hpriv,
				      &ahci_rtd1295_port_info,
				      &ahci_platform_sht);
	if (rc)
		dev_err(&data->pdev->dev,
			"deferred host init failed: %d\n", rc);
}

static int ahci_rtd1295_quirk_init(struct platform_device *pdev,
				    struct ahci_host_priv *hpriv)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem *ext;
	u32 val;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	ext = ioremap(res->start + RTD1295_MASK_ERR_SEL, 4);
	if (!ext)
		return -ENOMEM;
	dev_info(dev, "rx error select to mac original\n");
	writel(RTD1295_MASK_ERR_SEL_VAL, ext);
	iounmap(ext);

	val = readl(hpriv->mmio + RTD1295_PORT_MAP_FIX);
	writel(val | 0x3, hpriv->mmio + RTD1295_PORT_MAP_FIX);

	return 0;
}

static int ahci_rtd1295_probe(struct platform_device *pdev)
{
	struct ahci_rtd1295_data *data;
	struct ahci_host_priv *hpriv;
	const struct ahci_rtd1295_port_quirk *ports;
	unsigned int nports;
	int rc;

	ports = of_device_get_match_data(&pdev->dev);
	if (!ports)
		return -ENODEV;
	nports = (ports == ahci_rtd1295_ports_1296) ?
		 ARRAY_SIZE(ahci_rtd1295_ports_1296) :
		 ARRAY_SIZE(ahci_rtd1295_ports_1295);
	ahci_rtd1295_port_quirks_apply(&pdev->dev, ports, nports);

	hpriv = ahci_platform_get_resources(pdev, AHCI_PLATFORM_GET_RESETS);
	if (IS_ERR(hpriv))
		return PTR_ERR(hpriv);

	rc = ahci_platform_enable_resources(hpriv);
	if (rc)
		return rc;

	rc = ahci_rtd1295_quirk_init(pdev, hpriv);
	if (rc)
		goto disable_resources;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		rc = -ENOMEM;
		goto disable_resources;
	}
	data->pdev = pdev;
	data->hpriv = hpriv;
	INIT_DELAYED_WORK(&data->init_work, ahci_rtd1295_init_work);
	platform_set_drvdata(pdev, data);
	schedule_delayed_work(&data->init_work,
			      msecs_to_jiffies(RTD1295_HOSTINIT_DELAY_MS));

	return 0;

disable_resources:
	ahci_platform_disable_resources(hpriv);
	return rc;
}

static SIMPLE_DEV_PM_OPS(ahci_rtd1295_pm_ops, ahci_platform_suspend,
			  ahci_platform_resume);

static const struct of_device_id ahci_rtd1295_of_match[] = {
	{ .compatible = "realtek,rtd1295-ahci", .data = ahci_rtd1295_ports_1295 },
	{ .compatible = "realtek,rtd1296-ahci", .data = ahci_rtd1295_ports_1296 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ahci_rtd1295_of_match);

static struct platform_driver ahci_rtd1295_driver = {
	.probe = ahci_rtd1295_probe,
	.remove = ata_platform_remove_one,
	.shutdown = ahci_platform_shutdown,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = ahci_rtd1295_of_match,
		.pm = &ahci_rtd1295_pm_ops,
	},
};
module_platform_driver(ahci_rtd1295_driver);

MODULE_DESCRIPTION("Realtek RTD1295/RTD1296 AHCI SATA platform driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ahci_rtd1295");
