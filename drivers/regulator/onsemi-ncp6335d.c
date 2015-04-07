/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/log2.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/debugfs.h>
#include <linux/regulator/onsemi-ncp6335d.h>
#include <linux/string.h>

#define REG_NCP6335D_PID		0x03
#define REG_NCP6335D_PROGVSEL1		0x10
#define REG_NCP6335D_PROGVSEL0		0x11
#define REG_NCP6335D_PGOOD		0x12
#define REG_NCP6335D_TIMING		0x13
#define REG_NCP6335D_COMMAND		0x14

#define NCP6335D_MIN_VOLTAGE_UV		600000
#define NCP6335D_STEP_VOLTAGE_UV	6250
#define NCP6335D_VOLTAGE_STEPS		128
#define NCP6335D_MIN_SLEW_NS		166
#define NCP6335D_MAX_SLEW_NS		1333

#define NCP6335D_ENABLE			BIT(7)
#define NCP6335D_DVS_PWM_MODE		BIT(5)
#define NCP6335D_PWM_MODE1		BIT(6)
#define NCP6335D_PWM_MODE0		BIT(7)
#define NCP6335D_PGOOD_DISCHG		BIT(4)
#define NCP6335D_SLEEP_MODE		BIT(4)

#define NCP6335D_VOUT_SEL_MASK		0x7F
#define NCP6335D_SLEW_MASK		0x18
#define NCP6335D_SLEW_SHIFT		0x3

#define I2C_ADDR1				0x1C
#define I2C_ADDR2				0x18

struct ncp6335d_info {
	struct regulator_dev *regulator;
	struct regulator_init_data *init_data;
	struct regmap *regmap;
	struct device *dev;
	unsigned int vsel_reg;
	unsigned int vsel_backup_reg;
	unsigned int mode_bit;
	int curr_voltage;
	int slew_rate;
	bool is_suspend;

	unsigned int step_size;
	unsigned int min_voltage;
	unsigned int min_slew_ns;
	unsigned int max_slew_ns;
	unsigned int peek_poke_address;

	struct dentry *debug_root;
	struct mutex ncp_mutex;
};

	
static void dump_registers(struct ncp6335d_info *dd,
			unsigned int reg, const char *func)
{
	unsigned int val = 0;

	regmap_read(dd->regmap, reg, &val);
	dev_dbg(dd->dev, "%s: NCP6335D: Reg = %x, Val = %x\n", func, reg, val);
}

static void ncp633d_slew_delay(struct ncp6335d_info *dd,
					int prev_uV, int new_uV)
{
	u8 val;
	int delay;

	val = abs(prev_uV - new_uV) / dd->step_size;
	delay = ((val * dd->slew_rate) / 1000) + 1;

	dev_dbg(dd->dev, "Slew Delay = %d\n", delay);

	udelay(delay);
}

static void getAddr(struct i2c_client *client)
{
	if (client->addr == I2C_ADDR1)
		client->addr = I2C_ADDR2;
	else
		client->addr = I2C_ADDR1;
}

static int ncp6335d_enable(struct regulator_dev *rdev)
{
	int rc;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);
	struct i2c_client *client = to_i2c_client(dd->dev);

	rc = regmap_update_bits(dd->regmap, dd->vsel_reg,
				NCP6335D_ENABLE, NCP6335D_ENABLE);
	if (rc) {
		dev_err(dd->dev, "Unable to enable regualtor rc(%d), try another addr, current: %x\n", rc, client->addr);
		getAddr(client);
		rc = regmap_update_bits(dd->regmap, dd->vsel_reg,
				NCP6335D_ENABLE, NCP6335D_ENABLE);
		if (rc) {
			dev_err(dd->dev, "Unable to enable regualtor rc(%d), %x\n", rc, client->addr);
		}
	}

	dump_registers(dd, dd->vsel_reg, __func__);

	return rc;
}

static int ncp6335d_disable(struct regulator_dev *rdev)
{
	int rc;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);
	struct i2c_client *client = to_i2c_client(dd->dev);

	rc = regmap_update_bits(dd->regmap, dd->vsel_reg,
					NCP6335D_ENABLE, 0);
	if (rc) {
		dev_err(dd->dev, "Unable to disable regualtor rc(%d), %x\n", rc, client->addr);
		getAddr(client);
		rc = regmap_update_bits(dd->regmap, dd->vsel_reg,
					NCP6335D_ENABLE, 0);
		if (rc) {
			dev_err(dd->dev, "Unable to disable regualtor rc(%d), %x\n", rc, client->addr);
		}
	}

	dump_registers(dd, dd->vsel_reg, __func__);

	return rc;
}

