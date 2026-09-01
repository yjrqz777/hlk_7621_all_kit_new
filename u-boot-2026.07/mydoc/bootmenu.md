---
title: HLK-7621A U-Boot 配置 WebFlash 与 SPI Flash 双启动菜单
date: 2026-09-01 15:00:00
categories:
  - OpenWrt
tags:
  - MT7621
  - HLK-7621A
  - U-Boot
  - OpenWrt
  - WebFlash
  - BootMenu
---

# 1 目标

HLK-7621A 使用自己编译的 U-Boot 2026.07。开启启动菜单后，如果没有定义菜单环境变量，串口只会显示：

```text
*** U-Boot Boot Menu ***

  0. Exit

Hit any key to stop autoboot: 4
Press UP/DOWN to move, ENTER to select, ESC to quit
```

本文为启动菜单增加两个功能：

1. 默认从 SPI NOR Flash 启动 OpenWrt。
2. 手动进入 WebFlash 恢复服务。

配置完成后的菜单如下：

```text
*** U-Boot Boot Menu ***

  0. Boot OpenWrt from SPI Flash
  1. WebFlash Recovery
  2. Exit
```

`bootmenu_0` 是倒计时结束时自动执行的默认项目，因此将正常的 SPI Flash 启动放在第 0 项，避免设备每次开机都停在 WebFlash 服务中。

<!-- more -->

# 2 Flash 分区与镜像信息

OpenWrt 板级 DTS 中的 SPI NOR 分区布局为：

```text
0x000000  u-boot      大小 0x30000
0x030000  u-boot-env  大小 0x10000
0x040000  factory     大小 0x10000
0x050000  firmware    大小 0x1fb0000
```

正式运行使用的镜像是：

```text
openwrt-ramips-mt7621-hilink_hlk-7621a-evb-squashfs-sysupgrade.bin
```

该镜像应完整写入 `firmware` 分区，也就是 SPI Flash 偏移 `0x50000`。镜像开头是 legacy uImage，当前内核部分约 3.2 MiB，其镜像头指定：

```text
Load Address: 0x80001000
Entry Point:  0x80001000
```

U-Boot 可以先把 Flash 中前 4 MiB 内容读到 `0x82000000`，再交给 `bootm`。`bootm` 会检查 uImage，并按照镜像头将内核放到正确地址。

# 3 在 U-Boot 命令行中设置

先中断自动启动，进入 `=>` 提示符，然后执行：

```text
setenv boot_spi 'sf probe 0; sf read 0x82000000 0x50000 0x400000; bootm 0x82000000'
setenv bootmenu_0 'Boot OpenWrt from SPI Flash=run boot_spi'
setenv bootmenu_1 'WebFlash Recovery=webflash'
setenv bootcmd 'run boot_spi'
setenv bootdelay 5
```

各变量作用如下：

| 环境变量 | 作用 |
|---|---|
| `boot_spi` | 初始化 SPI NOR、读取 OpenWrt 内核并执行 `bootm` |
| `bootmenu_0` | 第一个菜单项，也是超时后的默认项目 |
| `bootmenu_1` | 第二个菜单项，启动 WebFlash |
| `bootcmd` | 不使用菜单时的默认启动命令，同时作为备用入口 |
| `bootdelay` | 菜单倒计时，单位为秒 |

先检查变量是否正确：

```text
printenv boot_spi bootmenu_0 bootmenu_1 bootcmd bootdelay
```

可以在不自动倒计时的情况下测试菜单：

```text
bootmenu -1
```

确认无误后，将环境保存到 SPI NOR：

```text
saveenv
reset
```

当前 U-Boot 的环境区应配置为：

```text
CONFIG_ENV_IS_IN_SPI_FLASH=y
CONFIG_ENV_OFFSET=0x30000
CONFIG_ENV_SIZE=0x1000
CONFIG_ENV_SECT_SIZE=0x10000
```

其中 `CONFIG_ENV_OFFSET=0x30000` 必须与 OpenWrt DTS 中的 `u-boot-env` 分区一致，防止 `saveenv` 覆盖 U-Boot、Factory 或 OpenWrt 固件。

# 4 将菜单固化到 U-Boot 源码

命令行设置适合立即验证。如果希望清空环境后仍然保留这些默认菜单，需要修改：

```text
include/configs/mt7621.h
```

将 `CFG_EXTRA_ENV_SETTINGS` 设置为：

```c
#define CFG_EXTRA_ENV_SETTINGS                                      \
	"button_cmd_0_name=recovery\0"                              \
	"button_cmd_0=webflash\0"                                   \
	"boot_spi=sf probe 0; sf read 0x82000000 0x50000 0x400000; bootm 0x82000000\0" \
	"bootmenu_0=Boot OpenWrt from SPI Flash=run boot_spi\0"      \
	"bootmenu_1=WebFlash Recovery=webflash\0"                    \
	"bootcmd=run boot_spi\0"
```

