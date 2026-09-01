// SPDX-License-Identifier: GPL-2.0+
/*
 * YJRQZ Boot - lwIP HTTP recovery console for the HLK-7621A.
 *
 * Normal recovery can only update the OpenWrt firmware partition. Raw
 * U-Boot, environment and factory/calibration restores require an exact-size
 * partition image and a target-specific confirmation phrase.
 */

#include <command.h>
#include <console.h>
#include <cpu_func.h>
#include <dm.h>
#include <env.h>
#include <image.h>
#include <lmb.h>
#include <malloc.h>
#include <mapmem.h>
#include <net.h>
#include <spi_flash.h>
#include <version_string.h>
#include <asm/global_data.h>
#include <asm/unaligned.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <lwip/ip4_addr.h>
#include <lwip/tcp.h>
#include <u-boot/crc.h>
#include <u-boot/schedule.h>

#include "webflash_assets.h"

DECLARE_GLOBAL_DATA_PTR;

#define WEBFLASH_HTTP_PORT		80
#define WEBFLASH_HEADER_SIZE		1536
#define WEBFLASH_REPLY_SIZE		1536
#define WEBFLASH_BODY_SIZE		512
#define WEBFLASH_IO_SIZE		(64 * 1024)
#define WEBFLASH_STREAM_CHUNK		1024
#define WEBFLASH_IDLE_TIMEOUT_MS	30000
#define WEBFLASH_REBOOT_DELAY_MS	5000
#define WEBFLASH_TCP_POLL_INTERVAL	4
#define WEBFLASH_FWTOOL_MAGIC		0x46577830 /* FWx0 */
#define WEBFLASH_FWTOOL_INFO		1
#define WEBFLASH_SQUASHFS_MAGIC		0x73717368
#define WEBFLASH_SQUASHFS_SB_SIZE	96

#define WEBFLASH_PART_PROTECTED	BIT(0)
#define WEBFLASH_PART_FIRMWARE	BIT(1)
#define WEBFLASH_PART_ENV	BIT(2)
#define WEBFLASH_PART_FACTORY	BIT(3)
#define WEBFLASH_PART_UBOOT	BIT(4)

enum webflash_rx_state {
	WEBFLASH_RX_HEADERS,
	WEBFLASH_RX_UPLOAD,
	WEBFLASH_RX_BODY,
	WEBFLASH_RX_REPLIED,
};

enum webflash_action {
	WEBFLASH_ACTION_NONE,
	WEBFLASH_ACTION_ENV_SAVE,
};

enum webflash_job_mode {
	WEBFLASH_JOB_NONE,
	WEBFLASH_JOB_FLASH,
	WEBFLASH_JOB_ERASE_SETTINGS,
};

enum webflash_job_phase {
	WEBFLASH_PHASE_IDLE,
	WEBFLASH_PHASE_ERASING,
	WEBFLASH_PHASE_WRITING,
	WEBFLASH_PHASE_VERIFYING,
	WEBFLASH_PHASE_DONE,
	WEBFLASH_PHASE_ERROR,
};

struct webflash_partition {
	const char *name;
	const char *filename;
	u32 offset;
	u32 size;
	u32 flags;
};

struct webflash_job {
	enum webflash_job_mode mode;
	enum webflash_job_phase phase;
	const struct webflash_partition *part;
	u32 erase_offset;
	u32 erase_size;
	u32 write_size;
	u32 processed;
	u32 source_crc;
	u32 verify_crc;
	char error[96];
};

struct webflash_ctx {
	struct tcp_pcb *listener;
	struct tcp_pcb *client;
	struct spi_flash *flash;
	const struct webflash_partition *upload_part;
	struct webflash_job job;
	ulong load_addr;
	ulong upload_size;
	ulong content_length;
	ulong last_activity;
	ulong completed_addr;
	ulong completed_size;
	ulong reply_pending;
	ulong reboot_start;
	u32 upload_crc;
	u32 stream_offset;
	u32 stream_remaining;
	const u8 *stream_memory;
	size_t header_len;
	size_t body_len;
	enum webflash_rx_state rx_state;
	enum webflash_action action;
	bool upload_valid;
	bool client_aborted;
	bool output_pending;
	bool close_pending;
	bool stream_active;
	bool stream_from_flash;
	bool reboot_pending;
	u8 *io_buf;
	char header[WEBFLASH_HEADER_SIZE];
	char reply[WEBFLASH_REPLY_SIZE];
	char body[WEBFLASH_BODY_SIZE];
	char upload_detail[128];
};

struct webflash_fwimage_header {
	__be32 version;
	__be32 flags;
} __packed;

struct webflash_fwimage_trailer {
	__be32 magic;
	__be32 crc32;
	u8 type;
	u8 pad[3];
	__be32 size;
} __packed;

static const struct webflash_partition webflash_partitions[] = {
	{
		.name = "uboot",
		.filename = "hlk-7621a-uboot.bin",
		.offset = CONFIG_WEBFLASH_UBOOT_OFFSET,
		.size = CONFIG_WEBFLASH_UBOOT_SIZE,
		.flags = WEBFLASH_PART_PROTECTED | WEBFLASH_PART_UBOOT,
	}, {
		.name = "environment",
		.filename = "hlk-7621a-environment.bin",
		.offset = CONFIG_WEBFLASH_ENV_OFFSET,
		.size = CONFIG_WEBFLASH_ENV_SIZE,
		.flags = WEBFLASH_PART_PROTECTED | WEBFLASH_PART_ENV,
	}, {
		.name = "factory",
		.filename = "hlk-7621a-factory.bin",
		.offset = CONFIG_WEBFLASH_FACTORY_OFFSET,
		.size = CONFIG_WEBFLASH_FACTORY_SIZE,
		.flags = WEBFLASH_PART_PROTECTED | WEBFLASH_PART_FACTORY,
	}, {
		.name = "firmware",
		.filename = "hlk-7621a-firmware.bin",
		.offset = CONFIG_WEBFLASH_FIRMWARE_OFFSET,
		.size = CONFIG_WEBFLASH_FIRMWARE_SIZE,
		.flags = WEBFLASH_PART_FIRMWARE,
	},
};

static const char *webflash_phase_name(enum webflash_job_phase phase)
{
	switch (phase) {
	case WEBFLASH_PHASE_ERASING:
		return "erasing";
	case WEBFLASH_PHASE_WRITING:
		return "writing";
	case WEBFLASH_PHASE_VERIFYING:
		return "verifying";
	case WEBFLASH_PHASE_DONE:
		return "done";
	case WEBFLASH_PHASE_ERROR:
		return "error";
	default:
		return "idle";
	}
}

static int webflash_stream_fill(struct webflash_ctx *ctx);

static bool webflash_job_active(const struct webflash_ctx *ctx)
{
	return ctx->job.phase == WEBFLASH_PHASE_ERASING ||
	       ctx->job.phase == WEBFLASH_PHASE_WRITING ||
	       ctx->job.phase == WEBFLASH_PHASE_VERIFYING;
}

static bool webflash_operation_blocked(const struct webflash_ctx *ctx)
{
	return webflash_job_active(ctx) || ctx->reboot_pending;
}

static const struct webflash_partition *webflash_partition_find(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(webflash_partitions); i++) {
		if (!strcmp(webflash_partitions[i].name, name))
			return &webflash_partitions[i];
	}

	return NULL;
}

static void webflash_detach_client(struct webflash_ctx *ctx)
{
	struct tcp_pcb *pcb = ctx->client;

	if (!pcb)
		return;

	tcp_arg(pcb, NULL);
	tcp_recv(pcb, NULL);
	tcp_sent(pcb, NULL);
	tcp_err(pcb, NULL);
	tcp_poll(pcb, NULL, 0);
	ctx->client = NULL;
}

