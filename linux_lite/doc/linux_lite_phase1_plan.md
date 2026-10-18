# Linux Lite 第一阶段方案：MT7621 + BusyBox + initramfs

> 状态：方案基线 v0.1  
> 当前阶段：只确定方案，不写构建代码  
> 目标：在独立的 `linux_lite` 目录中制作一个只包含 BusyBox 用户空间的极简 Linux，继续使用板子上现有的 U-Boot。

---

## 1. 项目目标

### 1.1 最终启动链

```text
现有 U-Boot（不修改 U-Boot 二进制）
    ↓
Linux Lite Kernel（自己的内核配置和 DTS）
    ↓
内嵌 initramfs
    ↓
/init
    ↓
静态 BusyBox
    ↓
串口 shell: / #
```

### 1.2 第一阶段范围

第一阶段只做：

- 自己控制 Linux Kernel 配置；
- 自己控制板级 DTS；
- 静态编译 BusyBox；
- 制作只包含 BusyBox、`/init` 和必要目录/设备节点的 rootfs；
- 将 rootfs 内嵌进内核，生成 legacy uImage；
- 通过现有 U-Boot 的 `tftpboot + bootm` 启动验证。

第一阶段不做：

- OpenWrt 用户空间；
- procd、netifd、ubus、uci、opkg；
- WiFi、USB、LED、以太网应用；
- 完整包管理系统；
- 独立工具链构建；
- 修改 U-Boot 源码。

---

## 2. 独立目录原则

项目目录：

```text
/home/yjrqz/hlk_7621_all_kit/hlk_7621_all_kit_new/linux_lite
```

OpenWrt 只作为参考资料和临时外部工具链来源，不再作为本项目的构建目录。

第一阶段允许使用已有的 OpenWrt 交叉工具链：

```text
/home/yjrqz/hlk_7621_all_kit/hlk_7621_all_kit_new/openwrt-24.10.8/staging_dir/toolchain-mipsel_24kc_gcc-13.3.0_musl/bin/mipsel-openwrt-linux-musl-gcc
```

但构建产物、源码、配置、脚本全部位于 `linux_lite`。  
后续如果需要完全独立，再使用 Buildroot 或 crosstool-NG 自建工具链。

---

## 3. 当前硬件与启动环境事实

### 3.1 目标板

```text
设备：HiLink HLK-7621A evaluation board
SoC：MediaTek MT7621
架构：MIPS32 little-endian
OpenWrt 目标架构：mipsel_24kc
串口：ttyS0，115200
```

### 3.2 当前 U-Boot 真机环境

真机 `printenv` 已确认：

```text
bootcmd=run boot_spi
boot_spi=sf probe 0; sf read 0x82000000 0x50000 0x400000; bootm 0x82000000
```

启动菜单：

```text
1. Boot OpenWrt from SPI Flash
2. WebFlash Recovery
0. Exit
```

菜单项 1 对应源码中的：

```c
bootmenu_0=Boot OpenWrt from SPI Flash=run boot_spi
bootcmd=run boot_spi
```

因此选择菜单项 1 就是执行 `boot_spi`。

### 3.3 `boot_spi` 的含义

```text
sf probe 0
```

初始化 SPI Flash。

```text
sf read 0x82000000 0x50000 0x400000
```

从 SPI Flash 偏移 `0x50000` 读取 `0x400000` 字节（4 MiB）到 RAM `0x82000000`。

```text
bootm 0x82000000
```

对 RAM 中的 legacy uImage 执行 `bootm`。

### 3.4 为什么正常 OpenWrt 固件能启动

普通 sysupgrade 镜像不是单个 initramfs，而是：

```text
sysupgrade.bin
├── uImage 内核：约 3.14 MiB
└── 追加的 SquashFS rootfs：约 5.2 MiB
```

`boot_spi` 只读 4 MiB，但前 3.14 MiB 已经是完整内核 uImage，足够 `bootm` 启动。

