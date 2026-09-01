---
title: HLK-7621 U-Boot(2026.07) WebFlash 与 OpenWrt RAM 启动记录
date: 2026-09-01 13:00:00
categories:
  - OpenWrt
tags:
  - MT7621
  - OpenWrt
  - U-Boot
  - lwIP
  - WebFlash
---

# 1 当前状态

- U-Boot 使用 lwIP 网络栈。
- Web 恢复命令为 `webflash`，监听 TCP 80 端口。
- 默认地址：`192.168.31.66/24`。
- 默认上传地址：`0x83000000`。
- 最大上传大小：`0x1fb0000`（33226752 字节，对应 firmware 分区大小）。
- 后台名称为 `YJRQZ Boot`，支持固件更新、回读校验、分区备份、白名单环境变量编辑和高级原始分区恢复。
- OpenWrt 固件上传完成后先在 RAM 中校验 uImage、CRC、fwtool 元数据及设备 ID；校验通过并二次确认后才修改 Flash。
- 固件写入按擦除、写入、回读 CRC 三阶段执行，完成后延时 5 秒自动重启。
- 普通固件更新不会写入 U-Boot、环境和 Factory；危险分区恢复要求等长备份、格式检查和目标专用确认口令。

启动服务：

```text
=> webflash
YJRQZ Boot listening on http://192.168.31.66/
SPI NOR: 33554432 bytes, erase size: 65536 bytes
Normal recovery protects U-Boot, environment and factory data.
Press Ctrl-C to stop.
```

只有执行 `webflash` 并停留在其轮询循环中，网页才可以访问；出现 `=>` 提示符时表示网页服务没有运行。

# 2 网络连接与访问

通过现有路由器访问时：

```text
主路由器 LAN 口
        │
        └── 开发板 LAN1/LAN2/LAN3/LAN4
```

不要接开发板 WAN 口。OpenWrt 默认禁止从 WAN 访问 LuCI。

电脑连接主路由器的普通 Wi-Fi，不能使用开启了客户端隔离的访客 Wi-Fi。电脑地址和开发板必须处于同一网段，例如：

```text
主路由器：192.168.31.1
电脑：    192.168.31.40
开发板：  192.168.31.66
掩码：    255.255.255.0
```

浏览器必须使用 HTTP：

```text
http://192.168.31.66/
```

Windows 测试命令：

```powershell
ping 192.168.31.66
Test-NetConnection 192.168.31.66 -Port 80
curl.exe --noproxy "*" -v --max-time 10 http://192.168.31.66/api/info
curl.exe --noproxy "*" -v --max-time 10 http://192.168.31.66/
```

PowerShell 中应明确使用 `curl.exe`，避免调用 `curl` 对应的 `Invoke-WebRequest` 别名。

# 3 TCP 能连接但 HTTP 返回 0 字节的问题

## 3.1 现象

- `ping` 正常，TTL 为 255。
- `Test-NetConnection ... -Port 80` 返回 `TcpTestSucceeded : True`。
- TCP 三次握手成功，客户端能发出 GET 请求。
- HTTP 一直收到 0 字节，最终超时。

## 3.2 根因

问题位于：

```text
net/lwip/net-lwip.c
```

原来的 `net_lwip_tx()` 只把第一个 pbuf 的 `p->payload`、`p->len` 交给网卡驱动。

```text
p->len      = 当前第一个 pbuf 的长度
p->tot_len  = 整条 pbuf 链的完整长度
```

TCP SYN/ACK 通常只有一个 pbuf，所以握手能够成功。HTTP 响应包含响应头和正文，可能形成多个 pbuf；只发送 `p->len` 会截断数据帧，客户端因此丢弃响应并等待到超时。

修复方法：如果存在 pbuf 链或首地址未对齐，先按 `p->tot_len` 把完整数据复制到一块连续、对齐的内存，再交给 U-Boot Ethernet 驱动发送。

该修复属于 lwIP 底层发送修复，不只对 WebFlash 有效。

## 3.3 HTTP 响应状态机

`cmd/lwip/webflash.c` 同时完成了以下处理：

