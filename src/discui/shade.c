#include "mpdc.h"
#include "ui.h"

#include <disc/settings.h>
#include <stdio.h>
#include <string.h>

#define OPEN_PX 80

static lv_obj_t *shade, *title, *artist, *play_lbl, *wifi, *bt, *order, *vol, *bright, *info;
static bool open_;

static void style_toggle(lv_obj_t *b, bool on)
{
	lv_obj_set_style_bg_color(b, on ? app.accent : lv_color_hex(0x333333), 0);
	lv_obj_set_style_text_color(lv_obj_get_child(b, 0), on ? lv_color_black() : lv_color_white(), 0);
}

static void refresh(void)
{
	const char *o;
	char line[96];

	if (!shade || !open_)
		return;
	lv_label_set_text(title, app.mpd.cur.uri[0] ? app.mpd.cur.title : "Nothing playing");
	lv_label_set_text(artist, app.mpd.cur.artist);
	lv_label_set_text(play_lbl, app.mpd.state == PS_PLAY ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	style_toggle(wifi, set_get_bool("wifi.enabled"));
	style_toggle(bt, set_get_bool("bt.enabled"));
	o = mpdc_order();
	lv_label_set_text(lv_obj_get_child(order, 0), !strcmp(o, "random") ? LV_SYMBOL_SHUFFLE : !strcmp(o, "single") ? "1" : !strcmp(o, "once") ? "1×" : !strcmp(o, "noloop") ? LV_SYMBOL_RIGHT : LV_SYMBOL_LOOP);
	if (!lv_obj_has_state(vol, LV_STATE_PRESSED))
		lv_slider_set_value(vol, app.sys.volume, LV_ANIM_OFF);
	if (!lv_obj_has_state(bright, LV_STATE_PRESSED))
		lv_slider_set_value(bright, set_get_int("ui.brightness"), LV_ANIM_OFF);
	snprintf(line, sizeof(line), "%s%s", app.sys.profile[0] ? app.sys.profile : "", app.sys.sleep_left ? "   " LV_SYMBOL_BELL " sleep" : "");
	if (app.sys.sleep_left)
		snprintf(line + strlen(line), sizeof(line) - strlen(line), " %d min", (app.sys.sleep_left + 59) / 60);
	lv_label_set_text(info, line);
}

static void on_event(uint32_t evs, void *ctx)
{
	(void)evs;
	(void)ctx;
	refresh();
}

void shade_close(void)
{
	if (!shade)
		return;
	open_ = false;
	lv_obj_set_hidden(shade, true);
	lv_obj_set_y(shade, -SCR_H);
}

static void show_at(int y)
{
	if (y > 0)
		y = 0;
	lv_obj_set_y(shade, y);
	lv_obj_set_hidden(shade, false);
	lv_obj_move_foreground(shade);
}

void shade_drag(int dy, bool released)
{
	if (!shade)
		return;
	if (!released) {
		if (!open_) {
			open_ = true;
			refresh();
		}
		show_at(-SCR_H + dy * 2);
		return;
	}
	if (dy >= OPEN_PX) {
		show_at(0);
		refresh();
	} else {
		shade_close();
	}
}

bool shade_is_open(void)
{
	return open_;
}

void shade_lift(int dy, bool released)
{
	if (!shade || !open_)
		return;
	if (!released) {
		lv_obj_set_y(shade, dy < 0 ? dy : 0);
		return;
	}
	if (dy <= -OPEN_PX)
		shade_close();
	else
		lv_obj_set_y(shade, 0);
}

static bool held;

static void radio_hold(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
		held = false;
		return;
	}
	held = true;
	shade_close();
	nav_open(lv_event_get_current_target_obj(e) == wifi ? &scr_wifi : &scr_bt, NULL);
}

static void ctl(lv_event_t *e)
{
	if (held) {
		held = false;
		return;
	}
	switch ((int)(intptr_t)lv_event_get_user_data(e)) {
	case 0:
		mpdc_prev();
		break;
	case 1:
		mpdc_toggle();
		break;
	case 2:
		mpdc_next();
		break;
	case 3:
		sysc_set("wifi.enabled", set_get_bool("wifi.enabled") ? "0" : "1");
		break;
	case 4:
		sysc_set("bt.enabled", set_get_bool("bt.enabled") ? "0" : "1");
		break;
	case 5: {
		static const char *const cycle[] = { "seq", "random", "single", "noloop" };
		const char *o = mpdc_order();
		int i;
		for (i = 0; i < 4 && strcmp(cycle[i], o); i++)
			;
		mpdc_set_order(cycle[(i + 1) % 4]);
		break;
	}
	case 6:
		shade_close();
		sysc_send("screen lock");
		break;
	}
}

