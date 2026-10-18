# MT7621 极简 Linux 启动链与用户空间接口知识总结

## 1. 整体结构：从 U-Boot 到用户程序

在 MT7621 这类嵌入式 SoC 上，一个典型 Linux 系统可以简化为：

```text
U-Boot
  ↓
Linux Kernel
  ↓
initramfs / rootfs
  ↓
/init 或 /sbin/init
  ↓
BusyBox / systemd / 自定义应用
```

各层职责：

| 层级 | 主要作用 |
|---|---|
| U-Boot | 初始化最基础硬件，加载 Linux 内核、设备树、rootfs/initramfs，并把控制权交给内核 |
| Linux Kernel | 管理 CPU、内存、中断、定时器、网络、文件系统、驱动等，并向用户空间提供系统调用 |
| initramfs / rootfs | 提供 Linux 用户空间所需的目录、库、程序和配置 |
| `/init` / `/sbin/init` | Linux 启动后的第一个用户空间进程，PID 1 |
| BusyBox | 提供 `sh`、`ls`、`mount`、`cat`、`ip` 等常用基础命令 |
| 应用程序 | 通过 libc/POSIX/Linux API 使用内核提供的功能 |

---

# 2. Linux Kernel 是什么

Linux Kernel 是整个系统的核心。

它不是 shell，也不提供 `ls`、`cp`、`wget` 这样的普通命令。

内核主要负责：

```text
Linux Kernel
├── CPU / 进程调度
├── 内存管理
├── 中断管理
├── 定时器
├── 文件系统
├── 网络协议栈
├── 系统调用
├── 设备驱动
├── 进程间通信
└── 权限与安全机制
```

以 MT7621 为例，Kernel 启动后可能初始化：

```text
CPU
RAM
MMU
UART
中断控制器
Timer
GPIO
SPI
NAND / NOR
Ethernet
MDIO / PHY
USB
PCIe
```

内核的核心价值可以理解为：

> 把底层复杂的硬件资源抽象成统一接口，让上层程序不需要直接操作寄存器。

例如上层程序访问串口：

```c
int fd = open("/dev/ttyS0", O_RDWR);
write(fd, data, len);
```

实际调用链是：

```text
应用程序
  ↓
open / write
  ↓
Linux syscall
  ↓
串口驱动
  ↓
MT7621 UART 寄存器
```

应用程序不需要知道 UART 寄存器地址。

---

# 3. initramfs 到底是什么

`initramfs` 可以理解为：

> Linux 启动阶段使用的一套“初始根文件系统”。

它本质上是一组目录和文件。

例如：

```text
rootfs/
├── init
├── bin/
│   └── busybox
├── dev/
├── proc/
├── sys/
└── etc/
```

这只是普通目录。

制作 initramfs 时，通常把它打包成：

```text
rootfs.cpio.gz
```

典型命令：

```bash
cd rootfs
find . | cpio -o -H newc | gzip > rootfs.cpio.gz
```

这条命令的含义：

```text
目录和文件
   ↓ find
得到文件列表
   ↓ cpio
打包成 cpio archive
   ↓ gzip
压缩
   ↓
rootfs.cpio.gz
```

注意：

```text
cpio = 打包
gzip = 压缩
```

和：

```text
tar + gzip → xxx.tar.gz
```

类似。

initramfs 常用：

```text
cpio + gzip
```

---

# 4. initramfs 如何被 Linux 使用

如果 Kernel 启动时得到了一个 initramfs，它会：

```text
rootfs.cpio.gz
      ↓
Kernel 解压
      ↓
展开到 RAM
      ↓
形成初始根目录 /
      ↓
寻找 /init
      ↓
执行 /init
```

最终 RAM 中可能是：

```text
/
├── init
├── bin/
│   └── busybox
├── dev/
├── proc/
└── sys/
```

所以：

> initramfs 启动后会成为 Linux 最初的 `/` 根目录。

它和普通 ext4 rootfs 的区别：

```text
initramfs
  ↓
启动时展开到 RAM
  ↓
断电消失

ext4 rootfs
  ↓
存储在 eMMC / SD / SSD / Flash
  ↓
断电仍然存在
```

---

# 5. Kernel 如何得到 initramfs

