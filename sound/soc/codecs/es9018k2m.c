// SPDX-License-Identifier: GPL-2.0-only
/*
 * ES9018K2M ALSA SoC codec and ioctl control driver.
 */

#include <linux/delay.h>
#include <linux/bitfield.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/uaccess.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "es9018k2m.h"

#define ES9018K2M_REG_SYSTEM		0x00
#define ES9018K2M_REG_INPUT_CFG		0x01
#define ES9018K2M_REG_AUTOMUTE_TIME	0x04
#define ES9018K2M_REG_AUTOMUTE_LEVEL	0x05
#define ES9018K2M_REG_VOLUME_CTRL	0x06
#define ES9018K2M_REG_GENERAL		0x07
#define ES9018K2M_REG_GPIO_CFG		0x08
#define ES9018K2M_REG_MASTER_MODE	0x0a
#define ES9018K2M_REG_CHAN_MAP		0x0b
#define ES9018K2M_REG_DPLL_ASRC		0x0c
#define ES9018K2M_REG_THD_COMP		0x0d
#define ES9018K2M_REG_SOFT_START	0x0e
#define ES9018K2M_REG_VOLUME1		0x0f
#define ES9018K2M_REG_VOLUME2		0x10
#define ES9018K2M_REG_GPIO_OSF		0x15
#define ES9018K2M_REG_CHIP_STATUS	0x40
#define ES9018K2M_REG_GPIO_STATUS	0x41
#define ES9018K2M_REG_DPLL_LSB		0x42
#define ES9018K2M_REG_DPLL_MSB		0x45
#define ES9018K2M_REG_MAX		0x5d

#define ES9018K2M_SYSTEM_SOFT_RESET	BIT(0)
#define ES9018K2M_GENERAL_RESERVED	BIT(7)
#define ES9018K2M_GENERAL_MUTE_MASK	GENMASK(1, 0)
#define ES9018K2M_MASTER_ASRC_SLAVE	0x00
#define ES9018K2M_CHIP_ID_MASK		GENMASK(4, 2)
#define ES9018K2M_CHIP_ID_ES9018K2M	4
#define ES9018K2M_DEFAULT_MCLK		49152000U

struct es9018k2m_priv {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpiod;
	struct miscdevice miscdev;
	struct mutex lock;
	struct es9018k2m_volume volume;
	struct es9018k2m_format format;
	u8 mute;
	u32 mclk_hz;
};

static bool es9018k2m_readable_register(struct device *dev, unsigned int reg)
{
	return reg <= ES9018K2M_REG_MAX;
}

static bool es9018k2m_writeable_register(struct device *dev, unsigned int reg)
{
	return reg <= 0x1e;
}

static bool es9018k2m_volatile_register(struct device *dev, unsigned int reg)
{
	return reg >= ES9018K2M_REG_CHIP_STATUS && reg <= ES9018K2M_REG_MAX;
}

static const struct regmap_config es9018k2m_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = ES9018K2M_REG_MAX,
	.readable_reg = es9018k2m_readable_register,
	.writeable_reg = es9018k2m_writeable_register,
	.volatile_reg = es9018k2m_volatile_register,
};

static int es9018k2m_format_to_reg(const struct es9018k2m_format *format,
				   unsigned int *val)
{
	unsigned int bits;
	unsigned int mode;

	switch (format->bits) {
	case 16:
		bits = 0;
		break;
	case 24:
		bits = 1;
		break;
	case 32:
		bits = 2;
		break;
	default:
		return -EINVAL;
	}

	switch (format->mode) {
	case ES9018K2M_I2S_MODE_I2S:
		mode = 0;
		break;
	case ES9018K2M_I2S_MODE_LJ:
		mode = 1;
		break;
	default:
		return -EINVAL;
	}

	switch (format->input) {
	case ES9018K2M_INPUT_I2S:
	case ES9018K2M_INPUT_SPDIF:
	case ES9018K2M_INPUT_DSD:
		break;
	default:
		return -EINVAL;
	}

	*val = (bits << 6) | (mode << 4) | format->input;
	return 0;
}

static int es9018k2m_set_format_locked(struct es9018k2m_priv *es9018,
				       const struct es9018k2m_format *format)
{
	unsigned int val;
	int ret;

	ret = es9018k2m_format_to_reg(format, &val);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_INPUT_CFG, val);
	if (ret)
		return ret;

	es9018->format = *format;
	return 0;
}