static void webflash_abort_client(struct webflash_ctx *ctx)
{
	struct tcp_pcb *pcb = ctx->client;

	if (!pcb)
		return;

	if (ctx->rx_state == WEBFLASH_RX_UPLOAD)
		ctx->upload_valid = false;
	webflash_detach_client(ctx);
	ctx->client_aborted = true;
	tcp_abort(pcb);
}

static err_t webflash_try_close_client(struct webflash_ctx *ctx)
{
	struct tcp_pcb *pcb = ctx->client;
	err_t err;

	if (!pcb)
		return ERR_OK;

	err = tcp_close(pcb);
	if (err == ERR_OK) {
		ctx->client = NULL;
		ctx->close_pending = false;
		return ERR_OK;
	}
	if (err == ERR_MEM) {
		ctx->close_pending = true;
		return ERR_MEM;
	}

	printf("YJRQZ Boot tcp_close failed: %d\n", err);
	webflash_abort_client(ctx);
	return ERR_ABRT;
}

static int webflash_reply(struct webflash_ctx *ctx, const char *status,
			  const char *type, const char *body)
{
	struct tcp_pcb *pcb = ctx->client;
	char header[192];
	size_t body_len = strlen(body);
	int header_len;
	err_t err;

	if (!pcb)
		return -ENOTCONN;

	header_len = snprintf(header, sizeof(header),
			      "HTTP/1.0 %s\r\n"
			      "Content-Type: %s\r\n"
			      "Content-Length: %u\r\n"
			      "Cache-Control: no-store\r\n"
			      "Connection: close\r\n\r\n",
			      status, type, (unsigned int)body_len);
	if (header_len < 0 || header_len >= sizeof(header) ||
	    body_len > U16_MAX) {
		webflash_abort_client(ctx);
		return -EOVERFLOW;
	}

	err = tcp_write(pcb, header, header_len, TCP_WRITE_FLAG_COPY);
	if (err != ERR_OK)
		goto write_error;
	ctx->rx_state = WEBFLASH_RX_REPLIED;
	ctx->reply_pending = header_len + body_len;
	ctx->output_pending = false;
	ctx->close_pending = false;
	ctx->stream_memory = (const u8 *)body;
	ctx->stream_remaining = body_len;
	ctx->stream_from_flash = false;
	ctx->stream_active = body_len != 0;
	if (webflash_stream_fill(ctx)) {
		err = ERR_VAL;
		goto write_error;
	}

	err = tcp_output(pcb);
	if (err == ERR_MEM) {
		ctx->output_pending = true;
		return 0;
	}
	if (err != ERR_OK)
		goto write_error;

	return 0;

write_error:
	printf("YJRQZ Boot response write failed: %d\n", err);
	webflash_abort_client(ctx);
	return -EIO;
}

static void webflash_error(struct webflash_ctx *ctx, const char *status,
			   const char *message)
{
	snprintf(ctx->reply, sizeof(ctx->reply),
		 "{\"ok\":false,\"error\":\"%s\"}", message);
	webflash_reply(ctx, status, "application/json", ctx->reply);
}

static int webflash_ok(struct webflash_ctx *ctx, const char *message)
{
	snprintf(ctx->reply, sizeof(ctx->reply),
		 "{\"ok\":true,\"message\":\"%s\"}", message);
	return webflash_reply(ctx, "200 OK", "application/json", ctx->reply);
}

static bool webflash_path_is(const char *header, size_t header_len,
			     const char *method, const char *path)
{
	size_t method_len = strlen(method);
	size_t path_len = strlen(path);
	size_t request_len = method_len + path_len + 2;

	return header_len > request_len &&
	       !strncmp(header, method, method_len) &&
	       header[method_len] == ' ' &&
	       !strncmp(header + method_len + 1, path, path_len) &&
	       header[method_len + path_len + 1] == ' ';
}

static int webflash_header_value(struct webflash_ctx *ctx, const char *name,
				 char *value, size_t value_size)
{
	const char *line = strstr(ctx->header, "\r\n");
	size_t name_len = strlen(name);

	if (!line)
		return -EINVAL;
	line += 2;

	while (*line && strncmp(line, "\r\n", 2)) {
		const char *end = strstr(line, "\r\n");
		const char *colon;
		const char *start;
		size_t len;

		if (!end)
			break;
		colon = memchr(line, ':', end - line);
		if (colon && colon - line == name_len &&
		    !strncasecmp(line, name, name_len)) {
			start = colon + 1;
			while (start < end && (*start == ' ' || *start == '\t'))
				start++;
			len = end - start;
			if (len >= value_size)
				return -E2BIG;
			memcpy(value, start, len);
			value[len] = '\0';
			return 0;
		}
		line = end + 2;
	}

	return -ENOENT;
}

static int webflash_get_content_length(struct webflash_ctx *ctx, ulong *size)
{
	char value[32];
	char *end;

	if (webflash_header_value(ctx, "Content-Length", value, sizeof(value)))
		return -ENOENT;
	*size = simple_strtoul(value, &end, 10);
	if (end == value || *end)
		return -EINVAL;

	return 0;
}

static bool webflash_range_valid(struct webflash_ctx *ctx, ulong size)
{
	ulong ram_end = gd->ram_base + gd->ram_size;

	if (ctx->load_addr + size < ctx->load_addr)
		return false;
	if (ram_end < gd->ram_base || ctx->load_addr < gd->ram_base ||
	    ctx->load_addr + size > ram_end)
		return false;
	if (CONFIG_IS_ENABLED(LMB) && lmb_read_check(ctx->load_addr, size))
		return false;

	return true;
}

static const u8 *webflash_memmem(const u8 *data, size_t data_len,
				 const char *needle)
{
	size_t needle_len = strlen(needle);
	size_t i;

	if (!needle_len || needle_len > data_len)
		return NULL;
	for (i = 0; i <= data_len - needle_len; i++) {
		if (!memcmp(data + i, needle, needle_len))
			return data + i;
	}

	return NULL;
}

static bool webflash_json_array_has(const u8 *json, size_t json_len,
				    const char *key, const char *value)
{
	const u8 *p = webflash_memmem(json, json_len, key);
	const u8 *end = json + json_len;
	size_t value_len = strlen(value);

	if (!p)
		return false;
	p += strlen(key);
	while (p < end && *p != '[')
		p++;
	if (p == end)
		return false;
	p++;

	while (p < end && *p != ']') {
		const u8 *start;

		while (p < end && *p != '"' && *p != ']')
			p++;
		if (p == end || *p == ']')
			break;
		start = ++p;
		while (p < end && *p != '"') {
			if (*p == '\\' && p + 1 < end)
				p++;
			p++;
		}
		if (p == end)
			break;
		if (p - start == value_len && !memcmp(start, value, value_len))
			return true;
		p++;
	}

	return false;
}

