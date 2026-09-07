---
title: OpenWrt MT7621 GPIO13 LED 配置
date: 2026-08-24 22:00:00
description: 在 OpenWrt 中为 HiLink HLK-7621A 配置 GPIO13 LED，并启用系统心跳触发器
top_img: /img/openwrt/openwrt_logo_text_horizontal_blue_and_dark_blue.svg
cover: /img/openwrt/openwrt_text_blue_and_dark_blue.svg
categories:
  - OpenWrt
tags:
  - MT7621
  - OpenWrt
  - U-Boot
  - ASCII
---

# OpenWrt MT7621 GPIO13 LED 配置

本文记录 HiLink HLK-7621A EVB 在 U-Boot 和 OpenWrt 中配置同一颗 GPIO13 LED 的关键区别及配置方法。

## U-Boot 与 OpenWrt 的 GPIO 引用

U-Boot 的 `mt7621.dtsi` 将 GPIO 划分为多个 bank，GPIO13 使用：

```dts
gpios = <&gpio0 13 GPIO_ACTIVE_LOW>;
```

OpenWrt 使用统一的 GPIO 控制器，GPIO13 必须写成：

```dts
gpios = <&gpio 13 GPIO_ACTIVE_LOW>;
```

两种写法控制的是同一个物理 GPIO13，但对应的 DTS GPIO 控制器模型不同，不能直接混用。

## OpenWrt 设备树配置

文件：`target/linux/ramips/dts/mt7621_hilink_hlk-7621a-evb.dts`

GPIO13 是 JTAG 的 JTDO 引脚，需要切换为普通 GPIO，并注册为 LED：

```dts
leds {
	compatible = "gpio-leds";

	boot_led: led-boot {
		label = "boot";
		gpios = <&gpio 13 GPIO_ACTIVE_LOW>;
	};
};
```

同时配置 pinmux：

```dts
&state_default {
	gpio {
		groups = "jtag", "wdt";
		function = "gpio";
	};
};
```

`GPIO_ACTIVE_LOW` 表示低电平点亮 LED。

## 启用系统心跳

文件：`target/linux/ramips/mt7621/base-files/etc/board.d/01_leds`

在 `case $board in` 中加入：

```sh
hilink,hlk-7621a-evb)
	ucidef_set_led_heartbeat "boot" "Boot LED" "boot"
	;;
```

其中板卡名称必须与 DTS 中的 `compatible` 一致，LED 名称必须与 DTS 中的 `label = "boot"` 一致。

## 启动后验证

```sh
ls /sys/class/leds/
cat /sys/class/leds/boot/trigger
```

看到 `boot` 且输出中显示 `[heartbeat]`，表示 LED 已注册并使用系统心跳触发器。

手动测试：

```sh
echo none > /sys/class/leds/boot/trigger
echo 1 > /sys/class/leds/boot/brightness
echo 0 > /sys/class/leds/boot/brightness
```

使用 `GPIO_ACTIVE_LOW` 后，Linux 会自动处理电平反相，逻辑上的 `brightness=1` 表示点亮。