相关构建选项至少应包括：

```text
CONFIG_BOOTDELAY=5
CONFIG_AUTOBOOT_MENU_SHOW=y
CONFIG_MENU=y
CONFIG_CMD_BOOTMENU=y
CONFIG_CMD_BOOTM=y
CONFIG_CMD_SF=y
CONFIG_CMD_WEBFLASH=y
CONFIG_ENV_IS_IN_SPI_FLASH=y
CONFIG_ENV_OFFSET=0x30000
CONFIG_ENV_SECT_SIZE=0x10000
```

当前菜单默认项为屏幕上的 `0. Exit`。由于 Exit 在内部是继两个功能项之后的第 3 项，环境变量使用：

```text
bootmenu_default=2
```

倒计时结束会退出菜单并进入 U-Boot 命令行；倒计时期间按任意普通键会停止自动选择，并停留在默认的 Exit 项。

如果使用 `menuconfig` 修改了 `build/.config`，应同步生成或更新 defconfig，避免清理构建目录后丢失配置：

```bash
make O=build savedefconfig
cp build/defconfig configs/mt7621_rfb_defconfig
```

重新编译 U-Boot：

```bash
make O=build -j$(nproc)
```

# 5 已保存环境与源码默认环境的关系

`CFG_EXTRA_ENV_SETTINGS` 提供的是编译时默认环境。如果 SPI 的 `0x30000` 中已经存在有效环境，U-Boot 会优先加载 SPI 中的旧环境，因此重新刷入 U-Boot 后，新增加的默认菜单可能不会立即出现。

有两种处理方法。

方法一：只设置本文涉及的变量并执行 `saveenv`。这种方式不会影响 MAC、网络参数等其他环境变量，风险较低。

方法二：恢复全部编译默认值：

```text
env default -a
saveenv
reset
```

注意，`env default -a` 会清除所有用户保存的环境变量。执行前应使用 `printenv` 记录需要保留的内容。

# 6 WebFlash 的当前行为

选择菜单中的 `WebFlash Recovery` 后，相当于执行：

```text
webflash
```

当前实现监听：

```text
http://192.168.31.66/
```

但目前 WebFlash **只把上传文件放入 RAM，不会擦除或写入 SPI Flash**。上传地址默认为 `0x83000000`，上传完成后会设置：

```text
fileaddr=83000000
filesize=<实际上传大小>
```

因此，这个菜单项目前是 Web 上传和恢复入口，不是全自动刷机入口。上传 initramfs 镜像后，可以按 `Ctrl-C` 退出 WebFlash，再检查并从 RAM 启动：

```text
iminfo ${fileaddr}
bootm ${fileaddr}
```

用于这种 RAM 启动的镜像是：

```text
openwrt-ramips-mt7621-hilink_hlk-7621a-evb-initramfs-kernel.bin
```

不要把 `squashfs-sysupgrade.bin` 当作完整的 RAM 系统使用。它的根文件系统需要位于 SPI Flash 的 `firmware` 分区中。

# 7 排查方法

## 7.1 菜单仍然只有 Exit

检查菜单变量：

```text
printenv bootmenu_0 bootmenu_1
```

如果变量不存在，说明 SPI 中的旧环境覆盖了源码默认环境，或者变量尚未执行 `saveenv`。

## 7.2 SPI 启动项提示找不到镜像

手动读取并检查 uImage：

```text
sf probe 0
sf read 0x82000000 0x50000 0x400000
iminfo 0x82000000
```

如果 `iminfo` 校验失败，应确认：

- `squashfs-sysupgrade.bin` 是否从 `0x50000` 开始写入；
- 写入长度是否等于固件文件的实际大小；
- 是否误写了 initramfs 镜像；
- SPI NOR 读取是否正常。

## 7.3 倒计时结束后没有自动启动

检查第一个菜单项和默认命令：

```text
printenv bootmenu_0 bootcmd bootdelay
```

应当得到类似结果：

```text
bootmenu_0=Boot OpenWrt from SPI Flash=run boot_spi
bootcmd=run boot_spi
bootdelay=5
```

# 8 最终启动流程

正常开机时：

```text
U-Boot 初始化
    -> 显示 bootmenu，默认选中第 0 项
    -> 倒计时结束
    -> sf probe 0
    -> 从 SPI 0x50000 读取 0x400000 字节到 RAM 0x82000000
    -> bootm 0x82000000
    -> OpenWrt 内核启动并挂载 SPI 中的 squashfs rootfs
```

需要恢复时，在倒计时期间选择 `WebFlash Recovery`，即可进入浏览器上传界面。