static int ncp6335d_get_voltage(struct regulator_dev *rdev)
{
	unsigned int val;
	int rc;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);
	struct i2c_client *client = to_i2c_client(dd->dev);

	mutex_lock(&dd->ncp_mutex);
	if (dd->is_suspend) {
		rc = dd->curr_voltage;
		dev_dbg(dd->dev, "Get voltage after suspend, (%d)\n", rc);
		goto out;
	}

	rc = regmap_read(dd->regmap, dd->vsel_reg, &val);
	if (rc) {
		dev_err(dd->dev, "Unable to get volatge rc(%d), %x\n", rc, client->addr);
		getAddr(client);
		rc = regmap_read(dd->regmap, dd->vsel_reg, &val);
		if (rc) {
			dev_err(dd->dev, "Unable to get voltage rc(%d), %x\n", rc, client->addr);
			goto out;
		}
	}
	dd->curr_voltage = ((val & NCP6335D_VOUT_SEL_MASK) * dd->step_size) +
				dd->min_voltage;

	rc = dd->curr_voltage;

	dump_registers(dd, dd->vsel_reg, __func__);

out:
	mutex_unlock(&dd->ncp_mutex);
	return rc;
}

static int ncp6335d_set_voltage(struct regulator_dev *rdev,
			int min_uV, int max_uV, unsigned *selector)
{
	int rc = 0, set_val, new_uV;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);
	struct i2c_client *client = to_i2c_client(dd->dev);

	set_val = DIV_ROUND_UP(min_uV - dd->min_voltage, dd->step_size);
	new_uV = (set_val * dd->step_size) + dd->min_voltage;
	if (new_uV > max_uV) {
		dev_err(dd->dev, "Unable to set volatge (%d %d)\n",
							min_uV, max_uV);
		return -EINVAL;
	}

	mutex_lock(&dd->ncp_mutex);
	if (dd->is_suspend) {
		dev_info(dd->dev, "Ignore voltage setting after suspend: %d\n",
					new_uV);
		goto out;
	}

	rc = regmap_update_bits(dd->regmap, dd->vsel_reg,
		NCP6335D_VOUT_SEL_MASK, (set_val & NCP6335D_VOUT_SEL_MASK));
	if (rc) {
		dev_err(dd->dev, "Unable to set volatge (%d %d), %x\n",
							min_uV, max_uV, client->addr);
		getAddr(client);
		rc = regmap_update_bits(dd->regmap, dd->vsel_reg,
			NCP6335D_VOUT_SEL_MASK, (set_val & NCP6335D_VOUT_SEL_MASK));
		if (rc)
			dev_err(dd->dev, "Unable to set volatge1 (%d %d), %x\n",
							min_uV, max_uV, client->addr);
	}

	if (!rc) {
		ncp633d_slew_delay(dd, dd->curr_voltage, new_uV);
		dd->curr_voltage = new_uV;
	}

	dump_registers(dd, dd->vsel_reg, __func__);
out:
	mutex_unlock(&dd->ncp_mutex);
	return rc;
}

static int ncp6335d_list_voltage(struct regulator_dev *rdev,
					unsigned selector)
{
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);

	if (selector >= NCP6335D_VOLTAGE_STEPS)
		return 0;

	return selector * dd->step_size + dd->min_voltage;
}