static int webflash_validate_fwtool(const u8 *image, u32 image_size,
				    char *detail, size_t detail_size)
{
	u32 cursor = image_size;
	int depth;

	for (depth = 0; depth < 3; depth++) {
		const struct webflash_fwimage_trailer *trailer;
		const struct webflash_fwimage_header *fw_header;
		const u8 *json;
		u32 trailer_offset;
		u32 record_size;
		u32 data_start;
		u32 data_len;
		u32 stored_crc;
		u32 calc_crc;

		if (cursor < sizeof(*trailer))
			break;
		trailer_offset = cursor - sizeof(*trailer);
		trailer = (const void *)(image + trailer_offset);
		if (be32_to_cpu(trailer->magic) != WEBFLASH_FWTOOL_MAGIC)
			break;
		record_size = be32_to_cpu(trailer->size);
		if (record_size < sizeof(*trailer) || record_size > cursor) {
			snprintf(detail, detail_size, "invalid OpenWrt metadata size");
			return -EINVAL;
		}

		calc_crc = crc32_no_comp(~0U, image, trailer_offset);
		stored_crc = be32_to_cpu(trailer->crc32);
		if (calc_crc != stored_crc) {
			snprintf(detail, detail_size, "OpenWrt metadata CRC mismatch");
			return -EBADMSG;
		}

		data_start = cursor - record_size;
		data_len = record_size - sizeof(*trailer);
		if (trailer->type == WEBFLASH_FWTOOL_INFO) {
			if (data_len <= sizeof(*fw_header)) {
				snprintf(detail, detail_size, "OpenWrt metadata is empty");
				return -EINVAL;
			}
			fw_header = (const void *)(image + data_start);
			if (fw_header->version) {
				snprintf(detail, detail_size, "unsupported metadata version");
				return -EPROTONOSUPPORT;
			}
			json = image + data_start + sizeof(*fw_header);
			data_len -= sizeof(*fw_header);
			if (!webflash_json_array_has(json, data_len,
						     "\"supported_devices\"",
						     CONFIG_WEBFLASH_DEVICE_ID) &&
			    !webflash_json_array_has(json, data_len,
						     "\"new_supported_devices\"",
						     CONFIG_WEBFLASH_DEVICE_ID)) {
				snprintf(detail, detail_size,
					 "image does not support %s",
					 CONFIG_WEBFLASH_DEVICE_ID);
				return -ENODEV;
			}
			return 0;
		}

		/* A signature record may follow the metadata record. */
		cursor = data_start;
	}

	snprintf(detail, detail_size, "OpenWrt sysupgrade metadata not found");
	return -ENOENT;
}

static bool webflash_buffer_uniform(const u8 *data, u32 size)
{
	u8 value = data[0];
	u32 i;

	for (i = 1; i < size; i++) {
		if (data[i] != value)
			return false;
	}

	return true;
}

static int webflash_validate_upload(struct webflash_ctx *ctx)
{
	const struct webflash_partition *part = ctx->upload_part;
	const u8 *data;
	int ret = 0;

	data = map_sysmem(ctx->completed_addr, ctx->completed_size);
	ctx->upload_crc = crc32_wd(0, data, ctx->completed_size, 64 * 1024);

	if (part->flags & WEBFLASH_PART_FIRMWARE) {
		const struct legacy_img_hdr *header = (const void *)data;
		u32 legacy_size;

		if (ctx->completed_size < sizeof(*header)) {
			snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
				 "image is smaller than a uImage header");
			ret = -EINVAL;
			goto out;
		}
		if (!image_check_magic(header) || !image_check_hcrc(header)) {
			snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
				 "invalid uImage header or header CRC");
			ret = -EBADMSG;
			goto out;
		}
		legacy_size = image_get_image_size(header);
		if (legacy_size > ctx->completed_size) {
			snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
				 "truncated uImage payload");
			ret = -EFBIG;
			goto out;
		}
		if (!image_check_os(header, IH_OS_LINUX) ||
		    !image_check_arch(header, IH_ARCH_MIPS) ||
		    !image_check_type(header, IH_TYPE_KERNEL)) {
			snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
				 "uImage is not a MIPS Linux kernel image");
			ret = -ENOEXEC;
			goto out;
		}
		if (!image_check_dcrc(header)) {
			snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
				 "uImage data CRC mismatch");
			ret = -EBADMSG;
			goto out;
		}
		ret = webflash_validate_fwtool(data, ctx->completed_size,
					       ctx->upload_detail,
					       sizeof(ctx->upload_detail));
		if (!ret)
			snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
				 "OpenWrt image verified for %s",
				 CONFIG_WEBFLASH_DEVICE_ID);
	} else if (part->flags & WEBFLASH_PART_ENV) {
		u32 stored_crc;
		u32 calc_crc;

		if (ctx->completed_size < CONFIG_ENV_SIZE) {
			ret = -EINVAL;
			goto protected_error;
		}
		stored_crc = get_unaligned_le32(data);
		calc_crc = crc32(0, data + sizeof(u32),
				 CONFIG_ENV_SIZE - sizeof(u32));
		if (stored_crc != calc_crc) {
			snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
				 "environment CRC mismatch");
			ret = -EBADMSG;
		} else {
			snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
				 "raw environment partition verified");
		}
	} else {
		if (webflash_buffer_uniform(data, ctx->completed_size)) {
			ret = -EINVAL;
			goto protected_error;
		}
		if ((part->flags & WEBFLASH_PART_UBOOT) &&
		    (ctx->completed_size < 0x44 || memcmp(data + 0x40, "7621", 4))) {
			snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
				 "MT7621 U-Boot marker not found");
			ret = -ENOEXEC;
		} else {
			snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
				 "exact-size raw %s partition image accepted",
				 part->name);
		}
	}

	goto out;

protected_error:
	snprintf(ctx->upload_detail, sizeof(ctx->upload_detail),
		 "raw %s image is empty or invalid", part->name);
out:
	unmap_sysmem(data);
	ctx->upload_valid = !ret;
	return ret;
}

static int webflash_prepare_upload(struct webflash_ctx *ctx,
				   const struct webflash_partition *part)
{
	ulong size;
	int ret;

	if (webflash_operation_blocked(ctx)) {
		webflash_error(ctx, "409 Conflict", "device is flashing or rebooting");
		return -EBUSY;
	}
	ret = webflash_get_content_length(ctx, &size);
	if (ret == -ENOENT) {
		webflash_error(ctx, "411 Length Required", "Content-Length required");
		return ret;
	}
	if (ret || !size || size > part->size ||
	    ((part->flags & WEBFLASH_PART_PROTECTED) && size != part->size)) {
		webflash_error(ctx, "413 Content Too Large",
			       "upload size does not match the selected partition");
		return -EFBIG;
	}
	if (!webflash_range_valid(ctx, size)) {
		webflash_error(ctx, "409 Conflict", "upload does not fit safe RAM");
		return -ERANGE;
	}

	memset(&ctx->job, 0, sizeof(ctx->job));
	ctx->upload_part = part;
	ctx->content_length = size;
	ctx->upload_size = 0;
	ctx->completed_size = 0;
	ctx->upload_valid = false;
	ctx->upload_detail[0] = '\0';
	ctx->rx_state = WEBFLASH_RX_UPLOAD;
	printf("YJRQZ Boot upload: %s, %lu bytes to 0x%08lx\n",
	       part->name, size, ctx->load_addr);
	return 0;
}

static int webflash_store_upload(struct webflash_ctx *ctx, const void *data,
				 size_t len)
{
	void *ptr;
	ulong remaining = ctx->content_length - ctx->upload_size;
	int ret;

	if (len > remaining) {
		webflash_error(ctx, "400 Bad Request", "body exceeds Content-Length");
		return -EFBIG;
	}

	ptr = map_sysmem(ctx->load_addr + ctx->upload_size, len);
	memcpy(ptr, data, len);
	unmap_sysmem(ptr);
	ctx->upload_size += len;

	if (ctx->upload_size != ctx->content_length)
		return 0;

	ctx->completed_addr = ctx->load_addr;
	ctx->completed_size = ctx->upload_size;
	if (env_set_hex("fileaddr", ctx->completed_addr) ||
	    env_set_hex("filesize", ctx->completed_size)) {
		webflash_error(ctx, "500 Internal Server Error",
			       "failed to update upload environment");
		return -EIO;
	}

	ret = webflash_validate_upload(ctx);
	snprintf(ctx->reply, sizeof(ctx->reply),
		 "{\"ok\":%s,\"target\":\"%s\",\"size\":%lu,"
		 "\"crc32\":\"%08x\",\"detail\":\"%s\"}",
		 ret ? "false" : "true", ctx->upload_part->name,
		 ctx->completed_size, ctx->upload_crc, ctx->upload_detail);
	webflash_reply(ctx, ret ? "422 Unprocessable Content" : "200 OK",
		       "application/json", ctx->reply);
	return ret;
}

