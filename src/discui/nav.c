#include "ui.h"

#include <disc/paths.h>
#include <disc/settings.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEPTH 16
#define STATUS_Y 12
#define STATUS_MAX_W 146

static struct entry {
	const struct screen_ops *ops;
	lv_obj_t *scr;
} stack[MAX_DEPTH];
static int depth;
static lv_obj_t *status;

static const struct screen_ops *const all[] = { &scr_home, &scr_folders, &scr_np, &scr_queue, &scr_playlists, &scr_library, &scr_settings, &scr_wifi, &scr_bt };

static void remember(void)
{
	if (depth)
		state_set("screen", stack[depth - 1].ops->name);
}

static void show(struct entry *e)
{
	lv_screen_load(e->scr);
	if (e->ops->shown)
		e->ops->shown(e->scr);
	remember();
}

void nav_push(const struct screen_ops *ops, void *arg)
{
	lv_obj_t *scr;

	if (depth == MAX_DEPTH)
		return;
	scr = ops->create(arg);
	if (!scr)
		return;
	stack[depth++] = (struct entry){ ops, scr };
	show(&stack[depth - 1]);
}

static void pop(void)
{
	lv_obj_t *old = stack[--depth].scr;

	show(&stack[depth - 1]);
	lv_obj_delete(old);
}

bool nav_back(void)
{
	if (ui_modal_close())
		return true;
	if (shade_is_open()) {
		shade_close();
		return true;
	}
	if (!depth)
		return false;
	struct entry *top = &stack[depth - 1];
	if (top->ops->back && top->ops->back(top->scr))
		return true;
	if (depth <= 1)
		return false;
	pop();
	return true;
}

void nav_home(void)
{
	ui_modal_close_all();
	shade_close();
	while (depth > 1)
		pop();
}

void nav_open(const struct screen_ops *ops, void *arg)
{
	ui_modal_close_all();
	shade_close();
	if (depth && stack[depth - 1].ops == ops && !arg)
		return;
	while (depth > 1)
		pop();
	if (ops != &scr_home)
		nav_push(ops, arg);
}

const struct screen_ops *nav_top(void)
{
	return depth ? stack[depth - 1].ops : NULL;
}

const char *nav_describe(char *buf, size_t len)
{
	size_t o = 0;

	buf[0] = 0;
	for (int i = 0; i < depth && o < len; i++)
		o += snprintf(buf + o, len - o, "%s%s", i ? ">" : "", stack[i].ops->name);
	return buf;
}

bool nav_goto_name(const char *name)
{
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		if (!strcmp(all[i]->name, name)) {
			nav_open(all[i], NULL);
			return true;
		}
	return false;
}

static bool time_synced(void)
{
	return access(disc_run_path("time.synced"), F_OK) == 0;
}

static const char *battery_symbol(int cap)
{
	return cap >= 90 ? LV_SYMBOL_BATTERY_FULL : cap >= 65 ? LV_SYMBOL_BATTERY_3 : cap >= 40 ? LV_SYMBOL_BATTERY_2 : cap >= 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
}

static void status_text(const char *clock, bool mark, bool glyph, bool powered)
{
	char bat[48] = "", bt[24] = "";
	const char *pre = !strcmp(app.bat.state, "charging") ? LV_SYMBOL_CHARGE : powered ? LV_SYMBOL_USB : "";

	if (app.bat.valid && glyph)
		snprintf(bat, sizeof(bat), "%s%s%s %d%%", pre, pre[0] ? " " : "", battery_symbol(app.bat.cap), app.bat.cap);
	else if (app.bat.valid)
		snprintf(bat, sizeof(bat), "%s%d%%", pre, app.bat.cap);
	if (app.sys.bt_bat >= 0)
		snprintf(bt, sizeof(bt), " " LV_SYMBOL_BLUETOOTH "%d%%", app.sys.bt_bat);
	else if (app.sys.out_bt)
		snprintf(bt, sizeof(bt), "  " LV_SYMBOL_BLUETOOTH);
	lv_label_set_text_fmt(status, "%s%s%s%s%s", clock, mark ? "?" : "", !clock[0] ? "" : app.sys.bt_bat >= 0 ? " " : "   ", bat, bt);
	lv_obj_update_layout(status);
}

static void status_update(uint32_t evs, void *ctx)
{
	char clock[16] = "";
	time_t now = time(NULL);
	struct tm tm;
	bool powered = app.bat.valid && strcmp(app.bat.state, "battery"), mark;

	(void)evs;
	(void)ctx;
	if (!app.sys.screen_on)
		return;
	localtime_r(&now, &tm);
	if (tm.tm_year >= 125 && (time_synced() || strcmp(set_get("lock.unsynced"), "hide")))
		strftime(clock, sizeof(clock), set_get_bool("time.24h") ? "%H:%M" : "%I:%M", &tm);
	mark = clock[0] && !time_synced();
	status_text(clock, mark, true, powered);
	if (lv_obj_get_width(status) > STATUS_MAX_W && powered)
		status_text(clock, mark, false, powered);
	if (lv_obj_get_width(status) > STATUS_MAX_W)
		status_text(clock, false, !powered, powered);
	lv_obj_set_style_text_color(status, !powered && app.bat.valid && app.bat.cap <= 10 ? lv_palette_main(LV_PALETTE_RED) : lv_color_hex(0xc0c0c0), 0);
}

void status_set_visible(bool on)
{
	if (status)
		lv_obj_set_hidden(status, !on);
}

static const struct screen_ops *by_name(const char *name)
{
	for (size_t i = 0; name && i < sizeof(all) / sizeof(all[0]); i++)
		if (!strcmp(all[i]->name, name))
			return all[i];
	return NULL;
}

void nav_start(void)
{
	const char *start = set_get("ui.start_screen");
	const struct screen_ops *target = NULL;

	status = ui_label(lv_layer_top(), &lv_font_montserrat_14, "");
	lv_obj_align(status, LV_ALIGN_TOP_MID, 0, STATUS_Y);
	lv_obj_set_clickable(status, false);
	app_listen(status, EV_BAT | EV_SECOND | EV_SYS | EV_SETTINGS | EV_VOLUME, status_update, NULL);
	lock_init();
	if (strcmp(disc_boot_mode(), "player")) {
		nav_push(&scr_mode, NULL);
		status_update(0, NULL);
		return;
	}
	shade_init();
	if (app.safe_mode)
		target = &scr_folders;
	else if (!strcmp(start, "last"))
		target = by_name(state_get("screen"));
	else
		target = by_name(start);
	nav_push(&scr_home, NULL);
	if (target && target != &scr_home)
		nav_push(target, NULL);
	status_update(0, NULL);
}
