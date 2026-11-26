PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
override CFLAGS += -Wall -Wextra -std=gnu11 -D_GNU_SOURCE $(shell $(PKG_CONFIG) --cflags lvgl libmpdclient)
override LDLIBS += $(shell $(PKG_CONFIG) --libs lvgl libmpdclient) -ldiscd -lm -lpthread
DESTDIR ?=

discui: main.c battery.c hw.c player.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

install: discui
	install -D -m 0755 discui $(DESTDIR)/usr/bin/discui

clean:
	rm -f discui