static int webflash_percent_decode(char *value)
{
	char *src = value;
	char *dst = value;

	while (*src) {
		if (*src == '+') {
			*dst++ = ' ';
			src++;
		} else if (*src == '%' && src[1] && src[2]) {
			int high = hex_to_bin(src[1]);
			int low = hex_to_bin(src[2]);

			if (high < 0 || low < 0)
				return -EINVAL;
			*dst++ = (high << 4) | low;
			src += 3;
		} else {
			*dst++ = *src++;
		}
	}
	*dst = '\0';
	return 0;
}

static int webflash_form_value(const char *body, const char *key, char *value,
			       size_t value_size)
{
	const char *field = body;
	size_t key_len = strlen(key);

	while (*field) {
		const char *next = strchr(field, '&');
		const char *field_end = next ? next : field + strlen(field);
		const char *equal = memchr(field, '=', field_end - field);
		size_t len;

		if (equal && equal - field == key_len &&
		    !memcmp(field, key, key_len)) {
			len = field_end - equal - 1;
			if (len >= value_size)
				return -E2BIG;
			memcpy(value, equal + 1, len);
			value[len] = '\0';
			return webflash_percent_decode(value);
		}
		if (!next)
			break;
		field = next + 1;
	}

	return -ENOENT;
}

static bool webflash_ipv4_valid(const char *value)
{
	ip4_addr_t addr;

	return value[0] && ip4addr_aton(value, &addr);
}

static int webflash_env_save(struct webflash_ctx *ctx)
{
	char ipaddr[24];
	char netmask[24];
	char serverip[24];
	char bootdelay[12];
	ip4_addr_t mask;
	char *end;
	ulong delay;

	if (webflash_form_value(ctx->body, "ipaddr", ipaddr, sizeof(ipaddr)) ||
	    webflash_form_value(ctx->body, "netmask", netmask, sizeof(netmask)) ||
	    webflash_form_value(ctx->body, "serverip", serverip,
				 sizeof(serverip)) ||
	    webflash_form_value(ctx->body, "bootdelay", bootdelay,
				 sizeof(bootdelay))) {
		webflash_error(ctx, "400 Bad Request", "missing environment field");
		return -EINVAL;
	}
	if (!webflash_ipv4_valid(ipaddr) ||
	    !ip4addr_aton(netmask, &mask) ||
	    !ip4_addr_netmask_valid(mask.addr) ||
	    (serverip[0] && !webflash_ipv4_valid(serverip))) {
		webflash_error(ctx, "400 Bad Request", "invalid IPv4 setting");
		return -EINVAL;
	}
	delay = simple_strtoul(bootdelay, &end, 10);
	if (end == bootdelay || *end || delay > 30) {
		webflash_error(ctx, "400 Bad Request", "bootdelay must be 0 to 30");
		return -EINVAL;
	}

	if (env_set("ipaddr", ipaddr) || env_set("netmask", netmask) ||
	    env_set("serverip", serverip[0] ? serverip : NULL) ||
	    env_set("bootdelay", bootdelay) || env_save()) {
		webflash_error(ctx, "500 Internal Server Error",
			       "failed to save environment");
		return -EIO;
	}

	return webflash_ok(ctx, "environment saved; reboot to apply network changes");
}

static int webflash_handle_body(struct webflash_ctx *ctx)
{
	switch (ctx->action) {
	case WEBFLASH_ACTION_ENV_SAVE:
		return webflash_env_save(ctx);
	default:
		webflash_error(ctx, "400 Bad Request", "invalid action body");
		return -EINVAL;
	}
}

static int webflash_store_body(struct webflash_ctx *ctx, const void *data,
			       size_t len)
{
	ulong remaining = ctx->content_length - ctx->body_len;

	if (len > remaining || ctx->body_len + len >= sizeof(ctx->body)) {
		webflash_error(ctx, "413 Content Too Large", "action body too large");
		return -EFBIG;
	}
	memcpy(ctx->body + ctx->body_len, data, len);
	ctx->body_len += len;
	ctx->body[ctx->body_len] = '\0';
	if (ctx->body_len == ctx->content_length)
		return webflash_handle_body(ctx);

	return 0;
}

static int webflash_prepare_body(struct webflash_ctx *ctx,
				 enum webflash_action action)
{
	ulong size;
	int ret = webflash_get_content_length(ctx, &size);

	if (webflash_operation_blocked(ctx)) {
		webflash_error(ctx, "409 Conflict", "device is flashing or rebooting");
		return -EBUSY;
	}
	if (ret == -ENOENT) {
		webflash_error(ctx, "411 Length Required", "Content-Length required");
		return ret;
	}
	if (ret || !size || size >= sizeof(ctx->body)) {
		webflash_error(ctx, "413 Content Too Large", "invalid action body size");
		return -EFBIG;
	}
	ctx->content_length = size;
	ctx->body_len = 0;
	ctx->body[0] = '\0';
	ctx->action = action;
	ctx->rx_state = WEBFLASH_RX_BODY;
	return 0;
}

static const char *webflash_expected_confirmation(
					const struct webflash_partition *part)
{
	if (part->flags & WEBFLASH_PART_UBOOT)
		return "RESTORE-UBOOT";
	if (part->flags & WEBFLASH_PART_FACTORY)
		return "RESTORE-FACTORY";
	if (part->flags & WEBFLASH_PART_ENV)
		return "RESTORE-ENVIRONMENT";
	return "FLASH-FIRMWARE";
}

static int webflash_check_confirmation(struct webflash_ctx *ctx,
				       const char *expected)
{
	char confirmation[40];

	if (webflash_header_value(ctx, "X-Safeboot-Confirm", confirmation,
				  sizeof(confirmation)) ||
	    strcmp(confirmation, expected)) {
		webflash_error(ctx, "403 Forbidden", "confirmation phrase rejected");
		return -EACCES;
	}

	return 0;
}

static void webflash_start_flash_job(struct webflash_ctx *ctx)
{
	struct webflash_job *job = &ctx->job;

	memset(job, 0, sizeof(*job));
	job->mode = WEBFLASH_JOB_FLASH;
	job->phase = WEBFLASH_PHASE_ERASING;
	job->part = ctx->upload_part;
	job->erase_offset = job->part->offset;
	job->erase_size = job->part->size;
	job->write_size = ctx->completed_size;
	job->source_crc = ctx->upload_crc;
	printf("YJRQZ Boot flash job: %s offset 0x%x erase 0x%x write 0x%x\n",
	       job->part->name, job->erase_offset, job->erase_size,
	       job->write_size);
}

static void webflash_job_fail(struct webflash_ctx *ctx, const char *operation,
			      int ret)
{
	ctx->job.phase = WEBFLASH_PHASE_ERROR;
	snprintf(ctx->job.error, sizeof(ctx->job.error), "%s failed (%d)",
		 operation, ret);
	printf("YJRQZ Boot: %s\n", ctx->job.error);
}

static void webflash_schedule_reboot(struct webflash_ctx *ctx)
{
	ctx->reboot_start = get_timer(0);
	ctx->reboot_pending = true;
}

static u32 webflash_job_chunk(struct webflash_ctx *ctx, u32 remaining,
			      bool erase)
{
	u32 chunk = min_t(u32, remaining, WEBFLASH_IO_SIZE);
	u32 erase_size = ctx->flash->sector_size;

	if (!erase)
		return chunk;
	if (erase_size > WEBFLASH_IO_SIZE)
		return min(remaining, erase_size);
	chunk = rounddown(chunk, erase_size);
	return chunk ? chunk : min(remaining, erase_size);
}