- 注册 `tcp_sent()` 回调。
- 响应进入发送队列后，不立即关闭 TCP。
- 等客户端确认全部响应数据后再调用 `tcp_close()`。
- `tcp_output()` 或 `tcp_close()` 遇到 `ERR_MEM` 时，通过 `tcp_poll()` 重试。
- 串口打印连接、接收、请求、响应和确认信息。

正常日志示例：

```text
Web client connected: 192.168.31.40:57693
Web received: 85 bytes
Web request: GET /api/info HTTP/1.1
Web response: 200 OK, 342 bytes queued
Web acknowledged: 342 bytes, 0 remaining
```

浏览器请求 `/favicon.ico` 返回 `404 Not Found` 只是没有提供网页图标，不影响页面和上传功能。

# 4 从 RAM 临时启动 OpenWrt

用于 RAM 启动的镜像：

```text
openwrt-ramips-mt7621-hilink_hlk-7621a-evb-initramfs-kernel.bin
```

新版 YJRQZ Boot 网页只接受带目标设备 fwtool 元数据的 squashfs sysupgrade 镜像，因此会主动拒绝 initramfs。临时 RAM 启动继续使用 TFTP：

```text
=> tftpboot ${loadaddr} openwrt-ramips-mt7621-hilink_hlk-7621a-evb-initramfs-kernel.bin
=> iminfo ${fileaddr}
=> bootm ${fileaddr}
```

不要使用 `go`。`initramfs-kernel.bin` 包含内核和临时根文件系统，可以完全从 RAM 启动；复位后 RAM 系统消失，Flash 内容不变。

`squashfs-sysupgrade.bin` 用于写入 firmware 分区，不应当作完整的 RAM 系统直接运行。

# 5 U-Boot 与 OpenWrt 串口波特率

当前配置：

```text
U-Boot：  115200 8N1
OpenWrt： 57600 8N1
```

OpenWrt 的 `target/linux/ramips/dts/mt7621.dtsi` 默认包含：

```dts
chosen {
	bootargs = "console=ttyS0,57600";
};
```

板级 DTS 引用了公共 `mt7621.dtsi`，但没有覆盖该属性，所以 Linux 初始化串口后会从 115200 切换到 57600，串口工具仍停在 115200 时就会显示乱码。

临时方法：Linux 开始输出时，将串口工具切换到 57600 8N1、无流控。

永久统一为 115200 时，应在板级文件：

```text
openwrt-24.10.8/target/linux/ramips/dts/mt7621_hilink_hlk-7621a-evb.dts
```

的根节点中覆盖：

```dts
chosen {
	bootargs = "console=ttyS0,115200";
};
```

不要修改公共 `mt7621.dtsi`，否则会影响其他 MT7621 设备。

# 6 OpenWrt 临时使用 192.168.31.66

当前 OpenWrt 默认 LAN 接口为 `br-lan`，默认地址为 `192.168.1.1/24`。BusyBox 精简版 `ip` 不支持 `ip -br addr`，使用：

```sh
ip addr show
```

在 initramfs 中临时增加地址：

```sh
ip addr add 192.168.31.66/24 dev br-lan
ip link set br-lan up
```

开发板 LAN 接入已有主路由器时，必须停止开发板 DHCP，防止两个 DHCP 服务器同时工作：

```sh
/etc/init.d/dnsmasq stop
```

需要 OpenWrt 本身访问外网时：

```sh
ip route replace default via 192.168.31.1 dev br-lan
```

LuCI 未响应时重启 uhttpd：

```sh
/etc/init.d/uhttpd restart
```

访问地址：

```text
http://192.168.31.66/cgi-bin/luci
```

`ip addr add`、停止 dnsmasq 和 initramfs 内的 UCI 修改在重启后都会消失。

# 7 正式系统持久设置 LAN 地址

正式将 OpenWrt 写入 Flash 后，使用 UCI 持久保存：

```sh
uci set network.lan.proto='static'
uci set network.lan.ipaddr='192.168.31.66'
uci set network.lan.netmask='255.255.255.0'
uci set network.lan.gateway='192.168.31.1'
uci set network.lan.dns='192.168.31.1'
uci commit network

uci set dhcp.lan.ignore='1'
uci commit dhcp

/etc/init.d/network restart
/etc/init.d/dnsmasq restart
/etc/init.d/uhttpd restart
```

`192.168.31.66` 必须确认没有被主路由器 DHCP 分配给其他设备。
