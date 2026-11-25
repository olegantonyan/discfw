################################################################################
#
# discui
#
################################################################################

DISCUI_VERSION = 0.1
DISCUI_SITE = $(BR2_EXTERNAL_DISC_PATH)/src/discui
DISCUI_SITE_METHOD = local
DISCUI_LICENSE = GPL-2.0+
DISCUI_DEPENDENCIES = host-pkgconf lvgl libmpdclient discd

define DISCUI_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) PKG_CONFIG=$(PKG_CONFIG_HOST_BINARY) -C $(@D)
endef

define DISCUI_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