static void webflash_job_step(struct webflash_ctx *ctx)
{
	struct webflash_job *job = &ctx->job;
	u32 remaining;
	u32 chunk;
	void *source;
	int ret;

	switch (job->phase) {
	case WEBFLASH_PHASE_ERASING:
		remaining = job->erase_size - job->processed;
		chunk = webflash_job_chunk(ctx, remaining, true);
		ret = spi_flash_erase(ctx->flash,
				      job->erase_offset + job->processed, chunk);
		if (ret) {
			webflash_job_fail(ctx, "erase", ret);
			return;
		}
		job->processed += chunk;
		if (job->processed == job->erase_size) {
			job->processed = 0;
			if (job->mode == WEBFLASH_JOB_ERASE_SETTINGS) {
				job->phase = WEBFLASH_PHASE_DONE;
				webflash_schedule_reboot(ctx);
			} else {
				job->phase = WEBFLASH_PHASE_WRITING;
			}
		}
		break;

	case WEBFLASH_PHASE_WRITING:
		remaining = job->write_size - job->processed;
		chunk = webflash_job_chunk(ctx, remaining, false);
		source = map_sysmem(ctx->completed_addr + job->processed, chunk);
		ret = spi_flash_write(ctx->flash, job->part->offset + job->processed,
				      chunk, source);
		unmap_sysmem(source);
		if (ret) {
			webflash_job_fail(ctx, "write", ret);
			return;
		}
		job->processed += chunk;
		if (job->processed == job->write_size) {
			job->processed = 0;
			job->verify_crc = 0;
			job->phase = WEBFLASH_PHASE_VERIFYING;
		}
		break;

	case WEBFLASH_PHASE_VERIFYING:
		remaining = job->write_size - job->processed;
		chunk = webflash_job_chunk(ctx, remaining, false);
		ret = spi_flash_read(ctx->flash, job->part->offset + job->processed,
				     chunk, ctx->io_buf);
		if (ret) {
			webflash_job_fail(ctx, "verify read", ret);
			return;
		}
		job->verify_crc = crc32(job->verify_crc, ctx->io_buf, chunk);
		job->processed += chunk;
		if (job->processed == job->write_size) {
			if (job->verify_crc != job->source_crc) {
				webflash_job_fail(ctx, "verify CRC", -EBADMSG);
				return;
			}
			job->phase = WEBFLASH_PHASE_DONE;
			webflash_schedule_reboot(ctx);
			puts("YJRQZ Boot flash and verification complete\n");
		}
		break;
	default:
		break;
	}
}

static unsigned int webflash_job_progress(const struct webflash_job *job)
{
	if (job->phase == WEBFLASH_PHASE_DONE)
		return 100;
	if (job->phase == WEBFLASH_PHASE_ERROR ||
	    job->phase == WEBFLASH_PHASE_IDLE)
		return 0;
	if (job->mode == WEBFLASH_JOB_ERASE_SETTINGS)
		return job->erase_size ? job->processed * 100ULL /
					      job->erase_size : 0;
	if (job->phase == WEBFLASH_PHASE_ERASING)
		return job->erase_size ? job->processed * 35ULL /
					      job->erase_size : 0;
	if (job->phase == WEBFLASH_PHASE_WRITING)
		return 35 + (job->write_size ? job->processed * 40ULL /
						 job->write_size : 0);
	return 75 + (job->write_size ? job->processed * 25ULL /
					     job->write_size : 0);
}

static int webflash_start_uploaded_flash(struct webflash_ctx *ctx)
{
	const char *expected;

	if (webflash_operation_blocked(ctx)) {
		webflash_error(ctx, "409 Conflict", "device is flashing or rebooting");
		return -EBUSY;
	}
	if (!ctx->upload_part || !ctx->upload_valid || !ctx->completed_size) {
		webflash_error(ctx, "409 Conflict", "no validated upload is ready");
		return -EINVAL;
	}
	expected = webflash_expected_confirmation(ctx->upload_part);
	if (webflash_check_confirmation(ctx, expected))
		return -EACCES;

	webflash_start_flash_job(ctx);
	snprintf(ctx->reply, sizeof(ctx->reply),
		 "{\"ok\":true,\"target\":\"%s\",\"phase\":\"erasing\"}",
		 ctx->upload_part->name);
	return webflash_reply(ctx, "202 Accepted", "application/json", ctx->reply);
}

static int webflash_find_overlay(struct webflash_ctx *ctx, u32 *offset)
{
	u32 start = CONFIG_WEBFLASH_FIRMWARE_OFFSET;
	u32 end = start + CONFIG_WEBFLASH_FIRMWARE_SIZE;
	u32 pos;

	for (pos = start; pos + WEBFLASH_SQUASHFS_SB_SIZE <= end;
	     pos += WEBFLASH_IO_SIZE - (WEBFLASH_SQUASHFS_SB_SIZE - 1)) {
		u32 read_len = min_t(u32, WEBFLASH_IO_SIZE, end - pos);
		u32 i;
		int ret;

		schedule();
		ret = spi_flash_read(ctx->flash, pos, read_len, ctx->io_buf);
		if (ret)
			return ret;
		for (i = 0; i + WEBFLASH_SQUASHFS_SB_SIZE <= read_len; i++) {
			const u8 *sb = ctx->io_buf + i;
			u64 bytes_used;
			u64 rootfs_end;
			u32 candidate;

			if (get_unaligned_le32(sb) != WEBFLASH_SQUASHFS_MAGIC)
				continue;
			if (get_unaligned_le16(sb + 28) != 4 ||
			    get_unaligned_le16(sb + 30) != 0)
				continue;
			bytes_used = get_unaligned_le64(sb + 40);
			if (bytes_used < WEBFLASH_SQUASHFS_SB_SIZE)
				continue;
			rootfs_end = (u64)pos + i + bytes_used;
			if (rootfs_end >= end)
				continue;
			candidate = roundup((u32)rootfs_end,
					    ctx->flash->sector_size);
			if (candidate < end) {
				*offset = candidate;
				return 0;
			}
		}
	}

	return -ENOENT;
}

static int webflash_factory_reset(struct webflash_ctx *ctx)
{
	u32 overlay;
	int ret;

	if (webflash_operation_blocked(ctx)) {
		webflash_error(ctx, "409 Conflict", "device is flashing or rebooting");
		return -EBUSY;
	}
	if (webflash_check_confirmation(ctx, "ERASE-SETTINGS"))
		return -EACCES;
	ret = webflash_find_overlay(ctx, &overlay);
	if (ret) {
		webflash_error(ctx, "422 Unprocessable Content",
			       "OpenWrt rootfs_data boundary was not found");
		return ret;
	}

	memset(&ctx->job, 0, sizeof(ctx->job));
	ctx->job.mode = WEBFLASH_JOB_ERASE_SETTINGS;
	ctx->job.phase = WEBFLASH_PHASE_ERASING;
	ctx->job.part = webflash_partition_find("firmware");
	ctx->job.erase_offset = overlay;
	ctx->job.erase_size = CONFIG_WEBFLASH_FIRMWARE_OFFSET +
				      CONFIG_WEBFLASH_FIRMWARE_SIZE - overlay;
	printf("YJRQZ Boot factory reset: erase 0x%x + 0x%x\n", overlay,
	       ctx->job.erase_size);
	snprintf(ctx->reply, sizeof(ctx->reply),
		 "{\"ok\":true,\"phase\":\"erasing\",\"offset\":\"0x%08x\"}",
		 overlay);
	return webflash_reply(ctx, "202 Accepted", "application/json", ctx->reply);
}

