/* SPDX-License-Identifier: GPL-2.0 */
/*
 *
 * Includes for ch343.c
 *
 */

#ifndef _CH343_H
#define _CH343_H

#define IGNORE_RTSDTR
#undef IGNORE_RTSDTR

/*
 * Baud rate and default timeout
 */
#define DEFAULT_BAUD_RATE 9600
#define DEFAULT_TIMEOUT 2000

/*
 * CMSPAR, some architectures can't have space and mark parity.
 */
#ifndef CMSPAR
#define CMSPAR 0
#endif

/*
 * Major and minor numbers.
 */
#define CH343_TTY_MAJOR 170
#define CH343_TTY_MINORS 256

#define CH343_MINOR_INVALID CH343_TTY_MINORS

#define USB_MINOR_BASE 70

/*
 * Vendor requests used by WCH chips.
 *
 * 这组 CMD_* 不是 CDC-ACM 标准请求，而是沁恒芯片自己的控制命令。
 * 串口数据仍然走 bulk 端点；波特率、线控、break、芯片识别等控制面
 * 配置才通过这些 vendor control request 完成。
 */
#define USB_RT_CH343 (USB_TYPE_CLASS | USB_RECIP_INTERFACE)

#define CMD_R 0x95
#define CMD_W 0x9A
#define CMD_C1 0xA1
#define CMD_C2 0xA4
#define CMD_C3 0x05
#define CMD_C4 0xA8
#define CMD_C5 0x5E
#define CMD_C6 0x5F
#define CMD_C7 0xC0
#define CMD_C8 0xC1

#define CH343_CTO_O 0x10
#define CH343_CTO_D 0x20
#define CH343_CTO_R 0x40
#define CH343_CTO_A 0x80
#define CH343_CTI_C 0x01
#define CH343_CTI_DS 0x02
#define CH343_CTI_R 0x04
#define CH343_CTI_DC 0x08
#define CH343_CTI_ST 0x0f

#define CH343_CTT_M 0x08
#define CH343_CTT_F 0x44
#define CH343_CTT_P 0x04
#define CH343_CTT_O 0x02
#define CH343_CTT_B 0x01

#define CH343_LO 0x02
#define CH343_LE 0x04
#define CH343_LB
#define CH343_LP 0x00
#define CH343_LF 0x40
#define CH343_LM 0x08

#define CH343_L_R_CT 0x80
#define CH343_L_R_CL 0x04
#define CH343_L_R_T 0x08

#define CH343_L_E_R 0x80
#define CH343_L_E_T 0x40
#define CH343_L_P_S 0x38
#define CH343_L_P_M 0x28
#define CH343_L_P_E 0x18
#define CH343_L_P_O 0x08
#define CH343_L_SB 0x04
#define CH343_L_C8 0x03
#define CH343_L_C7 0x02
#define CH343_L_C6 0x01
#define CH343_L_C5 0x00

#define CH343_N_B 0x80
#define CH343_N_AB 0x10

#define CH343_NW 2
#define CH343_NR 2

#define IOID 0x13572468

/*
 * 写缓冲。tty write() 先把用户数据复制到这里，再用 bulk OUT URB
 * 异步提交给 USB core。CH343_NW 决定并行写缓冲数量。
 */
struct ch343_wb {
	unsigned char *buf;
	dma_addr_t dmah;
	int len;
	int use;
	struct urb *urb;
	struct ch343 *instance;
};

/*
 * 读缓冲。驱动长期挂起若干 bulk IN URB，设备一有串口数据就回调到
 * ch343_read_bulk_callback()，再送入 tty flip buffer。
 */
struct ch343_rb {
	int size;
	unsigned char *base;
	dma_addr_t dma;
	int index;
	struct ch343 *instance;
};

/*
 * 驱动内部保存的串口格式。名字像 CDC line coding，但真正下发硬件时
 * 会被 ch343_tty_set_termios() 翻译成 CMD_C1 等私有控制命令。
 */
struct usb_ch343_line_coding {
	__u32 dwDTERate;
	__u8 bCharFormat;
#define USB_CH343_1_STOP_BITS 0
#define USB_CH343_1_5_STOP_BITS 1
#define USB_CH343_2_STOP_BITS 2