常见有两种方式。

## 5.1 initramfs 编译进 Kernel

Linux Kernel 配置支持：

```text
CONFIG_INITRAMFS_SOURCE
```

例如：

```text
CONFIG_INITRAMFS_SOURCE="/home/user/rootfs"
```

编译时：

```text
rootfs/
   ↓
Kernel Build System
   ↓
直接嵌入 Kernel 镜像
```

启动结构：

```text
U-Boot
  ↓
Kernel
  ├── Kernel 本体
  └── 内嵌 initramfs
```

这种方式适合极简系统和 bring-up。

---

## 5.2 U-Boot 单独加载 initramfs

例如内存布局：

```text
Kernel          0x81000000
rootfs.cpio.gz  0x82000000
DTB             0x83000000
```

然后由 U-Boot 启动。

对于传统 U-Boot `bootm`，典型概念是：

```text
bootm kernel_addr ramdisk_addr dtb_addr
```

注意：

> `bootm` 是 U-Boot 命令，不是 Linux Kernel 命令。

启动关系：

```text
U-Boot
  ↓ bootm
Linux Kernel
  ↓
initramfs
  ↓
/init
```

---

# 6. `/init` 是什么

`/init` 是一个文件，不是文件夹。

例如：

```text
rootfs/
├── init        ← 文件
├── bin/
│   └── busybox
├── dev/
├── proc/
└── sys/
```

Kernel 完成自身初始化以后，会尝试执行：

```text
/init
```

这个进程通常会成为：

```text
PID 1
```

也就是 Linux 用户空间第一个进程。

---

# 7. `/init` 为什么需要可执行权限

Linux 文件除了“文件内容”，还带有元数据：

```text
文件名
内容
权限 mode
uid
gid
时间戳
```

例如：

```text
-rw-r--r-- init
```

等价于：

```text
0644
```

不能直接执行。

而：

```text
-rwxr-xr-x init
```

等价于：

```text
0755
```

可以执行。

所以在制作 rootfs 时，应：

```bash
chmod +x rootfs/init
```

更准确地说：

> 不是“Kernel 启动后执行 chmod”，而是在打包 initramfs 前就把权限设置正确。

因为 `cpio` 会保存权限信息。

完整过程：

```text
PC 上创建 init
  ↓
chmod +x init
  ↓
init 权限 = 0755
  ↓
cpio 打包
  ↓
权限一起保存
  ↓
Kernel 解压
  ↓
/init 仍然是 0755
  ↓
Kernel 可以执行
```

如果 `/init` 不可执行，可能导致：

```text
Failed to execute /init
Kernel panic
No working init found
```

---

# 8. 一个最小 `/init` 脚本

示例：

```sh
#!/bin/busybox sh

/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev

echo "MT7621 Linux started"

/bin/busybox sh
```

逐行解释。

---

## 8.1 Shebang

```sh
#!/bin/busybox sh
```

含义：

> 使用 `/bin/busybox` 的 `sh` 功能解释这个脚本。

流程：

```text
Kernel 执行 /init
      ↓
读取第一行 #!/bin/busybox sh
      ↓
启动 /bin/busybox
      ↓
BusyBox 以 sh 模式解析 /init
```

---

## 8.2 挂载 `/proc`

```sh
/bin/busybox mount -t proc proc /proc
```

挂载 Linux proc 虚拟文件系统。

之后可以访问：

```text
/proc/cpuinfo
/proc/meminfo
/proc/interrupts
/proc/mounts
```

例如：

```bash
cat /proc/cpuinfo
```

---

## 8.3 挂载 `/sys`

```sh
/bin/busybox mount -t sysfs sysfs /sys
```

`sysfs` 用于暴露设备、驱动、总线、class 等内核对象。

常见目录：

```text
/sys/class
/sys/devices
/sys/bus
```

---

## 8.4 挂载 `/dev`

```sh
/bin/busybox mount -t devtmpfs devtmpfs /dev
```

设备节点通常会出现在：

```text
/dev/console
/dev/ttyS0
/dev/null
/dev/mtd0
```

它们是用户空间访问驱动的重要入口。

---

## 8.5 启动 shell

```sh
/bin/busybox sh
```

之后串口可能进入：

