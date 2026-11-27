#include "mpdc.h"
#include "ui.h"

#include <disc/settings.h>
#include <stdio.h>
#include <string.h>

#define OPEN_PX 80

static lv_obj_t *shade, *song, *title, *artist, *play_lbl, *wifi, *bt, *vol, *bright, *info;
static bool open_;

static void style_toggle(lv_obj_t *b, bool on)
{
	lv_obj_set_style_bg_color(b, on ? app.accent : lv_color_hex(0x333333), 0);
	lv_obj_set_style_text_color(lv_obj_get_child(b, 0), on ? lv_color_black() : lv_color_white(), 0);
}

static void refresh(void)
{
	char line[96];

	if (!shade || !open_)
		return;
	lv_label_set_text(title, app.mpd.cur.uri[0] ? app.mpd.cur.title : "Nothing playing");
	lv_label_set_text(artist, app.mpd.cur.artist);
	lv_label_set_text(play_lbl, app.mpd.state == PS_PLAY ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	style_toggle(wifi, set_get_bool("wifi.enabled"));
	style_toggle(bt, set_get_bool("bt.enabled"));
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
	}
}

static void go(lv_event_t *e)
{
	shade_close();
	nav_open(lv_event_get_user_data(e), NULL);
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

static lv_obj_t *small_button(lv_obj_t *parent, const char *sym, int size, lv_event_cb_t cb, void *arg)
{
	lv_obj_t *b = ui_round_button(parent, sym, size, cb, arg);

	lv_obj_set_style_text_font(lv_obj_get_child(b, 0), size < 50 ? &lv_font_montserrat_20 : app.font.icon, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(0x333333), 0);
	return b;
}

static lv_obj_t *slider(int y, const char *sym, int min, int max, lv_event_cb_t cb, lv_event_code_t code)
{
	lv_obj_t *r = row(y, 260, 32), *s;

	ui_label(r, &lv_font_montserrat_20, sym);
	s = lv_slider_create(r);
	lv_obj_set_size(s, 200, 12);
	lv_obj_set_ext_click_area(s, 8);
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
	lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 22);

	song = lv_obj_create(shade);
	lv_obj_remove_style_all(song);
	lv_obj_set_size(song, 276, 52);
	lv_obj_align(song, LV_ALIGN_TOP_MID, 0, 40);
	lv_obj_set_style_radius(song, 16, 0);
	lv_obj_set_style_bg_color(song, lv_color_hex(0x2a2a2a), 0);
	lv_obj_set_style_bg_opa(song, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_scrollable(song, false);
	lv_obj_set_clickable(song, true);
	lv_obj_add_event_cb(song, go, LV_EVENT_CLICKED, (void *)&scr_np);
	title = ui_label(song, app.font.m, "");
	lv_obj_set_size(title, 250, lv_font_get_line_height(app.font.m));
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);
	artist = ui_label(song, app.font.s, "");
	lv_obj_set_size(artist, 250, lv_font_get_line_height(app.font.s));
	lv_obj_set_style_text_align(artist, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(artist, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_obj_set_style_text_color(artist, lv_color_hex(0xb0b0b0), 0);
	lv_obj_align(artist, LV_ALIGN_BOTTOM_MID, 0, -2);

	r = row(98, 264, 66);
	small_button(r, LV_SYMBOL_PREV, 54, ctl, (void *)0);
	b = small_button(r, LV_SYMBOL_PLAY, 66, ctl, (void *)1);
	lv_obj_set_style_bg_color(b, app.accent, 0);
	play_lbl = lv_obj_get_child(b, 0);
	small_button(r, LV_SYMBOL_NEXT, 54, ctl, (void *)2);

	r = row(170, 220, 56);
	wifi = small_button(r, LV_SYMBOL_WIFI, 56, ctl, (void *)3);
	bt = small_button(r, LV_SYMBOL_BLUETOOTH, 56, ctl, (void *)4);
	for (int i = 0; i < 2; i++) {
		lv_obj_add_event_cb(i ? bt : wifi, radio_hold, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(i ? bt : wifi, radio_hold, LV_EVENT_LONG_PRESSED, NULL);
	}

	vol = slider(234, LV_SYMBOL_VOLUME_MAX, 0, 100, vol_changed, LV_EVENT_VALUE_CHANGED);
	bright = slider(272, LV_SYMBOL_IMAGE, 1, 40, bright_changed, LV_EVENT_RELEASED);

	b = small_button(shade, LV_SYMBOL_SETTINGS, 48, go, (void *)&scr_settings);
	lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 308);

	app_listen(shade, EV_STATUS | EV_SONG | EV_VOLUME | EV_SETTINGS | EV_OPTIONS | EV_SYS, on_event, NULL);
	shade_close();
}
