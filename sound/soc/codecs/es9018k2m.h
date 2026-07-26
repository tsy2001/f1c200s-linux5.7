#ifndef _ES9018K2M_H
#define _ES9018K2M_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ES9018K2M_IOCTL_MAGIC	'E'

#define ES9018K2M_INPUT_I2S	0
#define ES9018K2M_INPUT_SPDIF	1
#define ES9018K2M_INPUT_DSD	3

#define ES9018K2M_I2S_MODE_I2S	0
#define ES9018K2M_I2S_MODE_LJ	1

struct es9018k2m_volume {
	__u8 left;
	__u8 right;
};

struct es9018k2m_format {
	__u8 bits;
	__u8 mode;
	__u8 input;
	__u8 reserved;
};

struct es9018k2m_reg {
	__u8 reg;
	__u8 val;
};

struct es9018k2m_status {
	__u8 chip_status;
	__u8 gpio_status;
	__u8 muted;
	__u8 reserved;
	__u32 dpll_num;
	__u32 sample_rate;
};

#define ES9018K2M_IOCTL_GET_STATUS \
	_IOR(ES9018K2M_IOCTL_MAGIC, 0x00, struct es9018k2m_status)
#define ES9018K2M_IOCTL_SET_VOLUME \
	_IOW(ES9018K2M_IOCTL_MAGIC, 0x01, struct es9018k2m_volume)
#define ES9018K2M_IOCTL_GET_VOLUME \
	_IOR(ES9018K2M_IOCTL_MAGIC, 0x02, struct es9018k2m_volume)
#define ES9018K2M_IOCTL_SET_MUTE \
	_IOW(ES9018K2M_IOCTL_MAGIC, 0x03, __u8)
#define ES9018K2M_IOCTL_GET_MUTE \
	_IOR(ES9018K2M_IOCTL_MAGIC, 0x04, __u8)
#define ES9018K2M_IOCTL_SET_FORMAT \
	_IOW(ES9018K2M_IOCTL_MAGIC, 0x05, struct es9018k2m_format)
#define ES9018K2M_IOCTL_GET_FORMAT \
	_IOR(ES9018K2M_IOCTL_MAGIC, 0x06, struct es9018k2m_format)
#define ES9018K2M_IOCTL_READ_REG \
	_IOWR(ES9018K2M_IOCTL_MAGIC, 0x07, struct es9018k2m_reg)
#define ES9018K2M_IOCTL_WRITE_REG \
	_IOW(ES9018K2M_IOCTL_MAGIC, 0x08, struct es9018k2m_reg)
#define ES9018K2M_IOCTL_RESET \
	_IO(ES9018K2M_IOCTL_MAGIC, 0x09)

#endif /* _ES9018K2M_H */
