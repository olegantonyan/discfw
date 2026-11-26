PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
override CFLAGS += -Wall -Wextra -std=gnu11 -D_GNU_SOURCE $(EXTRA_CFLAGS)
override LDFLAGS += $(EXTRA_LDFLAGS)
DESTDIR ?=
SRCS := $(wildcard *.c)
HDRS := $(wildcard *.h)
LIBS := libmpdclient libpng libjpeg zlib

all: discui

discui: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags lvgl $(LIBS)) -o $@ $(SRCS) $(LDFLAGS) $(shell $(PKG_CONFIG) --libs lvgl $(LIBS)) -ldisc -lm -lpthread

HOST_LVGL ?=
LV_CONF_DIR ?=

host: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -I$(HOST_LVGL)/include -I$(LV_CONF_DIR) -DLV_KCONFIG_IGNORE -DLV_CONF_INCLUDE_SIMPLE \
		$(shell $(PKG_CONFIG) --cflags $(LIBS)) -o discui $(SRCS) $(LDFLAGS) $(HOST_LVGL)/lib/liblvgl.a \
		$(shell $(PKG_CONFIG) --libs $(LIBS)) -ldisc -lm -lpthread

install: discui
	install -D -m 0755 discui $(DESTDIR)/usr/bin/discui

clean:
	rm -f discui

.PHONY: all host install clean
