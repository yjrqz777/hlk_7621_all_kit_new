// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2000
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

/* #define	DEBUG	*/

#include <autoboot.h>
#include <button.h>
#include <bootstage.h>
#include <bootstd.h>
#include <cli.h>
#include <command.h>
#include <console.h>
#include <env.h>
#include <fdtdec.h>
#include <init.h>
#include <net.h>
#include <version_string.h>
#include <efi_loader.h>
#include <event.h>

static void run_preboot_environment_command(void)
{
	char *p;

	p = env_get("preboot");
	if (p != NULL) {
		int prev = 0;

		if (IS_ENABLED(CONFIG_AUTOBOOT_KEYED))
			prev = disable_ctrlc(1); /* disable Ctrl-C checking */

		run_command_list(p, -1, 0);

		if (IS_ENABLED(CONFIG_AUTOBOOT_KEYED))
			disable_ctrlc(prev);	/* restore Ctrl-C checking */
	}
}

/* We come here after U-Boot is initialised and ready to process commands */
void main_loop(void)
{
	/* 保存最终要执行的启动命令，通常来自环境变量 bootcmd */
	const char *s;

	/*
	 * 记录进入 main_loop 的启动阶段，用于统计启动耗时。
	 * 当前未启用 CONFIG_BOOTSTAGE 时，这段代码不会产生实际功能。
	 */
	bootstage_mark_name(BOOTSTAGE_ID_MAIN_LOOP, "main_loop");/* 未使用 */

	/*
	 * 将 U-Boot 版本写入环境变量 ver。
	 * 当前未启用 CONFIG_VERSION_VARIABLE，不会实际执行。
	 */
	if (IS_ENABLED(CONFIG_VERSION_VARIABLE))/* 未使用 */
		env_set("ver", version_string);

	/*
	 * 初始化命令行解析器（例如 Hush Shell）及相关终端状态。
	 * 当前未启用 Hush Parser 时，这个函数基本为空。
	 */
	cli_init();/* 未使用 */

	/*
	 * 在自动启动前执行环境变量 preboot 中保存的命令。
	 * 当前未启用 CONFIG_USE_PREBOOT，不会实际执行。
	 */
	if (IS_ENABLED(CONFIG_USE_PREBOOT))/* 未使用 */
		run_preboot_environment_command();

	/*
	 * 通知事件处理程序：preboot 阶段已经结束。
	 * FWU 多 Bank 更新等功能可以在这里进行开机检查。
	 */
	if (event_notify_null(EVT_POST_PREBOOT))
		return;

	/*
	 * 开机时通过 TFTP 检查并更新 U-Boot。
	 * 当前未启用 CONFIG_UPDATE_TFTP，不会实际执行。
	 */
	if (IS_ENABLED(CONFIG_UPDATE_TFTP))/* 未使用 */
		update_tftp(0UL, NULL, NULL);

	/*
	 * 提前检查磁盘上的 UEFI Capsule，并执行 Capsule 固件升级。
	 * 当前 MT7621 配置没有启用该功能。
	 */
	if (IS_ENABLED(CONFIG_EFI_CAPSULE_ON_DISK_EARLY)) {/* 未使用 */
		/* efi_init_early() already called */
		if (efi_init_obj_list() == EFI_SUCCESS)
			efi_launch_capsules();
	}

	/*
	 * 检查开机按键，并执行对应的 button_cmd_x 环境命令。
	 * 当前配置用于“按住 WDT_RST_N 恢复键进入 webflash”。
	 */
	process_button_cmds();

	/*
	 * 读取 bootdelay，显示启动菜单，并根据启动状态选择
	 * bootcmd、altbootcmd 或 failbootcmd，返回最终启动命令。
	 */
	s = bootdelay_process();

	/*
	 * 允许设备树 /config/bootcmd 覆盖启动命令。
	 * 当 /config/bootsecure 非零时，以受限制的安全模式执行该命令。
	 * 这与是否启用 CMD_FDT 命令无关。
	 */
	if (cli_process_fdt(&s))
		cli_secure_boot_cmd(s);

	/*
	 * 显示自动启动倒计时；未被按键中断时执行上面选出的启动命令。
	 * 启动被中断或命令执行结束后，会继续进入下面的命令行。
	 */
	autoboot_command(s);

	/*
	 * 使用 Standard Boot Program 自动寻找并启动操作系统。
	 * 当前未启用 CONFIG_BOOTSTD_PROG，不会实际执行。
	 */
	if (IS_ENABLED(CONFIG_BOOTSTD_PROG)) {
		int ret;

		ret = bootstd_prog_boot();
		printf("Standard boot failed (err=%dE)\n", ret);
		panic("Failed to boot");
	}

	/*
	 * 自动启动失败或被中断后，进入永久的串口命令行循环。
	 * 可在这里手动执行 webflash、usb、bootmenu、sf 等命令。
	 */
	cli_loop();

	/* cli_loop 正常情况下不会返回；若异常返回则停止系统。 */
	panic("No CLI available");
}
