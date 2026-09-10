// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cosmetic power-off for WD My Cloud Home (Monarch, RTD1295) and WD My
 * Cloud Home Duo (Pelican, RTD1296).
 *
 * Neither this SoC's mainline support nor the vendor 4.9.330 GPL source
 * implements a real hardware power-off for this board: the DTS used to
 * carry a "realtek,rtd129x-coolboot-poweroff" node, but that compatible
 * string matches no driver anywhere, mainline or vendor (checked both
 * trees directly -- see README.md). The vendor's own g2227-regulator.c
 * .shutdown() hook (already ported byte-for-byte,
 * drivers/regulator/g2227-regulator.c) only sequences down the PMIC's
 * own core-voltage rails as the SoC's last act before something
 * EXTERNAL is expected to cut the board's main 12V feed -- and no such
 * external trigger (a "power hold"/"pwr_en" GPIO) was found anywhere in
 * either board's vendor DTS. `halt`/`poweroff` on this hardware
 * therefore both just park the CPU with the board still fully powered:
 * fan spinning, front LED lit, USB VBUS live.
 *
 * This driver does not attempt to cut real board power -- there is
 * currently no known way to do that from software on this hardware. It
 * quiets the things under this SoC's own direct control that otherwise
 * stay conspicuously live.
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
 *    for). Monarch's stock-firmware log shows a completely different
 *    split: one port on a single misc-gpio bit (19), the other two
 *    (sharing one physical port) on rtk_iso_gpio line 1. misc-gpio has
 *    no mainline gpiolib controller in this port (raw MMIO only, same
 *    style as the PWM OCD poke above and ahci_rtd1295.c's own SB2 gate)
 *    so wd,misc-gpio-vbus-bit's DIR/DATO windows are optional raw pokes,
 *    independent of and in addition to wd,usb-vbus-gpios's gpiod array.
 *
 * Real disk spin-down is handled separately, through the SCSI layer's
 * own existing sd_shutdown() mechanism (the manage_shutdown sysfs
 * attribute, enabled via udev at boot) rather than reimplemented here.
 */

#include <linux/bitops.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#define WDMC_PWM_OCD		0x0
#define WDMC_MAX_PWM_CHANNELS	4

struct wdmc_poweroff_data {
	void __iomem *pwm_base;
	struct gpio_descs *usb_vbus_gpios;
	void __iomem *misc_gpio_dato;
	void __iomem *misc_gpio_dir;
	unsigned int misc_gpio_vbus_bit;
	unsigned int pwm_channels[WDMC_MAX_PWM_CHANNELS];
	unsigned int n_pwm_channels;
};

static struct wdmc_poweroff_data *wdmc_poweroff;

static void wdmc_poweroff_handler(void)
{
	unsigned int i;
	u32 val;

	if (!wdmc_poweroff)
		return;

	for (i = 0; i < wdmc_poweroff->n_pwm_channels; i++) {
		unsigned int shift = wdmc_poweroff->pwm_channels[i] * 8;

		val = readl(wdmc_poweroff->pwm_base + WDMC_PWM_OCD);
		val &= ~(0xffU << shift);
		writel(val, wdmc_poweroff->pwm_base + WDMC_PWM_OCD);
	}

	if (wdmc_poweroff->misc_gpio_dir && wdmc_poweroff->misc_gpio_dato) {
		unsigned int bit = wdmc_poweroff->misc_gpio_vbus_bit;

		/* Force the pad to output mode ourselves, same reasoning
		 * as the gpiod lines below: don't depend on anything
		 * upstream having already done it.
		 */
		val = readl(wdmc_poweroff->misc_gpio_dir);
		writel(val | BIT(bit), wdmc_poweroff->misc_gpio_dir);

		val = readl(wdmc_poweroff->misc_gpio_dato);
		val &= ~BIT(bit);
		writel(val, wdmc_poweroff->misc_gpio_dato);
	}

	if (wdmc_poweroff->usb_vbus_gpios) {
		/*
		 * Confirmed on real hardware that this needs to actually
		 * be held low, not just pulsed -- the machine parks (IRQs
		 * off, CPUs stopped) shortly after this returns, which is
		 * exactly what leaves it held indefinitely from here on.
		 */
		for (i = 0; i < wdmc_poweroff->usb_vbus_gpios->ndescs; i++)
			gpiod_direction_output(wdmc_poweroff->usb_vbus_gpios->desc[i], 0);
	}
}

static int wdmc_poweroff_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wdmc_poweroff_data *data;
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

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		data->misc_gpio_dato = devm_ioremap(dev, res->start, resource_size(res));
		if (!data->misc_gpio_dato)
			return -ENOMEM;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (res) {
		data->misc_gpio_dir = devm_ioremap(dev, res->start, resource_size(res));
		if (!data->misc_gpio_dir)
			return -ENOMEM;
	}

	if (data->misc_gpio_dir && data->misc_gpio_dato) {
		ret = of_property_read_u32(dev->of_node, "wd,misc-gpio-vbus-bit",
					    &data->misc_gpio_vbus_bit);
		if (ret || data->misc_gpio_vbus_bit >= 32)
			return dev_err_probe(dev, -EINVAL,
					      "bad or missing wd,misc-gpio-vbus-bit\n");
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

	wdmc_poweroff = data;
	pm_power_off = wdmc_poweroff_handler;
	platform_set_drvdata(pdev, data);

	dev_info(dev, "registered as pm_power_off (%u pwm channel(s), %u usb vbus gpio(s))\n",
		 data->n_pwm_channels,
		 data->usb_vbus_gpios ? data->usb_vbus_gpios->ndescs : 0);

	return 0;
}

static void wdmc_poweroff_remove(struct platform_device *pdev)
{
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

MODULE_DESCRIPTION("Cosmetic power-off (LED/fan/USB VBUS) for WD My Cloud Home / Duo");
MODULE_LICENSE("GPL");
