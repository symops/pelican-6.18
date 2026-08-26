// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek RTD1295/RTD1296 fan tachometer (RPM readback)
 *
 * A separate, self-contained IP block from the PWM controller that drives
 * fan speed -- confirmed register-for-register against the vendor 4.9.330
 * tree's drivers/soc/realtek/common/rtk_fan.c ("realtek,rtk-fan"): counts
 * tach pulses over a fixed window (REG_TIMER_TV, in 90kHz reference-clock
 * ticks) and fires an interrupt when the window completes, latching the
 * pulse count into REG_COUNTER_CV for the driver to read back and convert
 * to RPM. This driver only ports that RPM-feedback half; fan speed
 * *control* is already handled by the generic mainline pwm-fan driver on
 * the same physical PWM channel (see the board DTS fan0 node) -- the
 * vendor's own rtk_fan.c also drives the PWM line itself, but there's no
 * need to duplicate that here.
 *
 * No mainline clock/reset provider exists for this SoC's CRT bits (same
 * situation as drivers/ata/ahci_rtd1295.c, drivers/mmc/host/
 * dw_mmc-rtd129x.c, etc.), so the clock gate and pinmux are raw register
 * pokes rather than DT clocks=/pinctrl-0= properties, matching those
 * drivers' established pattern. The clock gate lives in the same
 * CRT+0x10 register drivers/ata/ahci_rtd1295.c's port-1 clock-gate enable
 * writes (different bits), so it's protected by that driver's shared
 * rtd129x_crt_lock the same way drivers/mmc/host/dw_mmc-rtd129x.c's own
 * CRT+0x0C access already is.
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#define RTD_FAN_CTRL			0x00
#define RTD_FAN_CTRL_EN			BIT(0)
#define RTD_FAN_CTRL_INT_EN		BIT(1)

#define RTD_FAN_DEBOUNCE		0x04
#define RTD_FAN_DEBOUNCE_WEN		BIT(3)
#define RTD_FAN_DEBOUNCE_MASK		0x7

#define RTD_FAN_TIMER_TV		0x08
#define RTD_FAN_COUNTER_CV		0x10

#define RTD_FAN_TIMER_CLK_HZ		90000
#define RTD_FAN_TIMER_TV_DEFAULT	0x100000
#define RTD_FAN_DEBOUNCE_DEFAULT	0x0
#define RTD_FAN_FACTOR_DEFAULT		2

/* CRT registers (0x98000000+), shared with other CRT reset/clock-gate
 * consumers -- see drivers/ata/ahci_rtd1295.c and drivers/mmc/host/
 * dw_mmc-rtd129x.c for the same pattern.
 */
#define RTD129X_CRT_BASE		0x98000000
#define RTD129X_CRT_CLKEN2		0x10	/* FAN bit 29 */
#define RTD129X_CRT_CLKEN2_FAN_BIT	29

/* SB2 crossbar block (0x9801a000+), shared physical region with the
 * mainline rtd129x.dtsi "sb2: syscon@1a000" node (unused elsewhere in this
 * port). MUXPAD4 bits [23:22] select the function of the pin the vendor
 * DTS calls "gpio_9" -- confirmed against the vendor's own
 * drivers/pinctrl/realtek/pinctrl-rtd129x.h pmux table
 * (pmux_base=PMUX_BASE_SB2, pmux_regoff=0x910, pmux_regbit=22,
 * pmux_regbitmsk=0x3) and rtd-1295-pinctrl.dtsi's dc_fan_sensor_pins
 * (realtek,pins = "gpio_9", realtek,function = "dc_fan_sensor" ->
 * RTK_FUNCTION(0x2, "dc_fan_sensor")). No mainline pinctrl driver exists
 * for this SoC either (same situation as the eMMC pinmux in
 * dw_mmc-rtd129x.c's rtd129x_emmc_pinmux_init()), so poked directly.
 */
#define RTD129X_SB2_BASE		0x9801a000
#define RTD129X_SB2_MUXPAD4		0x910
#define RTD129X_SB2_MUXPAD4_FAN_SHIFT	22
#define RTD129X_SB2_MUXPAD4_FAN_MASK	0x3
#define RTD129X_SB2_MUXPAD4_FAN_SENSOR	0x2

extern spinlock_t rtd129x_crt_lock;

struct rtd129x_fan_tach {
	struct device *dev;
	void __iomem *regs;
	u32 timer_target;
	u32 fan_factor;
	unsigned long speed_rpm;
};