static int webflash_stream_fill(struct webflash_ctx *ctx)
{
	while (ctx->stream_remaining && tcp_sndbuf(ctx->client)) {
		u16_t len = min_t(u32, ctx->stream_remaining,
				  min_t(u16_t, tcp_sndbuf(ctx->client),
					WEBFLASH_STREAM_CHUNK));
		const void *data;
		err_t err;
		int ret;

		if (ctx->stream_from_flash) {
			ret = spi_flash_read(ctx->flash, ctx->stream_offset, len,
					     ctx->io_buf);
			if (ret)
				return ret;
			data = ctx->io_buf;
		} else {
			data = ctx->stream_memory;
		}
		err = tcp_write(ctx->client, data, len, TCP_WRITE_FLAG_COPY);
		if (err == ERR_MEM)
			break;
		if (err != ERR_OK)
			return -EIO;
		if (ctx->stream_from_flash)
			ctx->stream_offset += len;
		else
			ctx->stream_memory += len;
		ctx->stream_remaining -= len;
	}
	if (!ctx->stream_remaining)
		ctx->stream_active = false;

	return 0;
}

static int webflash_start_download(struct webflash_ctx *ctx, const char *name)
{
	const struct webflash_partition *part;
	char header[256];
	u32 offset;
	u32 size;
	const char *filename;
	int header_len;
	err_t err;

	if (webflash_operation_blocked(ctx)) {
		webflash_error(ctx, "409 Conflict", "backup unavailable while flashing");
		return -EBUSY;
	}
	if (!strcmp(name, "fullflash")) {
		offset = 0;
		size = ctx->flash->size;
		filename = "hlk-7621a-fullflash.bin";
	} else {
		part = webflash_partition_find(name);
		if (!part) {
			webflash_error(ctx, "404 Not Found", "unknown backup partition");
			return -ENOENT;
		}
		offset = part->offset;
		size = part->size;
		filename = part->filename;
	}

	header_len = snprintf(header, sizeof(header),
			      "HTTP/1.0 200 OK\r\n"
			      "Content-Type: application/octet-stream\r\n"
			      "Content-Disposition: attachment; filename=\"%s\"\r\n"
			      "Content-Length: %u\r\n"
			      "Cache-Control: no-store\r\n"
			      "Connection: close\r\n\r\n",
			      filename, size);
	if (header_len < 0 || header_len >= sizeof(header))
		return -EOVERFLOW;
	err = tcp_write(ctx->client, header, header_len, TCP_WRITE_FLAG_COPY);
	if (err != ERR_OK) {
		webflash_abort_client(ctx);
		return -EIO;
	}

	ctx->rx_state = WEBFLASH_RX_REPLIED;
	ctx->reply_pending = header_len + size;
	ctx->stream_offset = offset;
	ctx->stream_remaining = size;
	ctx->stream_memory = NULL;
	ctx->stream_from_flash = true;
	ctx->stream_active = true;
	ctx->output_pending = false;
	ctx->close_pending = false;
	if (webflash_stream_fill(ctx)) {
		webflash_abort_client(ctx);
		return -EIO;
	}
	err = tcp_output(ctx->client);
	if (err == ERR_MEM)
		ctx->output_pending = true;
	else if (err != ERR_OK) {
		webflash_abort_client(ctx);
		return -EIO;
	}
	return 0;
}

static int webflash_info(struct webflash_ctx *ctx)
{
	const char *ip = env_get("ipaddr");

	snprintf(ctx->reply, sizeof(ctx->reply),
		 "{\"product\":\"%s\",\"version\":\"%s\","
		 "\"device\":\"%s\",\"ip\":\"%s\","
		 "\"load_address\":\"0x%08lx\",\"max_upload\":%lu,"
		 "\"flash_size\":%u,\"erase_size\":%u,"
		 "\"partitions\":["
		 "{\"name\":\"uboot\",\"offset\":%u,\"size\":%u,\"protected\":true},"
		 "{\"name\":\"environment\",\"offset\":%u,\"size\":%u,\"protected\":true},"
		 "{\"name\":\"factory\",\"offset\":%u,\"size\":%u,\"protected\":true},"
		 "{\"name\":\"firmware\",\"offset\":%u,\"size\":%u,\"protected\":false}]}",
		 CONFIG_WEBFLASH_PRODUCT_NAME, version_string,
		 CONFIG_WEBFLASH_DEVICE_ID, ip ?: "0.0.0.0", ctx->load_addr,
		 (ulong)CONFIG_WEBFLASH_FIRMWARE_SIZE, ctx->flash->size,
		 ctx->flash->sector_size,
		 CONFIG_WEBFLASH_UBOOT_OFFSET, CONFIG_WEBFLASH_UBOOT_SIZE,
		 CONFIG_WEBFLASH_ENV_OFFSET, CONFIG_WEBFLASH_ENV_SIZE,
		 CONFIG_WEBFLASH_FACTORY_OFFSET, CONFIG_WEBFLASH_FACTORY_SIZE,
		 CONFIG_WEBFLASH_FIRMWARE_OFFSET, CONFIG_WEBFLASH_FIRMWARE_SIZE);
	return webflash_reply(ctx, "200 OK", "application/json", ctx->reply);
}

static int webflash_job_status(struct webflash_ctx *ctx)
{
	struct webflash_job *job = &ctx->job;
	u32 total = job->phase == WEBFLASH_PHASE_ERASING ? job->erase_size :
		    job->write_size;

	snprintf(ctx->reply, sizeof(ctx->reply),
		 "{\"ok\":true,\"active\":%s,\"phase\":\"%s\","
		 "\"progress\":%u,\"processed\":%u,\"total\":%u,"
		 "\"target\":\"%s\",\"error\":\"%s\","
		 "\"reboot_pending\":%s}",
		 webflash_job_active(ctx) ? "true" : "false",
		 webflash_phase_name(job->phase), webflash_job_progress(job),
		 job->processed, total, job->part ? job->part->name : "",
		 job->error, ctx->reboot_pending ? "true" : "false");
	return webflash_reply(ctx, "200 OK", "application/json", ctx->reply);
}

static int webflash_env_info(struct webflash_ctx *ctx)
{
	const char *ipaddr = env_get("ipaddr");
	const char *netmask = env_get("netmask");
	const char *serverip = env_get("serverip");
	const char *bootdelay = env_get("bootdelay");

	snprintf(ctx->reply, sizeof(ctx->reply),
		 "{\"ipaddr\":\"%s\",\"netmask\":\"%s\","
		 "\"serverip\":\"%s\",\"bootdelay\":\"%s\"}",
		 ipaddr ?: "", netmask ?: "", serverip ?: "",
		 bootdelay ?: "");
	return webflash_reply(ctx, "200 OK", "application/json", ctx->reply);
}

static int webflash_env_reset(struct webflash_ctx *ctx)
{
	if (webflash_operation_blocked(ctx)) {
		webflash_error(ctx, "409 Conflict", "device is flashing or rebooting");
		return -EBUSY;
	}
	if (webflash_check_confirmation(ctx, "RESET-ENVIRONMENT"))
		return -EACCES;
	env_set_default("Reset by YJRQZ Boot", 0);
	if (env_save()) {
		webflash_error(ctx, "500 Internal Server Error",
			       "failed to save default environment");
		return -EIO;
	}
	return webflash_ok(ctx, "default boot environment restored");
}

