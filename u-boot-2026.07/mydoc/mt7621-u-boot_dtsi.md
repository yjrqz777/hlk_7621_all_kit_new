---
title: HLK-7621-uboot(2026.07)笔记-02
date: 2026-08-22 20:30:00
categories:
  - OpenWrt
tags:
  - MT7621
  - OpenWrt
  - U-Boot
  - tpl
---

# arch/mips/dts/mt7621-u-boot.dtsi
## 语法
```
节点名 {
    属性;
    子节点 {
        属性;
    };
};
```

## spl-img节点
```
spl-img
│
├── 输出：u-boot-spl-ddr.img
│
└── mkimage
    │
    ├── 参数：描述架构、镜像类型、加载地址、入口地址……
    │
    └── 输入：u-boot-spl-ddr.bin
```

> 将原始 SPL 二进制交给 `mkimage` 进行镜像格式封装。

---

## `filename`

```dts
spl-img {
    filename = "u-boot-spl-ddr.img";
};
```

表示这个 Binman 节点最终生成的文件名为：

```text
u-boot-spl-ddr.img
```

---

## `mkimage` 节点

```dts
mkimage {
    ...
};
```

表示 Binman 不直接复制数据，而是调用 U-Boot 的：

```text
mkimage
```

工具处理输入数据。

整体可以理解为：

```text
输入数据
   ↓
mkimage
   ↓
带镜像格式的数据
```

---

## `blob` 是 mkimage 的输入数据

```dts
blob {
    filename = "u-boot-spl-ddr.bin";
};
```

表示：

```text
u-boot-spl-ddr.bin
```

作为 `mkimage` 的输入。

`blob` 可以理解为：

> 一块不需要 Binman 解析内部结构的二进制数据。

因此：

```text
blob
 ↓
u-boot-spl-ddr.bin
 ↓
作为 mkimage 的 payload
```

---

## SPI NOR 启动时的 mkimage 参数

非 NAND 启动时使用：

```dts
args = "-A", "mips",
       "-T", "standalone",
       "-O", "u-boot",
       "-C", "none",
       "-n", "MT7621 U-Boot SPL",
       "-a", __stringify(CONFIG_SPL_TEXT_BASE),
       "-e", __stringify(CONFIG_SPL_TEXT_BASE);
```


### -A mips
> 架构是 MIPS

### -T standalone
> 镜像类型是 standalone

### -O u-boot
> 操作系统类型标记为 U-Boot

### -C none
> `u-boot-spl-ddr.bin` 本身不进行压缩。

### -n "MT7621 U-Boot SPL"
> 镜像名字

### -a CONFIG_SPL_TEXT_BASE
> SPL 应该被加载到哪个内存地址。

### -e CONFIG_SPL_TEXT_BASE
> SPL 加载完成后，从哪个地址开始执行。


## `__stringify`

```c
__stringify(CONFIG_SPL_TEXT_BASE)
```
用于把一个宏转换成字符串。
因为 `mkimage` 的参数需要字符串形式。
---

## `.bin` 和 `.img` 的关系

输入：


u-boot-spl-ddr.bin: 属于原始二进制数据。
结构可以理解为：

```text
[SPL][MT7621 stage SRAM code]
```
经过 `mkimage` 后生成：

u-boot-spl-ddr.img结构大致变成：

```text
+-------------------------+
| Image Header            |
+-------------------------+
| u-boot-spl-ddr.bin      |
|                         |
| SPL                     |
| stage SRAM code         |
+-------------------------+
```

.bin: 
> 主要表示原始二进制内容。
.img:
> 已经按照某种镜像格式封装的数据。

---

## 16. 整个节点的结构

```text
spl-img
│
├── 输出文件
│   └── u-boot-spl-ddr.img
│
└── mkimage
    │
    ├── args
    │   ├── CPU架构
    │   ├── 镜像类型
    │   ├── OS类型
    │   ├── 压缩方式
    │   ├── 镜像名称
    │   ├── Load Address
    │   └── Entry Point
    │
    └── blob
        └── u-boot-spl-ddr.bin
```

---

## 17. 最终流程

```text
u-boot-spl.bin
        +
mt7621_stage_sram.bin
        ↓
u-boot-spl-ddr.bin
        ↓
      mkimage
        ↓
加入镜像 Header / 镜像格式信息
        ↓
u-boot-spl-ddr.img
```

核心理解：

> `spl-img` 节点的作用，就是使用 `mkimage` 将 `u-boot-spl-ddr.bin` 封装成 `u-boot-spl-ddr.img`。
