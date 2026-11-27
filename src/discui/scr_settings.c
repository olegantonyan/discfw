#include "mpdc.h"
#include "ui.h"

#include <disc/conn.h>
#include <disc/kv.h>
#include <disc/settings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>

#define ZONES "/usr/share/discfw/zones.tsv"
#define MAX_ROWS 96

struct page_def {
	const char *id, *title, *icon;
};

static const struct page_def pages[] = {
	{ "playback", "Playback", LV_SYMBOL_PLAY },     { "audio", "Audio", LV_SYMBOL_VOLUME_MAX },
	{ "display", "Display", LV_SYMBOL_IMAGE },      { "lock", "Lock screen", LV_SYMBOL_EYE_CLOSE },
	{ "buttons", "Buttons", LV_SYMBOL_KEYBOARD },   { "power", "Power", LV_SYMBOL_POWER },
	{ "battery", "Battery", LV_SYMBOL_BATTERY_3 },  { "library", "Library", LV_SYMBOL_DIRECTORY },
	{ "wifi", "Wi-Fi", LV_SYMBOL_WIFI },            { "bluetooth", "Bluetooth", LV_SYMBOL_BLUETOOTH },
	{ "time", "Time", LV_SYMBOL_BELL },             { "system", "System", LV_SYMBOL_SETTINGS },
};
#define N_PAGES (int)(sizeof(pages) / sizeof(pages[0]))

enum kind { R_SET, R_ACTION, R_INFO, R_PAGE };

enum action {
	A_SCAN, A_RESCAN, A_LIB_RESET, A_REBOOT, A_POWEROFF, A_RESTART_UI, A_RESET_UI, A_RESET_SETTINGS, A_ZONE, A_PROFILE,
	A_SLEEP, A_WIFI, A_BT
};

struct srow {
	enum kind kind;
	int def;
	int action;
	char text[160], sub[160];
	const char *arg;
};

struct spage {
	lv_obj_t *scr, *list;
	char id[32];
	struct srow rows[MAX_ROWS];
	int n;
	lv_timer_t *timer;
	char bat[512];
	int target;
};

static const char *page_title(const char *id)
{
	static char buf[64];

	for (int i = 0; i < N_PAGES; i++)
		if (!strcmp(pages[i].id, id))
			return pages[i].title;
	if (!strncmp(id, "power.", 6)) {
		snprintf(buf, sizeof(buf), "Profile: %s", id + 6);
		return buf;
	}
	return id;
}

static int root_count(void *ctx)
{
	(void)ctx;
	return N_PAGES;
}

static void root_row(void *ctx, int idx, struct vrow *r)
{
	(void)ctx;
	r->icon = pages[idx].icon;
	snprintf(r->text, sizeof(r->text), "%s", pages[idx].title);
}

static void root_click(void *ctx, int idx)
{
	(void)ctx;
	nav_push(&scr_settings_page, (void *)pages[idx].id);
}

static const struct vlist_ops root_ops = { .count = root_count, .row = root_row, .click = root_click };