Linux 内核启动后通过 MTD/SPI Flash 驱动自行访问后面的 SquashFS rootfs，因此 U-Boot 不需要把整个固件读到 RAM。

### 3.5 initramfs 镜像与 4 MiB 的关系

initramfs 镜像的 rootfs 被内嵌在 uImage 中。

当前现有 initramfs uImage 数据长度约 8.08 MiB，因此如果直接把它放到 Flash 后用默认 `boot_spi` 读取 4 MiB，镜像不能被完整加载。

第一阶段开发验证优先使用 TFTP，避免受 `boot_spi` 读取长度影响：

```text
tftpboot 0x82000000 <uImage>
bootm 0x82000000
```

后续如果要写 Flash 并用默认 `boot_spi` 自动启动，需要满足：

- 最终 uImage 小于 4 MiB；或
- 修改 U-Boot 环境变量中的 `sf read` 长度；或
- 继续使用 TFTP/手动启动。

修改 `boot_spi` 属于环境变量修改，不是修改 U-Boot 二进制。

---

## 4. 工具链方案 A

### 4.1 使用工具链

第一阶段使用：

```text
mipsel-openwrt-linux-musl-gcc
GCC 13.3.0
target: mipsel-openwrt-linux-musl
libc: musl
float: soft-float
```

这不是进入 OpenWrt 构建系统，而是将其作为外部交叉编译器使用。

### 4.2 必须显式传入目标参数

不能只设置 `CROSS_COMPILE`，构建时还需要以下目标参数：

```text
-mips32r2
-mtune=24kc
-msoft-float
-mno-branch-likely
```

推荐形式：

```sh
export CROSS_COMPILE=mipsel-openwrt-linux-musl-
export CFLAGS="-Os -mips32r2 -mtune=24kc -msoft-float -mno-branch-likely"
```

具体构建时由脚本统一管理，不依赖编译器自身的默认架构。

### 4.3 BusyBox 链接方式

第一阶段必须静态编译：

```text
CONFIG_STATIC=y
```

目标 rootfs 不包含动态链接器和 libc。

注意：静态 BusyBox 只消除动态库依赖，不能消除 CPU 指令集和 ABI 差异，因此仍使用匹配的 musl 软浮点工具链。

---

## 5. 计划目录结构

```text
linux_lite/
├── README.md
├── Makefile
│
├── doc/
│   └── linux_lite_phase1_plan.md
│
├── config/
│   ├── busybox.config
│   ├── kernel.config
│   └── board.dts
│
├── src/
│   ├── linux/
│   └── busybox/
│
├── third_party/
│   └── lzma-loader/            # 如需要，沿用已验证的 uImage 打包方式
│
├── rootfs-overlay/
│   ├── init
│   ├── dev/
│   ├── etc/
│   ├── proc/
│   ├── sys/
│   └── tmp/
│
├── scripts/
│   ├── prepare.sh
│   ├── build-busybox.sh
│   ├── build-rootfs.sh
│   ├── build-kernel.sh
│   └── make-uimage.sh
│
├── dl/
├── build/
└── out/
    ├── busybox
    ├── rootfs.cpio
    └── uImage
```

---

## 6. 最小 rootfs 设计

### 6.1 目录与文件

```text
/
├── init                 # 0755，启动脚本
├── bin/
│   ├── busybox          # 静态编译，0755
│   └── sh -> busybox
├── dev/
│   ├── console          # c 5 1
│   ├── null             # c 1 3
│   └── tty              # c 5 0
├── proc/
├── sys/
└── tmp/
```

可使用 `busybox --install -s /bin` 创建常用 applet 符号链接；第一阶段也可以只保留 `sh`、`mount` 等必要 applet。

### 6.2 `/init` 初始逻辑

```sh
#!/bin/busybox sh

/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t tmpfs tmpfs /tmp

echo "MT7621 Linux Lite started"
exec /bin/busybox sh
```

