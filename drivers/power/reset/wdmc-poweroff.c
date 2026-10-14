// SPDX-License-Identifier: GPL-2.0-only
/*
 * Power-off for WD My Cloud Home (Monarch, RTD1295) and WD My Cloud Home
 * Duo (Pelican, RTD1296), as WD's 4.9 kernel does it: the audio CPU
 * firmware, asked to "suspend to coolboot", puts the G2227 PMIC to sleep,
 * and the PMIC then turns off the rails whose sleep mode the g2227
 * regulator driver set to off at shutdown (the DTS's
 * regulator-state-coldboot nodes -- that driver must be loaded, it is a
 * module on Pelican). Before that this driver turns off what the SoC
 * drives directly and disarms the watchdog. Ported from
 * symops/monarch-6.18 PR #6; NOT yet verified on real Duo hardware:
 *
 *  - PWM channel(s) for the SYS LED (and fan, Duo only): the OCD
 *    register (offset 0x0 within the pwm@d0 block) with 0 written to a
 *    channel's 8-bit field is this hardware's own encoding for
 *    "disabled" (see pwm-rtd129x.c's get_state(): "ocd == 0 is how
 *    apply() encodes disabled") -- the exact same effect pwm_disable()
 *    produces on that channel, just reached directly instead of through
 *    a pwm_device this driver doesn't own. This register packs all 4
 *    channels' OCD fields into one 32-bit word, so only 4 bytes need
 *    mapping regardless of how many channels are listed. Left as a raw
 *    devm_ioremap() poke (not devm_platform_ioremap_resource()) because
 *    the pwm@d0 block itself is already exclusively owned by the real
 *    pwm-rtd129x.c driver (pwm-leds/pwm-fan); a second exclusive claim
 *    on the same page would collide with it the same way an earlier
 *    mistake did in the Reset button work.
 *
 *  - USB VBUS, on boards where the physical port-power GPIO line(s) have
 *    been empirically confirmed: the only way this was ever pinned down
 *    was by reading the vendor's own rtk_usb_manager.c driver together
 *    with a captured *stock-firmware* boot log from the exact physical
 *    unit being ported (not the generic reference-board DTS, which
 *    turned out to list a different, incomplete GPIO set than what
 *    retail firmware actually uses on either board), then confirming
 *    each candidate line on real hardware by holding it low for several
 *    seconds -- a brief 1-second pulse, tried first, swept every single
 *    bit of misc-gpio (both 32-bit banks) and every rtk_iso_gpio line
 *    with zero visible effect on VBUS; a 4-second hold on the exact same
 *    lines that had just "failed" is what actually cut power, and
 *    restored it again when driven back high.
 *
 *    Duo's two external ports are both on rtk_iso_gpio (lines 34/26,
 *    bottom/top bay), requested as normal gpiod consumers --
 *    rtk_iso_gpio is already a proper mainline gpiolib controller
 *    (drivers/gpio/gpio-rtd.c) with no competing exclusive claim on
 *    either line (only line 20, the Reset button, is otherwise spoken
 *    for). Monarch's split is different (one port on misc-gpio) and is
 *    handled by that board's own copy of this driver; nothing misc-gpio
 *    related is needed here.
 *
 * Real disk spin-down is handled separately, through the SCSI layer's
 * own existing sd_shutdown() mechanism (the manage_shutdown sysfs
 * attribute, enabled via udev at boot) rather than reimplemented here.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/mfd/g2227.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#define WDMC_PWM_OCD		0x0
#define WDMC_MAX_PWM_CHANNELS	4

#define WDMC_WDT_EN_MASK	0xff
#define WDMC_WDT_DISABLED	0xa5

#define WDMC_UART_MCR		0x10
#define WDMC_UART_LSR		0x14
#define WDMC_UART_MCR_LOOP	BIT(4)
#define WDMC_UART_MCR_DTR	BIT(0)
#define WDMC_UART_MCR_RTS	BIT(1)
#define WDMC_UART_MCR_AFCE	BIT(5)
#define WDMC_UART_LSR_TEMT	BIT(6)

/* How many times the power-off request is sent (1 s each) if the audio CPU does not answer */
#define WDMC_IPC_ATTEMPTS	3

/* struct rtk_ipc_shm (WD 4.9 rtk_ipc_shm.h) at 0xc4 into the RPC page, big-endian */
#define WDMC_IPC_AUDIO_RPC_FLAG	(0xc4 + 0x0c)
#define WDMC_IPC_SUSPEND_MASK	(0xc4 + 0x10)
#define WDMC_IPC_SUSPEND_FLAG	(0xc4 + 0x14)

