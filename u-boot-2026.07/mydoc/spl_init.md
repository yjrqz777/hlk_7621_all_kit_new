---
title: HLK-7621-uboot(2026.07)笔记-04
date: 2026-08-23 10:30:00
categories:
  - OpenWrt
tags:
  - MT7621
  - OpenWrt
  - U-Boot
  - spl
---

# arch/mips/mach-mtmips/mt7621/spl/spl.c

## spl_init()

```
spl_init()
   │
   ├─ 判断是否已经做过 spl_early_init()
   │
   ├─ 如果没有
   │     ↓
   │   spl_common_init()
   │
   └─ 最后设置
         gd->flags |= GD_FLG_SPL_INIT
```
## spl_common_init()
├─ 初始化早期 malloc
├─ 初始化 bootstage 启动记录
├─ 初始化 log 日志系统
├─ 准备设备树 OF
└─ 初始化 Driver Model，并扫描/probe 设备

### SYS_MALLOC_F(CONFIG_SPL_SYS_MALLOC_F)
- 当前阶段是否启用了早期 malloc
### bootstage_init()
- 初始化 bootstage 启动阶段记录系统
### xpl_is_first_phase()
- 判断当前 SPL/TPL/VPL 是不是整个启动链的第一个阶段
### bootstage_mark_name(get_bootstage_id(true), xpl_name(xpl_phase()));
- 给当前启动阶段做一个开始标记
### OF_REAL(CONFIG_SPL_OF_REAL)
- 表示当前阶段使用真实的 Device Tree/FDT
### fdtdec_setup();
- 准备当前阶段的设备树，让后面的代码可以读取 DT 节点和属性
### DM(CONFIG_SPL_DM)
- 当前阶段是否启用了 U-Boot Driver Model。
### bootstage
- U-Boot 的启动阶段记录/计时机制
- 记录启动过程中某个阶段什么时候开始、结束、耗时多少，方便分析启动流程和启动速度
### OF(Open Firmware)
- 在 U-Boot 里主要指设备树（Device Tree / FDT）相关机制
### Driver Model（DM）
- U-Boot 的统一设备和驱动管理框架
- 统一管理串口、SPI、I2C、GPIO、网卡等设备，把设备按 uclass 分类，并负责设备与驱动的匹配、创建和初始化
```
OF = 硬件说明书
DM = 设备管理系统

设备树 OF
   ↓
描述 uart@1e000c00
   ↓
DM 扫描设备树
   ↓
创建串口 udevice
   ↓
根据 compatible 匹配 serial_mtk 驱动
   ↓
probe()
   ↓
串口可用

```