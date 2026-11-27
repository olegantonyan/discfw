################################################################################
#
# libdisc
#
################################################################################

LIBDISC_VERSION = 0.1
LIBDISC_SITE = $(BR2_EXTERNAL_DISC_PATH)/src/common
LIBDISC_SITE_METHOD = local
LIBDISC_LICENSE = GPL-3.0+
LIBDISC_INSTALL_STAGING = YES
LIBDISC_INSTALL_TARGET = NO

define LIBDISC_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) clean
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define LIBDISC_INSTALL_STAGING_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) DESTDIR=$(STAGING_DIR) install-staging
endef

$(eval $(generic-package))
