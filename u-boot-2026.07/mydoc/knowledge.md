---
title: HLK-7621-uboot(2026.07)笔记-knowledge
date: 2026-08-26 20:30:00
categories:
  - OpenWrt
tags:
  - MT7621
  - OpenWrt
  - U-Boot
  - tpl
---

# 1 u-boot 
## 1.1 FIT (Flattened Image Tree)
- 是 U-Boot 的一种"打包容器"格式（本质是一个描述性 DTB，把内核、设备树、initramfs 等多个文件打包在一起）。  

|             | 传统 uImage                     | FIT 镜像 |
|-----------  | -----------                     | ----------- |
|内核文件     | 单独一个 uImage（含 header）      | 打包进 .itb 容器  |
|设备树       | dtb	单独一个文件                  | 可一起打包进 .itb |
|initramfs    | 单独一个文件                      | 可一起打包进 .itb |
|校验         | 只有 CRC                          | 可带 sha256 校验、可多内核/多配置 |

FIT 主要给 需要多套内核+多套设备树、或需要更强完整性校验 的场景用（比如一个固件适配多个硬件型号，靠 FIT 里塞多个 dtb 来切换）。

## 1.2 PXE(Preboot eXecution Environment)
- 一种从网络启动的机制
- 板子没有本地可启动系统时，通过 DHCP + TFTP 从服务器下载内核和启动脚本（类似装机时 PXE 网启 Linux 那套）