```text
MT7621 Linux started

/ #
```

出现：

```text
/ #
```

基本说明：

```text
U-Boot
→ Kernel
→ initramfs
→ /init
→ BusyBox shell
```

整条启动链已经成功。

---

# 9. BusyBox 是什么

BusyBox 是一个开源项目。

它把大量常见 Unix/Linux 命令实现到一个可执行文件里。

例如：

```text
/bin/busybox
```

内部可以包含：

```text
sh
ls
cat
cp
mv
rm
mkdir
mount
dmesg
ping
ip
ifconfig
reboot
```

可以直接执行：

```bash
/bin/busybox ls
/bin/busybox cat /proc/cpuinfo
/bin/busybox dmesg
```

也可以创建符号链接：

```text
/bin/ls    -> busybox
/bin/cat   -> busybox
/bin/sh    -> busybox
```

这样就可以直接：

```bash
ls
cat
sh
```

BusyBox 可以自动创建链接，例如：

```bash
busybox --install -s /bin
```

---

# 10. BusyBox 为什么推荐静态编译

如果 BusyBox 是动态链接的：

```text
busybox
  ↓
依赖 libc
  ↓
还需要动态链接器
```

那么 rootfs 里还必须包含：

```text
/lib/ld-*.so
/lib/libc.so
...
```

否则可能出现：

```text
No such file or directory
```

即使 `busybox` 文件明明存在，也可能因为动态链接器缺失而无法运行。

极简系统推荐：

```text
CONFIG_STATIC=y
```

静态编译后：

```text
/bin/busybox
```

基本包含自身所需运行库，不依赖外部动态 libc。

特别适合最初 bring-up。

---

# 11. BusyBox 必须针对 MT7621 编译

MT7621 是 MIPS 小端平台。

不能直接把 PC 上的：

```text
x86_64 BusyBox
```

复制到 MT7621 运行。

否则可能：

```text
Exec format error
```

必须使用针对目标平台的交叉工具链，例如：

```text
mipsel-linux-gnu-gcc
```

生成适合：

```text
MIPS
32-bit
Little Endian
```

的程序。

---

# 12. 交叉工具链的作用

对于 x86 PC 开发 MT7621：

```text
开发机：x86_64
目标机：MIPS MT7621
```

PC 上运行的 GCC 默认生成 x86_64 程序，不能给 MT7621 使用。

因此需要：

```text
Cross Compiler
```

例如：

```text
mipsel-linux-gnu-gcc
```

用途：

```text
在 x86 PC 上运行 GCC
       ↓
生成 MIPS Little Endian 程序
       ↓
放到 MT7621 执行
```

---

# 13. Kernel 与用户空间是否必须使用同一个工具链

严格来说：

> 不要求必须是完全相同的一套 GCC，但目标架构和 ABI 必须兼容。

例如：

```text
Kernel
  ↓
MIPS32 Little Endian

BusyBox
  ↓
MIPS32 Little Endian

自定义程序
  ↓
MIPS32 Little Endian
```

实际开发中，为了减少 ABI、libc 和架构问题，通常推荐使用同一套交叉工具链。

例如：

```text
Linux Kernel
    ↓
mipsel-linux-gnu-gcc

BusyBox
    ↓
mipsel-linux-gnu-gcc

自己的 app
    ↓
mipsel-linux-gnu-gcc
```

---

# 14. Kernel 本身不依赖 libc

这是一个重要概念。

用户程序：

```text
App
 ↓
glibc / musl / uClibc
 ↓
syscall
 ↓
Kernel
```

但 Kernel：

```text
Linux Kernel
```

不会再链接 glibc。

内核自身就是底层运行环境。

所以：

```text
Kernel 工具链
```

主要负责：

```text
生成目标 CPU 可执行的机器码
```

而用户空间工具链还通常包含：

```text
编译器
头文件
libc
启动代码
动态链接器
系统库
```

---

# 15. 用户程序如何“知道 Linux 有哪些接口”

Linux 上层应用通常通过：

```text
libc
POSIX API
Linux API
系统头文件
man 手册
```

来知道可调用接口。

常见头文件：

```c
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
```

常见接口：

```c
open()
read()
write()
close()

socket()
connect()
send()
recv()

fork()
execve()

mmap()
ioctl()
epoll()
```