static int es9018k2m_set_volume_locked(struct es9018k2m_priv *es9018,
				       const struct es9018k2m_volume *volume)
{
	int ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_VOLUME1,
			   volume->left);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_VOLUME2,
			   volume->right);
	if (ret)
		return ret;

	es9018->volume = *volume;
	return 0;
}

static int es9018k2m_set_mute_locked(struct es9018k2m_priv *es9018, u8 mute)
{
	u8 reg_mute;
	int ret;

	reg_mute = mute ? ES9018K2M_GENERAL_MUTE_MASK : 0;
	ret = regmap_update_bits(es9018->regmap, ES9018K2M_REG_GENERAL,
				 ES9018K2M_GENERAL_MUTE_MASK, reg_mute);
	if (ret)
		return ret;

	es9018->mute = !!mute;
	return 0;
}

static void es9018k2m_hw_reset(struct es9018k2m_priv *es9018)
{
	if (!es9018->reset_gpiod)
		return;

	gpiod_set_value_cansleep(es9018->reset_gpiod, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(es9018->reset_gpiod, 0);
	usleep_range(10000, 20000);
}

static int es9018k2m_soft_reset(struct es9018k2m_priv *es9018)
{
	int ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_SYSTEM,
			   ES9018K2M_SYSTEM_SOFT_RESET);
	if (ret)
		return ret;

	usleep_range(1000, 2000);
	return regmap_write(es9018->regmap, ES9018K2M_REG_SYSTEM, 0x00);
}

static int es9018k2m_init_chip_locked(struct es9018k2m_priv *es9018)
{
	struct es9018k2m_format format = {
		.bits = 32,
		.mode = ES9018K2M_I2S_MODE_I2S,
		.input = ES9018K2M_INPUT_I2S,
	};
	struct es9018k2m_volume volume = { 0x00, 0x00 };
	unsigned int status;
	int ret;

	es9018k2m_hw_reset(es9018);

	ret = es9018k2m_soft_reset(es9018);
	if (ret)
		return ret;

	ret = regmap_read(es9018->regmap, ES9018K2M_REG_CHIP_STATUS, &status);
	if (ret)
		return ret;

	if (FIELD_GET(ES9018K2M_CHIP_ID_MASK, status) !=
	    ES9018K2M_CHIP_ID_ES9018K2M)
		dev_warn(es9018->dev, "unexpected chip status 0x%02x\n",
			 status);

	ret = es9018k2m_set_format_locked(es9018, &format);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_AUTOMUTE_TIME, 0x00);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_AUTOMUTE_LEVEL,
			   0x68);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_VOLUME_CTRL, 0x4a);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_GENERAL,
			   ES9018K2M_GENERAL_RESERVED);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_GPIO_CFG, 0x10);
	if (ret)
		return ret;

	/*
	 * Slave PCM mode, DPLL/ASRC enabled, sync_mode disabled. stop_div=0
	 * gives the broadest PCM-rate compatibility according to the datasheet.
	 */
	ret = regmap_write(es9018->regmap, ES9018K2M_REG_MASTER_MODE,
			   ES9018K2M_MASTER_ASRC_SLAVE);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_CHAN_MAP, 0x02);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_DPLL_ASRC, 0x5a);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_THD_COMP, 0x40);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_SOFT_START, 0x8a);
	if (ret)
		return ret;

	ret = regmap_write(es9018->regmap, ES9018K2M_REG_GPIO_OSF, 0x00);
	if (ret)
		return ret;

	ret = es9018k2m_set_volume_locked(es9018, &volume);
	if (ret)
		return ret;

	return es9018k2m_set_mute_locked(es9018, 0);
}

static int es9018k2m_read_status_locked(struct es9018k2m_priv *es9018,
					struct es9018k2m_status *status)
{
	unsigned int val;
	u64 rate;
	int i, ret;

	memset(status, 0, sizeof(*status));
	status->muted = es9018->mute;

	ret = regmap_read(es9018->regmap, ES9018K2M_REG_CHIP_STATUS, &val);
	if (ret)
		return ret;
	status->chip_status = val;

	ret = regmap_read(es9018->regmap, ES9018K2M_REG_GPIO_STATUS, &val);
	if (ret)
		return ret;
	status->gpio_status = val;

	for (i = 0; i < 4; i++) {
		ret = regmap_read(es9018->regmap,
				  ES9018K2M_REG_DPLL_LSB + i, &val);
		if (ret)
			return ret;
		status->dpll_num |= (val & 0xff) << (i * 8);
	}

	rate = (u64)status->dpll_num * es9018->mclk_hz;
	status->sample_rate = rate >> 32;

