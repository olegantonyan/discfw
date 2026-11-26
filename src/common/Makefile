CFLAGS ?= -O2
override CFLAGS += -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -fPIC -Iinclude
DESTDIR ?=
SRCS := conn.c kv.c paths.c settings.c settings_schema.c text.c
HDRS := $(wildcard include/disc/*.h)
OBJS := $(SRCS:.c=.o)
TESTS := test/test_text test/test_kv test/test_settings

all: libdisc.a

libdisc.a: $(OBJS)
	$(AR) rcs $@ $^

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

test/%: test/%.c libdisc.a
	$(CC) $(CFLAGS) -o $@ $< libdisc.a

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

install-staging: libdisc.a
	install -D -m 0644 libdisc.a $(DESTDIR)/usr/lib/libdisc.a
	for h in $(HDRS); do install -D -m 0644 $$h $(DESTDIR)/usr/$$h; done

clean:
	rm -f $(OBJS) libdisc.a $(TESTS)

.PHONY: all test install-staging clean