查询文档：

```bash
man 2 open
man 2 socket
man 2 connect
man 3 getaddrinfo
```

其中：

```text
man 2 → system call
man 3 → library function
```

---

# 16. libc 函数和 syscall 的关系

不是所有标准库函数都会进入 Kernel。

例如：

```c
strlen()
memcpy()
strcmp()
```

通常完全在用户态完成。

而：

```c
open()
read()
write()
socket()
connect()
```

最终会进入 Kernel。

例如：

```text
printf()
  ↓
libc 处理格式化
  ↓
write()
  ↓
syscall
  ↓
Kernel
```

---

# 17. 工具链里有 API，不代表 Kernel 一定支持

这是嵌入式 Linux 中非常重要的一点。

比如代码可以成功编译：

```c
socket(AF_INET, SOCK_STREAM, 0);
```

但如果 Kernel 没开启网络支持：

```text
CONFIG_NET=n
```

或者：

```text
CONFIG_INET=n
```

则运行时不能正常使用 IPv4/TCP。

所以：

```text
工具链 / libc
    ↓
告诉应用“这个 API 怎么调用”

Linux Kernel
    ↓
决定“这个功能实际上有没有”
```

---

# 18. Kernel 配置与用户程序需求的关系

例如实现 HTTP 下载器，需要：

```c
socket()
connect()
send()
recv()
```

那么 Kernel 至少要具备：

```text
网络基础支持
IPv4
TCP
Ethernet 驱动
PHY 驱动
```

逻辑关系：

```text
wget
 ↓
socket API
 ↓
TCP
 ↓
IPv4
 ↓
Ethernet
 ↓
MT7621 Ethernet Driver
 ↓
MAC
 ↓
PHY
 ↓
网线
```

如果某层缺失，上层就无法正常工作。

---

# 19. 自己实现一个最简 wget 的思路

实现一个最小 HTTP 下载器，可以按：

```text
解析 URL
 ↓
DNS
 ↓
建立 TCP
 ↓
发送 HTTP 请求
 ↓
接收 HTTP Response
 ↓
解析 Header
 ↓
写入文件
```

例如：

```text
http://example.com/test.bin
```

需要解析成：

```text
协议：http
Host：example.com
Port：80
Path：/test.bin
```

---

## 19.1 DNS

使用：

```c
getaddrinfo()
```

例如：

```c
struct addrinfo *res;

getaddrinfo("example.com", "80", NULL, &res);
```

注意：

> Linux Kernel 并没有简单的 `dns_lookup()` syscall。

DNS 通常由 libc/resolver 配合 socket 完成。

---

## 19.2 创建 TCP Socket

```c
int fd = socket(AF_INET, SOCK_STREAM, 0);
```

含义：

```text
AF_INET      → IPv4
SOCK_STREAM  → TCP
```

Kernel 返回：

```text
file descriptor
```

例如：

```text
fd = 3
```

---

## 19.3 TCP 连接

```c
connect(fd, ...);
```

内部过程：

```text
应用程序
  ↓ connect
Linux TCP
  ↓
IP
  ↓
Ethernet
  ↓
MT7621 MAC
  ↓
PHY
  ↓
服务器
```

应用程序不需要自己处理：

```text
ARP
TCP SYN
TCP ACK
TCP 重传
IP 分片
Ethernet Frame
```

这些由 Linux Kernel 网络栈完成。

---

## 19.4 发送 HTTP 请求

例如：

```text
GET /test.bin HTTP/1.1
Host: example.com
Connection: close

```

代码：

```c
const char *req =
    "GET /test.bin HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "Connection: close\r\n"
    "\r\n";

send(fd, req, strlen(req), 0);
```

---

## 19.5 接收 HTTP 数据

```c
char buf[4096];

while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
    ...
}
```

服务器返回可能类似：

```text
HTTP/1.1 200 OK
Content-Length: 12345
Content-Type: application/octet-stream

<文件内容>
```

HTTP Header 与 Body 之间通常通过：

```text
\r\n\r\n
```

分隔。

---

## 19.6 保存文件

```c
int out = open(
    "test.bin",
    O_CREAT | O_WRONLY | O_TRUNC,
    0644
);
```