### 6.3 当前内核不能使用 devtmpfs

已检查当前已有内核配置：

```text
# CONFIG_DEVTMPFS is not set
```

因此第一阶段不要执行：

```sh
mount -t devtmpfs devtmpfs /dev
```

必须在 initramfs 中包含静态 `/dev/console`、`/dev/null` 等设备节点。  
后续若要自动创建设备节点，再考虑启用 `CONFIG_DEVTMPFS` / `CONFIG_DEVTMPFS_MOUNT`。

---

## 7. Kernel 第一阶段配置方向

第一阶段只保留启动到串口 shell 所需能力。

必须关注：

```text
MIPS 32-bit little-endian
MT7621 支持
CPU 类型 mips32r2
串口 8250 / ttyS0
initramfs 支持
initramfs 压缩支持
设备树支持
内嵌 DTB 支持
procfs / sysfs / tmpfs
```

关键方向：

```text
CONFIG_BLK_DEV_INITRD=y
CONFIG_INITRAMFS_SOURCE="/path/to/linux_lite/rootfs"
CONFIG_INITRAMFS_COMPRESSION_LZMA=y 或 GZIP
CONFIG_RD_LZMA=y 或 CONFIG_RD_GZIP=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_MIPS_RAW_APPENDED_DTB=y
CONFIG_CMDLINE_FROM_DTB=y
```

DTS 中必须保留：

```text
console=ttyS0,115200
```

初期可以基于以下 DTS 参考并裁剪：

```text
openwrt-24.10.8/target/linux/ramips/dts/mt7621_hilink_hlk-7621a-evb.dts
```

但复制/改写后的 DTS 必须位于 `linux_lite`，构建过程不依赖 OpenWrt 目录。

---

## 8. uImage 打包路线

### 8.1 U-Boot 约束

当前 U-Boot 已确认：

```text
CONFIG_LEGACY_IMAGE_FORMAT=y
CONFIG_CMD_BOOTM=y
CONFIG_FIT is not set
```

因此第一阶段必须输出 **legacy uImage**，不能使用 FIT。

### 8.2 初始打包策略

优先复用已验证的 `uimage-lzma-loader` 思路：

```text
Kernel
  ↓
append DTB
  ↓
LZMA 压缩
  ↓
lzma-loader
  ↓
mkimage 生成 legacy uImage
```

可以将必要的 loader 源码放入：

```text
linux_lite/third_party/lzma-loader/
```

打包脚本负责生成：

```text
linux_lite/out/uImage
```

如果该路线遇到问题，备用方案是：

```text
vmlinux.bin + appended DTB
  ↓
mkimage 生成 legacy uImage
  ↓
U-Boot bootm
```

备用方案需要验证加载地址、入口地址和 MIPS boot 约定。

### 8.3 第一验证方式：TFTP

不要一开始写 Flash。

U-Boot 中执行：

```text
tftpboot 0x82000000 uImage
bootm 0x82000000
```

验收条件：

```text
Starting kernel ...
Run /init as init process
MT7621 Linux Lite started
/ #
```

---

## 9. 构建步骤

### 步骤 1：准备源码

```text
Linux Kernel 源码
BusyBox 源码
MT7621 板级 DTS
```

源码放入 `linux_lite/src` 或 `linux_lite/dl`，不在 OpenWrt 目录内构建。

### 步骤 2：编译静态 BusyBox

目标：

```text
linux_lite/out/busybox
```

要求：

- `CONFIG_STATIC=y`
- MIPS32 little-endian
- mips32r2
- soft-float
- 不依赖 `/lib/ld-musl-mipsel-sf.so.1`

### 步骤 3：创建 rootfs

创建最小目录树，放入 BusyBox 和 `/init`，确保：

```text
init 权限 = 0755
busybox 权限 = 0755
/dev/console 等设备节点存在
```

### 步骤 4：配置内核并内嵌 initramfs

