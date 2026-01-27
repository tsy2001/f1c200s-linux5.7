// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * rc-alientek.c - Keymap for Alientek remote (placeholder)
 *
 * Replace the scancodes below with values from your remote.
 * You can capture scancodes with: ir-keytable -t
 */

#include <linux/module.h>
#include <media/rc-map.h>

static struct rc_map_table alientek[] = {
	/* TODO: replace the scancodes with your remote's values */
	{ RC_SCANCODE_NEC(0x00, 0x40), KEY_PAUSE },
	{ RC_SCANCODE_NEC(0x00, 0x47), KEY_EJECTCD },
	{ RC_SCANCODE_NEC(0x00, 0x45), KEY_POWER },
	{ RC_SCANCODE_NEC(0x00, 0x4A), KEY_ENTER },
	{ RC_SCANCODE_NEC(0x00, 0x46), KEY_UP },
	{ RC_SCANCODE_NEC(0x00, 0x15), KEY_DOWN },
	{ RC_SCANCODE_NEC(0x00, 0x44), KEY_PREVIOUS },
	{ RC_SCANCODE_NEC(0x00, 0x43), KEY_NEXT },
	{ RC_SCANCODE_NEC(0x00, 0x07), KEY_VOLUMEDOWN },
	{ RC_SCANCODE_NEC(0x00, 0x09), KEY_VOLUMEUP },
	{ RC_SCANCODE_NEC(0x00, 0x42), KEY_NUMERIC_0 },
	{ RC_SCANCODE_NEC(0x00, 0x16), KEY_NUMERIC_1 },
	{ RC_SCANCODE_NEC(0x00, 0x19), KEY_NUMERIC_2 },
	{ RC_SCANCODE_NEC(0x00, 0x0D), KEY_NUMERIC_3 },
	{ RC_SCANCODE_NEC(0x00, 0x0C), KEY_NUMERIC_4 },
	{ RC_SCANCODE_NEC(0x00, 0x18), KEY_NUMERIC_5 },
	{ RC_SCANCODE_NEC(0x00, 0x5E), KEY_NUMERIC_6 },
	{ RC_SCANCODE_NEC(0x00, 0x08), KEY_NUMERIC_7 },
	{ RC_SCANCODE_NEC(0x00, 0x1C), KEY_NUMERIC_8 },
	{ RC_SCANCODE_NEC(0x00, 0x5A), KEY_NUMERIC_9 },
};

static struct rc_map_list alientek_map = {
	.map = {
		.scan     = alientek,
		.size     = ARRAY_SIZE(alientek),
		.rc_proto = RC_PROTO_NEC,
		.name     = RC_MAP_ALIENTEK,
	}
};

static int __init init_rc_map_alientek(void)
{
	return rc_map_register(&alientek_map);
}

static void __exit exit_rc_map_alientek(void)
{
	rc_map_unregister(&alientek_map);
}

module_init(init_rc_map_alientek);
module_exit(exit_rc_map_alientek);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Codex");
MODULE_DESCRIPTION("Keymap for Alientek remote (placeholder)");