再：

```c
write(out, data, len);
```

调用链：

```text
recv
 ↓
应用程序
 ↓
write
 ↓
Kernel VFS
 ↓
具体文件系统
 ↓
Flash / SD / RAM
```

---

# 20. 最简 wget 所需核心 API

可以总结为：

```text
getaddrinfo()
socket()
connect()
send()
recv()

open()
write()
close()
```

也就是说：

> 一个最简单的 HTTP 下载器主要依赖 Linux socket API 和文件 API。

---

# 21. 为什么初学时先做 HTTP，不做 HTTPS

HTTP：

```text
HTTP
 ↓
TCP
 ↓
IP
```

HTTPS：

```text
HTTP
 ↓
TLS
 ↓
TCP
 ↓
IP
```

TLS 包含：

```text
证书
密钥交换
对称加密
摘要
身份认证
加密套件
握手状态机
```

自己从零实现工作量很大。

实际项目通常使用：

```text
OpenSSL
mbedTLS
wolfSSL
```

因此极简学习项目建议先实现：

```text
wget http://192.168.1.100/test.bin
```

先不做：

```text
HTTPS
HTTP/2
重定向
代理
Cookie
认证
chunked encoding
```

---

# 22. initramfs 不是必须的

Linux 可以有不同启动方式。

## 方式 1：直接使用 initramfs

```text
U-Boot
 ↓
Kernel
 ↓
initramfs
 ↓
/init
 ↓
BusyBox
```

适合：

```text
极简系统
bring-up
救援环境
测试环境
嵌入式设备
```

---

## 方式 2：直接挂载真实 rootfs

例如：

```text
U-Boot
 ↓
Kernel
 ↓
ext4 rootfs
 ↓
/sbin/init
```

Kernel bootargs 可能包含：

```text
root=/dev/mmcblk0p2 rootfstype=ext4
```

---

## 方式 3：initramfs 作为过渡

完整 Linux 常见：

```text
Kernel
 ↓
initramfs
 ↓
/init
 ↓
加载驱动
找到真正 rootfs
 ↓
switch_root
 ↓
真实 rootfs
 ↓
/sbin/init
```

这时 initramfs 只是启动中间环境。

---

# 23. 一个适合 MT7621 学习的最小 rootfs

最简单可以是：

```text
rootfs/
├── init
├── bin/
│   └── busybox
├── dev/
├── proc/
└── sys/
```

制作步骤：

```bash
mkdir -p rootfs/{bin,dev,proc,sys}

cp busybox rootfs/bin/

chmod +x rootfs/bin/busybox
chmod +x rootfs/init
```

打包：

```bash
cd rootfs
find . | cpio -o -H newc | gzip > ../rootfs.cpio.gz
```

最终：

```text
rootfs.cpio.gz
```

可以作为 initramfs 使用。

---

# 24. 推荐的 MT7621 学习路线

建议按以下顺序：

```text
第一阶段
U-Boot
 ↓
Kernel
 ↓
串口看到 Kernel log
```

然后：

```text
第二阶段
Kernel
 ↓
initramfs
 ↓
/init
 ↓
BusyBox shell
 ↓
/ #
```

然后验证：

```text
/proc
/sys
/dev
```

再进一步：

```text
GPIO
UART
Ethernet
USB
PCIe
SPI
Flash
```

最后：

```text
BusyBox initramfs
 ↓
完整 rootfs
 ↓
自己的用户程序
 ↓
网络应用
 ↓
Debian/OpenWrt 用户空间
```

---

# 25. Buildroot 的作用

如果不想全部手工制作，可以使用 Buildroot。

Buildroot 可以自动完成：

```text
Cross Toolchain
Linux Kernel
BusyBox
RootFS
Initramfs
Bootloader
第三方软件包
```

最终可能生成：

```text
output/images/
├── rootfs.cpio
├── rootfs.cpio.gz
├── rootfs.tar
├── kernel image
└── dtb
```

所以 Buildroot 本质上是：

> 嵌入式 Linux 系统构建工具。

它不是 Linux Kernel，也不是 rootfs，而是负责把这些东西自动构建出来。

---

# 26. Debian 与极简 Linux 的关系

