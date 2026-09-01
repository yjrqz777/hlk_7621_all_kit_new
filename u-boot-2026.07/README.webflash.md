# HLK-7621A Web 刷机设计说明

本文记录 HLK-7621A Web 恢复/刷机功能使用的 Flash 布局、镜像格式和安全限制。

分区信息来自 OpenWrt 24.10.8：

```text
/home/yjrqz/hlk_7621_all_kit/hlk_7621_all_kit_new/openwrt-24.10.8/
target/linux/ramips/dts/mt7621_hilink_hlk-7621a-evb.dts
```

## Flash 分区布局

SPI NOR 总容量为 32 MiB，地址范围为 `0x000000`～`0x1ffffff`。

| 分区 | 起始地址 | 大小 | 结束地址（不包含） | YJRQZ Boot 策略 |
|---|---:|---:|---:|---|
| U-Boot | `0x000000` | `0x030000`（192 KiB） | `0x030000` | 默认保护；仅允许等长备份恢复和专用口令确认 |
| U-Boot 环境 | `0x030000` | `0x010000`（64 KiB） | `0x040000` | 白名单编辑；原始恢复需 CRC、等长文件和专用口令 |
| Factory | `0x040000` | `0x010000`（64 KiB） | `0x050000` | 默认保护；仅允许等长、非空备份恢复和专用口令确认 |
| Firmware | `0x050000` | `0x1fb0000`（32448 KiB） | `0x2000000` | 校验设备型号后允许更新 |

Factory 分区内的无线 EEPROM 位于 Factory 分区偏移 `0x8000`，即绝对地址：

```text
0x048000～0x0481ff
```

该区域包含无线校准数据。普通固件更新、清除 OpenWrt 配置和环境变量操作都不会擦除或覆盖它；高级恢复仅用于写回本机已验证的完整 Factory 备份。

## U-Boot 大小限制

U-Boot 分区大小固定为：

```text
0x30000 = 196608 bytes = 192 KiB
```

最终生成的 `build/u-boot-mt7621.bin` 必须小于或等于 196608 字节。超过该大小会覆盖从 `0x30000` 开始的 U-Boot 环境分区，因此应当视为构建失败。

可使用以下检查逻辑：

```sh
test "$(stat -c %s build/u-boot-mt7621.bin)" -le 196608
```

## 安全模型

正常更新只允许写 Firmware 分区：

```c
#define WEBFLASH_FIRMWARE_OFFSET  0x00050000
#define WEBFLASH_FIRMWARE_SIZE    0x01fb0000
#define WEBFLASH_FLASH_END        0x02000000
```

写入操作必须同时满足：

```c
offset >= WEBFLASH_FIRMWARE_OFFSET;
size > 0;
size <= WEBFLASH_FIRMWARE_SIZE;
offset + size >= offset; /* 防止整数溢出 */
offset + size <= WEBFLASH_FLASH_END;
```

U-Boot、环境和 Factory 的原始写入只放在“高级功能”中，并同时要求：目标分区等长文件、目标特有的基础格式检查、页面二次确认以及 HTTP 确认口令。建议在任何高级写入前先下载 Factory 和 Full Flash 备份。

## 可接受的 OpenWrt 镜像

网页应接受该设备的 squashfs sysupgrade 镜像：

```text
openwrt-ramips-mt7621-hilink_hlk-7621a-evb-squashfs-sysupgrade.bin
```

该镜像应从 Flash 偏移 `0x50000` 开始写入。

当前实际检查过的示例镜像：

```text
OpenWrt 版本：24.10.8
目标平台：ramips/mt7621
设备：hilink,hlk-7621a-evb
文件大小：8782925 bytes
```

以下 initramfs 镜像仅用于加载到 RAM 后临时启动，不能作为 Web 刷机镜像写入 Firmware 分区：

```text
openwrt-ramips-mt7621-hilink_hlk-7621a-evb-initramfs-kernel.bin
```

## 上传和刷写流程

推荐流程：

```text
浏览器选择 sysupgrade 镜像
  -> 上传完整镜像到 DDR
  -> 检查内存写入边界
  -> 校验镜像长度和格式
  -> 校验设备型号
  -> 擦除 Firmware 分区
  -> 将镜像写入 0x50000
  -> 从 Flash 回读并校验
  -> 校验成功后重启
```

禁止一边接收网络数据一边写 Flash。必须先将镜像完整接收到 DDR 并通过校验，以避免上传中断时损坏现有固件。

## 镜像校验要求

执行任何 Flash 擦除前，至少完成以下检查：

1. 上传长度大于零且不超过 `0x1fb0000`。
2. 文件开头为 legacy uImage magic `0x27051956`。
3. uImage Header CRC 正确。
4. uImage Kernel Data CRC 正确。
5. OpenWrt 镜像元数据中的目标设备为 `hilink,hlk-7621a-evb`。
6. 全部写入范围位于 `0x50000`～`0x1ffffff`。
7. Flash 写入完成后进行回读校验。

只检查文件名或文件大小是不够的，文件名可由用户任意修改。

## 擦除策略

为了避免旧 rootfs/overlay 数据残留，推荐擦除完整 Firmware 分区：

```text
起始地址：0x00050000
擦除长度：0x01fb0000
```

随后只写入实际上传的 sysupgrade 镜像长度。完整擦除 Firmware 分区会清除旧系统配置，升级后的 OpenWrt 将使用新固件的默认配置。

擦除和写入期间必须保证 U-Boot、环境及 Factory 分区保持不变，并在耗时操作中喂硬件看门狗。

## HTTP 功能范围

服务基于 lwIP Raw TCP API，使用单连接、`Content-Length` 和 `Connection: close`。页面资源内嵌，不依赖互联网。当前接口为：

```text
GET  /
GET  /api/info
GET  /api/job
GET  /api/env
GET  /api/backup/{uboot,environment,factory,firmware,fullflash}
POST /api/upload/{firmware,uboot,environment,factory}
POST /api/flash
POST /api/factory-reset
POST /api/env
POST /api/env/reset
POST /api/reboot
```

上传正文使用 `application/octet-stream`，不实现 multipart、HTTPS、CGI、SSI、目录浏览、HTTP keep-alive 或 chunked encoding。刷写任务按“擦除、写入、回读 CRC”三阶段执行，成功后延时 5 秒重启。

Web 服务只应在用户主动执行恢复命令或按住物理恢复按键时启动，不应在正常启动流程中长期开放。