static int webflash_handle_headers(struct webflash_ctx *ctx)
{
	const struct webflash_partition *part;
	const char *name;
	const char *line_end = strstr(ctx->header, "\r\n");
	int line_len = line_end ? line_end - ctx->header : ctx->header_len;

	printf("YJRQZ Boot request: %.*s\n", line_len, ctx->header);

	if (webflash_path_is(ctx->header, ctx->header_len, "GET", "/") ||
	    webflash_path_is(ctx->header, ctx->header_len, "GET", "/index.html"))
		return webflash_reply(ctx, "200 OK", "text/html; charset=utf-8",
				      webflash_index_html);
	if (webflash_path_is(ctx->header, ctx->header_len, "GET", "/api/info"))
		return webflash_info(ctx);
	if (webflash_path_is(ctx->header, ctx->header_len, "GET", "/api/job"))
		return webflash_job_status(ctx);
	if (webflash_path_is(ctx->header, ctx->header_len, "GET", "/api/env"))
		return webflash_env_info(ctx);

	name = NULL;
	if (webflash_path_is(ctx->header, ctx->header_len, "GET",
			     "/api/backup/uboot"))
		name = "uboot";
	else if (webflash_path_is(ctx->header, ctx->header_len, "GET",
				  "/api/backup/environment"))
		name = "environment";
	else if (webflash_path_is(ctx->header, ctx->header_len, "GET",
				  "/api/backup/factory"))
		name = "factory";
	else if (webflash_path_is(ctx->header, ctx->header_len, "GET",
				  "/api/backup/firmware"))
		name = "firmware";
	else if (webflash_path_is(ctx->header, ctx->header_len, "GET",
				  "/api/backup/fullflash"))
		name = "fullflash";
	if (name)
		return webflash_start_download(ctx, name);

	part = NULL;
	if (webflash_path_is(ctx->header, ctx->header_len, "POST",
			     "/api/upload/firmware"))
		part = webflash_partition_find("firmware");
	else if (webflash_path_is(ctx->header, ctx->header_len, "POST",
				  "/api/upload/uboot"))
		part = webflash_partition_find("uboot");
	else if (webflash_path_is(ctx->header, ctx->header_len, "POST",
				  "/api/upload/environment"))
		part = webflash_partition_find("environment");
	else if (webflash_path_is(ctx->header, ctx->header_len, "POST",
				  "/api/upload/factory"))
		part = webflash_partition_find("factory");
	if (part)
		return webflash_prepare_upload(ctx, part);

	if (webflash_path_is(ctx->header, ctx->header_len, "POST", "/api/flash"))
		return webflash_start_uploaded_flash(ctx);
	if (webflash_path_is(ctx->header, ctx->header_len, "POST",
			     "/api/factory-reset"))
		return webflash_factory_reset(ctx);
	if (webflash_path_is(ctx->header, ctx->header_len, "POST", "/api/env"))
		return webflash_prepare_body(ctx, WEBFLASH_ACTION_ENV_SAVE);
	if (webflash_path_is(ctx->header, ctx->header_len, "POST",
			     "/api/env/reset"))
		return webflash_env_reset(ctx);
	if (webflash_path_is(ctx->header, ctx->header_len, "POST", "/api/reboot")) {
		if (webflash_job_active(ctx) ||
		    ctx->job.phase == WEBFLASH_PHASE_ERROR) {
			webflash_error(ctx, "409 Conflict",
				       "reboot blocked after an incomplete flash operation");
			return -EBUSY;
		}
		if (ctx->reboot_pending)
			return webflash_ok(ctx, "device reboot is already pending");
		webflash_schedule_reboot(ctx);
		return webflash_ok(ctx, "device will reboot shortly");
	}

	webflash_error(ctx, "404 Not Found", "not found");
	return -ENOENT;
}

static int webflash_consume(struct webflash_ctx *ctx, const u8 *data, size_t len)
{
	while (len && ctx->rx_state != WEBFLASH_RX_REPLIED) {
		if (ctx->rx_state == WEBFLASH_RX_UPLOAD)
			return webflash_store_upload(ctx, data, len);
		if (ctx->rx_state == WEBFLASH_RX_BODY)
			return webflash_store_body(ctx, data, len);

		if (ctx->header_len + 1 >= sizeof(ctx->header)) {
			webflash_error(ctx, "431 Request Header Fields Too Large",
				       "HTTP header too large");
			return -E2BIG;
		}
		ctx->header[ctx->header_len++] = *data++;
		ctx->header[ctx->header_len] = '\0';
		len--;
		if (ctx->header_len >= 4 &&
		    !memcmp(ctx->header + ctx->header_len - 4, "\r\n\r\n", 4)) {
			if (webflash_handle_headers(ctx))
				return -EINVAL;
		}
	}

	return 0;
}

static err_t webflash_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
			   err_t err)
{
	struct webflash_ctx *ctx = arg;
	struct pbuf *q;
	err_t close_err;

	if (!p) {
		close_err = webflash_try_close_client(ctx);
		return close_err == ERR_ABRT ? ERR_ABRT : ERR_OK;
	}
	if (err != ERR_OK) {
		pbuf_free(p);
		webflash_abort_client(ctx);
		return ERR_ABRT;
	}

	ctx->last_activity = get_timer(0);
	tcp_recved(pcb, p->tot_len);
	for (q = p; q && ctx->client; q = q->next) {
		if (webflash_consume(ctx, q->payload, q->len))
			break;
	}
	pbuf_free(p);
	return ctx->client_aborted ? ERR_ABRT : ERR_OK;
}

static err_t webflash_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	struct webflash_ctx *ctx = arg;
	err_t err;

	if (pcb != ctx->client)
		return ERR_VAL;
	ctx->last_activity = get_timer(0);
	ctx->reply_pending = len >= ctx->reply_pending ? 0 :
			     ctx->reply_pending - len;
	if (ctx->stream_active) {
		if (webflash_stream_fill(ctx)) {
			webflash_abort_client(ctx);
			return ERR_ABRT;
		}
		err = tcp_output(pcb);
		if (err == ERR_MEM)
			ctx->output_pending = true;
		else if (err != ERR_OK) {
			webflash_abort_client(ctx);
			return ERR_ABRT;
		}
	}
	if (ctx->rx_state != WEBFLASH_RX_REPLIED || ctx->reply_pending ||
	    ctx->stream_active)
		return ERR_OK;

	ctx->output_pending = false;
	ctx->close_pending = true;
	err = webflash_try_close_client(ctx);
	return err == ERR_ABRT ? ERR_ABRT : ERR_OK;
}

static void webflash_tcp_error(void *arg, err_t err)
{
	struct webflash_ctx *ctx = arg;

	if (ctx->rx_state == WEBFLASH_RX_UPLOAD)
		ctx->upload_valid = false;
	ctx->client = NULL;
	ctx->reply_pending = 0;
	ctx->stream_active = false;
	ctx->output_pending = false;
	ctx->close_pending = false;
	if (err != ERR_ABRT)
		printf("YJRQZ Boot connection error: %d\n", err);
}

static err_t webflash_poll(void *arg, struct tcp_pcb *pcb)
{
	struct webflash_ctx *ctx = arg;
	err_t err;

	if (ctx->stream_active && webflash_stream_fill(ctx)) {
		webflash_abort_client(ctx);
		return ERR_ABRT;
	}
	if (ctx->output_pending || ctx->stream_active) {
		err = tcp_output(pcb);
		if (err == ERR_OK)
			ctx->output_pending = false;
		else if (err != ERR_MEM) {
			webflash_abort_client(ctx);
			return ERR_ABRT;
		}
	}
	if (ctx->close_pending && !ctx->reply_pending) {
		err = webflash_try_close_client(ctx);
		if (err == ERR_OK)
			return ERR_OK;
		if (err == ERR_ABRT)
			return ERR_ABRT;
	}
	if (get_timer(ctx->last_activity) > WEBFLASH_IDLE_TIMEOUT_MS) {
		puts("YJRQZ Boot connection timed out\n");
		webflash_abort_client(ctx);
		return ERR_ABRT;
	}
	return ERR_OK;
}

