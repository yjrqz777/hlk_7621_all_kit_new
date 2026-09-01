/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 MediaTek Inc. All rights reserved.
 *
 * Author: Weijie Gao <weijie.gao@mediatek.com>
 */

#ifndef __CONFIG_MT7621_H
#define __CONFIG_MT7621_H

#define CFG_SYS_SDRAM_BASE		0x80000000

#define CFG_MAX_MEM_MAPPED		0x1c000000

#define CFG_SYS_INIT_SP_OFFSET		0x800000

/* Hold the recovery button during power-on to start the web flash server. */
#define CFG_EXTRA_ENV_SETTINGS                                      \
	"button_cmd_0_name=recovery\0"                              \
	"button_cmd_0=webflash\0"                                   \
	"boot_spi=sf probe 0; sf read 0x82000000 0x50000 0x400000; bootm 0x82000000\0" \
	"bootmenu_0=Boot OpenWrt from SPI Flash=run boot_spi\0"      \
	"bootmenu_1=WebFlash Recovery=webflash\0"                    \
	/* Exit is the third internal entry although its shortcut is 0. */ \
	"bootmenu_default=2\0"                                      \
	"bootcmd=run boot_spi\0"

/* Serial SPL */
#if defined(CONFIG_XPL_BUILD) && defined(CONFIG_SPL_SERIAL)
#define CFG_SYS_NS16550_CLK		50000000
#define CFG_SYS_NS16550_COM1		0xbe000c00
#endif

/* Serial common */
#define CFG_SYS_BAUDRATE_TABLE		{ 9600, 19200, 38400, 57600, 115200, \
					  230400, 460800, 921600 }

/* Dummy value */
#define CFG_SYS_UBOOT_BASE		0

#endif /* __CONFIG_MT7621_H */