static const struct screen_ops *const shortcuts[] = { &scr_np, &scr_folders, &scr_playlists, &scr_queue, &scr_settings };
static const char *const shortcut_syms[] = { LV_SYMBOL_PLAY, LV_SYMBOL_DIRECTORY, LV_SYMBOL_LIST, LV_SYMBOL_SHUFFLE, LV_SYMBOL_SETTINGS };

static void go(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);

	shade_close();
	nav_open(shortcuts[i], NULL);
}

static void vol_changed(lv_event_t *e)
{
	sysc_send("vol %d", (int)lv_slider_get_value(lv_event_get_target_obj(e)));
}

static void bright_changed(lv_event_t *e)
{
	sysc_set_int("ui.brightness", lv_slider_get_value(lv_event_get_target_obj(e)));
}

static lv_obj_t *row(int y, int w, int h)
{
	lv_obj_t *r = lv_obj_create(shade);

	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, w, h);
	lv_obj_align(r, LV_ALIGN_TOP_MID, 0, y);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(r, false);
	return r;
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *sym, int size, lv_event_cb_t cb, intptr_t arg)
{
	lv_obj_t *b = ui_round_button(parent, sym, size, cb, (void *)arg);

	lv_obj_set_style_text_font(lv_obj_get_child(b, 0), size < 50 ? &lv_font_montserrat_20 : app.font.icon, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(0x333333), 0);
	return b;
}

static lv_obj_t *slider(int y, const char *sym, int min, int max, lv_event_cb_t cb, lv_event_code_t code)
{
	lv_obj_t *r = row(y, 250, 36), *l, *s;

	l = ui_label(r, &lv_font_montserrat_20, sym);
	(void)l;
	s = lv_slider_create(r);
	lv_obj_set_width(s, 190);
	lv_slider_set_range(s, min, max);
	lv_obj_add_event_cb(s, cb, code, NULL);
	return s;
}

void shade_init(void)
{
	lv_obj_t *r, *b;

	shade = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(shade);
	lv_obj_set_size(shade, SCR_W, SCR_H);
	lv_obj_set_style_bg_color(shade, lv_color_hex(0x101010), 0);
	lv_obj_set_style_bg_opa(shade, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(shade, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_scrollable(shade, false);

	info = ui_label(shade, &lv_font_montserrat_14, "");
	lv_obj_set_style_text_color(info, lv_color_hex(0x9a9a9a), 0);
	lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 26);
	title = ui_label(shade, app.font.m, "");
	lv_obj_set_size(title, 240, lv_font_get_line_height(app.font.m));
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);
	artist = ui_label(shade, app.font.s, "");
	lv_obj_set_size(artist, 260, lv_font_get_line_height(app.font.s));
	lv_obj_set_style_text_align(artist, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(artist, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_obj_set_style_text_color(artist, lv_color_hex(0xb0b0b0), 0);
	lv_obj_align(artist, LV_ALIGN_TOP_MID, 0, 70);

	r = row(94, 220, 56);
	small_button(r, LV_SYMBOL_PREV, 44, ctl, 0);
	b = small_button(r, LV_SYMBOL_PLAY, 54, ctl, 1);
	lv_obj_set_style_bg_color(b, app.accent, 0);
	play_lbl = lv_obj_get_child(b, 0);
	small_button(r, LV_SYMBOL_NEXT, 44, ctl, 2);

	r = row(156, 300, 50);
	wifi = small_button(r, LV_SYMBOL_WIFI, 46, ctl, 3);
	bt = small_button(r, LV_SYMBOL_BLUETOOTH, 46, ctl, 4);
	for (int i = 0; i < 2; i++) {
		lv_obj_add_event_cb(i ? bt : wifi, radio_hold, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(i ? bt : wifi, radio_hold, LV_EVENT_LONG_PRESSED, NULL);
	}
	order = small_button(r, LV_SYMBOL_LOOP, 46, ctl, 5);
	small_button(r, LV_SYMBOL_EYE_CLOSE, 46, ctl, 6);

	vol = slider(214, LV_SYMBOL_VOLUME_MAX, 0, 100, vol_changed, LV_EVENT_VALUE_CHANGED);
	bright = slider(254, LV_SYMBOL_IMAGE, 1, 40, bright_changed, LV_EVENT_RELEASED);

	r = row(296, 230, 44);
	for (int i = 0; i < 5; i++)
		small_button(r, shortcut_syms[i], 40, go, i);

	app_listen(shade, EV_STATUS | EV_SONG | EV_VOLUME | EV_SETTINGS | EV_OPTIONS | EV_SYS, on_event, NULL);
	shade_close();
}
