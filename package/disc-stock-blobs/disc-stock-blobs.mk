################################################################################
#
# disc-stock-blobs
#
################################################################################

DISC_STOCK_BLOBS_VERSION = v257
DISC_STOCK_BLOBS_OTA = $(call qstrip,$(BR2_PACKAGE_DISC_STOCK_BLOBS_OTA))
DISC_STOCK_BLOBS_SITE = $(patsubst %/,%,$(dir $(DISC_STOCK_BLOBS_OTA)))
DISC_STOCK_BLOBS_SOURCE = $(notdir $(DISC_STOCK_BLOBS_OTA))
DISC_STOCK_BLOBS_SITE_METHOD = file
DISC_STOCK_BLOBS_LICENSE = GPL-2.0 (kernel, modules), proprietary (radio firmware)
DISC_STOCK_BLOBS_REDISTRIBUTE = NO
DISC_STOCK_BLOBS_DEPENDENCIES = host-libopenssl host-squashfs
DISC_STOCK_BLOBS_INSTALL_IMAGES = YES

DISC_STOCK_BLOBS_XIMAGE_SHA256 = 1a9484efcb64c391b23910306e867cb628af07e059d0d1ccc13e85269e030ed2
DISC_STOCK_BLOBS_ROOTFS_SHA256 = 111e4dd7ee3d7ff91ba7e61181690be7ffd22bd1bbd13ad513f5f015ffb302ae
DISC_STOCK_BLOBS_DEC = $(HOST_DIR)/bin/openssl enc -d -aes-256-cbc -pbkdf2 -k fo123
DISC_STOCK_BLOBS_MODDIR = $(TARGET_DIR)/lib/modules/4.4.94/fiio

define DISC_STOCK_BLOBS_EXTRACT_CMDS
	$(UNZIP) -d $(@D)/ota $(DISC_STOCK_BLOBS_DL_DIR)/$(DISC_STOCK_BLOBS_SOURCE)
endef

define DISC_STOCK_BLOBS_BUILD_CMDS
	cd $(@D)/ota/main_os/ota_v* && \
		$(DISC_STOCK_BLOBS_DEC) -in ota_update.in.enc -out $(@D)/manifest.txt && \
		for f in $$(ls rootfs.squashfs.[0-9]*.enc | sort -t. -k3,3n); do $(DISC_STOCK_BLOBS_DEC) -in $$f || exit 1; done > $(@D)/rootfs.squashfs && \
		for f in $$(ls xImage.[0-9]*.enc | sort -t. -k2,2n); do $(DISC_STOCK_BLOBS_DEC) -in $$f || exit 1; done > $(@D)/xImage
	echo "$(DISC_STOCK_BLOBS_XIMAGE_SHA256)  $(@D)/xImage" | sha256sum -c -
	echo "$(DISC_STOCK_BLOBS_ROOTFS_SHA256)  $(@D)/rootfs.squashfs" | sha256sum -c -
	$(HOST_DIR)/bin/unsquashfs -n -f -d $(@D)/rootfs $(@D)/rootfs.squashfs module_driver lib/firmware
endef

define DISC_STOCK_BLOBS_INSTALL_TARGET_CMDS
	mkdir -p $(DISC_STOCK_BLOBS_MODDIR) $(TARGET_DIR)/lib/firmware
	cp $(@D)/rootfs/module_driver/*.ko $(@D)/rootfs/module_driver/*.sh $(DISC_STOCK_BLOBS_MODDIR)/
	rm -f $(DISC_STOCK_BLOBS_MODDIR)/driver_default_init_script.sh
	cp -r $(@D)/rootfs/lib/firmware/wifi_bcm $(@D)/rootfs/lib/firmware/bt_bcm $(TARGET_DIR)/lib/firmware/
endef

define DISC_STOCK_BLOBS_INSTALL_IMAGES_CMDS
	$(INSTALL) -D -m 0644 $(@D)/xImage $(BINARIES_DIR)/xImage-stock-v257
endef

$(eval $(generic-package))
