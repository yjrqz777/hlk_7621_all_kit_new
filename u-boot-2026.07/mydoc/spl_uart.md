---
title: HLK-7621-uboot(2026.07)笔记-05
date: 2026-08-23 10:13:00
categories:
  - OpenWrt
tags:
  - MT7621
  - OpenWrt
  - U-Boot
  - spl
  - uart
---

```
preloader_console_init()
        ↓
serial_init()
        ↓
get_current()
        ↓
default_serial_console()
        ↓
mtk_hsuart1_device.start()
        ↓
mtk_serial1_init()          ← 真正初始化 UART
        ↓
_mtk_serial_setbrg()        ← 设置波特率
```

# arch/mips/mach-mtmips/mt7621/spl/spl.c
## mtmips_spl_serial_init()
- 清位
# common/spl/spl.c
## preloader_console_init()
- 初始化串口
# drivers/serial/serial.c
## get_current()->start()
- 初始化函数
### DECLARE_HSUART（函数通过一系列宏创建）
```
mtk_serial1_init()
{
    // 关闭 UART 中断
    writel(0, &regs->ier);

    // 配置 Modem Control Register
    writel(UART_MCRVAL, &regs->mcr);

    // 配置 FIFO
    writel(UART_FCRVAL, &regs->fcr);

    // 设置波特率
    _mtk_serial_setbrg(...);

    return 0;
}
```

## DECLARE_HSUART(1, "mtk-hsuart0");（用宏批量生成一个串口设备结构体，并绑定对应的操作函数）
- 注册初始化start
```
static struct mtk_serial_priv mtk_hsuart1 = {
    ...
};

static int mtk_serial1_init(void)
{
    ...
}

static void mtk_serial1_putc(...)
{
    ...
}

struct serial_device mtk_hsuart1_device = {
    .name  = "mtk-hsuart0",
    .start = mtk_serial1_init,
    .putc  = mtk_serial1_putc,
    ...
};
```