使 `CONFIG_INITRAMFS_SOURCE` 指向 `linux_lite` 自己的 rootfs 目录。

编译生成内核对象，确认 initramfs 已被包含。

### 步骤 5：生成 legacy uImage

输出：

```text
linux_lite/out/uImage
```

### 步骤 6：TFTP 启动验证

U-Boot：

```text
tftpboot 0x82000000 uImage
bootm 0x82000000
```

### 步骤 7：最小验收

串口 shell 中执行：

```sh
uname -a
cat /proc/cpuinfo
cat /proc/meminfo
mount
ls /dev
poweroff -f
```

---

## 10. 风险清单

| 风险 | 影响 | 应对 |
|---|---|---|
| 内核 DTS/加载方式不正确 | 内核无串口输出或无法启动 | 先复用已验证的 MT7621 DTS 和 lzma-loader 打包方式 |
| 内核配置缺少 initramfs | 找不到 `/init`，Kernel panic | 检查 `CONFIG_BLK_DEV_INITRD` 和 `CONFIG_INITRAMFS_SOURCE` |
| BusyBox 动态链接 | rootfs 缺少 musl loader | 第一阶段强制 `CONFIG_STATIC=y` |
| `CONFIG_DEVTMPFS` 未启用 | `/dev/console` 缺失 | 静态创建 `/dev` 节点 |
| init 权限错误 | `Failed to execute /init` | 打包前检查 init 为 0755 |
| uImage 超过 4 MiB | 默认 `boot_spi` 不能完整加载 | 先用 TFTP；需要 Flash 自动启动时改大 `sf read` 长度或压缩镜像 |
| U-Boot 未配置 TFTP 网络 | 无法 tftpboot | 先验证 U-Boot 网络，或改为写 Flash 测试 |
| 工具链参数不一致 | 用户空间 ABI 不匹配 | 固定 `-mips32r2 -mtune=24kc -msoft-float` |
| 混用 glibc/musl 动态程序 | 运行时找不到 loader/libc | 第一阶段全部静态，统一 musl 工具链 |

---

## 11. 第一阶段验收标准

必须同时满足：

```text
1. U-Boot 不修改，仍可使用现有 bootm 启动；
2. uImage 由 linux_lite 自己的构建流程生成；
3. 用户空间只有静态 BusyBox 和最小 initramfs；
4. 串口进入 BusyBox shell；
5. uname -a 显示 MIPS Linux；
6. /proc、/sys 可挂载；
7. 可以执行 poweroff 或 reboot 验证系统控制。
```

看到：

```text
MT7621 Linux Lite started
/ #
```

即第一阶段成功。

---

## 12. 下一会话从这里开始

下一会话建议先做：

1. 建立 `linux_lite` 的目录骨架和空的构建脚本；
2. 确定内核源码版本和获取方式；
3. 确定 BusyBox 源码路径和静态配置；
4. 先编译静态 BusyBox，确认 ELF 属性和依赖；
5. 再制作 rootfs 和内核镜像。

当前不要做：

- 不要修改 U-Boot；
- 不要先写 Flash；
- 不要先启用 WiFi/USB/网络用户空间；
- 不要同时切换成 glibc 工具链；
- 不要在 OpenWrt 目录中修改代码。

---

## 13. 关键决策记录

| 项目 | 决策 |
|---|---|
| 项目目录 | `linux_lite`，独立于 OpenWrt |
| U-Boot | 保持现有，不修改二进制 |
| 第一启动方式 | TFTP + `bootm` |
| 用户空间 | 仅静态 BusyBox |
| 根文件系统 | 内嵌 initramfs |
| 工具链 | `mipsel-openwrt-linux-musl-gcc`（方案 A） |
| libc | musl，静态链接 |
| 内核 | 自己的配置和 DTS |
| 镜像格式 | legacy uImage |
| 开发顺序 | 先 TFTP，后 Flash |

