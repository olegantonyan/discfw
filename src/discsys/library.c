#include "discsys.h"

#include <disc/paths.h>
#include <disc/settings.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <mpd/client.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define CID "/sys/block/mmcblk0/device/cid"

static bool mounted;
static bool mpd_seen;

bool library_mounted(void)
{
	char line[512], dev[128], dir[256];
	const char *root = disc_music_root();
	FILE *f = fopen("/proc/self/mounts", "r");
	bool found = false;

	if (sys.fake_hw)
		return access(root, R_OK) == 0;
	while (f && !found && fgets(line, sizeof(line), f))
		if (sscanf(line, "%127s %255s", dev, dir) == 2 && !strcmp(dir, root))
			found = true;
	if (f)
		fclose(f);
	return found;
}

static void card_id(char *out, size_t len)
{
	FILE *f = fopen(CID, "r");

	out[0] = 0;
	if (f) {
		if (fgets(out, len, f))
			out[strcspn(out, "\n")] = 0;
		fclose(f);
	}
}

static unsigned cid_hash(const char *s)
{
	unsigned h = 2166136261u;

	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h & 0x7fffffff;
}

bool library_update(const char *reason)
{
	struct mpd_connection *c;

	if (!library_mounted()) {
		logmsg(LOG_WARNING, "not scanning (%s): music card not mounted", reason);
		return false;
	}
	if (!(c = mpdc()) || !mpd_run_update(c, NULL)) {
		mpdc_report("update");
		return false;
	}
	logmsg(LOG_INFO, "library scan started (%s)", reason);
	return true;
}

static bool used_over_usb(void)
{
	const char *marker = disc_data_path("usb-storage.used");

	if (access(marker, F_OK))
		return false;
	remove(marker);
	return set_get_bool("lib.scan_after_usb");
}

static void check(const char *reason)
{
	const char *policy = set_get("lib.scan");
	char cid[96];
	bool usb;
	int h;

	if (!mounted || !mpd_seen)
		return;
	usb = used_over_usb();
	if (!usb && !strcmp(policy, "manual"))
		return;
	card_id(cid, sizeof(cid));
	h = cid[0] ? (int)cid_hash(cid) : 0;
	if (!usb && !strcmp(policy, "card") && h && state_get_int("card.cid", -1) == h)
		return;
	if (library_update(usb ? "after USB storage" : reason))
		state_set_int("card.cid", h);
}

static void announce(void)
{
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "!mount");
	kv_int(&b, "sd", mounted);
	ev_kv(&b);
	eq_apply();
}

static gboolean on_mounts(gint fd, GIOCondition cond, gpointer data)
{
	bool now;

	(void)fd;
	(void)cond;
	(void)data;
	now = library_mounted();
	if (now != mounted) {
		mounted = now;
		logmsg(LOG_INFO, "music card %s", mounted ? "mounted" : "gone");
		announce();
		if (mounted)
			check("card mounted");
	}
	return G_SOURCE_CONTINUE;
}

void library_on_mpd(void)
{
	if (mpd_seen)
		return;
	mpd_seen = true;
	check("boot");
}

void library_init(void)
{
	int fd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);

	mounted = library_mounted();
	if (fd >= 0)
		g_unix_fd_add(fd, G_IO_PRI | G_IO_ERR, on_mounts, NULL);
}