static int ncp6335d_set_mode(struct regulator_dev *rdev,
					unsigned int mode)
{
	int rc;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);
	struct i2c_client *client = to_i2c_client(dd->dev);

	
	if (mode != REGULATOR_MODE_FAST && mode != REGULATOR_MODE_NORMAL) {
		dev_err(dd->dev, "Mode %d not supported\n", mode);
		return -EINVAL;
	}

	rc = regmap_update_bits(dd->regmap, REG_NCP6335D_COMMAND, dd->mode_bit,
			(mode == REGULATOR_MODE_FAST) ? dd->mode_bit : 0);
	if (rc) {
		dev_err(dd->dev, "Unable to set operating mode rc(%d), %x", rc, client->addr);
		getAddr(client);
		rc = regmap_update_bits(dd->regmap, REG_NCP6335D_COMMAND, dd->mode_bit,
			(mode == REGULATOR_MODE_FAST) ? dd->mode_bit : 0);
		if (rc)
			dev_err(dd->dev, "Unable to set operating mode rc1(%d), %x", rc, client->addr);
		return rc;
	}

	rc = regmap_update_bits(dd->regmap, REG_NCP6335D_COMMAND,
					NCP6335D_DVS_PWM_MODE,
					(mode == REGULATOR_MODE_FAST) ?
					NCP6335D_DVS_PWM_MODE : 0);
	if (rc) {
		dev_err(dd->dev, "Unable to set DVS trans. mode rc(%d), %x", rc, client->addr);
		getAddr(client);
		rc = regmap_update_bits(dd->regmap, REG_NCP6335D_COMMAND,
					NCP6335D_DVS_PWM_MODE,
					(mode == REGULATOR_MODE_FAST) ?
					NCP6335D_DVS_PWM_MODE : 0);
		if (rc)
			dev_err(dd->dev, "Unable to set DVS trans. mode rc1(%d), %x", rc, client->addr);
	}

	dump_registers(dd, REG_NCP6335D_COMMAND, __func__);

	return rc;
}

static unsigned int ncp6335d_get_mode(struct regulator_dev *rdev)
{
	unsigned int val;
	int rc;
	struct ncp6335d_info *dd = rdev_get_drvdata(rdev);
	struct i2c_client *client = to_i2c_client(dd->dev);

	rc = regmap_read(dd->regmap, REG_NCP6335D_COMMAND, &val);
	if (rc) {
		dev_err(dd->dev, "Unable to get regulator mode rc(%d), %x\n", rc, client->addr);
		getAddr(client);
		rc = regmap_read(dd->regmap, REG_NCP6335D_COMMAND, &val);
		if (rc) {
			dev_err(dd->dev, "Unable to get regulator mode rc1(%d), %x\n", rc, client->addr);
			return rc;
		}
	}

	dump_registers(dd, REG_NCP6335D_COMMAND, __func__);

	if (val & dd->mode_bit)
		return REGULATOR_MODE_FAST;

	return REGULATOR_MODE_NORMAL;
}

static struct regulator_ops ncp6335d_ops = {
	.set_voltage = ncp6335d_set_voltage,
	.get_voltage = ncp6335d_get_voltage,
	.list_voltage = ncp6335d_list_voltage,
	.enable = ncp6335d_enable,
	.disable = ncp6335d_disable,
	.set_mode = ncp6335d_set_mode,
	.get_mode = ncp6335d_get_mode,
};

static struct regulator_desc rdesc = {
	.name = "ncp6335d",
	.owner = THIS_MODULE,
	.n_voltages = NCP6335D_VOLTAGE_STEPS,
	.ops = &ncp6335d_ops,
};

static int ncp6335d_restore_working_reg(struct device_node *node,
					struct ncp6335d_info *dd)
{
	int ret;
	unsigned int val;
	struct i2c_client *client = to_i2c_client(dd->dev);

	
	ret = regmap_read(dd->regmap, dd->vsel_backup_reg, &val);
	if (ret < 0) {
		dev_err(dd->dev, "Failed to get backup data from reg %d, ret = %d, %x\n",
			dd->vsel_backup_reg, ret, client->addr);
		getAddr(client);
		ret = regmap_read(dd->regmap, dd->vsel_backup_reg, &val);
		if (ret < 0) {
			dev_err(dd->dev, "Failed to get backup data from reg1 %d, ret = %d, %x\n",
				dd->vsel_backup_reg, ret, client->addr);
			return ret;
		}
	}

	ret = regmap_update_bits(dd->regmap, dd->vsel_reg,
					NCP6335D_VOUT_SEL_MASK, val);
	if (ret < 0) {
		dev_err(dd->dev, "Failed to update working reg %d, ret = %d, %x\n",
			dd->vsel_reg,  ret, client->addr);
		getAddr(client);
		ret = regmap_update_bits(dd->regmap, dd->vsel_reg,
					NCP6335D_VOUT_SEL_MASK, val);
		if (ret < 0) {
			dev_err(dd->dev, "Failed to update working reg1 %d, ret = %d, %x\n",
				dd->vsel_reg,  ret, client->addr);
			return ret;
		}
	}

	return ret;
}

static int ncp6335d_parse_gpio(struct device_node *node,
					struct ncp6335d_info *dd)
{
	int ret = 0, gpio;
	enum of_gpio_flags flags;

