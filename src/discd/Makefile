CFLAGS ?= -O2
override CFLAGS += -Wall -Wextra -std=gnu11 -D_GNU_SOURCE $(EXTRA_CFLAGS)
override LDFLAGS += $(EXTRA_LDFLAGS)
DESTDIR ?=

all: discd discctl discreset libdisc_hook.so

discd: discd.c power.c power.h cs43131_ioctl.h
	$(CC) $(CFLAGS) -o $@ discd.c power.c $(LDFLAGS) -ldisc

discctl: discctl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -ldisc

discreset: discreset.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

libdisc_hook.so: disc_hook.c
	$(CC) $(CFLAGS) -fPIC -DPIC -shared -o $@ $< $(LDFLAGS) -ldisc -lasound

install: all
	install -D -m 0755 discd $(DESTDIR)/usr/sbin/discd
	install -D -m 0755 discctl $(DESTDIR)/usr/bin/discctl
	install -D -m 0755 discreset $(DESTDIR)/usr/sbin/discreset
	install -D -m 0755 libdisc_hook.so $(DESTDIR)/usr/lib/alsa-lib/libdisc_hook.so

clean:
	rm -f discd discctl discreset libdisc_hook.so