static err_t webflash_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct webflash_ctx *ctx = arg;

	if (err != ERR_OK || !pcb)
		return ERR_VAL;
	if (ctx->client) {
		tcp_abort(pcb);
		return ERR_ABRT;
	}

	ctx->client = pcb;
	ctx->header_len = 0;
	ctx->body_len = 0;
	ctx->upload_size = 0;
	ctx->content_length = 0;
	ctx->last_activity = get_timer(0);
	ctx->rx_state = WEBFLASH_RX_HEADERS;
	ctx->action = WEBFLASH_ACTION_NONE;
	ctx->client_aborted = false;
	ctx->reply_pending = 0;
	ctx->stream_remaining = 0;
	ctx->stream_memory = NULL;
	ctx->stream_active = false;
	ctx->stream_from_flash = false;
	ctx->output_pending = false;
	ctx->close_pending = false;
	ctx->header[0] = '\0';

	tcp_setprio(pcb, TCP_PRIO_MIN);
	tcp_nagle_disable(pcb);
	tcp_arg(pcb, ctx);
	tcp_recv(pcb, webflash_recv);
	tcp_sent(pcb, webflash_sent);
	tcp_err(pcb, webflash_tcp_error);
	tcp_poll(pcb, webflash_poll, WEBFLASH_TCP_POLL_INTERVAL);
	return ERR_OK;
}

static int webflash_start_listener(struct webflash_ctx *ctx)
{
	struct tcp_pcb *pcb;
	err_t err;

	pcb = tcp_new();
	if (!pcb)
		return -ENOMEM;
	err = tcp_bind(pcb, IP_ADDR_ANY, WEBFLASH_HTTP_PORT);
	if (err != ERR_OK) {
		tcp_abort(pcb);
		return -EADDRINUSE;
	}
	ctx->listener = tcp_listen_with_backlog(pcb, 1);
	if (!ctx->listener) {
		tcp_abort(pcb);
		return -ENOMEM;
	}
	tcp_arg(ctx->listener, ctx);
	tcp_accept(ctx->listener, webflash_accept);
	return 0;
}

static int webflash_apply_default_network(void)
{
	const char *value;

	value = env_get("ipaddr");
	if (!value || !*value) {
		if (!*CONFIG_WEBFLASH_DEFAULT_IPADDR ||
		    env_set("ipaddr", CONFIG_WEBFLASH_DEFAULT_IPADDR))
			return -EINVAL;
	}
	value = env_get("netmask");
	if (!value || !*value) {
		if (!*CONFIG_WEBFLASH_DEFAULT_NETMASK ||
		    env_set("netmask", CONFIG_WEBFLASH_DEFAULT_NETMASK))
			return -EINVAL;
	}
	return 0;
}

static int webflash_init_flash(struct webflash_ctx *ctx)
{
	struct udevice *dev;
	u64 required = (u64)CONFIG_WEBFLASH_FIRMWARE_OFFSET +
		       CONFIG_WEBFLASH_FIRMWARE_SIZE;
	u32 erase_size;
	int ret;

	ret = spi_flash_probe_bus_cs(CONFIG_SF_DEFAULT_BUS, CONFIG_SF_DEFAULT_CS,
				     &dev);
	if (ret)
		return ret;
	ctx->flash = dev_get_uclass_priv(dev);
	if (!ctx->flash || !ctx->flash->sector_size ||
	    required > ctx->flash->size)
		return -ENOSPC;
	erase_size = ctx->flash->sector_size;
	if (CONFIG_WEBFLASH_UBOOT_OFFSET + CONFIG_WEBFLASH_UBOOT_SIZE >
		CONFIG_WEBFLASH_ENV_OFFSET ||
	    CONFIG_WEBFLASH_ENV_OFFSET + CONFIG_WEBFLASH_ENV_SIZE >
		CONFIG_WEBFLASH_FACTORY_OFFSET ||
	    CONFIG_WEBFLASH_FACTORY_OFFSET + CONFIG_WEBFLASH_FACTORY_SIZE >
		CONFIG_WEBFLASH_FIRMWARE_OFFSET ||
	    CONFIG_WEBFLASH_ENV_SIZE < CONFIG_ENV_SIZE ||
	    CONFIG_WEBFLASH_UBOOT_OFFSET % erase_size ||
	    CONFIG_WEBFLASH_UBOOT_SIZE % erase_size ||
	    CONFIG_WEBFLASH_ENV_OFFSET % erase_size ||
	    CONFIG_WEBFLASH_ENV_SIZE % erase_size ||
	    CONFIG_WEBFLASH_FACTORY_OFFSET % erase_size ||
	    CONFIG_WEBFLASH_FACTORY_SIZE % erase_size ||
	    CONFIG_WEBFLASH_FIRMWARE_OFFSET % erase_size ||
	    CONFIG_WEBFLASH_FIRMWARE_SIZE % erase_size)
		return -EINVAL;

	ctx->io_buf = memalign(ARCH_DMA_MINALIGN, WEBFLASH_IO_SIZE);
	return ctx->io_buf ? 0 : -ENOMEM;
}

static int do_webflash(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	struct webflash_ctx ctx = {};
	struct udevice *udev;
	struct netif *netif;
	const char *ip;
	char *end;
	int ret;

	if (argc > 2)
		return CMD_RET_USAGE;
	ctx.load_addr = env_get_hex("loadaddr", CONFIG_SYS_LOAD_ADDR);
	if (argc == 2) {
		ctx.load_addr = hextoul(argv[1], &end);
		if (*end)
			return CMD_RET_USAGE;
	}
	ret = webflash_apply_default_network();
	if (ret)
		return CMD_RET_FAILURE;
	ret = webflash_init_flash(&ctx);
	if (ret) {
		printf("YJRQZ Boot SPI flash initialization failed: %d\n", ret);
		return CMD_RET_FAILURE;
	}
	ip = env_get("ipaddr");
	ret = net_lwip_eth_start();
	if (ret)
		goto out_free;
	udev = eth_get_dev();
	netif = net_lwip_new_netif(udev);
	if (!netif) {
		ret = -ENODEV;
		goto out_eth;
	}
	ret = webflash_start_listener(&ctx);
	if (ret)
		goto out_netif;

	printf("%s listening on http://%s/\n", CONFIG_WEBFLASH_PRODUCT_NAME, ip);
	printf("SPI NOR: %u bytes, erase size: %u bytes\n",
	       ctx.flash->size, ctx.flash->sector_size);
	puts("Normal recovery protects U-Boot, environment and factory data.\n");
	puts("Press Ctrl-C to stop.\n");

	while (!ctrlc()) {
		net_lwip_rx(udev, netif);
		if (webflash_job_active(&ctx))
			webflash_job_step(&ctx);
		if (ctx.reboot_pending &&
		    get_timer(ctx.reboot_start) >= WEBFLASH_REBOOT_DELAY_MS) {
			puts("YJRQZ Boot rebooting\n");
			reset_cpu();
		}
	}

	puts("\nStopping YJRQZ Boot\n");
	webflash_abort_client(&ctx);
	if (ctx.listener) {
		tcp_arg(ctx.listener, NULL);
		tcp_accept(ctx.listener, NULL);
		if (tcp_close(ctx.listener) != ERR_OK)
			tcp_abort(ctx.listener);
	}
out_netif:
	net_lwip_remove_netif(netif);
out_eth:
	net_lwip_eth_stop();
out_free:
	free(ctx.io_buf);
	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(webflash, 2, 0, do_webflash,
	   "start the YJRQZ Boot Web recovery console",
	   "[loadAddress]");
