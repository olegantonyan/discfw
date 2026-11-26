#include "discsys.h"

#include <dirent.h>
#include <disc/settings.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <mpd/client.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

#define RAW_MIN_MS 20

enum press { PR_SHORT, PR_DOUBLE, PR_LONG };
static const char *const press_names[] = { "short", "double", "long" };

struct key {
	const char *name;
	int base[2], klong, kdouble;
	bool raw, down, long_fired, seen_press;
	int taps;
	gint64 t_press;
	guint long_src, repeat_src, double_src;
};

static struct key keys[] = {
	{ .name = "play", .base = { 250, -1 }, .klong = 268, .kdouble = 269 },
	{ .name = "volup", .base = { 251, -1 }, .klong = 263, .kdouble = 266 },
	{ .name = "voldown", .base = { 252, -1 }, .klong = 262, .kdouble = 267 },
	{ .name = "power", .base = { 259, 265 }, .klong = 264, .kdouble = -1 },
};
#define N_KEYS (int)(sizeof(keys) / sizeof(keys[0]))

static int key_fd = -1;

static bool off_context(void)
{
	return sys.screen == SCR_OFF || sys.locked;
}

static const char *binding(struct key *k, enum press p)
{
	char name[48];

	snprintf(name, sizeof(name), "keys.%s.%s.%s", k->name, press_names[p], off_context() ? "off" : "on");
	const char *v = set_get(name);
	return v ? v : "none";
}

static bool is_power(struct key *k)
{
	return !strcmp(k->name, "power");
}

static bool repeats(const char *action)
{
	return !strcmp(action, "vol_up") || !strcmp(action, "vol_down") || !strcmp(action, "seek_fwd") || !strcmp(action, "seek_back");
}

static void toggle_play(void)
{
	struct mpd_connection *c = mpdc();
	struct mpd_status *s;

	if (!c || !(s = mpd_run_status(c))) {
		mpdc_report("status");
		return;
	}
	enum mpd_state st = mpd_status_get_state(s);
	mpd_status_free(s);
	if (st == MPD_STATE_PLAY)
		mpd_run_pause(c, true);
	else if (st == MPD_STATE_PAUSE)
		mpd_run_pause(c, false);
	else
		mpd_run_play(c);
	mpdc_report("toggle");
}

static void cycle_setting(const char *key, const char *const *order, int n)
{
	const char *cur = set_get(key);
	int i;

	for (i = 0; i < n && strcmp(order[i], cur); i++)
		;
	cfg_set(key, order[(i + 1) % n]);
}

bool action_run(const char *a)
{
	struct mpd_connection *c;

	if (!strcmp(a, "none"))
		return true;
	if (!strcmp(a, "play_pause")) {
		toggle_play();
	} else if (!strcmp(a, "next") || !strcmp(a, "prev")) {
		if ((c = mpdc()) && !(a[0] == 'n' ? mpd_run_next(c) : mpd_run_previous(c)))
			mpdc_report(a);
	} else if (!strcmp(a, "vol_up") || !strcmp(a, "vol_down")) {
		volume_step((a[4] == 'u' ? 1 : -1) * set_get_int("audio.vol_step"));
	} else if (!strcmp(a, "seek_fwd") || !strcmp(a, "seek_back")) {
		int d = set_get_int("keys.seek_s");
		if ((c = mpdc()) && !mpd_run_seek_current(c, a[5] == 'f' ? d : -d, true))
			mpdc_report("seek");
	} else if (!strcmp(a, "screen")) {
		screen_toggle();
	} else if (!strcmp(a, "lock")) {
		screen_set(SCR_OFF, true);
	} else if (!strcmp(a, "order")) {
		static const char *const order[] = { "seq", "random", "single", "noloop" };
		cycle_setting("play.order", order, 4);
		ev_toast(set_get("play.order"));
	} else if (!strcmp(a, "bt") || !strcmp(a, "wifi")) {
		const char *key = a[0] == 'b' ? "bt.enabled" : "wifi.enabled";
		cfg_set(key, set_get_bool(key) ? "0" : "1");
		ev_toast(set_get_bool(key) ? (a[0] == 'b' ? "Bluetooth on" : "Wi-Fi on") : (a[0] == 'b' ? "Bluetooth off" : "Wi-Fi off"));
	} else if (!strcmp(a, "sleep")) {
		static const int steps[] = { 0, 15, 30, 60, 90 };
		int cur = (sys.sleep_left_s + 59) / 60, next = 15;
		for (int i = 0; i < 5; i++)
			if (cur > 0 && cur <= steps[i]) {
				next = i + 1 < 5 ? steps[i + 1] : 0;
				break;
			}
		power_sleep_timer(next);
	} else if (!strcmp(a, "poweroff")) {
		power_off(false);
	} else {
		return false;
	}
	return true;
}

static void fire(struct key *k, enum press p)
{
	const char *a = is_power(k) && p == PR_LONG ? "poweroff" : binding(k, p);
	struct kvbuf b;

	if (sys.shutting_down)
		return;
	if (sys.screen != SCR_OFF && strcmp(a, "screen"))
		screen_activity();
	kv_init(&b);
	kv_raw(&b, "!key");
	kv_str(&b, "key", k->name);
	kv_str(&b, "press", press_names[p]);
	kv_str(&b, "action", a);
	ev_kv(&b);
	action_run(a);
}

static gboolean on_repeat(gpointer data)
{
	struct key *k = data;

	if (!k->down) {
		k->repeat_src = 0;
		return G_SOURCE_REMOVE;
	}
	fire(k, PR_LONG);
	return G_SOURCE_CONTINUE;
}