	if (!of_find_property(node, "onnn,vsel-gpio", NULL))
		return ret;

	
	gpio = of_get_named_gpio_flags(node,
			"onnn,vsel-gpio", 0, &flags);
	if (!gpio_is_valid(gpio)) {
		if (gpio != -EPROBE_DEFER)
			dev_err(dd->dev, "Could not get vsel, ret = %d\n",
				gpio);
		return gpio;
	}

	ret = devm_gpio_request(dd->dev, gpio, "ncp6335d_vsel");
	if (ret) {
		dev_err(dd->dev, "Failed to obtain gpio %d ret = %d\n",
				gpio, ret);
			return ret;
	}

	ret = gpio_direction_output(gpio, flags & OF_GPIO_ACTIVE_LOW ? 0 : 1);
	if (ret) {
		dev_err(dd->dev, "Failed to set GPIO %d to: %s, ret = %d",
				gpio, flags & OF_GPIO_ACTIVE_LOW ?
				"GPIO_LOW" : "GPIO_HIGH", ret);
		return ret;
	}

	return ret;
}

static int ncp6335d_init(struct i2c_client *client, struct ncp6335d_info *dd,
			const struct ncp6335d_platform_data *pdata)
{
	int rc;
	unsigned int val;

	switch (pdata->default_vsel) {
	case NCP6335D_VSEL0:
		dd->vsel_reg = REG_NCP6335D_PROGVSEL0;
		dd->vsel_backup_reg = REG_NCP6335D_PROGVSEL1;
		dd->mode_bit = NCP6335D_PWM_MODE0;
	break;
	case NCP6335D_VSEL1:
		dd->vsel_reg = REG_NCP6335D_PROGVSEL1;
		dd->vsel_backup_reg = REG_NCP6335D_PROGVSEL0;
		dd->mode_bit = NCP6335D_PWM_MODE1;
	break;
	default:
		dev_err(dd->dev, "Invalid VSEL ID %d\n", pdata->default_vsel);
		return -EINVAL;
	}

	if (of_property_read_bool(client->dev.of_node, "onnn,restore-reg")) {
		rc = ncp6335d_restore_working_reg(client->dev.of_node, dd);
		if (rc)
			return rc;
	}

	rc = ncp6335d_parse_gpio(client->dev.of_node, dd);
	if (rc)
		return rc;

	
	rc = regmap_read(dd->regmap, dd->vsel_reg, &val);
	if (rc) {
		dev_err(dd->dev, "Unable to get volatge rc(%d), %x", rc, client->addr);
		getAddr(client);
		rc = regmap_read(dd->regmap, dd->vsel_reg, &val);
		if (rc) {
			dev_err(dd->dev, "Unable to get volatge rc1(%d), %x", rc, client->addr);
			return rc;
		}
	}
	dd->curr_voltage = ((val & NCP6335D_VOUT_SEL_MASK) *
				dd->step_size) + dd->min_voltage;

	
	rc = regmap_update_bits(dd->regmap, REG_NCP6335D_PGOOD,
					NCP6335D_PGOOD_DISCHG,
					(pdata->discharge_enable ?
					NCP6335D_PGOOD_DISCHG : 0));
	if (rc) {
		dev_err(dd->dev, "Unable to set Active Discharge rc(%d), %x\n", rc, client->addr);
		getAddr(client);
		rc = regmap_update_bits(dd->regmap, REG_NCP6335D_PGOOD,
					NCP6335D_PGOOD_DISCHG,
					(pdata->discharge_enable ?
					NCP6335D_PGOOD_DISCHG : 0));
		if (rc) {
			dev_err(dd->dev, "Unable to set Active Discharge rc1(%d), %x\n", rc, client->addr);
			return -EINVAL;
		}
	}

	
	if (pdata->slew_rate_ns < dd->min_slew_ns ||
			pdata->slew_rate_ns > dd->max_slew_ns) {
		dev_err(dd->dev, "Invalid slew rate %d\n", pdata->slew_rate_ns);
		return -EINVAL;
	}

	dd->slew_rate = pdata->slew_rate_ns;
	val = DIV_ROUND_UP(pdata->slew_rate_ns, dd->min_slew_ns);
	val = ilog2(val);

	rc = regmap_update_bits(dd->regmap, REG_NCP6335D_TIMING,
			NCP6335D_SLEW_MASK, val << NCP6335D_SLEW_SHIFT);
	if (rc) {
		dev_err(dd->dev, "Unable to set slew rate rc(%d), %x\n", rc, client->addr);
		getAddr(client);
		rc = regmap_update_bits(dd->regmap, REG_NCP6335D_TIMING,
			NCP6335D_SLEW_MASK, val << NCP6335D_SLEW_SHIFT);
		if (rc)
			dev_err(dd->dev, "Unable to set slew rate rc1(%d), %x\n", rc, client->addr);
	}

	
	rc = regmap_update_bits(dd->regmap, REG_NCP6335D_COMMAND,
				NCP6335D_SLEEP_MODE, pdata->sleep_enable ?
						NCP6335D_SLEEP_MODE : 0);
	if (rc) {
		dev_err(dd->dev, "Unable to set sleep mode (%d), %x\n", rc, client->addr);
		getAddr(client);
		rc = regmap_update_bits(dd->regmap, REG_NCP6335D_COMMAND,
				NCP6335D_SLEEP_MODE, pdata->sleep_enable ?
						NCP6335D_SLEEP_MODE : 0);
		if (rc)
			dev_err(dd->dev, "Unable to set sleep mode (%d), %x\n", rc, client->addr);
	}

	dump_registers(dd, REG_NCP6335D_COMMAND, __func__);
	dump_registers(dd, REG_NCP6335D_PROGVSEL0, __func__);
	dump_registers(dd, REG_NCP6335D_TIMING, __func__);
	dump_registers(dd, REG_NCP6335D_PGOOD, __func__);

	return rc;
}

