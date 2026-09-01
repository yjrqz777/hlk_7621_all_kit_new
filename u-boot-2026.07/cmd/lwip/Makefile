obj-$(CONFIG_CMD_DHCP) += dhcp.o
obj-$(CONFIG_CMD_DNS) += dns.o
obj-$(CONFIG_CMD_NFS) += nfs.o
obj-$(CONFIG_CMD_PING) += ping.o
obj-$(CONFIG_CMD_SNTP) += sntp.o
obj-$(CONFIG_CMD_TFTPBOOT) += tftp.o
obj-$(CONFIG_CMD_WEBFLASH) += webflash.o
obj-$(CONFIG_CMD_WGET) += wget.o

webflash-src := $(srctree)/$(src)/webflash
webflash-yui-local := $(wildcard $(webflash-src)/vendors/yuicompressor-*.jar)
webflash-yui := $(strip $(if $(YUI_COMPRESSOR),$(YUI_COMPRESSOR),$(firstword $(webflash-yui-local))))
webflash-assets := $(obj)/webflash_assets.h

targets += webflash_assets.h
CFLAGS_webflash.o += -I$(obj)

quiet_cmd_webflash_assets = WEBASSET $@
      cmd_webflash_assets = $(CONFIG_SHELL) $(webflash-src)/makefsdata \
	$(webflash-src)/vendors/hlk-7621a/index.html \
	$(webflash-src)/vendors/hlk-7621a/style.css \
	"$(webflash-yui)" > $@

$(webflash-assets): $(webflash-src)/vendors/hlk-7621a/index.html \
			$(webflash-src)/vendors/hlk-7621a/style.css \
			$(webflash-src)/makefsdata $(webflash-yui) FORCE
	$(call if_changed,webflash_assets)

$(obj)/webflash.o: $(webflash-assets)