	__u8 bParityType;
#define USB_CH343_NO_PARITY 0
#define USB_CH343_ODD_PARITY 1
#define USB_CH343_EVEN_PARITY 2
#define USB_CH343_MARK_PARITY 3
#define USB_CH343_SPACE_PARITY 4

	__u8 bDataBits;
} __packed;

enum CHIPTYPE {
	CHIP_CH342F = 0x00,
	CHIP_CH342K,
	CHIP_CH343GP,
	CHIP_CH343G_AUTOBAUD,
	CHIP_CH343K,
	CHIP_CH343J,
	CHIP_CH344L,
	CHIP_CH344L_V2,
	CHIP_CH344Q,
	CHIP_CH347TF,
	CHIP_CH9101UH,
	CHIP_CH9101RY,
	CHIP_CH9102F,
	CHIP_CH9102X,
	CHIP_CH9103M,
	CHIP_CH9104L,
	CHIP_CH340B,
	CHIP_CH339W,
	CHIP_CH9111L_M0,
	CHIP_CH9111L_M1,
	CHIP_CH9114L,
	CHIP_CH9114W,
	CHIP_CH9114F,
	CHIP_CH346C_M0,
	CHIP_CH346C_M1,
	CHIP_CH346C_M2,
	CHIP_CH9105W,
	CHIP_CH9433K,
};

/*
 * 每个 CH343 串口实例的总状态对象。
 *
 * /dev/ttyCH343USBX 这个设备节点最终会通过 minor/index 找到一个
 * struct ch343。它里面同时保存 tty 端口、USB 接口、端点、URB、线控
 * 状态和芯片类型，是“设备节点”和“真实 USB 硬件”之间的桥。
 */
struct ch343 {
	struct usb_device *dev; /* 对应的 USB 设备 */
	struct usb_interface *control; /* 控制/状态接口 */
	struct usb_interface *data; /* 数据接口，bulk IN/OUT 在这里 */
	struct tty_port port; /* tty 层看到的串口端口 */
	struct urb *ctrlurb; /* interrupt IN 状态 URB */
	u8 *ctrl_buffer; /* ctrlurb 的 DMA 缓冲 */
	dma_addr_t ctrl_dma; /* ctrl_buffer 的 DMA 地址 */
	struct ch343_wb wb[CH343_NW];
	unsigned long read_urbs_free;
	struct urb *read_urbs[CH343_NR];
	struct ch343_rb read_buffers[CH343_NR];
	int rx_buflimit;
	int rx_endpoint;
	spinlock_t read_lock;
	int write_used; /* number of non-empty write buffers */
	int transmitting;
	spinlock_t write_lock;
	struct mutex mutex;
	struct mutex proc_mutex;
	bool disconnected;
	struct usb_ch343_line_coding line; /* baudrate, data format */
	struct work_struct work; /* used for line discipline waking up */
	unsigned int ctrlin; /* input lines (CTS, DSR, DCD, RI) */
	unsigned int ctrlout; /* output control lines (DTR, RTS) */
	struct async_icount iocount; /* counters for control line changes */
	struct async_icount oldcount; /* for comparison of counter */
	wait_queue_head_t wioctl; /* for ioctl */
	wait_queue_head_t sendioctl; /* for ioctl */
	unsigned int writesize; /* max packet size*/
	unsigned int readsize, ctrlsize; /* buffer sizes for freeing */
	unsigned int minor; /* ch343 minor number */
	unsigned char clocal; /* termios CLOCAL */
	unsigned int susp_count; /* number of suspended interfaces */
	u8 bInterval;
	struct usb_anchor delayed; /* used for a device about to be woken */
	unsigned long quirks;
	u8 iface;
	u8 num_ports;
	struct usb_interface *io_intf;
	struct kref kref;
	enum CHIPTYPE chiptype;
	bool iosupport;
	u16 idVendor;
	u16 idProduct;
	u8 gpio5dir;
	u32 io_id;
	u32 gfreq;
};

#define CDC_DATA_INTERFACE_TYPE 0x0a

/* constants describing various quirks and errors */
#define NO_UNION_NORMAL BIT(0)
#define SINGLE_RX_URB BIT(1)
#define NO_CAP_LINE BIT(2)
#define NO_DATA_INTERFACE BIT(4)
#define IGNORE_DEVICE BIT(5)
#define QUIRK_CONTROL_LINE_STATE BIT(6)
#define CLEAR_HALT_CONDITIONS BIT(7)

#endif