static struct regmap_config ncp6335d_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int ncp6335d_parse_dt(struct i2c_client *client,
				struct ncp6335d_info *dd)
{
	int rc;

	rc = of_property_read_u32(client->dev.of_node,
			"onnn,step-size", &dd->step_size);
	if (rc < 0) {
		dev_err(&client->dev, "step size missing: rc = %d.\n", rc);
		return rc;
	}

	rc = of_property_read_u32(client->dev.of_node,
			"onnn,min-slew-ns", &dd->min_slew_ns);
	if (rc < 0) {
		dev_err(&client->dev, "min slew us missing: rc = %d.\n", rc);
		return rc;
	}

	rc = of_property_read_u32(client->dev.of_node,
			"onnn,max-slew-ns", &dd->max_slew_ns);
	if (rc < 0) {
		dev_err(&client->dev, "max slew us missing: rc = %d.\n", rc);
		return rc;
	}

	rc = of_property_read_u32(client->dev.of_node,
			"onnn,min-setpoint", &dd->min_voltage);
	if (rc < 0) {
		dev_err(&client->dev, "min set point missing: rc = %d.\n", rc);
		return rc;
	}

	return rc;
}

static struct ncp6335d_platform_data *
	ncp6335d_get_of_platform_data(struct i2c_client *client)
{
	struct ncp6335d_platform_data *pdata = NULL;
	struct regulator_init_data *init_data;
	const char *mode_name;
	int rc;

	init_data = of_get_regulator_init_data(&client->dev,
				client->dev.of_node);
	if (!init_data) {
		dev_err(&client->dev, "regulator init data is missing\n");
		return pdata;
	}

	pdata = devm_kzalloc(&client->dev,
			sizeof(struct ncp6335d_platform_data), GFP_KERNEL);
	if (!pdata) {
		dev_err(&client->dev, "ncp6335d_platform_data allocation failed.\n");
		return pdata;
	}

	rc = of_property_read_u32(client->dev.of_node,
			"onnn,vsel", &pdata->default_vsel);
	if (rc < 0) {
		dev_err(&client->dev, "onnn,vsel property missing: rc = %d.\n",
			rc);
		return NULL;
	}

	rc = of_property_read_u32(client->dev.of_node,
			"onnn,slew-ns", &pdata->slew_rate_ns);
	if (rc < 0) {
		dev_err(&client->dev, "onnn,slew-ns property missing: rc = %d.\n",
			rc);
		return NULL;
	}

	pdata->discharge_enable = of_property_read_bool(client->dev.of_node,
						"onnn,discharge-enable");

	pdata->sleep_enable = of_property_read_bool(client->dev.of_node,
						"onnn,sleep-enable");

	pdata->init_data = init_data;

	init_data->constraints.input_uV = init_data->constraints.max_uV;
	init_data->constraints.valid_ops_mask =
				REGULATOR_CHANGE_VOLTAGE |
				REGULATOR_CHANGE_STATUS |
				REGULATOR_CHANGE_MODE;
	init_data->constraints.valid_modes_mask =
				REGULATOR_MODE_NORMAL |
				REGULATOR_MODE_FAST;

	rc = of_property_read_string(client->dev.of_node, "onnn,mode",
					&mode_name);
	if (!rc) {
		if (strcmp("pwm", mode_name) == 0) {
			init_data->constraints.initial_mode =
							REGULATOR_MODE_FAST;
		} else if (strcmp("auto", mode_name) == 0) {
			init_data->constraints.initial_mode =
							REGULATOR_MODE_NORMAL;
		} else {
			dev_err(&client->dev, "onnn,mode, unknown regulator mode: %s\n",
				mode_name);
			return NULL;
		}
	}

	return pdata;
}

