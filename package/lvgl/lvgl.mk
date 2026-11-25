################################################################################
#
# lvgl
#
################################################################################

LVGL_VERSION = 9.6.0
LVGL_SITE = $(call github,lvgl,lvgl,v$(LVGL_VERSION))
LVGL_LICENSE = MIT
LVGL_LICENSE_FILES = LICENCE.txt
LVGL_INSTALL_STAGING = YES
LVGL_INSTALL_TARGET = NO
LVGL_CONF_OPTS = \
	-DLV_BUILD_CONF_PATH=$(LVGL_PKGDIR)/lv_conf.h \
	-DLV_BUILD_INSTALL=ON \
	-DLV_FETCH_DEPENDENCIES=OFF \
	-DCONFIG_LV_BUILD_EXAMPLES=OFF \
	-DCONFIG_LV_BUILD_DEMOS=OFF \
	-DCONFIG_LV_USE_THORVG=OFF \
	-DCONFIG_LV_USE_THORVG_INTERNAL=OFF \
	-DBUILD_SHARED_LIBS=OFF

$(eval $(cmake-package))
