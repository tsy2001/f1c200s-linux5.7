// SPDX-License-Identifier: GPL-2.0
/*
 * CMP board module control driver.
 *
 * Device tree example:
 * cmp_module: cmp-module {
 *	compatible = "tsy,cmp-module";
 *	codec-mute-gpios = <&pio 3 13 GPIO_ACTIVE_HIGH>;
 *	usb-switch-gpios = <&pio 3 0 GPIO_ACTIVE_HIGH>;
 *	status = "okay";
 * };
 */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#define CMP_MODULE_DEVICE_NAME	"cmp-module"
#define CMP_MODULE_USB_MODE_PATH \
	"/sys/devices/platform/soc/1c13000.usb/musb-hdrc.1.auto/mode"

#define CMP_MODULE_IOC_MAGIC	'c'

enum cmp_module_gpio_flag {
	CMP_MODULE_GPIO_CODEC_MUTE = BIT(0),
	CMP_MODULE_GPIO_USB_SWITCH = BIT(1),
};

#define CMP_MODULE_GPIO_ALL \
	(CMP_MODULE_GPIO_CODEC_MUTE | CMP_MODULE_GPIO_USB_SWITCH)

struct cmp_module_gpio_state {
	__u32 mask;
	__u32 value;
};

enum cmp_module_usb_mode {
	CMP_MODULE_USB_MODE_HOST = 1,
	CMP_MODULE_USB_MODE_PERIPHERAL = 2,
	CMP_MODULE_USB_MODE_OTG = 3,
};

#define CMP_MODULE_IOC_SET_GPIOS \
	_IOW(CMP_MODULE_IOC_MAGIC, 0x01, struct cmp_module_gpio_state)
#define CMP_MODULE_IOC_GET_GPIOS \
	_IOWR(CMP_MODULE_IOC_MAGIC, 0x02, struct cmp_module_gpio_state)
#define CMP_MODULE_IOC_SET_USB_MODE \
	_IOW(CMP_MODULE_IOC_MAGIC, 0x03, __u32)

struct cmp_module_gpio {
	const char *name;
	const char *con_id;
	u32 flag;
	int default_value;
	struct gpio_desc *desc;
};

struct cmp_module {
	struct device *dev;
	struct device_node *np;
	struct miscdevice miscdev;
	struct mutex lock;
	unsigned int open_count;
	struct cmp_module_gpio gpios[2];
};

static struct cmp_module *cmp_module_from_file(struct file *file)
{
	return container_of(file->private_data, struct cmp_module, miscdev);
}

static struct cmp_module_gpio *cmp_module_find_gpio(struct cmp_module *cm,
						    u32 flag)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cm->gpios); i++) {
		if (cm->gpios[i].flag == flag)
			return &cm->gpios[i];
	}

	return NULL;
}

static int cmp_module_init_outputs(struct cmp_module *cm)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(cm->gpios); i++) {
		ret = gpiod_direction_output(cm->gpios[i].desc,
					     cm->gpios[i].default_value);
		if (ret) {
			dev_err(cm->dev, "failed to set %s output: %d\n",
				cm->gpios[i].name, ret);
			return ret;
		}
	}

	return 0;
}

static void cmp_module_set_gpio(struct cmp_module_gpio *gpio, bool value)
{
	gpiod_set_value_cansleep(gpio->desc, value);
}

static int cmp_module_get_gpio(struct cmp_module_gpio *gpio)
{
	return gpiod_get_value_cansleep(gpio->desc);
}

static int cmp_module_write_file(const char *path, const char *buf, size_t len)
{
	struct file *filp;
	loff_t pos = 0;
	ssize_t written;
	int ret = 0;

	filp = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	written = kernel_write(filp, buf, len, &pos);
	if (written < 0)
		ret = written;
	else if (written != len)
		ret = -EIO;

	filp_close(filp, NULL);

	return ret;
}

static int cmp_module_set_usb_mode(struct cmp_module *cm, __u32 mode)
{
	const char *mode_str;

	switch (mode) {
	case CMP_MODULE_USB_MODE_HOST:
		mode_str = "host";
		break;
	case CMP_MODULE_USB_MODE_PERIPHERAL:
		mode_str = "peripheral";
		break;
	case CMP_MODULE_USB_MODE_OTG:
		mode_str = "otg";
		break;
	default:
		return -EINVAL;
	}

	dev_dbg(cm->dev, "set usb mode: %s\n", mode_str);

	return cmp_module_write_file(CMP_MODULE_USB_MODE_PATH,
				     mode_str, strlen(mode_str));
}

static int cmp_module_open(struct inode *inode, struct file *file)
{
	struct cmp_module *cm = cmp_module_from_file(file);
	int ret = 0;

	mutex_lock(&cm->lock);

	if (cm->open_count == 0)
		ret = cmp_module_init_outputs(cm);

	if (!ret)
		cm->open_count++;

	mutex_unlock(&cm->lock);

	return ret;
}

static int cmp_module_release(struct inode *inode, struct file *file)
{
	struct cmp_module *cm = cmp_module_from_file(file);

	mutex_lock(&cm->lock);
	if (cm->open_count > 0)
		cm->open_count--;
	mutex_unlock(&cm->lock);

	return 0;
}

