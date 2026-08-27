---
title: HLK-7621 U-Boot(2026.07) 启动方式分析
---

# 硬件与镜像现状

- SoC: MediaTek MT7621 (MIPS, 小端 mipsel 24Kc)
- 存储: SPI NOR Flash
- U-Boot: v2026.07，MTK 官方 mt7621_rfb 平台，构建产物在 `u-boot-2026.07/build/`
- 烧到 0x0 的完整固件: `build/u-boot-mt7621.bin` (约 180KB)

# U-Boot 关键能力（据 build/.config）

| 项 | 值 | 含义 |
|----|----|----|
| CONFIG_LEGACY_IMAGE_FORMAT | y | 支持传统 uImage (bootm) |
| CONFIG_FIT | n | FIT 已关 |
| CONFIG_BOOTSTD / BOOTMETH_* | n | 标准启动流程全关 |
| CONFIG_CMD_BOOTM | y | `bootm` 可用 |
| CONFIG_CMD_BOOTZ | n | 无 `bootz` |
| CONFIG_CMD_SF / SPI_FLASH | y | `sf probe/read/write` 可用 |
| CONFIG_CMD_TFTPBOOT / NET | y | TFTP 网络启动可用 |
| CONFIG_ENV_IS_IN_SPI_FLASH | y | env 存在 SPI 0x30000 |
| CONFIG_SYS_LOAD_ADDR | 0x83000000 | 默认加载地址 |
| CONFIG_BOOTDELAY | 10 | 倒计时 10 秒 |
| CONFIG_USE_BOOTCOMMAND | n | 未预置 bootcmd，需自己 setenv |

结论：**这套 U-Boot 只认“传统 uImage”，非常适合 OpenWrt ramips/mt7621 的 legacy uImage 镜像**。

# OpenWrt 镜像（HLK-7621A EVB, 24.10.8, kernel 6.6.144）

`bin/targets/ramips/mt7621/` 下两个都是 **u-boot legacy uImage**（magic 0x27051956），
Load/Entry = 0x80001000，OS=Linux MIPS，未压缩，DTB 已内嵌：

1. `openwrt-...-initramfs-kernel.bin` (约 8.4MB)
   - 内核 + initramfs 根文件系统打包成单个 uImage
   - 用于“直接载入内存启动”，不需要根文件系统分区
2. `openwrt-...-squashfs-sysupgrade.bin` (约 8.7MB)
   - 开头是同样的内核 uImage（约 3.3MB），后面追加 squashfs rootfs + padding
   - 用于整体烧进 `firmware` 分区

# 目标 Flash 分区布局（来自内核 DTS）

```
0x000000  u-boot      (0x0    ~ 0x30000)  192K  ← 烧 u-boot-mt7621.bin
0x030000  u-boot-env  (0x30000 ~ 0x40000)  64K   ← U-Boot env
0x040000  factory     (0x40000 ~ 0x50000)  64K   ← 出厂信息/MAC
0x050000  firmware    (0x50000 ~ 0x2000000) 31.5M ← 内核+rootfs (denx,uimage)
```

u-boot-mt7621.bin 约 180KB < 192KB，可完整装入 u-boot 分区，不越界到 env。

# 启动方式对比

## 方式 A — TFTP 载入内存启动（initramfs 镜像）⭐ 推荐
最适合：首次启动 / 调试 / 恢复 / 测试。
```
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.100
tftpboot 0x82000000 openwrt-...-initramfs-kernel.bin
bootm 0x82000000
```
bootm 会自动按 uImage 头部把内核搬到 0x80001000 并跳转。
优点：自包含（内核+rootfs 都在内存），无需根文件系统分区、无需复杂 bootargs。

## 方式 B — 烧进 SPI NOR 从 Flash 启动（sysupgrade 镜像）
最适合：永久/正式安装到板子。
先把 sysupgrade.bin 整体写进 firmware 分区 (0x50000)，再用 U-Boot 读取并 bootm：
```
# 首次烧写（在 U-Boot 里）
sf probe
sf erase 0x50000 0x1fb0000
tftpboot 0x82000000 openwrt-...-squashfs-sysupgrade.bin
sf write 0x82000000 0x50000 ${filesize}

# 之后每次启动
sf probe
sf read 0x82000000 0x50000 0x400000
bootm 0x82000000
```
内核会自动根据 DTS 的 `firmware`(denx,uimage) 分区定位并挂载 squashfs rootfs。

# 结论：最适合哪种？
- 你这套 U-Boot 是 **legacy-uImage 专用**，而 OpenWrt 这个板子的两个镜像正好也是 **legacy uImage**，完全匹配、无需改镜像。
- **日常跑/调试用 initramfs-kernel.bin 走 TFTP（方式 A）最省事**；
- **定稿后把 squashfs-sysupgrade.bin 烧进 0x50000 分区（方式 B）做正式系统**。