struct wdmc_poweroff_data {
	void __iomem *pwm_base;
	struct gpio_descs *usb_vbus_gpios;
	void __iomem *hdd_dir;
	void __iomem *hdd_dato;
	u32 hdd_mask;
	void __iomem *wdt_ctl;
	void __iomem *uart;
	void __iomem *ipc;
	struct device_node *pmic;
	bool prepare_ran;
	unsigned int pwm_channels[WDMC_MAX_PWM_CHANNELS];
	unsigned int n_pwm_channels;
};

static struct wdmc_poweroff_data *wdmc_poweroff;

/* What the SoC drives directly: LED/fan PWM, USB VBUS, the drive-bay supplies, the watchdog */
static void wdmc_quiesce(struct wdmc_poweroff_data *d)
{
	unsigned int i;
	u32 val;

	for (i = 0; i < d->n_pwm_channels; i++) {
		unsigned int shift = d->pwm_channels[i] * 8;

		val = readl(d->pwm_base + WDMC_PWM_OCD);
		val &= ~(0xffU << shift);
		writel(val, d->pwm_base + WDMC_PWM_OCD);
	}

	if (d->usb_vbus_gpios) {
		/*
		 * Confirmed on real hardware that this needs to actually
		 * be held low, not just pulsed -- the machine parks (IRQs
		 * off, CPUs stopped) shortly after this returns, which is
		 * exactly what leaves it held indefinitely from here on.
		 */
		for (i = 0; i < d->usb_vbus_gpios->ndescs; i++)
			gpiod_direction_output(d->usb_vbus_gpios->desc[i], 0);
	}

	if (d->hdd_mask) {
		/* Drive-bay supplies (active high); the disks were stopped by sd_shutdown() */
		val = readl(d->hdd_dir);
		writel(val | d->hdd_mask, d->hdd_dir);
		val = readl(d->hdd_dato);
		writel(val & ~d->hdd_mask, d->hdd_dato);
	}

	if (d->wdt_ctl) {
		val = readl(d->wdt_ctl) & ~WDMC_WDT_EN_MASK;
		writel(val | WDMC_WDT_DISABLED, d->wdt_ctl);
	}
}

/* Ask the audio CPU to suspend to coolboot; true if it took the request (the board is about to lose power) */
static bool wdmc_acpu_poweroff(struct wdmc_poweroff_data *d)
{
	void __iomem *ipc = d->ipc;
	void __iomem *uart = d->uart;
	int attempt, i;
	u32 mcr = 0;

	if (!ipc)
		return false;

	/* The firmware prints on UART0 before taking the request: loop it back to drain */
	if (uart) {
		for (i = 0; i < 10000; i++) {
			if (readl(uart + WDMC_UART_LSR) & WDMC_UART_LSR_TEMT)
				break;
			udelay(10);
		}
		mcr = readl(uart + WDMC_UART_MCR);
	}

	/*
	 * Suspend version 2, author SCPU, coolboot; the firmware clears the
	 * request. On real hardware the audio CPU's RPC loop sometimes does not
	 * run at all (it does not even acknowledge the SB2 doorbell) and then no
	 * number of retries helps, hence the PMIC fallback in the caller.
	 */
	for (attempt = 1; attempt <= WDMC_IPC_ATTEMPTS; attempt++) {
		if (uart)
			/* loopback ties CTS to RTS: keep RTS up and auto flow control off so TX cannot stall */
			writel((mcr | WDMC_UART_MCR_LOOP | WDMC_UART_MCR_RTS | WDMC_UART_MCR_DTR) &
			       ~WDMC_UART_MCR_AFCE, uart + WDMC_UART_MCR);
		iowrite32be(0x00020000, ipc + WDMC_IPC_SUSPEND_MASK);
		iowrite32be(0x40000002, ipc + WDMC_IPC_SUSPEND_FLAG);
		iowrite32be(0xdeadffff, ipc + WDMC_IPC_AUDIO_RPC_FLAG);
		for (i = 0; i < 1000 && ioread32be(ipc + WDMC_IPC_AUDIO_RPC_FLAG); i++)
			mdelay(1);
		if (i < 1000)
			break;
		/* Not taken: undo the loopback to say so, then ask again */
		if (uart)
			writel(mcr, uart + WDMC_UART_MCR);
		pr_emerg("wdmc-poweroff: attempt %d/%d: the audio CPU did not take the request: rpc=%08x mask=%08x flag=%08x\n",
			 attempt, WDMC_IPC_ATTEMPTS,
			 ioread32be(ipc + WDMC_IPC_AUDIO_RPC_FLAG),
			 ioread32be(ipc + WDMC_IPC_SUSPEND_MASK),
			 ioread32be(ipc + WDMC_IPC_SUSPEND_FLAG));
	}
	/* Still running means the power was not cut: undo the loopback to say why */
	if (uart)
		writel(mcr, uart + WDMC_UART_MCR);
	pr_emerg("wdmc-poweroff: the audio CPU %s the power-off request (attempt %d, %d ms): rpc=%08x mask=%08x flag=%08x\n",
		 attempt <= WDMC_IPC_ATTEMPTS ? "took" : "never took", attempt, i,
		 ioread32be(ipc + WDMC_IPC_AUDIO_RPC_FLAG),
		 ioread32be(ipc + WDMC_IPC_SUSPEND_MASK),
		 ioread32be(ipc + WDMC_IPC_SUSPEND_FLAG));
	return attempt <= WDMC_IPC_ATTEMPTS;
}