	return 0;
}

static long es9018k2m_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct miscdevice *miscdev = file->private_data;
	struct es9018k2m_priv *es9018 =
		container_of(miscdev, struct es9018k2m_priv, miscdev);
	void __user *argp = (void __user *)arg;
	struct es9018k2m_volume volume;
	struct es9018k2m_format format;
	struct es9018k2m_status status;
	struct es9018k2m_reg reg;
	unsigned int val;
	u8 mute;
	int ret = 0;

	if (_IOC_TYPE(cmd) != ES9018K2M_IOCTL_MAGIC)
		return -ENOTTY;

	mutex_lock(&es9018->lock);

	switch (cmd) {
	case ES9018K2M_IOCTL_GET_STATUS:
		ret = es9018k2m_read_status_locked(es9018, &status);
		if (!ret && copy_to_user(argp, &status, sizeof(status)))
			ret = -EFAULT;
		break;
	case ES9018K2M_IOCTL_SET_VOLUME:
		if (copy_from_user(&volume, argp, sizeof(volume))) {
			ret = -EFAULT;
			break;
		}
		ret = es9018k2m_set_volume_locked(es9018, &volume);
		break;
	case ES9018K2M_IOCTL_GET_VOLUME:
		volume = es9018->volume;
		if (copy_to_user(argp, &volume, sizeof(volume)))
			ret = -EFAULT;
		break;
	case ES9018K2M_IOCTL_SET_MUTE:
		if (copy_from_user(&mute, argp, sizeof(mute))) {
			ret = -EFAULT;
			break;
		}
		ret = es9018k2m_set_mute_locked(es9018, mute);
		break;
	case ES9018K2M_IOCTL_GET_MUTE:
		mute = es9018->mute;
		if (copy_to_user(argp, &mute, sizeof(mute)))
			ret = -EFAULT;
		break;
	case ES9018K2M_IOCTL_SET_FORMAT:
		if (copy_from_user(&format, argp, sizeof(format))) {
			ret = -EFAULT;
			break;
		}
		ret = es9018k2m_set_format_locked(es9018, &format);
		break;
	case ES9018K2M_IOCTL_GET_FORMAT:
		format = es9018->format;
		if (copy_to_user(argp, &format, sizeof(format)))
			ret = -EFAULT;
		break;
	case ES9018K2M_IOCTL_READ_REG:
		if (copy_from_user(&reg, argp, sizeof(reg))) {
			ret = -EFAULT;
			break;
		}
		if (!es9018k2m_readable_register(es9018->dev, reg.reg)) {
			ret = -EINVAL;
			break;
		}
		ret = regmap_read(es9018->regmap, reg.reg, &val);
		if (ret)
			break;
		reg.val = val;
		if (copy_to_user(argp, &reg, sizeof(reg)))
			ret = -EFAULT;
		break;
	case ES9018K2M_IOCTL_WRITE_REG:
		if (copy_from_user(&reg, argp, sizeof(reg))) {
			ret = -EFAULT;
			break;
		}
		if (!es9018k2m_writeable_register(es9018->dev, reg.reg)) {
			ret = -EINVAL;
			break;
		}
		ret = regmap_write(es9018->regmap, reg.reg, reg.val);
		if (!ret) {
			if (reg.reg == ES9018K2M_REG_VOLUME1)
				es9018->volume.left = reg.val;
			else if (reg.reg == ES9018K2M_REG_VOLUME2)
				es9018->volume.right = reg.val;
			else if (reg.reg == ES9018K2M_REG_GENERAL)
				es9018->mute = !!(reg.val &
					ES9018K2M_GENERAL_MUTE_MASK);
		}
		break;
	case ES9018K2M_IOCTL_RESET:
		ret = es9018k2m_init_chip_locked(es9018);
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&es9018->lock);
	return ret;
}

static const struct file_operations es9018k2m_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = es9018k2m_ioctl,
	.compat_ioctl = es9018k2m_ioctl,
	.llseek = no_llseek,
};

static int es9018k2m_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct es9018k2m_priv *es9018 =
		snd_soc_component_get_drvdata(component);
	struct es9018k2m_format format = es9018->format;
	unsigned int width = params_physical_width(params);
	int ret;

	if (width != 16 && width != 24 && width != 32)
		width = params_width(params);

	format.bits = width;
	format.mode = ES9018K2M_I2S_MODE_I2S;
	format.input = ES9018K2M_INPUT_I2S;

	mutex_lock(&es9018->lock);
	ret = es9018k2m_set_format_locked(es9018, &format);
	mutex_unlock(&es9018->lock);

	return ret;
}

