################################################################################
#
# discsys
#
################################################################################

DISCSYS_VERSION = 0.1
DISCSYS_SITE = $(BR2_EXTERNAL_DISC_PATH)/src/discsys
DISCSYS_SITE_METHOD = local
DISCSYS_LICENSE = GPL-3.0+
DISCSYS_DEPENDENCIES = host-pkgconf libdisc libglib2 libmpdclient

define DISCSYS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) clean
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) PKG_CONFIG=$(PKG_CONFIG_HOST_BINARY) -C $(@D)
endef

define DISCSYS_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) DESTDIR=$(TARGET_DIR) install
	$(INSTALL) -D -m 0644 $(@D)/zones.tsv $(TARGET_DIR)/usr/share/discfw/zones.tsv
endef

$(eval $(generic-package))