/*
 * Switch every rail off through the PMIC. Its ONOFF register holds one bit per
 * rail, so one byte written over I2C removes the power from the whole board,
 * 3.3 V and the ISO domain included, at the same moment. Confirmed on a Duo:
 * 0 W, and the board starts normally when power is applied again.
 *
 * Leaving the ISO domain (DC5) and 3.3 V (DC1) on while the rest collapses is
 * NOT an alternative: tried on the same board it left it drawing ~4 W with the
 * fan at full speed and the flash busy, because the isolation the audio CPU
 * performs before its coolboot was missing.
 *
 * Returns only if the write failed, or if the board is somehow still running.
 */
static void wdmc_pmic_cut(struct wdmc_poweroff_data *d)
{
	struct i2c_client *pmic;
	int onoff, ret;

	if (!d->pmic)
		return;
	pmic = of_find_i2c_device_by_node(d->pmic);
	if (!pmic) {
		pr_emerg("wdmc-poweroff: no PMIC on I2C (the PMIC modules are not loaded?), cannot cut the rails\n");
		return;
	}
	onoff = i2c_smbus_read_byte_data(pmic, G2227_REG_ONOFF);
	pr_emerg("wdmc-poweroff: PMIC ONOFF=%02x: switching every rail off\n", onoff);
	ret = i2c_smbus_write_byte_data(pmic, G2227_REG_ONOFF, 0);
	/* the board normally loses all power here */
	mdelay(500);
	pr_emerg("wdmc-poweroff: still running 0.5 s after the PMIC ONOFF write (ret=%d)\n", ret);
	put_device(&pmic->dev);
}

/*
 * Runs after device_shutdown() (the disks are stopped) and may sleep, with
 * interrupts on, so the PMIC can be reached over its normal I2C driver.
 * The audio CPU's coolboot is only the fallback: its RPC loop sometimes never
 * runs on real hardware (it does not even acknowledge the SB2 doorbell) and
 * the board then stays powered at ~1.7 W.
 */
static int wdmc_poweroff_prepare(struct sys_off_data *sd)
{
	struct wdmc_poweroff_data *d = sd->cb_data;

	d->prepare_ran = true;
	wdmc_quiesce(d);
	wdmc_pmic_cut(d);
	wdmc_acpu_poweroff(d);
	return NOTIFY_DONE;
}

static void wdmc_poweroff_handler(void)
{
	if (!wdmc_poweroff)
		return;

	wdmc_quiesce(wdmc_poweroff);
	/* Normally the prepare handler above has already done the real work */
	if (!wdmc_poweroff->prepare_ran)
		wdmc_acpu_poweroff(wdmc_poweroff);

	for (;;)
		wfi();
}

