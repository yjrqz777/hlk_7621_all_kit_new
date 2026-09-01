// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal lwIP HTTP recovery server.
 *
 * The RAM-upload stage deliberately contains no flash erase/write path.
 */

#include <command.h>
#include <console.h>
#include <env.h>
#include <lmb.h>
#include <mapmem.h>
#include <net.h>
#include <version_string.h>
#include <asm/global_data.h>
#include <linux/kernel.h>
#include <lwip/tcp.h>

#include "webflash_assets.h"

DECLARE_GLOBAL_DATA_PTR;

#define WEBFLASH_HTTP_PORT	80
#define WEBFLASH_HEADER_SIZE	1024
#define WEBFLASH_REPLY_SIZE	512
#define WEBFLASH_IDLE_TIMEOUT_MS	30000
#define WEBFLASH_TCP_POLL_INTERVAL 4

enum webflash_rx_state {
	WEBFLASH_RX_HEADERS,
	WEBFLASH_RX_UPLOAD,
	WEBFLASH_RX_REPLIED,
};

struct webflash_ctx {
	struct tcp_pcb *listener;
	struct tcp_pcb *client;
	ulong load_addr;
	ulong upload_size;
	ulong content_length;
	ulong last_activity;
	ulong completed_addr;
	ulong completed_size;
	size_t header_len;
	enum webflash_rx_state rx_state;
	bool client_aborted;
	char header[WEBFLASH_HEADER_SIZE];
	char reply[WEBFLASH_REPLY_SIZE];
};

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

static void webflash_close_client(struct webflash_ctx *ctx, bool abort)
{
	struct tcp_pcb *pcb = ctx->client;
	err_t err = ERR_OK;

	if (!pcb)
		return;

	webflash_detach_client(ctx);
	if (!abort)
		err = tcp_close(pcb);
	if (abort || err != ERR_OK) {
		ctx->client_aborted = true;
		tcp_abort(pcb);
	}
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
	if (header_len < 0 || header_len >= (int)sizeof(header) ||
	    body_len > U16_MAX) {
		webflash_close_client(ctx, true);
		return -EOVERFLOW;
	}

	err = tcp_write(pcb, header, (u16_t)header_len, TCP_WRITE_FLAG_COPY);
	if (err == ERR_OK)
		err = tcp_write(pcb, body, (u16_t)body_len, TCP_WRITE_FLAG_COPY);
	if (err == ERR_OK)
		err = tcp_output(pcb);
	if (err != ERR_OK) {
		webflash_close_client(ctx, true);
		return -EIO;
	}

	ctx->rx_state = WEBFLASH_RX_REPLIED;
	webflash_close_client(ctx, false);
	return 0;
}