static long cmp_module_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct cmp_module *cm = cmp_module_from_file(file);
	struct cmp_module_gpio_state state;
	__u32 usb_mode;
	u32 flag;
	int value;
	int ret = 0;

	switch (cmd) {
	case CMP_MODULE_IOC_SET_GPIOS:
		if (copy_from_user(&state, (void __user *)arg, sizeof(state)))
			return -EFAULT;

		if (!state.mask || (state.mask & ~CMP_MODULE_GPIO_ALL))
			return -EINVAL;

		mutex_lock(&cm->lock);
		for (flag = 1; flag <= CMP_MODULE_GPIO_ALL; flag <<= 1) {
			struct cmp_module_gpio *gpio;

			if (!(state.mask & flag))
				continue;

			gpio = cmp_module_find_gpio(cm, flag);
			if (!gpio) {
				ret = -EINVAL;
				break;
			}

			cmp_module_set_gpio(gpio, !!(state.value & flag));
		}
		mutex_unlock(&cm->lock);
		return ret;

	case CMP_MODULE_IOC_GET_GPIOS:
		if (copy_from_user(&state, (void __user *)arg, sizeof(state)))
			return -EFAULT;

		if (!state.mask)
			state.mask = CMP_MODULE_GPIO_ALL;
		if (state.mask & ~CMP_MODULE_GPIO_ALL)
			return -EINVAL;

		state.value = 0;

		mutex_lock(&cm->lock);
		for (flag = 1; flag <= CMP_MODULE_GPIO_ALL; flag <<= 1) {
			struct cmp_module_gpio *gpio;

			if (!(state.mask & flag))
				continue;

			gpio = cmp_module_find_gpio(cm, flag);
			if (!gpio) {
				ret = -EINVAL;
				break;
			}

			value = cmp_module_get_gpio(gpio);
			if (value < 0) {
				ret = value;
				break;
			}
			if (value)
				state.value |= flag;
		}
		mutex_unlock(&cm->lock);

		if (ret)
			return ret;

		if (copy_to_user((void __user *)arg, &state, sizeof(state)))
			return -EFAULT;

		return 0;

	case CMP_MODULE_IOC_SET_USB_MODE:
		if (get_user(usb_mode, (__u32 __user *)arg))
			return -EFAULT;

		mutex_lock(&cm->lock);
		ret = cmp_module_set_usb_mode(cm, usb_mode);
		mutex_unlock(&cm->lock);

		return ret;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations cmp_module_fops = {
	.owner = THIS_MODULE,
	.open = cmp_module_open,
	.release = cmp_module_release,
	.unlocked_ioctl = cmp_module_ioctl,
	.llseek = no_llseek,
};

static int cmp_module_probe(struct platform_device *pdev)
{
	struct cmp_module *cm;
	struct device_node *np = pdev->dev.of_node;
	unsigned int i;
	int ret;

	if (!np)
		return -ENODEV;

	cm = devm_kzalloc(&pdev->dev, sizeof(*cm), GFP_KERNEL);
	if (!cm)
		return -ENOMEM;

	cm->dev = &pdev->dev;
	cm->np = np;
	mutex_init(&cm->lock);

	cm->gpios[0].name = "codec-mute";
	cm->gpios[0].con_id = "codec-mute";
	cm->gpios[0].flag = CMP_MODULE_GPIO_CODEC_MUTE;
	cm->gpios[0].default_value = 0;

	cm->gpios[1].name = "usb-switch";
	cm->gpios[1].con_id = "usb-switch";
	cm->gpios[1].flag = CMP_MODULE_GPIO_USB_SWITCH;
	cm->gpios[1].default_value = 1;

	for (i = 0; i < ARRAY_SIZE(cm->gpios); i++) {
		cm->gpios[i].desc = devm_gpiod_get(cm->dev,
				cm->gpios[i].con_id, GPIOD_ASIS);
		if (IS_ERR(cm->gpios[i].desc)) {
			ret = PTR_ERR(cm->gpios[i].desc);
			dev_err(cm->dev, "failed to get %s gpio: %d\n",
				cm->gpios[i].name, ret);
			return ret;
		}
	}

	ret = cmp_module_set_usb_mode(cm, CMP_MODULE_USB_MODE_HOST);
	if (ret)
		dev_warn(cm->dev, "failed to set default usb host mode: %d\n",
			 ret);

	cm->miscdev.minor = MISC_DYNAMIC_MINOR;
	cm->miscdev.name = CMP_MODULE_DEVICE_NAME;
	cm->miscdev.fops = &cmp_module_fops;
	cm->miscdev.parent = cm->dev;

	platform_set_drvdata(pdev, cm);

	ret = misc_register(&cm->miscdev);
	if (ret) {
		dev_err(cm->dev, "failed to register /dev/%s: %d\n",
			CMP_MODULE_DEVICE_NAME, ret);
		return ret;
	}

	dev_info(cm->dev, "registered /dev/%s from node %pOF\n",
		 CMP_MODULE_DEVICE_NAME, cm->np);

	return 0;
}

static int cmp_module_remove(struct platform_device *pdev)
{
	struct cmp_module *cm = platform_get_drvdata(pdev);

	misc_deregister(&cm->miscdev);

	return 0;
}

static const struct of_device_id cmp_module_of_match[] = {
	{ .compatible = "tsy,cmp-module" },
	{ }
};
MODULE_DEVICE_TABLE(of, cmp_module_of_match);

static struct platform_driver cmp_module_driver = {
	.probe = cmp_module_probe,
	.remove = cmp_module_remove,
	.driver = {
		.name = CMP_MODULE_DEVICE_NAME,
		.of_match_table = cmp_module_of_match,
	},
};
module_platform_driver(cmp_module_driver);

MODULE_AUTHOR("tsy");
MODULE_DESCRIPTION("CMP board module GPIO control driver");
MODULE_LICENSE("GPL");
