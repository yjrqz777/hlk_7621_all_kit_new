/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) Sean Anderson <seanga2@gmail.com>
 */
#ifndef	_SPL_LOAD_H_
#define	_SPL_LOAD_H_

#include <image.h>
#include <imx_container.h>
#include <mapmem.h>
#include <spl.h>

static inline int _spl_load(struct spl_image_info *spl_image,
			    const struct spl_boot_device *bootdev,
			    struct spl_load_info *info, size_t size,
			    size_t offset) /* yjrqz-spl8 */
{
	struct legacy_img_hdr *header =
		spl_get_load_buffer(-sizeof(*header), sizeof(*header));
	ulong base_offset, image_offset, overhead;
	int read, ret;

	log_debug("\nloading hdr from %lx to %p\n", (ulong)offset, header);
	// printf("\nloading hdr from %lx to %p\n", (ulong)offset, header);
	/*
	read = spl_nor_load_read(
		info,
		offset,          // NOR Flash 读取地址
		header大小,      // 要读取多少字节
		header           // 数据读到这里
	);
	*/
	read = info->read(info, 
					offset, 
					ALIGN(sizeof(*header),spl_get_bl_len(info)), 
					/*把 sizeof(*header) 向上对齐到 spl_get_bl_len(info) 的整数倍。*/
					header);

	if (read < (int)sizeof(*header))
		return -EIO;
	/*检索 image_set_magic*/
	if (image_get_magic(header) == FDT_MAGIC) {/*Flattened Device Tree 格式*/
		log_debug("Found FIT\n");
		if (CONFIG_IS_ENABLED(LOAD_FIT_FULL)) {/*没有使用 */
			void *buf;

			/*
			 * In order to support verifying images in the FIT, we
			 * need to load the whole FIT into memory. Try and
			 * guess how much we need to load by using the total
			 * size. This will fail for FITs with external data,
			 * but there's not much we can do about that.
			 */
			if (!size)
				size = round_up(fdt_totalsize(header), 4);
			buf = map_sysmem(CONFIG_SYS_LOAD_ADDR, size);
			read = info->read(info, offset,
					  ALIGN(size, spl_get_bl_len(info)),
					  buf);
			if (read < size)
				return -EIO;

			return spl_parse_image_header(spl_image, bootdev, buf);
		}

		if (CONFIG_IS_ENABLED(LOAD_FIT)) {/*没有使用 */
			log_debug("Simple loading\n");
			return spl_load_simple_fit(spl_image, info, offset,
						   header);
		}
		log_debug("No FIT support\n");
	}

	if (IS_ENABLED(CONFIG_SPL_LOAD_IMX_CONTAINER) &&/*没有使用 */
	    valid_container_hdr((void *)header))
		return spl_load_imx_container(spl_image, info, offset);

	/* LZMA 是一种数据压缩算法，全称 Lempel-Ziv-Markov chain Algorithm 
		把 U-Boot 镜像压缩，减少 Flash 占用；SPL 加载后再解压到 RAM。
	*/
	if (IS_ENABLED(CONFIG_SPL_LZMA) &&
	    image_get_magic(header) == IH_MAGIC && /* 对应 legacy image */
	    image_get_comp(header) == IH_COMP_LZMA) {
		spl_image->flags |= SPL_COPY_PAYLOAD_ONLY;
		ret = spl_parse_image_header(spl_image, bootdev, header);
		if (ret)
			return ret;

		return spl_load_legacy_lzma(spl_image, info, offset);
	}
	/*
	负责解析普通镜像头，把这些信息填进 spl_image
	镜像大小
	加载地址
	入口地址
	OS 类型
	压缩类型
	*/
	ret = spl_parse_image_header(spl_image, bootdev, header);/* yjrqz-spl8.1 */
	if (ret)
		return ret;

	base_offset = spl_image->offset;
	/* Only NOR sets this flag. */
	if (IS_ENABLED(CONFIG_SPL_NOR_SUPPORT) &&
	    spl_image->flags & SPL_COPY_PAYLOAD_ONLY)
		base_offset += sizeof(*header);
	image_offset = ALIGN_DOWN(base_offset, spl_get_bl_len(info));
	overhead = base_offset - image_offset;
	size = ALIGN(spl_image->size + overhead, spl_get_bl_len(info));

	read = info->read(info, offset + image_offset, size,
			  map_sysmem(spl_image->load_addr - overhead, size));

	if (read < 0)
		return read;

	return read < spl_image->size ? -EIO : 0;
}

/*
 * Although spl_load results in size reduction for callers, this is generally
 * not enough to counteract the bloat if there is only one caller. The core
 * problem is that the compiler can't optimize across translation units. The
 * general solution to this is CONFIG_LTO, but that is not available on all
 * architectures. Perform a pseudo-LTO just for this function by declaring it
 * inline if there is one caller, and extern otherwise.
 */
#define SPL_LOAD_USERS \
	IS_ENABLED(CONFIG_SPL_BLK_FS) + \
	IS_ENABLED(CONFIG_SPL_FS_EXT4) + \
	IS_ENABLED(CONFIG_SPL_FS_FAT) + \
	IS_ENABLED(CONFIG_SPL_SYS_MMCSD_RAW_MODE) + \
	(IS_ENABLED(CONFIG_SPL_NAND_SUPPORT) && !IS_ENABLED(CONFIG_SPL_UBI)) + \
	IS_ENABLED(CONFIG_SPL_NET) + \
	IS_ENABLED(CONFIG_SPL_NOR_SUPPORT) + \
	IS_ENABLED(CONFIG_SPL_SEMIHOSTING) + \
	IS_ENABLED(CONFIG_SPL_SPI_LOAD) + \
	0

#if SPL_LOAD_USERS > 1
/**
 * spl_load() - Parse a header and load the image
 * @spl_image: Image data which will be filled in by this function
 * @bootdev: The device to load from
 * @info: Describes how to load additional information from @bootdev. At the
 *        minimum, read() and bl_len must be populated.
 * @size: The size of the image, in bytes, if it is known in advance. Some boot
 *        devices (such as filesystems) know how big an image is before parsing
 *        the header. If 0, then the size will be determined from the header.
 * @offset: The offset from the start of @bootdev, in bytes. This should have
 *          the offset @header was loaded from. It will be added to any offsets
 *          passed to @info->read().
 *
 * This function determines the image type (FIT, legacy, i.MX, raw, etc), calls
 * the appropriate parsing function, determines the load address, and the loads
 * the image from storage. It is designed to replace ad-hoc image loading which
 * may not support all image types (especially when config options are
 * involved).
 *
 * Return: 0 on success, or a negative error on failure
 */
int spl_load(struct spl_image_info *spl_image,
	     const struct spl_boot_device *bootdev, struct spl_load_info *info,
	     size_t size, size_t offset);
#else
static inline int spl_load(struct spl_image_info *spl_image,
			   const struct spl_boot_device *bootdev,
			   struct spl_load_info *info, size_t size,
			   size_t offset)
{
	return _spl_load(spl_image, bootdev, info, size, offset);
}
#endif

#endif /* _SPL_LOAD_H_ */