static lv_obj_t *root_create(void *arg)
{
	lv_obj_t *scr = ui_screen("Settings", NULL), *list;

	(void)arg;
	list = vlist_create(scr, &root_ops, NULL);
	lv_obj_set_size(list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(list, 0, CONTENT_Y);
	vlist_reload(list);
	return scr;
}

const struct screen_ops scr_settings = { .name = "settings", .create = root_create };

static void value_text(const struct set_def *d, char *out, size_t len)
{
	const char *v = set_get(d->key);

	switch (d->type) {
	case SET_BOOL:
		snprintf(out, len, "%s", v[0] == '1' ? "On" : "Off");
		break;
	case SET_INT:
		if (!strcmp(v, "0") && (strstr(d->label, "after") || strstr(d->label, "when") || strstr(d->label, "0 = off")))
			snprintf(out, len, "Never / off");
		else
			snprintf(out, len, "%s %s", v, d->unit ? d->unit : "");
		break;
	case SET_ENUM:
		if (!set_choice_label(d, set_choice_index(d, v), out, len))
			snprintf(out, len, "%s", v);
		break;
	case SET_STR:
		snprintf(out, len, "%s", !strcmp(d->key, "net.mpd_password") && v[0] ? "••••" : v[0] ? v : "(empty)");
		break;
	}
}

static struct srow *add(struct spage *p, enum kind k)
{
	struct srow *r = &p->rows[p->n < MAX_ROWS ? p->n++ : MAX_ROWS - 1];

	memset(r, 0, sizeof(*r));
	r->kind = k;
	r->def = -1;
	return r;
}

static void add_info(struct spage *p, const char *text)
{
	snprintf(add(p, R_INFO)->text, sizeof(p->rows[0].text), "%s", text);
}

static void add_action(struct spage *p, int action, const char *text, const char *arg)
{
	struct srow *r = add(p, R_ACTION);

	r->action = action;
	r->arg = arg;
	snprintf(r->text, sizeof(r->text), "%s", text);
}

static void add_settings(struct spage *p, const char *page)
{
	for (int i = 0; i < set_ndefs; i++)
		if (!strcmp(set_defs[i].page, page)) {
			struct srow *r = add(p, R_SET);
			r->def = i;
		}
}

static const char *const states[] = { "battery", "On battery", "charging", "Charging", "full", "Charged", "held",
				      "Held at limit", "hot", "Too hot, paused", "idle", "Not charging", NULL };
static const char *const inputs[] = { "none", "No input", "sdp", "USB port", "adapter", "Charger", "otg", "USB host power",
				      NULL };
static const char *const stages[] = { "none", "not charging", "pre", "pre-charge", "fast", "fast charge", "done",
				      "terminated", NULL };

static const char *word(const char *key, const char *const *map)
{
	for (int i = 0; map[i]; i += 2)
		if (!strcmp(map[i], key))
			return map[i + 1];
	return key;
}

static void battery_rows(struct spage *p)
{
	char line[96], st[32], vbus[16], stage[16], health[32], typec[48];
	const char *b = p->bat;
	int cap = kv_get_int(b, "cap", -1), ma = kv_get_int(b, "ma", 0);


	if (cap < 0) {
		add_info(p, "Battery status unavailable");
		return;
	}
	kv_get(b, "state", st, sizeof(st));
	kv_get(b, "vbus", vbus, sizeof(vbus));
	kv_get(b, "stage", stage, sizeof(stage));
	kv_get(b, "health", health, sizeof(health));
	kv_get(b, "typec", typec, sizeof(typec));
	snprintf(line, sizeof(line), "%d%%   %.2f V   %.1f °C", cap, kv_get_int(b, "mv", 0) / 1000.0, kv_get_int(b, "temp", 0) / 10.0);
	add_info(p, line);
	snprintf(line, sizeof(line), "%s   %+d mA", word(st, states), ma);
	add_info(p, line);
	snprintf(line, sizeof(line), "%s · %s · %ld mA max", word(vbus, inputs), word(stage, stages), kv_get_int(b, "ichg", 0));
	add_info(p, line);
	snprintf(line, sizeof(line), "Cycles %ld · health %s", kv_get_int(b, "cycles", -1), health[0] ? health : "?");
	add_info(p, line);
	if (typec[0]) {
		snprintf(line, sizeof(line), "Type-C: %s", typec);
		add_info(p, line);
	}
}

static void system_rows(struct spage *p)
{
	char line[160], rel[96] = "";
	struct sysinfo si;
	struct statvfs vs;
	FILE *f = fopen("/etc/discfw-release", "r");

	if (f) {
		if (fgets(rel, sizeof(rel), f))
			rel[strcspn(rel, "\n")] = 0;
		fclose(f);
	}
	add_info(p, rel[0] ? rel : "discfw");
	if (sysinfo(&si) == 0) {
		snprintf(line, sizeof(line), "Up %ld h %02ld min · RAM free %lu MB", si.uptime / 3600, si.uptime / 60 % 60,
			 (unsigned long)((uint64_t)(si.freeram + si.bufferram) * si.mem_unit >> 20));
		add_info(p, line);
	}
	if (statvfs(disc_data_path("."), &vs) == 0) {
		snprintf(line, sizeof(line), "Data partition: %llu MB free", (unsigned long long)vs.f_bavail * vs.f_frsize >> 20);
		add_info(p, line);
	}
	if (statvfs(disc_music_root(), &vs) == 0 && vs.f_blocks) {
		snprintf(line, sizeof(line), "SD card: %llu / %llu GB free", (unsigned long long)vs.f_bavail * vs.f_frsize >> 30,
			 (unsigned long long)vs.f_blocks * vs.f_frsize >> 30);
		add_info(p, line);
	}
	add_action(p, A_RESTART_UI, "Restart interface", NULL);
	add_action(p, A_RESET_UI, "Forget folder positions…", NULL);
	add_action(p, A_RESET_SETTINGS, "Reset all settings…", NULL);
	add_action(p, A_REBOOT, "Reboot", NULL);
	add_action(p, A_POWEROFF, "Power off", NULL);
}

static void library_rows(struct spage *p)
{
	unsigned songs, albums, artists;
	unsigned long upd, play;
	char line[128];

	if (mpdc_stats(&songs, &albums, &artists, &upd, &play)) {
		snprintf(line, sizeof(line), "%u songs · %u albums · %u artists", songs, albums, artists);
		add_info(p, line);
		snprintf(line, sizeof(line), "%s", app.mpd.updating ? "Scanning…" : upd ? "Library up to date" : "Never scanned");
		add_info(p, line);
	} else {
		add_info(p, "Music service not running");
	}
	add_action(p, A_SCAN, "Scan for changes", NULL);
	add_action(p, A_RESCAN, "Rescan all files", NULL);
	add_action(p, A_LIB_RESET, "Reset library database…", NULL);
}

static void build(struct spage *p)
{
	static const char *const profiles[] = { "saver", "balanced", "performance", "custom" };
	static const char *const profile_titles[] = { "Edit profile: Saver", "Edit profile: Balanced", "Edit profile: Performance",
						      "Edit profile: Custom" };

	p->n = 0;
	if (!strcmp(p->id, "battery"))
		battery_rows(p);
	else if (!strcmp(p->id, "system"))
		system_rows(p);
	else if (!strcmp(p->id, "library"))
		library_rows(p);
	if (!strcmp(p->id, "wifi"))
		add_action(p, A_WIFI, "Networks", NULL);
	if (!strcmp(p->id, "bluetooth"))
		add_action(p, A_BT, "Devices", NULL);
	if (!strcmp(p->id, "playback"))
		add_action(p, A_SLEEP, app.sys.sleep_left ? "Sleep timer (running)" : "Sleep timer", NULL);
	add_settings(p, p->id);
	if (!strcmp(p->id, "power")) {
		char line[64];
		snprintf(line, sizeof(line), "Active now: %s (%s)", app.sys.profile[0] ? app.sys.profile : "?",
			 app.sys.powered ? "charger" : "battery");
		add_info(p, line);
		for (int i = 0; i < 4; i++)
			add_action(p, A_PROFILE, profile_titles[i], profiles[i]);
	}
}

static int count(void *ctx)
{
	return ((struct spage *)ctx)->n;
}

static void row(void *ctx, int idx, struct vrow *r)
{
	struct spage *p = ctx;
	struct srow *s = &p->rows[idx];

	switch (s->kind) {
	case R_SET:
		snprintf(r->text, sizeof(r->text), "%s", set_defs[s->def].label);
		if (!strcmp(set_defs[s->def].key, "time.zone"))
			snprintf(r->sub, sizeof(r->sub), "%s", set_get("time.zone"));
		else
			value_text(&set_defs[s->def], r->sub, sizeof(r->sub));
		break;
	case R_INFO:
		snprintf(r->text, sizeof(r->text), "%s", s->text);
		r->dim = true;
		break;
	case R_ACTION:
	case R_PAGE:
		snprintf(r->text, sizeof(r->text), "%s", s->text);
		r->icon = LV_SYMBOL_RIGHT;
		break;
	}
}

static void enum_picked(int idx, void *ctx)
{
	const struct set_def *d = ctx;
	char v[64];

	if (set_choice(d, idx, v, sizeof(v)))
		sysc_set(d->key, v);
}

static void int_set(int v, void *ctx)
{
	sysc_set_int(((const struct set_def *)ctx)->key, v);
}

static void str_set(const char *text, void *ctx)
{
	sysc_set(((const struct set_def *)ctx)->key, text);
}

static char **zones;
static int n_zones;

static void load_zones(void)
{
	char line[256];
	FILE *f;

	if (zones)
		return;
	f = fopen(ZONES, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = 0;
		zones = realloc(zones, (n_zones + 1) * sizeof(char *));
		zones[n_zones++] = strdup(line);
	}
	if (f)
		fclose(f);
	if (!zones) {
		zones = malloc(sizeof(char *));
		zones[n_zones++] = strdup("UTC");
	}
}

static void zone_picked(int idx, void *ctx)
{
	(void)ctx;
	sysc_set("time.zone", zones[idx]);
}

static void edit_setting(const struct set_def *d)
{
	char **labels;
	int n, cur;

	if (!strcmp(d->key, "time.zone")) {
		load_zones();
		cur = 0;
		for (int i = 0; i < n_zones; i++)
			if (!strcmp(zones[i], set_get("time.zone")))
				cur = i;
		ui_pick("Timezone", (const char *const *)zones, n_zones, cur, zone_picked, NULL);
		return;
	}
	switch (d->type) {
	case SET_BOOL:
		sysc_set(d->key, set_get_bool(d->key) ? "0" : "1");
		break;
	case SET_INT:
		ui_number_input(d->label, set_get_int(d->key), d->min, d->max, d->step, d->unit, int_set, (void *)d);
		break;
	case SET_ENUM:
		n = set_choice_count(d);
		labels = calloc(n, sizeof(char *));
		for (int i = 0; i < n; i++) {
			labels[i] = malloc(96);
			set_choice_label(d, i, labels[i], 96);
		}
		ui_pick(d->label, (const char *const *)labels, n, set_get_enum(d->key), enum_picked, (void *)d);
		for (int i = 0; i < n; i++)
			free(labels[i]);
		free(labels);
		break;
	case SET_STR:
		ui_text_input(d->label, set_get(d->key), !strcmp(d->key, "net.mpd_password"), str_set, (void *)d);
		break;
	}
}

static void lib_reset(void *ctx)
{
	(void)ctx;
	sysc_send("library reset");
}

static void reset_ui(void *ctx)
{
	(void)ctx;
	state_reset();
	ui_toast("Folder positions forgotten");
}

static void reset_settings(void *ctx)
{
	(void)ctx;
	for (int i = 0; i < set_ndefs; i++)
		if (!set_is_default(set_defs[i].key))
			sysc_set(set_defs[i].key, set_defs[i].def);
}

static void do_power(void *ctx)
{
	sysc_send("%s", (const char *)ctx);
}

static const int sleep_mins[] = { 0, 15, 30, 45, 60, 90, 120 };

static void sleep_picked(int idx, void *ctx)
{
	(void)ctx;
	sysc_send("sleep %d", sleep_mins[idx]);
}

static void click(void *ctx, int idx)
{
	static const char *const sleep_labels[] = { "Off", "15 min", "30 min", "45 min", "1 hour", "1.5 hours", "2 hours" };
	struct spage *p = ctx;
	struct srow *s = &p->rows[idx];
	char page[48];

	if (s->kind == R_SET) {
		edit_setting(&set_defs[s->def]);
		return;
	}
	if (s->kind != R_ACTION)
		return;
	switch (s->action) {
	case A_SCAN:
		sysc_send("library scan");
		ui_toast("Scanning in background");
		break;
	case A_RESCAN:
		sysc_send("library rescan");
		ui_toast("Rescanning all files in background");
		break;
	case A_LIB_RESET:
		ui_confirm("Delete the library database and scan again?", "Reset", lib_reset, NULL);
		break;
	case A_REBOOT:
		ui_confirm("Reboot now?", "Reboot", do_power, "reboot");
		break;
	case A_POWEROFF:
		ui_confirm("Power off now?", "Power off", do_power, "poweroff");
		break;
	case A_RESTART_UI:
		state_save();
		exit(0);
	case A_RESET_UI:
		ui_confirm("Forget remembered folder positions and playlist order?", "Forget", reset_ui, NULL);
		break;
	case A_RESET_SETTINGS:
		ui_confirm("Reset every setting to its default?", "Reset", reset_settings, NULL);
		break;
	case A_PROFILE:
		snprintf(page, sizeof(page), "power.%s", s->arg);
		nav_push(&scr_settings_page, strdup(page));
		break;
	case A_SLEEP:
		ui_pick("Sleep timer", sleep_labels, 7, -1, sleep_picked, NULL);
		break;
	case A_WIFI:
		nav_push(&scr_wifi, NULL);
		break;
	case A_BT:
		nav_push(&scr_bt, NULL);
		break;
	}
}

static const struct vlist_ops page_ops = { .count = count, .row = row, .click = click };

static void fetch_battery(struct spage *p)
{
	if (discd_request("battery\n", p->bat, sizeof(p->bat), 1000) < 0)
		p->bat[0] = 0;
}

static void refresh(struct spage *p)
{
	int first = vlist_first(p->list);

	if (!strcmp(p->id, "battery"))
		fetch_battery(p);
	build(p);
	vlist_reload(p->list);
	vlist_scroll_to(p->list, first, false);
}

static void timer_cb(lv_timer_t *t)
{
	struct spage *p = lv_timer_get_user_data(t);

	if (app.sys.screen_on && lv_screen_active() == p->scr)
		refresh(p);
}

static void on_event(uint32_t evs, void *ctx)
{
	(void)evs;
	refresh(ctx);
}

static void on_delete(lv_event_t *e)
{
	struct spage *p = lv_event_get_user_data(e);

	if (p->timer)
		lv_timer_delete(p->timer);
	free(p);
}

static lv_obj_t *page_create(void *arg)
{
	struct spage *p = calloc(1, sizeof(*p));

	snprintf(p->id, sizeof(p->id), "%s", (const char *)arg);
	if (!strncmp(p->id, "power.", 6))
		free(arg);
	p->scr = ui_screen(page_title(p->id), NULL);
	p->list = vlist_create(p->scr, &page_ops, p);
	lv_obj_set_size(p->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(p->list, 0, CONTENT_Y);
	if (!strcmp(p->id, "battery") || !strcmp(p->id, "system"))
		p->timer = lv_timer_create(timer_cb, 3000, p);
	refresh(p);
	app_listen(p->scr, EV_SETTINGS | EV_SYS | (!strcmp(p->id, "library") ? EV_UPDATE | EV_DB : 0), on_event, p);
	lv_obj_add_event_cb(p->scr, on_delete, LV_EVENT_DELETE, p);
	return p->scr;
}

void settings_open_page(const char *page)
{
	nav_push(&scr_settings_page, (void *)page);
}

const struct screen_ops scr_settings_page = { .name = "settings_page", .create = page_create };
