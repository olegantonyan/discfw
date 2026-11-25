BR_VERSION := 2026.02.3
BR_SHA256 := 5a59e7501b0b4ec52c41f4bfa79412320e0b37eae5f719605a258e8d0c6fc7fb
BR_TARBALL := dl/buildroot-$(BR_VERSION).tar.xz

TOP := $(CURDIR)
BR := $(TOP)/buildroot
OUT := $(TOP)/output
DEFCONFIG := disc_defconfig
BRMAKE := $(MAKE) -C $(BR) O=$(OUT) BR2_EXTERNAL=$(TOP)
PATCHES := $(sort $(wildcard $(TOP)/patches/buildroot/*.patch))

.PHONY: all defconfig menuconfig savedefconfig clean distclean
all: $(OUT)/.config
	$(BRMAKE)

$(BR_TARBALL):
	mkdir -p dl
	wget -O $@.tmp https://buildroot.org/downloads/buildroot-$(BR_VERSION).tar.xz
	mv $@.tmp $@

$(BR)/.disc-patched: $(BR_TARBALL) $(PATCHES)
	echo "$(BR_SHA256)  $(BR_TARBALL)" | sha256sum -c -
	rm -rf $(BR)
	mkdir -p $(BR)
	tar -xf $(BR_TARBALL) -C $(BR) --strip-components=1
	for p in $(PATCHES); do patch -d $(BR) -p1 < $$p || exit 1; done
	touch $@

$(OUT)/.config: $(BR)/.disc-patched configs/$(DEFCONFIG)
	$(BRMAKE) $(DEFCONFIG)

defconfig: $(BR)/.disc-patched
	$(BRMAKE) $(DEFCONFIG)

menuconfig savedefconfig clean: $(BR)/.disc-patched
	$(BRMAKE) $@

distclean:
	rm -rf $(OUT) $(BR)

br-%: $(OUT)/.config
	$(BRMAKE) $*
