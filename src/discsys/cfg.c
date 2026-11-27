#include "discsys.h"

#include <disc/paths.h>
#include <disc/settings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define SAVE_DELAY_S 2
#define ZONES "/usr/share/discfw/zones.tsv"
#define APPLY_HELPER "/usr/libexec/discfw/apply"

static guint save_src, reload_src;
static GHashTable *state;
static guint state_src;

static gboolean save_now(gpointer data)
{
	(void)data;
	save_src = 0;
	if (settings_save(SETTINGS_PATH))
		logmsg(LOG_ERR, "saving settings failed");
	return G_SOURCE_REMOVE;
}

void cfg_flush(void)
{
	if (save_src) {
		g_source_remove(save_src);
		save_now(NULL);
	}
	state_flush();
}

static gboolean discd_reload(gpointer data)
{
	(void)data;
	reload_src = 0;
	cfg_flush();
	hw_request("reload", NULL, 0);
	return G_SOURCE_REMOVE;
}

static bool zone_lookup(const char *zone, char *posix, size_t len)
{
	char line[256];
	FILE *f = fopen(ZONES, "r");
	bool found = false;

	if (!f)
		return false;
	while (!found && fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = 0;
		if (strcmp(line, zone))
			continue;
		tab[1 + strcspn(tab + 1, "\n")] = 0;
		snprintf(posix, len, "%s", tab + 1);
		found = true;
	}
	fclose(f);
	return found;
}

static void tz_apply(void)
{
	char posix[128];
	const char *zone = set_get("time.zone");
	FILE *f;

	if (!zone_lookup(zone, posix, sizeof(posix)))
		snprintf(posix, sizeof(posix), "UTC0");
	f = fopen(disc_data_path("TZ.new"), "w");
	if (f) {
		fprintf(f, "%s\n", posix);
		fclose(f);
		rename(disc_data_path("TZ.new"), disc_data_path("TZ"));
	}
	setenv("TZ", posix, 1);
	tzset();
}

static void spawn_apply(const char *key)
{
	char *argv[] = { APPLY_HELPER, (char *)key, NULL };
	GError *e = NULL;

	if (sys.fake_hw)
		return;
	cfg_flush();
	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &e)) {
		logmsg(LOG_ERR, "apply %s: %s", key, e->message);
		g_error_free(e);
	}
}

static bool prefix(const char *s, const char *p)
{
	return !strncmp(s, p, strlen(p));
}

static void apply(const char *key)
{
	if (prefix(key, "battery.") || prefix(key, "audio.")) {
		if (reload_src)
			g_source_remove(reload_src);
		reload_src = g_timeout_add(300, discd_reload, NULL);
	}
	if (!strcmp(key, "play.order"))
		mpdc_apply_order();
	else if (!strcmp(key, "play.replaygain"))
		mpdc_apply_replaygain();
	else if (!strcmp(key, "ui.brightness"))
		screen_brightness_changed();
	else if (prefix(key, "power.") || prefix(key, "keys."))
		screen_reschedule();
	else if (!strcmp(key, "time.zone"))
		tz_apply();
	else if (!strcmp(key, "wifi.enabled") || !strcmp(key, "bt.enabled") || prefix(key, "net.") || !strcmp(key, "bt.battery"))
		spawn_apply(key);
	else if (!strcmp(key, "audio.spdif")) {
		spawn_apply(key);
		ev_toast(set_get_bool(key) ? "S/PDIF on: the 3.5 mm jack is digital, fixed level" : "S/PDIF off");
	}
}

int cfg_set(const char *key, const char *val)
{
	struct kvbuf b;
	char old[512];
	const char *cur = set_get(key);
	int r;

	if (!cur)
		return -1;
	snprintf(old, sizeof(old), "%s", cur);
	r = set_put(key, val);
	if (r)
		return r;
	if (!strcmp(old, set_get(key)))
		return 0;
	if (save_src)
		g_source_remove(save_src);
	save_src = g_timeout_add_seconds(SAVE_DELAY_S, save_now, NULL);
	kv_init(&b);
	kv_raw(&b, "!set");
	kv_str(&b, "key", key);
	kv_str(&b, "val", set_get(key));
	ev_kv(&b);
	apply(key);
	return 0;
}

void cfg_list(struct kvbuf *out, const char *pfx)
{
	for (int i = 0; i < set_ndefs; i++) {
		const struct set_def *d = &set_defs[i];
		struct kvbuf b;
		if (pfx && *pfx && !prefix(d->key, pfx))
			continue;
		kv_init(&b);
		kv_raw(&b, "=");
		kv_str(&b, "key", d->key);
		kv_str(&b, "val", set_get(d->key));
		kv_str(&b, "def", d->def);
		kv_end(&b);
		kv_append(out, b.s);
		kv_free(&b);
	}
}

static void import_legacy(void)
{
	FILE *f = fopen(disc_data_path("charge_limit"), "r");
	char v[16];
	bool changed = false;

	if (f) {
		if (fgets(v, sizeof(v), f) && set_is_default("battery.limit")) {
			v[strcspn(v, "\n")] = 0;
			changed |= set_put("battery.limit", v) == 0;
		}
		fclose(f);
		remove(disc_data_path("charge_limit"));
	}
	if (!access(disc_data_path("wifi/enabled"), F_OK) && set_is_default("wifi.enabled"))
		changed |= set_put("wifi.enabled", "1") == 0;
	if (!access(disc_data_path("bluetooth/enabled"), F_OK) && set_is_default("bt.enabled"))
		changed |= set_put("bt.enabled", "1") == 0;
	if (changed)
		settings_save(SETTINGS_PATH);
}

void cfg_init(void)
{
	int bad = settings_load(SETTINGS_PATH);

	if (bad > 0)
		logmsg(LOG_WARNING, "settings: %d invalid values ignored", bad);
	import_legacy();
	tz_apply();
}

static gboolean state_save(gpointer data)
{
	GHashTableIter it;
	gpointer k, v;
	FILE *f = fopen(disc_data_path("state.new"), "w");

	(void)data;
	state_src = 0;
	if (!f)
		return G_SOURCE_REMOVE;
	g_hash_table_iter_init(&it, state);
	while (g_hash_table_iter_next(&it, &k, &v))
		fprintf(f, "%s=%s\n", (char *)k, (char *)v);
	if (fclose(f) == 0)
		rename(disc_data_path("state.new"), disc_data_path("state"));
	return G_SOURCE_REMOVE;
}

void state_flush(void)
{
	if (state_src) {
		g_source_remove(state_src);
		state_save(NULL);
	}
}

void state_init(void)
{
	char line[256];
	FILE *f = fopen(disc_data_path("state"), "r");

	state = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		eq[1 + strcspn(eq + 1, "\n")] = 0;
		g_hash_table_replace(state, g_strdup(line), g_strdup(eq + 1));
	}
	fclose(f);
}

int state_get_int(const char *key, int def)
{
	const char *v = g_hash_table_lookup(state, key);
	return v ? atoi(v) : def;
}

void state_set_int(const char *key, int val)
{
	char num[16];

	if (state_get_int(key, val + 1) == val)
		return;
	snprintf(num, sizeof(num), "%d", val);
	g_hash_table_replace(state, g_strdup(key), g_strdup(num));
	if (!state_src)
		state_src = g_timeout_add_seconds(10, state_save, NULL);
}
