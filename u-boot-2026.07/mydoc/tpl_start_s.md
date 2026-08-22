---
title: HLK-7621-uboot(2026.07)笔记-01
date: 2026-08-20 14:30:00
categories:
  - OpenWrt
tags:
  - MT7621
  - OpenWrt
  - U-Boot
---

# arch/mips/mach-mtmips/mt7621/tpl/start.S 
## bal 
- Branch And Link
1. 跳到 mips_cm_map
2. 把返回地址保存到 ra
## jr
Jump Register

## 0xbe10dff0 是什么地址？
MT7621 是 MIPS32。
MIPS32 经典地址空间里：
```
0x80000000 ~ 0x9fffffff    KSEG0
                            cached
                            不经过 TLB

0xa0000000 ~ 0xbfffffff    KSEG1
                            uncached
                            不经过 TLB
```
### KSEG0 KSEG1
- 是一段 CPU 的虚拟地址空间

## .set noreorder
- 它不是 CPU 指令，而是告诉 汇编器：不要擅自帮我重新排列指令。

## .macro init_wr sel 
- 定义一个汇编宏 #define init_wr(sel) ...

### Watchpoint
- 硬件监视点

### I R W
1. I：Instruction Fetch
   监视CPU取指令
2. R：Read
   监视数据读取
3. W：Write
   监视数据写入

### mtc0：Move To Coprocessor 0 (MIPS:CP0 = Coprocessor 0)
- cp0: 不是普通协处理器，它实际上是 CPU 的 系统控制寄存器组

类似 ARM Cortex-M 的：
SCB
NVIC
SysTick
MPU
Fault status

### MTC0 zero, CP0_WATCHLO,\sel
1. $zero = 0
2. CP0 WatchLo(sel)
3. WatchLo = 0

#### zero 是什么？ 
- MIPS 有一个特别的寄存器：$0 $zero 它永远等于：0

 
  
## uhi_mips_exception
- UHI 是 MIPS 的：Unified Hosting Interface
- 可以理解为 MIPS 的 semihosting 接口 (调试接口)
- 如果发生异常，就调用 UHI 把异常交给调试环境

## sdbbp 1
- Software Debug Breakpoint

```
#define SP_ADDR_TEMP		0xbe10dff0

	.set noreorder

	.macro init_wr sel
	MTC0	zero, CP0_WATCHLO,\sel
	mtc0	t1, CP0_WATCHHI,\sel
	.endm

	.macro uhi_mips_exception
	move	k0, t9		# preserve t9 in k0
	move	k1, a0		# preserve a0 in k1
	li	t9, 15		# UHI exception operation
	li	a0, 0		# Use hard register context
	sdbbp	1		# Invoke UHI operation
	.endm
```
实际上可以粗略翻译成:
```
/*
 * DDR还不能用。
 * 暂时使用MT7621内部SRAM作为栈。
 */
#define TEMP_STACK 0xbe10dff0;


/*
 * 不允许汇编器自动调换指令顺序，
 * 因为后面大量依赖MIPS delay slot和CP0操作顺序。
 */


/*
 * 清理某一组CPU硬件Watchpoint。
 */
#define init_watch_register(sel)        \
{                                       \
    CP0_WATCHLO[sel] = 0;               \
    CP0_WATCHHI[sel] = 0x7;             \
}


/*
 * 如果TPL早期发生TLB/cache/general等异常，
 * 通过MIPS UHI/debug接口通知调试环境。
 */
void early_exception(void)
{
    k0 = t9;
    k1 = a0;

    t9 = UHI_EXCEPTION;      // 15
    a0 = HARD_REGISTER_CONTEXT;

    SDBBP(1);
}
```

## ENTRY(_start)
- 把 `_start` 声明成一个可以被链接器识别的全局入口符号
- 从这里开始，是这段 TPL 镜像的入口 _start
## b reset
1. 执行 b reset
2. 执行它后面的那一条指令(MIPS 的 branch delay slot)
3. 才真正跳到 reset

### branch delay slot
- 分支延迟槽
- 不能空着：CPU 还是会去取 PC+4 那 4 个字节，当成 delay slot 指令执行

## mtc0 zero, CP0_COUNT(MIPS CPU 内部的一个计数器)
- 把一个通用寄存器的值写入 CP0 寄存器 (CP0_COUNT = 0)

## 1b 1f
1b: 跳转到前（backward）一个1标签
1f: 跳转到后（forward）一个1标签

## .org 0x200
- 镜像地址布局控制
- 将后面的代码放到 0x200，这里将异常地址放入 uhi_mips_exception 进行处理
```
文件offset 0x000 → CPU地址 0xBFC00000
文件offset 0x200 → CPU地址 0xBFC00200
文件offset 0x380 → CPU地址 0xBFC00380
文件offset 0x480 → CPU地址 0xBFC00480
```


# CM
- CM (Coherence Manager) :一致性管理器
: 两个 CPU Core 各自有 Cache 时，怎么保证看到的数据是一致的？

CM 是一个硬件模块，而 GCR 是你用来“配置和查看 CM”的那组寄存器

有很多控制寄存器，统称：
GCR(Global Configuration Registers)
全局配置寄存器
```
        Core 0
       ┌───────┐
       │ L1 D$ │
       │ L1 I$ │
       └───┬───┘
           │
           │
        ┌──┴───┐
        │  CM  │
        └──┬───┘
           │
           │
       ┌───┴───┐
       │ L1 D$ │
       │ L1 I$ │
       └───────┘
        Core 1
```



## mips_cm_map
检查 MT7621/MIPS CPU 是否存在 CM（Coherence Manager），如果存在，就检查并把 CM 的 GCR 寄存器窗口映射到 U-Boot 配置的 CONFIG_MIPS_CM_BASE 地址。

CPU 支持 CM 的话，把多核一致性管理器 CM 的寄存器窗口映射到 U-Boot 预期的位置；不支持的话什么也不干，直接返回。

让后面的 U-Boot 代码知道：“以后去固定地址找 CM 寄存器。

告诉 U-Boot“CM 硬件本体在哪”，而是告诉/保证 U-Boot 以后去哪个固定地址访问 CM 的 GCR 寄存器窗口

```
void mips_cm_map(void)
{
    if (Config3不存在)
        return;

    if (!(Config3 & CMGCR))
        return;

retry:
    current_base = CMGCRBASE << 4;

    if (current_base == CONFIG_MIPS_CM_BASE)
        return;

    // 通过当前GCR地址访问GCR寄存器
    GCR_BASE_UPPER = 0;
    GCR_BASE = CONFIG_MIPS_CM_BASE;

    goto retry;
}

确认 CM 存在
      ↓
找到当前 GCR 地址
      ↓
是不是已经在目标地址？
   ┌──────┴──────┐
   是            否
   ↓             ↓
 return       修改GCR BASE
                 ↓
              再检查

```

## mips_sram_init
1. 先复位 PSE SRAM。
2. 解除复位，并开启 RAM 模式和内存使能

## 	li	sp, SP_ADDR_TEMP
- 把 sp 指向 SRAM
- 设置的是临时栈
- 这个位置落在 MT7621 的 FE/PSE 片内 SRAM 区域附近

## 	bal	tpl_main
- 执行`tpl_main`