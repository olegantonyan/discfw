################################################################################
#
# discd
#
################################################################################

DISCD_VERSION = 0.1
DISCD_SITE = $(BR2_EXTERNAL_DISC_PATH)/src/discd
DISCD_SITE_METHOD = local
DISCD_LICENSE = GPL-2.0+
DISCD_DEPENDENCIES = alsa-lib
DISCD_INSTALL_STAGING = YES

define DISCD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define DISCD_INSTALL_STAGING_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) DESTDIR=$(STAGING_DIR) install-staging
endef

define DISCD_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
