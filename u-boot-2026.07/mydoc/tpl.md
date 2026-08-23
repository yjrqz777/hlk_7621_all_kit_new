---
title: HLK-7621-uboot(2026.07)笔记-03
date: 2026-08-21 20:30:00
categories:
  - OpenWrt
tags:
  - MT7621
  - OpenWrt
  - U-Boot
  - tpl
---

# arch/mips/mach-mtmips/mt7621/tpl/tpl.c

# `legacy_img_hdr` 总结

1. **`legacy_img_hdr` 是什么**

   `legacy_img_hdr` 是 U-Boot legacy image 的镜像头，放在 SPL 数据前面，用来描述后面的 SPL 镜像。

2. **它在这里的位置**

   ```text
   [TPL]
   [legacy_img_hdr]
   [SPL payload]
   ```

   TPL 通过：

   ```c
   __image_copy_end
   ```

   找到这个 header。

3. **主要字段**

   ```text
   ih_magic  -> 镜像标识
   ih_size   -> SPL 大小
   ih_load   -> SPL 加载地址
   ih_ep     -> SPL 入口地址
   ih_hcrc   -> header CRC
   ih_dcrc   -> SPL 数据 CRC
   ```

4. **谁写进去的**

   在编译/打包阶段，由电脑端的 `mkimage` 写入。

   主要通过：

   ```c
   image_set_magic()
   image_set_size()
   image_set_load()
   image_set_ep()
   ```

   把对应字段写进 `legacy_img_hdr`。

5. **TPL 怎么读取**

   TPL 运行时通过：

   ```c
   image_get_magic()
   image_get_size()
   image_get_load()
   image_get_ep()
   ```

   读取 header 中的字段。

6. **为什么先检查 magic**

   ```c
   if (image_get_magic(hdr) != IH_MAGIC)
       goto failed;
   ```

   用来确认 `__image_copy_end` 后面确实是合法的 SPL legacy image header。

   如果 magic 不正确，说明镜像位置、内容或格式有问题，TPL 就不会继续使用错误的 `size/load/ep`。

7. **字节序**

   `legacy_img_hdr` 中的 32 位字段按大端格式保存。

   写入镜像时：

   ```text
   cpu_to_uimage()
   -> cpu_to_be32()
   ```

   TPL 读取时：

   ```text
   uimage_to_cpu()
   -> be32_to_cpu()
   ```

8. **TPL 最终怎么利用它**

   ```text
   读取 legacy_img_hdr
          |
          v
   检查 ih_magic
          |
          v
   获取 ih_size / ih_load / ih_ep
          |
          v
   把 SPL 放入 L2 Cache
          |
          v
   跳到 ih_ep
          |
          v
   开始运行 SPL
   ```

## 一句话总结

`legacy_img_hdr` 就是 SPL 前面的启动说明头，告诉 TPL：**后面的 SPL 是否有效、大小是多少、加载到哪里、从哪里开始执行。**

## 跳转到SPL