static int get_reg(void *data, u64 *val)
{
	struct ncp6335d_info *dd = data;
	int rc;
	unsigned int temp = 0;
	struct i2c_client *client = to_i2c_client(dd->dev);

	rc = regmap_read(dd->regmap, dd->peek_poke_address, &temp);
	if (rc < 0) {
		dev_err(dd->dev, "Couldn't read reg %x rc = %d, %x\n",
				dd->peek_poke_address, rc, client->addr);
		getAddr(client);
		rc = regmap_read(dd->regmap, dd->peek_poke_address, &temp);
		if (rc < 0)
			dev_err(dd->dev, "Couldn't read reg1 %x rc = %d, %x\n",
				dd->peek_poke_address, rc, client->addr);
	} else
		*val = temp;

	return rc;
}

static int set_reg(void *data, u64 val)
{
	struct ncp6335d_info *dd = data;
	int rc;
	unsigned int temp = 0;
	struct i2c_client *client = to_i2c_client(dd->dev);

	temp = (unsigned int) val;
	rc = regmap_write(dd->regmap, dd->peek_poke_address, temp);
	if (rc < 0) {
		dev_err(dd->dev, "Couldn't write 0x%02x to 0x%02x rc= %d, %x\n",
			dd->peek_poke_address, temp, rc, client->addr);
		getAddr(client);
		rc = regmap_write(dd->regmap, dd->peek_poke_address, temp);
		if (rc < 0)
			dev_err(dd->dev, "Couldn't write 0x%02x to 0x%02x rc1= %d, %x\n",
				dd->peek_poke_address, temp, rc, client->addr);
	}

	return rc;
}
DEFINE_SIMPLE_ATTRIBUTE(poke_poke_debug_ops, get_reg, set_reg, "0x%02llx\n");