static irqreturn_t rtd129x_fan_tach_irq(int irq, void *data)
{
	struct rtd129x_fan_tach *fan = data;
	u32 counter = readl(fan->regs + RTD_FAN_COUNTER_CV);
	u64 speed;

	/* speed (RPM) = (counter / fan_factor) / (timer_target / CLK_HZ) * 60 */
	speed = div_u64((u64)counter * RTD_FAN_TIMER_CLK_HZ * 60,
			 (u64)fan->timer_target * fan->fan_factor);
	fan->speed_rpm = speed;

	return IRQ_HANDLED;
}

static int rtd129x_fan_tach_read(struct device *dev, enum hwmon_sensor_types type,
				  u32 attr, int channel, long *val)
{
	struct rtd129x_fan_tach *fan = dev_get_drvdata(dev);

	*val = fan->speed_rpm;
	return 0;
}

static const struct hwmon_channel_info *const rtd129x_fan_tach_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	NULL
};

static const struct hwmon_ops rtd129x_fan_tach_ops = {
	.visible = 0444,
	.read = rtd129x_fan_tach_read,
};

static const struct hwmon_chip_info rtd129x_fan_tach_chip_info = {
	.ops = &rtd129x_fan_tach_ops,
	.info = rtd129x_fan_tach_info,
};

static void rtd129x_fan_tach_crt_enable(void)
{
	void __iomem *crt;
	unsigned long flags;
	u32 val;

	crt = ioremap(RTD129X_CRT_BASE + RTD129X_CRT_CLKEN2, 4);
	if (!crt)
		return;

	spin_lock_irqsave(&rtd129x_crt_lock, flags);
	val = readl(crt);
	val |= BIT(RTD129X_CRT_CLKEN2_FAN_BIT);
	writel(val, crt);
	spin_unlock_irqrestore(&rtd129x_crt_lock, flags);

	iounmap(crt);
}

static void rtd129x_fan_tach_pinmux_init(void)
{
	void __iomem *sb2;
	u32 val;

	sb2 = ioremap(RTD129X_SB2_BASE + RTD129X_SB2_MUXPAD4, 4);
	if (!sb2)
		return;

	val = readl(sb2);
	val &= ~(RTD129X_SB2_MUXPAD4_FAN_MASK << RTD129X_SB2_MUXPAD4_FAN_SHIFT);
	val |= RTD129X_SB2_MUXPAD4_FAN_SENSOR << RTD129X_SB2_MUXPAD4_FAN_SHIFT;
	writel(val, sb2);

	iounmap(sb2);
}

static int rtd129x_fan_tach_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct rtd129x_fan_tach *fan;
	struct reset_control *rst;
	struct device *hdev;
	u32 debounce;
	int irq, ret;

	fan = devm_kzalloc(dev, sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;
	fan->dev = dev;

	fan->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(fan->regs))
		return PTR_ERR(fan->regs);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return PTR_ERR(rst);
	ret = reset_control_deassert(rst);
	if (ret)
		return ret;

	rtd129x_fan_tach_crt_enable();
	rtd129x_fan_tach_pinmux_init();

	if (of_property_read_u32(np, "timer_target", &fan->timer_target))
		fan->timer_target = RTD_FAN_TIMER_TV_DEFAULT;
	if (of_property_read_u32(np, "fan_factor", &fan->fan_factor))
		fan->fan_factor = RTD_FAN_FACTOR_DEFAULT;
	if (of_property_read_u32(np, "fan_debounce", &debounce))
		debounce = RTD_FAN_DEBOUNCE_DEFAULT;

	writel(RTD_FAN_DEBOUNCE_WEN | (debounce & RTD_FAN_DEBOUNCE_MASK),
	       fan->regs + RTD_FAN_DEBOUNCE);
	writel(fan->timer_target, fan->regs + RTD_FAN_TIMER_TV);

	ret = devm_request_irq(dev, irq, rtd129x_fan_tach_irq, 0,
				"rtd129x-fan-tach", fan);
	if (ret)
		return ret;

	writel(RTD_FAN_CTRL_EN | RTD_FAN_CTRL_INT_EN, fan->regs + RTD_FAN_CTRL);

	hdev = devm_hwmon_device_register_with_info(dev, "rtd129x_fan", fan,
						     &rtd129x_fan_tach_chip_info,
						     NULL);
	return PTR_ERR_OR_ZERO(hdev);
}

static const struct of_device_id rtd129x_fan_tach_match[] = {
	{ .compatible = "realtek,rtd1295-fan-tach" },
	{}
};
MODULE_DEVICE_TABLE(of, rtd129x_fan_tach_match);

static struct platform_driver rtd129x_fan_tach_driver = {
	.probe = rtd129x_fan_tach_probe,
	.driver = {
		.name = "rtd129x-fan-tach",
		.of_match_table = rtd129x_fan_tach_match,
	},
};
module_platform_driver(rtd129x_fan_tach_driver);

MODULE_DESCRIPTION("Realtek RTD129x fan tachometer driver");
MODULE_LICENSE("GPL");