static void webflash_error(struct webflash_ctx *ctx, const char *status,
			   const char *message)
{
	snprintf(ctx->reply, sizeof(ctx->reply),
		 "{\"ok\":false,\"error\":\"%s\"}", message);
	webflash_reply(ctx, status, "application/json", ctx->reply);
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

static int webflash_prepare_upload(struct webflash_ctx *ctx)
{
	const char *length;
	char *end;
	ulong size;
	ulong ram_end;

	length = strstr(ctx->header, "\r\nContent-Length:");
	if (!length) {
		webflash_error(ctx, "411 Length Required", "Content-Length required");
		return -EINVAL;
	}

	length += strlen("\r\nContent-Length:");
	while (*length == ' ' || *length == '\t')
		length++;
	size = simple_strtoul(length, &end, 10);
	if (end == length || (*end != '\r' && *end != '\n')) {
		webflash_error(ctx, "400 Bad Request", "invalid Content-Length");
		return -EINVAL;
	}
	if (!size || size > CONFIG_WEBFLASH_FIRMWARE_SIZE) {
		webflash_error(ctx, "413 Content Too Large", "firmware size rejected");
		return -EFBIG;
	}

	if (ctx->load_addr + size < ctx->load_addr) {
		webflash_error(ctx, "413 Content Too Large", "RAM address overflow");
		return -EOVERFLOW;
	}
	ram_end = gd->ram_base + gd->ram_size;
	if (ram_end < gd->ram_base || ctx->load_addr < gd->ram_base ||
	    ctx->load_addr + size > ram_end) {
		webflash_error(ctx, "413 Content Too Large", "upload is outside RAM");
		return -ERANGE;
	}
	if (CONFIG_IS_ENABLED(LMB) && lmb_read_check(ctx->load_addr, size)) {
		webflash_error(ctx, "409 Conflict", "upload overlaps reserved memory");
		return -EBUSY;
	}

	ctx->content_length = size;
	ctx->upload_size = 0;
	ctx->rx_state = WEBFLASH_RX_UPLOAD;
	printf("Web upload: %lu bytes to 0x%08lx\n", size, ctx->load_addr);
	return 0;
}

static int webflash_handle_headers(struct webflash_ctx *ctx)
{
	const char *ip = env_get("ipaddr");

	if (webflash_path_is(ctx->header, ctx->header_len, "GET", "/") ||
	    webflash_path_is(ctx->header, ctx->header_len,
			     "GET", "/index.html"))
		return webflash_reply(ctx, "200 OK", "text/html; charset=utf-8",
				      webflash_index_html);

	if (webflash_path_is(ctx->header, ctx->header_len,
			     "GET", "/style.css"))
		return webflash_reply(ctx, "200 OK", "text/css; charset=utf-8",
				      webflash_style_css);

	if (webflash_path_is(ctx->header, ctx->header_len, "GET", "/api/info")) {
		snprintf(ctx->reply, sizeof(ctx->reply),
			 "{\"version\":\"%s\",\"device\":\"%s\","
			 "\"ip\":\"%s\",\"load_address\":\"0x%08lx\","
			 "\"max_upload\":%lu,\"firmware_offset\":\"0x%08x\","
			 "\"uploaded\":%lu,\"flash_write\":false}",
			 version_string, CONFIG_WEBFLASH_DEVICE_ID, ip ?: "0.0.0.0",
			 ctx->load_addr, (ulong)CONFIG_WEBFLASH_FIRMWARE_SIZE,
			 CONFIG_WEBFLASH_FIRMWARE_OFFSET, ctx->completed_size);
		return webflash_reply(ctx, "200 OK", "application/json",
				      ctx->reply);
	}

	if (webflash_path_is(ctx->header, ctx->header_len,
			     "POST", "/api/upload"))
		return webflash_prepare_upload(ctx);

	webflash_error(ctx, "404 Not Found", "not found");
	return -ENOENT;
}

static int webflash_store(struct webflash_ctx *ctx, const void *data, size_t len)
{
	void *ptr;
	ulong remaining = ctx->content_length - ctx->upload_size;

	if (len > remaining) {
		webflash_error(ctx, "400 Bad Request", "body exceeds Content-Length");
		return -EFBIG;
	}

	ptr = map_sysmem(ctx->load_addr + ctx->upload_size, len);
	memcpy(ptr, data, len);
	unmap_sysmem(ptr);
	ctx->upload_size += len;

	if (ctx->upload_size == ctx->content_length) {
		ctx->completed_addr = ctx->load_addr;
		ctx->completed_size = ctx->upload_size;
		if (env_set_hex("fileaddr", ctx->completed_addr) ||
		    env_set_hex("filesize", ctx->completed_size)) {
			webflash_error(ctx, "500 Internal Server Error",
				       "failed to update environment");
			return -EIO;
		}

		printf("Web upload complete: %lu bytes at 0x%08lx (Flash unchanged)\n",
		       ctx->completed_size, ctx->completed_addr);
		snprintf(ctx->reply, sizeof(ctx->reply),
			 "{\"ok\":true,\"address\":\"0x%08lx\","
			 "\"size\":%lu,\"flash_write\":false}",
			 ctx->completed_addr, ctx->completed_size);
		webflash_reply(ctx, "200 OK", "application/json", ctx->reply);
	}

	return 0;
}

static int webflash_consume(struct webflash_ctx *ctx, const u8 *data, size_t len)
{
	while (len && ctx->rx_state != WEBFLASH_RX_REPLIED) {
		if (ctx->rx_state == WEBFLASH_RX_UPLOAD)
			return webflash_store(ctx, data, len);

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

	if (!p) {
		webflash_close_client(ctx, false);
		return ERR_OK;
	}
	if (err != ERR_OK) {
		pbuf_free(p);
		webflash_close_client(ctx, true);
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

static void webflash_tcp_error(void *arg, err_t err)
{
	struct webflash_ctx *ctx = arg;

	ctx->client = NULL;
	if (err != ERR_ABRT)
		printf("Web connection error: %d\n", err);
}

static err_t webflash_poll(void *arg, struct tcp_pcb *pcb)
{
	struct webflash_ctx *ctx = arg;

	if (get_timer(ctx->last_activity) > WEBFLASH_IDLE_TIMEOUT_MS) {
		puts("Web connection timed out\n");
		webflash_close_client(ctx, true);
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
	ctx->upload_size = 0;
	ctx->content_length = 0;
	ctx->last_activity = get_timer(0);
	ctx->rx_state = WEBFLASH_RX_HEADERS;
	ctx->client_aborted = false;
	ctx->header[0] = '\0';

	tcp_setprio(pcb, TCP_PRIO_MIN);
	tcp_nagle_disable(pcb);
	tcp_arg(pcb, ctx);
	tcp_recv(pcb, webflash_recv);
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
		    env_set("ipaddr", CONFIG_WEBFLASH_DEFAULT_IPADDR)) {
			puts("Unable to set the default Web recovery IP address\n");
			return -EINVAL;
		}
	}

	value = env_get("netmask");
	if (!value || !*value) {
		if (!*CONFIG_WEBFLASH_DEFAULT_NETMASK ||
		    env_set("netmask", CONFIG_WEBFLASH_DEFAULT_NETMASK)) {
			puts("Unable to set the default Web recovery network mask\n");
			return -EINVAL;
		}
	}

	return 0;
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
	ip = env_get("ipaddr");

	ret = net_lwip_eth_start();
	if (ret)
		return CMD_RET_FAILURE;
	udev = eth_get_dev();
	netif = net_lwip_new_netif(udev);
	if (!netif) {
		net_lwip_eth_stop();
		return CMD_RET_FAILURE;
	}

	ret = webflash_start_listener(&ctx);
	if (ret) {
		printf("Cannot start Web server: %d\n", ret);
		net_lwip_remove_netif(netif);
		net_lwip_eth_stop();
		return CMD_RET_FAILURE;
	}

	printf("Web recovery listening on http://%s/\n", ip);
	printf("Upload address: 0x%08lx, maximum size: 0x%x\n",
	       ctx.load_addr, CONFIG_WEBFLASH_FIRMWARE_SIZE);
	puts("RAM upload only; Flash writes are disabled. Press Ctrl-C to stop.\n");

	while (!ctrlc())
		net_lwip_rx(udev, netif);

	puts("\nStopping Web recovery\n");
	webflash_close_client(&ctx, true);
	if (ctx.listener) {
		tcp_arg(ctx.listener, NULL);
		tcp_accept(ctx.listener, NULL);
		if (tcp_close(ctx.listener) != ERR_OK)
			tcp_abort(ctx.listener);
	}
	net_lwip_remove_netif(netif);
	net_lwip_eth_stop();
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(webflash, 2, 0, do_webflash,
	   "start the lwIP Web recovery server (RAM upload only)",
	   "[loadAddress]");