static int ncp6335d_regulator_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	int rc;
	unsigned int val = 0;
	struct ncp6335d_info *dd;
	const struct ncp6335d_platform_data *pdata;
	struct regulator_config config = { };

	if (client->dev.of_node)
		pdata = ncp6335d_get_of_platform_data(client);
	else
		pdata = client->dev.platform_data;

	if (!pdata) {
		dev_err(&client->dev, "Platform data not specified\n");
		return -EINVAL;
	}

	dd = devm_kzalloc(&client->dev, sizeof(*dd), GFP_KERNEL);
	if (!dd) {
		dev_err(&client->dev, "Unable to allocate memory\n");
		return -ENOMEM;
	}

	if (client->dev.of_node) {
		rc = ncp6335d_parse_dt(client, dd);
		if (rc)
			return rc;
	} else {
		dd->step_size	= NCP6335D_STEP_VOLTAGE_UV;
		dd->min_voltage	= NCP6335D_MIN_VOLTAGE_UV;
		dd->min_slew_ns	= NCP6335D_MIN_SLEW_NS;
		dd->max_slew_ns	= NCP6335D_MAX_SLEW_NS;
	}

	dd->regmap = devm_regmap_init_i2c(client, &ncp6335d_regmap_config);
	if (IS_ERR(dd->regmap)) {
		dev_err(&client->dev, "Error allocating regmap\n");
		return PTR_ERR(dd->regmap);
	}

	client->addr = I2C_ADDR2;
	rc = regmap_read(dd->regmap, REG_NCP6335D_PID, &val);
	if (rc) {
		dev_err(&client->dev, "Unable to identify NCP6335D, rc(%d), %x\n",
									rc, client->addr);
		getAddr(client);
		rc = regmap_read(dd->regmap, REG_NCP6335D_PID, &val);
		if (rc) {
			dev_err(&client->dev, "Unable to identify NCP6335D, rc1(%d), %x\n",
									rc, client->addr);
			return rc;
		}
	}
	dev_info(&client->dev, "Detected Regulator NCP6335D PID = %d (client addr = %x)\n", val, client->addr);

	dd->init_data = pdata->init_data;
	dd->dev = &client->dev;
	i2c_set_clientdata(client, dd);
	mutex_init(&dd->ncp_mutex);

	rc = ncp6335d_init(client, dd, pdata);
	if (rc) {
		dev_err(&client->dev, "Unable to intialize the regulator\n");
		return -EINVAL;
	}

	config.dev = &client->dev;
	config.init_data = dd->init_data;
	config.regmap = dd->regmap;
	config.driver_data = dd;
	config.of_node = client->dev.of_node;

	dd->regulator = regulator_register(&rdesc, &config);

	if (IS_ERR(dd->regulator)) {
		dev_err(&client->dev, "Unable to register regulator rc(%ld)",
						PTR_ERR(dd->regulator));

		return PTR_ERR(dd->regulator);
	}

	dd->debug_root = debugfs_create_dir("ncp6335x", NULL);
	if (!dd->debug_root)
		dev_err(&client->dev, "Couldn't create debug dir\n");

	if (dd->debug_root) {
		struct dentry *ent;

		ent = debugfs_create_x32("address", S_IFREG | S_IWUSR | S_IRUGO,
					  dd->debug_root,
					  &(dd->peek_poke_address));
		if (!ent)
			dev_err(&client->dev, "Couldn't create address debug file rc = %d\n",
									rc);

		ent = debugfs_create_file("data", S_IFREG | S_IWUSR | S_IRUGO,
					  dd->debug_root, dd,
					  &poke_poke_debug_ops);
		if (!ent)
			dev_err(&client->dev, "Couldn't create data debug file rc = %d\n",
									rc);
	}

	return 0;
}

static int ncp6335d_regulator_remove(struct i2c_client *client)
{
	struct ncp6335d_info *dd = i2c_get_clientdata(client);

	regulator_unregister(dd->regulator);

	debugfs_remove_recursive(dd->debug_root);

	return 0;
}

static int ncp6335d_suspend_noirq(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ncp6335d_info *dd = i2c_get_clientdata(client);

	mutex_lock(&dd->ncp_mutex);
	dd->is_suspend = true;
	mutex_unlock(&dd->ncp_mutex);

	return 0;
}

static int ncp6335d_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ncp6335d_info *dd = i2c_get_clientdata(client);

	mutex_lock(&dd->ncp_mutex);
	dd->is_suspend = false;
	mutex_unlock(&dd->ncp_mutex);

	return 0;
}

static const struct dev_pm_ops ncp6335d_pm_ops = {
	.suspend_noirq = ncp6335d_suspend_noirq,
	.resume = ncp6335d_resume,
};

static struct of_device_id ncp6335d_match_table[] = {
	{ .compatible = "onnn,ncp6335d-regulator", },
	{},
};
MODULE_DEVICE_TABLE(of, ncp6335d_match_table);

static const struct i2c_device_id ncp6335d_id[] = {
	{"ncp6335d", -1},
	{ },
};

static struct i2c_driver ncp6335d_regulator_driver = {
	.driver = {
		.name = "ncp6335d-regulator",
		.owner = THIS_MODULE,
		.of_match_table = ncp6335d_match_table,
		.pm = &ncp6335d_pm_ops,
	},
	.probe = ncp6335d_regulator_probe,
	.remove = ncp6335d_regulator_remove,
	.id_table = ncp6335d_id,
};

int __init ncp6335d_regulator_init(void)
{
	static bool initialized;

	if (initialized)
		return 0;
	else
		initialized = true;

	return i2c_add_driver(&ncp6335d_regulator_driver);
}
EXPORT_SYMBOL(ncp6335d_regulator_init);
arch_initcall(ncp6335d_regulator_init);

static void __exit ncp6335d_regulator_exit(void)
{
	i2c_del_driver(&ncp6335d_regulator_driver);
}
module_exit(ncp6335d_regulator_exit);
MODULE_DESCRIPTION("OnSemi-NCP6335D regulator driver");
MODULE_LICENSE("GPL v2");