static gboolean on_long(gpointer data)
{
	struct key *k = data;

	k->long_src = 0;
	k->long_fired = true;
	fire(k, PR_LONG);
	if (!is_power(k) && repeats(binding(k, PR_LONG)))
		k->repeat_src = g_timeout_add(set_get_int("keys.repeat_ms"), on_repeat, k);
	return G_SOURCE_REMOVE;
}

static gboolean on_double_timeout(gpointer data)
{
	struct key *k = data;

	k->double_src = 0;
	k->taps = 0;
	fire(k, PR_SHORT);
	return G_SOURCE_REMOVE;
}

static void stop_timer(guint *src)
{
	if (*src)
		g_source_remove(*src);
	*src = 0;
}

static bool has_double(struct key *k)
{
	return strcmp(binding(k, PR_DOUBLE), "none") != 0;
}

static void press(struct key *k, gint64 t)
{
	k->seen_press = true;
	k->down = true;
	k->t_press = t;
	k->long_fired = false;
	if (k->raw)
		k->long_src = g_timeout_add(set_get_int(is_power(k) ? "keys.power_long_ms" : "keys.long_ms"), on_long, k);
}

static void release(struct key *k, gint64 t)
{
	bool was_down = k->down;

	k->down = false;
	stop_timer(&k->long_src);
	stop_timer(&k->repeat_src);
	if (!k->seen_press || !was_down)
		return;
	if (!k->raw && t - k->t_press > RAW_MIN_MS) {
		k->raw = true;
		logmsg(LOG_INFO, "key %s: kernel delivers press/release", k->name);
		if (t - k->t_press >= set_get_int(is_power(k) ? "keys.power_long_ms" : "keys.long_ms")) {
			fire(k, PR_LONG);
			return;
		}
	}
	if (!k->raw || k->long_fired) {
		if (!k->raw)
			fire(k, PR_SHORT);
		return;
	}
	if (!has_double(k)) {
		fire(k, PR_SHORT);
	} else if (k->taps) {
		stop_timer(&k->double_src);
		k->taps = 0;
		fire(k, PR_DOUBLE);
	} else {
		k->taps = 1;
		k->double_src = g_timeout_add(set_get_int("keys.double_ms"), on_double_timeout, k);
	}
}

static void kernel_hold(struct key *k, int value)
{
	if (k->raw)
		return;
	if (value == 0) {
		k->down = false;
		stop_timer(&k->repeat_src);
		return;
	}
	if (value != 1)
		return;
	k->down = true;
	fire(k, PR_LONG);
	stop_timer(&k->repeat_src);
	if (!is_power(k) && repeats(binding(k, PR_LONG)))
		k->repeat_src = g_timeout_add(set_get_int("keys.repeat_ms"), on_repeat, k);
}

static void kernel_double(struct key *k)
{
	if (!k->raw) {
		fire(k, PR_DOUBLE);
		return;
	}
	if (k->taps) {
		stop_timer(&k->double_src);
		k->taps = 0;
		fire(k, PR_DOUBLE);
	}
}

static void on_event(int code, int value, gint64 t)
{
	for (int i = 0; i < N_KEYS; i++) {
		struct key *k = &keys[i];
		if (code == k->base[0] || code == k->base[1]) {
			if (value == 1)
				press(k, t);
			else if (value == 0)
				release(k, t);
			return;
		}
		if (code == k->klong) {
			kernel_hold(k, value);
			return;
		}
		if (code == k->kdouble) {
			if (value == 1)
				kernel_double(k);
			return;
		}
	}
	logmsg(LOG_DEBUG, "unhandled key code %d value %d", code, value);
}

static gboolean on_keys(gint fd, GIOCondition cond, gpointer data)
{
	struct {
		uint32_t sec, usec;
		uint16_t type, code;
		int32_t value;
	} ev;

	(void)cond;
	(void)data;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev))
		if (ev.type == EV_KEY)
			on_event(ev.code, ev.value, now_ms());
	return G_SOURCE_CONTINUE;
}

bool keys_power_held(void)
{
	struct key *k = &keys[N_KEYS - 1];
	unsigned long bits[(KEY_MAX + 1) / (8 * sizeof(long)) + 1];

	if (k->raw)
		return k->down;
	if (key_fd < 0 || ioctl(key_fd, EVIOCGKEY(sizeof(bits)), bits) < 0)
		return false;
	int codes[3] = { k->base[0], k->base[1], k->klong };

	if (k->down)
		return true;
	for (int i = 0; i < 3; i++) {
		int c = codes[i];
		if (c >= 0 && (bits[c / (8 * sizeof(long))] >> (c % (8 * sizeof(long)))) & 1)
			return true;
	}
	return false;
}

void keys_simulate(const char *name, const char *p)
{
	for (int i = 0; i < N_KEYS; i++) {
		if (strcmp(keys[i].name, name))
			continue;
		for (int j = 0; j < 3; j++)
			if (!strcmp(p, press_names[j]))
				fire(&keys[i], j);
	}
}

int input_find(const char *name, char *path, size_t len)
{
	char p[300], n[64];
	struct dirent *e;
	DIR *dir = opendir("/sys/class/input");
	int found = -1;

	while (dir && found && (e = readdir(dir))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/sys/class/input/%s/device/name", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f)
			continue;
		if (fgets(n, sizeof(n), f) && !strncmp(n, name, strlen(name))) {
			snprintf(path, len, "/dev/input/%s", e->d_name);
			found = 0;
		}
		fclose(f);
	}
	if (dir)
		closedir(dir);
	return found;
}

void keys_init(void)
{
	char path[64];

	if (sys.fake_hw)
		return;
	if (input_find("x2000_key", path, sizeof(path)) || (key_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) {
		logmsg(LOG_ERR, "key device not found");
		return;
	}
	g_unix_fd_add(key_fd, G_IO_IN, on_keys, NULL);
}