static int wdmc_poweroff_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wdmc_poweroff_data *data;
	struct device_node *rmem;
	struct resource *res;
	int ret, i;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	data->pwm_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!data->pwm_base)
		return -ENOMEM;

	/* reg[1] watchdog control, reg[2] UART0: both optional */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res && !(data->wdt_ctl = devm_ioremap(dev, res->start, resource_size(res))))
		return -ENOMEM;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (res && !(data->uart = devm_ioremap(dev, res->start, resource_size(res))))
		return -ENOMEM;

	/*
	 * reg[3]/reg[4]: misc-gpio DIR/DATO of the bank holding the drive-bay
	 * supplies, wd,misc-gpio-hdd-bits: their bit numbers within that bank.
	 * Raw MMIO, like ahci_rtd1295.c, which raised these lines at boot.
	 */
	ret = of_property_count_u32_elems(dev->of_node, "wd,misc-gpio-hdd-bits");
	if (ret > 0) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
		if (!res)
			return dev_err_probe(dev, -EINVAL, "wd,misc-gpio-hdd-bits needs reg[3] (DIR)\n");
		data->hdd_dir = devm_ioremap(dev, res->start, resource_size(res));
		res = platform_get_resource(pdev, IORESOURCE_MEM, 4);
		if (!res)
			return dev_err_probe(dev, -EINVAL, "wd,misc-gpio-hdd-bits needs reg[4] (DATO)\n");
		data->hdd_dato = devm_ioremap(dev, res->start, resource_size(res));
		if (!data->hdd_dir || !data->hdd_dato)
			return -ENOMEM;
		for (i = 0; i < ret; i++) {
			u32 bit;

			of_property_read_u32_index(dev->of_node, "wd,misc-gpio-hdd-bits", i, &bit);
			if (bit >= 32)
				return dev_err_probe(dev, -EINVAL, "bad wd,misc-gpio-hdd-bits\n");
			data->hdd_mask |= BIT(bit);
		}
	}

	/* The RPC page must be no-map: the audio CPU reads it uncached */
	rmem = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (rmem) {
		struct resource ipc;

		ret = of_address_to_resource(rmem, 0, &ipc);
		of_node_put(rmem);
		if (ret)
			return dev_err_probe(dev, ret, "bad memory-region\n");
		data->ipc = devm_ioremap(dev, ipc.start, resource_size(&ipc));
		if (!data->ipc)
			return dev_err_probe(dev, -ENOMEM, "cannot map memory-region\n");
	}

	/*
	 * GPIOD_ASIS: don't touch the line's current state at probe time
	 * (the board is already up and these ports are already powered);
	 * the shutdown handler is the only place this ever gets driven.
	 */
	data->usb_vbus_gpios = devm_gpiod_get_array_optional(dev, "wd,usb-vbus",
							       GPIOD_ASIS);
	if (IS_ERR(data->usb_vbus_gpios))
		return dev_err_probe(dev, PTR_ERR(data->usb_vbus_gpios),
				      "failed to get usb-vbus gpios\n");

	ret = of_property_count_u32_elems(dev->of_node, "wd,pwm-off-channels");
	if (ret < 0 || ret > WDMC_MAX_PWM_CHANNELS)
		return dev_err_probe(dev, -EINVAL,
				      "bad or missing wd,pwm-off-channels\n");
	data->n_pwm_channels = ret;

	for (i = 0; i < ret; i++) {
		u32 ch;

		of_property_read_u32_index(dev->of_node, "wd,pwm-off-channels", i, &ch);
		if (ch >= WDMC_MAX_PWM_CHANNELS)
			return dev_err_probe(dev, -EINVAL,
					      "channel %u out of range\n", ch);
		data->pwm_channels[i] = ch;
	}

	if (pm_power_off)
		return dev_err_probe(dev, -EBUSY,
				      "pm_power_off already claimed\n");

	/* The PMIC's I2C adapter is a module that probes later: look the client up at power-off */
	data->pmic = of_parse_phandle(dev->of_node, "wd,pmic", 0);

	ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF_PREPARE,
					    SYS_OFF_PRIO_DEFAULT, wdmc_poweroff_prepare, data);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register the power-off prepare handler\n");

	wdmc_poweroff = data;
	pm_power_off = wdmc_poweroff_handler;
	platform_set_drvdata(pdev, data);

	dev_info(dev, "registered as pm_power_off (%u pwm channel(s), %u usb vbus gpio(s)%s)\n",
		 data->n_pwm_channels,
		 data->usb_vbus_gpios ? data->usb_vbus_gpios->ndescs : 0,
		 data->ipc ? ", audio CPU coolboot" : "");

	return 0;
}

static void wdmc_poweroff_remove(struct platform_device *pdev)
{
	struct wdmc_poweroff_data *data = platform_get_drvdata(pdev);

	of_node_put(data->pmic);
	if (pm_power_off == wdmc_poweroff_handler) {
		pm_power_off = NULL;
		wdmc_poweroff = NULL;
	}
}

static const struct of_device_id wdmc_poweroff_of_match[] = {
	{ .compatible = "wd,mycloud-home-poweroff" },
	{ }
};
MODULE_DEVICE_TABLE(of, wdmc_poweroff_of_match);

static struct platform_driver wdmc_poweroff_driver = {
	.probe = wdmc_poweroff_probe,
	.remove = wdmc_poweroff_remove,
	.driver = {
		.name = "wdmc-poweroff",
		.of_match_table = wdmc_poweroff_of_match,
	},
};
module_platform_driver(wdmc_poweroff_driver);

MODULE_DESCRIPTION("Power-off (audio CPU coolboot + LED/fan/USB VBUS) for WD My Cloud Home / Duo");
MODULE_LICENSE("GPL");
