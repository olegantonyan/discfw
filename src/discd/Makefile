CFLAGS ?= -O2
override CFLAGS += -Wall -Wextra -std=gnu11 -D_GNU_SOURCE $(EXTRA_CFLAGS)
override LDFLAGS += $(EXTRA_LDFLAGS)
DESTDIR ?=

all: discd discctl discreset discuac libdisc_hook.so libasound_module_pcm_disc_eq.so

discd: discd.c power.c power.h jack.c jack.h cs43131_ioctl.h
	$(CC) $(CFLAGS) -o $@ discd.c power.c jack.c $(LDFLAGS) -ldisc

discctl: discctl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -ldisc

discuac: discuac.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -ldisc -lasound -lpthread

discreset: discreset.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

libdisc_hook.so: disc_hook.c
	$(CC) $(CFLAGS) -fPIC -DPIC -shared -o $@ $< $(LDFLAGS) -ldisc -lasound

libasound_module_pcm_disc_eq.so: disc_eq.c
	$(CC) $(CFLAGS) -fPIC -DPIC -shared -o $@ $< $(LDFLAGS) -ldisc -lasound -lm

install: all
	install -D -m 0755 discd $(DESTDIR)/usr/sbin/discd
	install -D -m 0755 discctl $(DESTDIR)/usr/bin/discctl
	install -D -m 0755 discreset $(DESTDIR)/usr/sbin/discreset
	install -D -m 0755 discuac $(DESTDIR)/usr/bin/discuac
	install -D -m 0755 libdisc_hook.so $(DESTDIR)/usr/lib/alsa-lib/libdisc_hook.so
	install -D -m 0755 libasound_module_pcm_disc_eq.so $(DESTDIR)/usr/lib/alsa-lib/libasound_module_pcm_disc_eq.so

clean:
	rm -f discd discctl discreset discuac libdisc_hook.so libasound_module_pcm_disc_eq.so