static int es9018k2m_mute_stream(struct snd_soc_dai *dai, int mute,
				 int direction)
{
	struct snd_soc_component *component = dai->component;
	struct es9018k2m_priv *es9018 =
		snd_soc_component_get_drvdata(component);
	int ret;

	mutex_lock(&es9018->lock);
	ret = es9018k2m_set_mute_locked(es9018, mute);
	mutex_unlock(&es9018->lock);

	return ret;
}

static const struct snd_soc_dai_ops es9018k2m_dai_ops = {
	.hw_params = es9018k2m_hw_params,
	.mute_stream = es9018k2m_mute_stream,
};

static struct snd_soc_dai_driver es9018k2m_dai = {
	.name = "es9018k2m-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &es9018k2m_dai_ops,
};

static const struct snd_soc_component_driver es9018k2m_component = {
	.idle_bias_on = 1,
	.use_pmdown_time = 1,
	.endianness = 1,
	.non_legacy_dai_naming = 1,
};

static void es9018k2m_misc_deregister(void *data)
{
	misc_deregister(data);
}

static int es9018k2m_i2c_probe(struct i2c_client *i2c,
			       const struct i2c_device_id *id)
{
	struct es9018k2m_priv *es9018;
	u32 mclk_hz;
	int ret;

	es9018 = devm_kzalloc(&i2c->dev, sizeof(*es9018), GFP_KERNEL);
	if (!es9018)
		return -ENOMEM;

	es9018->dev = &i2c->dev;
	mutex_init(&es9018->lock);

	es9018->regmap = devm_regmap_init_i2c(i2c,
					      &es9018k2m_regmap_config);
	if (IS_ERR(es9018->regmap))
		return PTR_ERR(es9018->regmap);

	es9018->reset_gpiod = devm_gpiod_get_optional(&i2c->dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(es9018->reset_gpiod))
		return PTR_ERR(es9018->reset_gpiod);

	if (device_property_read_u32(&i2c->dev, "mclk-frequency",
				     &mclk_hz))
		mclk_hz = ES9018K2M_DEFAULT_MCLK;
	es9018->mclk_hz = mclk_hz;

	i2c_set_clientdata(i2c, es9018);

	mutex_lock(&es9018->lock);
	ret = es9018k2m_init_chip_locked(es9018);
	mutex_unlock(&es9018->lock);
	if (ret) {
		dev_err(&i2c->dev, "failed to initialize codec: %d\n", ret);
		return ret;
	}

	es9018->miscdev.minor = MISC_DYNAMIC_MINOR;
	es9018->miscdev.name = "es9018k2m";
	es9018->miscdev.fops = &es9018k2m_fops;
	es9018->miscdev.parent = &i2c->dev;

	ret = misc_register(&es9018->miscdev);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&i2c->dev, es9018k2m_misc_deregister,
				       &es9018->miscdev);
	if (ret)
		return ret;

	return devm_snd_soc_register_component(&i2c->dev,
					       &es9018k2m_component,
					       &es9018k2m_dai, 1);
}

static int es9018k2m_i2c_remove(struct i2c_client *i2c)
{
	struct es9018k2m_priv *es9018 = i2c_get_clientdata(i2c);

	mutex_lock(&es9018->lock);
	es9018k2m_set_mute_locked(es9018, 1);
	regmap_update_bits(es9018->regmap, ES9018K2M_REG_SOFT_START,
			   BIT(7), 0);
	mutex_unlock(&es9018->lock);

	return 0;
}

static const struct i2c_device_id es9018k2m_i2c_id[] = {
	{ "es9018k2m", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, es9018k2m_i2c_id);

static const struct of_device_id es9018k2m_of_match[] = {
	{ .compatible = "tsy,es9018k2m" },
	{ }
};
MODULE_DEVICE_TABLE(of, es9018k2m_of_match);

static struct i2c_driver es9018k2m_i2c_driver = {
	.driver = {
		.name = "es9018k2m",
		.of_match_table = es9018k2m_of_match,
	},
	.probe = es9018k2m_i2c_probe,
	.remove = es9018k2m_i2c_remove,
	.id_table = es9018k2m_i2c_id,
};

module_i2c_driver(es9018k2m_i2c_driver);

MODULE_DESCRIPTION("ASoC ES9018K2M DAC codec driver");
MODULE_AUTHOR("tsy <tsy88123@outlook.com>");
MODULE_LICENSE("GPL v2");
