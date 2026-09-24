/* Copyright © SixtyFPS GmbH <info@slint.dev>
 * SPDX-License-Identifier: MIT
 *
 * Power-cycle the M5Stack Core2's Grove 5V (BUS_5V) at boot.
 *
 * The board definition declares bus_5v but leaves it off, so a unit on
 * Port A gets no power and never answers on I2C. Turning it on is not
 * enough either: the AXP192 survives an MCU reset, and if the previous
 * firmware left the boost (EXTEN) on, the unit keeps running from before
 * and a driver such as VL53L0X fails its init. Switch it off for sure,
 * wait for the rail to drain, then switch it on, all before the sensor
 * drivers initialize.
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(core2_bus_power, LOG_LEVEL_INF);

#define BUS_5V DT_NODELABEL(bus_5v)
/* The AXP192 node: the parent of the GPIO controller that drives bus_5v. */
#define AXP192 DT_PARENT(DT_GPIO_CTLR(BUS_5V, enable_gpios))

#define AXP192_REG_EXTEN 0x10
#define AXP192_EXTEN_BIT BIT(2)

static const struct device *const bus_pwr_en = DEVICE_DT_GET(DT_NODELABEL(bus_pwr_en));
static const struct device *const bus_5v = DEVICE_DT_GET(BUS_5V);
static const struct i2c_dt_spec pmic = I2C_DT_SPEC_GET(AXP192);

/* EXTEN as read back from the AXP192, or -1 if it cannot be read. */
static int exten(void)
{
	uint8_t reg;

	if (!i2c_is_ready_dt(&pmic) || i2c_reg_read_byte_dt(&pmic, AXP192_REG_EXTEN, &reg) != 0) {
		return -1;
	}
	return (reg & AXP192_EXTEN_BIT) ? 1 : 0;
}

/* Switch BUS_5V off. regulator_disable() is a no-op while the reference
 * count is 0, so take one first; fall back to the register if the
 * regulator still leaves EXTEN set. */
static bool bus_5v_off(void)
{
	regulator_enable(bus_5v);
	regulator_disable(bus_5v);
	if (exten() == 0) {
		return true;
	}
	i2c_reg_update_byte_dt(&pmic, AXP192_REG_EXTEN, AXP192_EXTEN_BIT, 0);
	return exten() == 0;
}

static int core2_bus_power_init(void)
{
	if (!device_is_ready(bus_pwr_en) || !device_is_ready(bus_5v)) {
		LOG_ERR("BUS_5V regulators not ready");
		return 0;
	}
	regulator_enable(bus_pwr_en); /* GPIO0 as LDO: cut the VBUS input */
	k_msleep(10);
	if (!bus_5v_off()) {
		LOG_ERR("could not switch BUS_5V off");
		return 0;
	}
	k_msleep(200); /* let the unit drain */
	regulator_enable(bus_5v);
	k_msleep(100); /* let the unit boot */
	LOG_INF("BUS_5V on (EXTEN=%d)", exten());
	return 0;
}

/* After the AXP192 drivers (MFD 70, regulator 71, GPIO 72, fixed regulator 75),
 * before the sensor drivers (90). */
BUILD_ASSERT(CONFIG_REGULATOR_FIXED_INIT_PRIORITY < 80);
#ifdef CONFIG_SENSOR
BUILD_ASSERT(CONFIG_SENSOR_INIT_PRIORITY > 80);
#endif
SYS_INIT(core2_bus_power_init, POST_KERNEL, 80);
