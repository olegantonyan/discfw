PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
override CFLAGS += -Wall -Wextra -std=gnu11 -D_GNU_SOURCE $(EXTRA_CFLAGS)
override LDFLAGS += $(EXTRA_LDFLAGS)
PKGS := glib-2.0 gio-2.0 gio-unix-2.0 libmpdclient
DESTDIR ?=
SRCS := $(wildcard *.c)

all: discsys discfw

discsys: $(SRCS) discsys.h
	$(CC) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags $(PKGS)) -o $@ $(SRCS) $(LDFLAGS) $(shell $(PKG_CONFIG) --libs $(PKGS)) -ldisc

discfw: cli/discfw.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -ldisc

install: discsys discfw
	install -D -m 0755 discsys $(DESTDIR)/usr/sbin/discsys
	install -D -m 0755 discfw $(DESTDIR)/usr/bin/discfw

clean:
	rm -f discsys discfw

.PHONY: all install clean