Debian 主要是一个完整的 Linux 用户空间发行版。

概念上：

```text
Linux Kernel
  +
Debian RootFS
  =
Debian 系统
```

而：

```text
Linux Kernel
  +
BusyBox RootFS
  =
极简嵌入式 Linux
```

它们底层可以使用同一个 Linux Kernel。

区别主要在用户空间。

Debian RootFS 通常包含：

```text
glibc
systemd
apt
bash
coreutils
dpkg
网络工具
大量库
```

BusyBox RootFS 可以只有：

```text
busybox
/init
少量配置
```

---

# 27. Linux 的核心分层模型

可以最终记成：

```text
应用程序
  ↓
libc / POSIX / Linux API
  ↓
syscall
  ↓
Linux Kernel
  ↓
驱动
  ↓
硬件
```

例如下载文件：

```text
wget
 ↓
getaddrinfo
socket
connect
send
recv
 ↓
Linux TCP/IP
 ↓
Ethernet Driver
 ↓
MT7621 MAC
 ↓
PHY
```

例如访问串口：

```text
App
 ↓
open / write / read
 ↓
Kernel
 ↓
UART Driver
 ↓
UART Hardware
```

例如操作普通文件：

```text
App
 ↓
open / read / write
 ↓
VFS
 ↓
ext4 / UBIFS / SquashFS
 ↓
存储设备
```

---

# 28. 最关键的几个概念

## Kernel

负责：

```text
管理硬件
管理资源
提供 syscall
提供驱动
提供网络
提供文件系统
```

---

## initramfs

本质：

```text
一套目录和文件
 ↓
cpio 打包
 ↓
可选 gzip 压缩
 ↓
Kernel 启动时解到 RAM
```

---

## `/init`

本质：

```text
一个可执行文件或脚本
```

作用：

```text
Kernel 启动后的第一个用户空间程序
PID 1
```

---

## BusyBox

本质：

```text
一个程序
```

内部实现：

```text
sh
ls
cat
mount
cp
rm
ping
...
```

---

## 交叉工具链

作用：

```text
在 PC 上
 ↓
编译
 ↓
生成 MT7621 可执行代码
```

---

## libc

作用：

```text
给应用程序提供 C 标准库 / POSIX / syscall wrapper
```

例如：

```text
printf
malloc
open
read
write
socket
connect
```

---

## Kernel 配置

决定：

```text
TCP 是否存在
IPv4 是否存在
USB 是否存在
某个文件系统是否存在
某个设备驱动是否存在
```

所以：

> 用户程序能编译，不代表内核运行时一定支持。

---

# 29. 一张总图

```text
                     开发机 PC
                        │
              mipsel 交叉工具链
                        │
        ┌───────────────┼───────────────┐
        │               │               │
   Linux Kernel       BusyBox       自定义 App
        │               │               │
        └───────────────┼───────────────┘
                        │
                   rootfs/
                        │
               cpio + gzip
                        │
               rootfs.cpio.gz
                        │
                        ▼
                    MT7621
                        │
                     U-Boot
                        │
                     bootm
                        │
                        ▼
                 Linux Kernel
                        │
              初始化 CPU / RAM
              初始化中断 / Timer
              初始化设备驱动
              初始化网络 / VFS
                        │
                        ▼
               解压 initramfs
                        │
                        ▼
                       /
                  ┌─────┴─────┐
                /init       /bin
                              │
                           busybox
                        │
                        ▼
                     PID 1
                        │
              mount /proc /sys /dev
                        │
                        ▼
                  BusyBox shell
                        │
                        ▼
                      / #
```

---

# 30. 最终一句话理解

整个嵌入式 Linux 可以概括成：

> U-Boot 把 Kernel 启动起来；Kernel 初始化和管理硬件，并提供系统调用；initramfs/rootfs 提供用户空间文件；`/init` 启动第一个用户进程；BusyBox 或其他程序通过 libc 和 syscall 使用 Kernel 提供的能力。

对于 MT7621 学习，最推荐先跑通：

```text
U-Boot
→ Linux Kernel
→ initramfs
→ /init
→ 静态 BusyBox
→ / #
```

只要能进入 `/ #`，就已经完成了一个真正可运行的最小 Linux 系统。
