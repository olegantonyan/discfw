CFLAGS ?= -O2
override CFLAGS += -Wall -Wextra -std=gnu11 -D_GNU_SOURCE
DESTDIR ?=

all: discd discctl discreset libdisc_hook.so libdiscd.a

libdiscd.a: proto.c
	$(CC) $(CFLAGS) -fPIC -c -o proto.o proto.c
	$(AR) rcs $@ proto.o

discd: discd.c proto.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

discctl: discctl.c proto.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

discreset: discreset.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

libdisc_hook.so: disc_hook.c proto.c
	$(CC) $(CFLAGS) -fPIC -DPIC -shared -o $@ $^ $(LDFLAGS) -lasound

install: all
	install -D -m 0755 discd $(DESTDIR)/usr/sbin/discd
	install -D -m 0755 discctl $(DESTDIR)/usr/bin/discctl
	install -D -m 0755 discreset $(DESTDIR)/usr/sbin/discreset
	install -D -m 0755 libdisc_hook.so $(DESTDIR)/usr/lib/alsa-lib/libdisc_hook.so

install-staging: libdiscd.a
	install -D -m 0644 libdiscd.a $(DESTDIR)/usr/lib/libdiscd.a
	install -D -m 0644 proto.h $(DESTDIR)/usr/include/discd/proto.h

clean:
	rm -f discd discctl discreset libdisc_hook.so libdiscd.a proto.o
