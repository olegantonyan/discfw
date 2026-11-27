#include "ui.h"

#include <disc/paths.h>
#include <disc/settings.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UNLOCK_PX 60

static lv_obj_t *lock, *clock_lbl, *date_lbl, *bat_lbl, *song_lbl, *artist_lbl, *cover_box, *cover_img, *hint;
static int press_y;

static void refresh(void)
{
	char buf[64];
	time_t now = time(NULL);
	struct tm tm;
	bool synced = access(disc_run_path("time.synced"), F_OK) == 0, valid;
	const char *unsynced = set_get("lock.unsynced");
	const lv_image_dsc_t *c;

	if (!lock || lv_obj_is_hidden(lock))
		return;
	localtime_r(&now, &tm);
	valid = tm.tm_year >= 125 && (synced || strcmp(unsynced, "hide"));
	lv_obj_set_hidden(clock_lbl, !valid);
	if (valid) {
		strftime(buf, sizeof(buf), set_get_bool("time.24h") ? "%H:%M" : "%I:%M", &tm);
		if (!synced && !strcmp(unsynced, "mark"))
			strcat(buf, "?");
		lv_label_set_text(clock_lbl, buf);
	}
	lv_obj_set_hidden(date_lbl, !valid || !set_get_bool("lock.date"));
	strftime(buf, sizeof(buf), "%A, %d %B", &tm);
	lv_label_set_text(date_lbl, buf);
	lv_obj_set_hidden(bat_lbl, !set_get_bool("lock.battery") || (!app.bat.valid && app.sys.bt_bat < 0));
	buf[0] = 0;
	if (app.bat.valid)
		snprintf(buf, sizeof(buf), "%s%d%%", !strcmp(app.bat.state, "charging") ? LV_SYMBOL_CHARGE " " : "", app.bat.cap);
	if (app.sys.bt_bat >= 0)
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s" LV_SYMBOL_BLUETOOTH " %d%%", buf[0] ? "   " : "", app.sys.bt_bat);
	lv_label_set_text(bat_lbl, buf);
	bool song = set_get_bool("lock.song") && app.mpd.cur.uri[0];
	lv_obj_set_hidden(song_lbl, !song);
	lv_obj_set_hidden(artist_lbl, !song);
	lv_label_set_text(song_lbl, app.mpd.cur.title);
	lv_label_set_text(artist_lbl, app.mpd.cur.artist);
	c = cover_current();
	lv_obj_set_hidden(cover_box, !set_get_bool("lock.cover") || !c);
	if (c) {
		lv_image_set_src(cover_img, c);
		lv_image_set_scale(cover_img, 256 * 96 / (c->header.w ? c->header.w : 360));
		lv_obj_center(cover_img);
	}
}

static void on_event(uint32_t evs, void *ctx)
{
	(void)evs;
	(void)ctx;
	refresh();
}

void lock_update(void)
{
	bool show = app.sys.locked && app.sys.screen_on && app.sys.connected;

	if (!lock)
		return;
	lv_obj_set_hidden(lock, !show);
	status_set_visible(!show);
	if (show) {
		ui_modal_close_all();
		shade_close();
		lv_obj_move_foreground(lock);
		refresh();
	}
}

static void touch(lv_event_t *e)
{
	lv_point_t p;
	lv_event_code_t c = lv_event_get_code(e);

	lv_indev_get_point(lv_indev_active(), &p);
	if (c == LV_EVENT_PRESSED) {
		press_y = p.y;
		sysc_send("screen activity");
	} else if (c == LV_EVENT_RELEASED && press_y - p.y >= UNLOCK_PX) {
		app.sys.locked = false;
		lock_update();
		sysc_send("screen unlock");
	}
}

void lock_init(void)
{
	lock = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(lock);
	lv_obj_set_size(lock, SCR_W, SCR_H);
	lv_obj_set_style_bg_color(lock, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(lock, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(lock, false);
	lv_obj_add_event_cb(lock, touch, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(lock, touch, LV_EVENT_RELEASED, NULL);

	bat_lbl = ui_label(lock, &lv_font_montserrat_16, "");
	lv_obj_align(bat_lbl, LV_ALIGN_TOP_MID, 0, 24);
	clock_lbl = ui_label(lock, app.font.clock, "");
	lv_obj_align(clock_lbl, LV_ALIGN_TOP_MID, 0, 54);
	date_lbl = ui_label(lock, app.font.m, "");
	lv_obj_set_style_text_color(date_lbl, lv_color_hex(0xb0b0b0), 0);
	lv_obj_align(date_lbl, LV_ALIGN_TOP_MID, 0, 138);
	cover_box = lv_obj_create(lock);
	lv_obj_remove_style_all(cover_box);
	lv_obj_set_size(cover_box, 96, 96);
	lv_obj_set_style_radius(cover_box, 12, 0);
	lv_obj_set_style_clip_corner(cover_box, true, 0);
	lv_obj_align(cover_box, LV_ALIGN_TOP_MID, 0, 166);
	lv_obj_set_clickable(cover_box, false);
	cover_img = lv_image_create(cover_box);
	song_lbl = ui_label(lock, app.font.m, "");
	lv_obj_set_size(song_lbl, 250, lv_font_get_line_height(app.font.m));
	lv_obj_set_style_text_align(song_lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(song_lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_obj_align(song_lbl, LV_ALIGN_TOP_MID, 0, 268);
	artist_lbl = ui_label(lock, app.font.s, "");
	lv_obj_set_size(artist_lbl, 220, lv_font_get_line_height(app.font.s));
	lv_obj_set_style_text_align(artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(artist_lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_obj_set_style_text_color(artist_lbl, lv_color_hex(0xb0b0b0), 0);
	lv_obj_align(artist_lbl, LV_ALIGN_TOP_MID, 0, 292);
	hint = ui_label(lock, &lv_font_montserrat_14, LV_SYMBOL_UP " swipe up");
	lv_obj_set_style_text_color(hint, lv_color_hex(0x707070), 0);
	lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);
	app_listen(lock, EV_SECOND | EV_SONG | EV_BAT | EV_SYS | EV_COVER | EV_SETTINGS, on_event, NULL);
	lv_obj_set_hidden(lock, true);
}
