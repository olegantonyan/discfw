#include "discsys.h"

#include <disc/paths.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define LUN_FILE "/sys/kernel/config/usb_gadget/disc/functions/mass_storage.0/lun.0/file"
#define UNPLUG_S 3

static bool usb_seen, exported;
static guint unplug_src;

const char *mode_name(void)
{
	return sys.mode == MODE_STORAGE ? "storage" : sys.mode == MODE_USBDAC ? "usbdac" : "player";
}

bool mode_usb(void)
{
	return sys.bat.vbus[0] && strcmp(sys.bat.vbus, "none") && strcmp(sys.bat.vbus, "otg");
}

static void announce(void)
{
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "!mode");
	kv_str(&b, "name", mode_name());
	kv_int(&b, "usb", mode_usb());
	kv_int(&b, "always", sys.mode_always);
	ev_kv(&b);
}

static void leave(const char *why)
{
	logmsg(LOG_INFO, "leaving %s mode: %s", mode_name(), why);
	power_off(true);
}

static gboolean unplug_check(gpointer data)
{
	(void)data;
	unplug_src = 0;
	if (!mode_usb())
		leave("USB unplugged");
	return G_SOURCE_REMOVE;
}

void mode_on_bat(void)
{
	static int last = -1;
	int usb = mode_usb();

	if (usb != last) {
		last = usb;
		announce();
	}
	if (sys.mode != MODE_STORAGE || sys.mode_always)
		return;
	if (usb)
		usb_seen = true;
	else if (usb_seen && !unplug_src)
		unplug_src = g_timeout_add_seconds(UNPLUG_S, unplug_check, NULL);
}

static gboolean eject_poll(gpointer data)
{
	char buf[64] = "";
	FILE *f = fopen(LUN_FILE, "r");

	(void)data;
	if (!f)
		return G_SOURCE_CONTINUE;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = 0;
	fclose(f);
	if (buf[0] && buf[0] != '\n') {
		exported = true;
	} else if (exported) {
		leave("ejected by the computer");
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static int write_mode_file(const char *file, const char *name)
{
	char tmp[256];
	const char *path = disc_data_path(file);
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (!(f = fopen(tmp, "w")))
		return -1;
	fprintf(f, "%s\n", name);
	if (fflush(f) || fsync(fileno(f)) || fclose(f) || rename(tmp, path))
		return -1;
	return 0;
}

static int remove_mode_file(const char *file)
{
	return unlink(disc_data_path(file)) && errno != ENOENT ? -1 : 0;
}

static int save_mode(const char *name, bool always)
{
	if (!strcmp(name, "player"))
		return remove_mode_file("mode.next") || remove_mode_file("mode.default");
	if (always)
		return write_mode_file("mode.default", name) || remove_mode_file("mode.next");
	return write_mode_file("mode.next", name) || remove_mode_file("mode.default");
}

int mode_request(const char *name, const char *when, char *err, size_t errlen)
{
	bool player = !strcmp(name, "player"), always = !strcmp(when, "always");

	if ((!player && strcmp(name, "storage") && strcmp(name, "usbdac")) || (when[0] && !always && strcmp(when, "once"))) {
		snprintf(err, errlen, "usage: mode player|storage|usbdac [once|always]");
		return -1;
	}
	if (!player && !strcmp(name, mode_name())) {
		snprintf(err, errlen, "already in %s mode", name);
		return -1;
	}
	if (!strcmp(name, "storage") && !sys.fake_hw && access("/sys/block/mmcblk0", F_OK)) {
		snprintf(err, errlen, "no music card");
		return -1;
	}
	if (save_mode(name, always)) {
		snprintf(err, errlen, "cannot save the mode: %s", strerror(errno));
		return -1;
	}
	if (player && sys.mode == MODE_PLAYER)
		return 0;
	logmsg(LOG_INFO, "restarting into %s mode%s", name, always ? " (always)" : "");
	power_off(true);
	return 0;
}

void mode_uac(int rate)
{
	struct kvbuf b;

	if (rate == sys.uac_rate)
		return;
	sys.uac_rate = rate;
	kv_init(&b);
	kv_raw(&b, "!uac");
	kv_int(&b, "rate", rate);
	ev_kv(&b);
}

void mode_init(void)
{
	const char *m = disc_boot_mode();
	FILE *f;

	sys.mode = !strcmp(m, "storage") ? MODE_STORAGE : !strcmp(m, "usbdac") ? MODE_USBDAC : MODE_PLAYER;
	if (sys.mode == MODE_PLAYER)
		return;
	if ((f = fopen(disc_data_path("mode.default"), "r"))) {
		char line[32] = "";
		if (fgets(line, sizeof(line), f))
			line[strcspn(line, "\n")] = 0;
		sys.mode_always = !strcmp(line, m);
		fclose(f);
	}
	logmsg(LOG_INFO, "%s mode%s", m, sys.mode_always ? " (always)" : "");
	volume_init();
	if (sys.mode != MODE_STORAGE)
		return;
	if ((f = fopen(disc_data_path("usb-storage.used"), "w")))
		fclose(f);
	if (!sys.fake_hw && !sys.mode_always)
		g_timeout_add_seconds(1, eject_poll, NULL);
}
