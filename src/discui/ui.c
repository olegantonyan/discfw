#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MODALS 6
#define TOAST_MS 2500
#define VOLUME_MS 1500

struct modal {
	lv_obj_t *root;
	void (*on_close)(struct modal *m, int result);
	void *cb, *ctx;
	char **items;
	int n_items;
	lv_obj_t *ta, *value, *slider;
	int val, min, max, step;
	char unit[16];
};

static struct modal *modals[MAX_MODALS];
static int n_modals;
static lv_obj_t *toast, *volume, *vol_arc, *vol_label, *shutdown, *lowbat;
static lv_timer_t *toast_timer, *volume_timer;

const char *fmt_time(char *buf, size_t len, unsigned ms)
{
	unsigned s = ms / 1000;

	if (s >= 3600)
		snprintf(buf, len, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(buf, len, "%u:%02u", s / 60, s % 60);
	return buf;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_label_set_text(l, text ? text : "");
	return l;
}

lv_obj_t *ui_screen(const char *title, lv_obj_t **title_label)
{
	lv_obj_t *scr = lv_obj_create(NULL), *t;

	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_scrollable(scr, false);
	t = ui_label(scr, app.font.title, title);
	lv_obj_set_size(t, 230, lv_font_get_line_height(app.font.title));
	lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_obj_align(t, LV_ALIGN_TOP_MID, 0, HEADER_Y);
	if (title_label)
		*title_label = t;
	return scr;
}

lv_obj_t *ui_round_button(lv_obj_t *parent, const char *sym, int size, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *b = lv_button_create(parent), *l = lv_label_create(b);

	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_label_set_text(l, sym);
	lv_obj_set_style_text_font(l, size >= 70 ? app.font.icon_big : app.font.icon, 0);
	lv_obj_center(l);
	if (cb)
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	return b;
}

lv_obj_t *ui_text_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *b = lv_button_create(parent), *l = lv_label_create(b);

	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_radius(b, 20, 0);
	lv_obj_set_style_text_font(l, app.font.m, 0);
	lv_label_set_text(l, text);
	lv_obj_center(l);
	if (cb)
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	return b;
}

static void toast_hide(lv_timer_t *t)
{
	(void)t;
	toast_timer = NULL;
	if (toast)
		lv_obj_set_hidden(toast, true);
}

void ui_toast(const char *text)
{
	if (!toast) {
		toast = lv_label_create(lv_layer_top());
		lv_obj_set_style_text_font(toast, app.font.m, 0);
		lv_obj_set_style_bg_color(toast, lv_color_hex(0x303030), 0);
		lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
		lv_obj_set_style_radius(toast, 14, 0);
		lv_obj_set_style_pad_hor(toast, 14, 0);
		lv_obj_set_style_pad_ver(toast, 8, 0);
		lv_obj_set_style_text_align(toast, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(toast, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_max_width(toast, 250, 0);
		lv_obj_set_clickable(toast, false);
	}
	lv_label_set_text(toast, text);
	lv_obj_set_width(toast, LV_SIZE_CONTENT);
	lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -46);
	lv_obj_set_hidden(toast, false);
	lv_obj_move_foreground(toast);
	if (toast_timer)
		lv_timer_delete(toast_timer);
	toast_timer = lv_timer_create(toast_hide, TOAST_MS, NULL);
	lv_timer_set_repeat_count(toast_timer, 1);
}

static void volume_hide(lv_timer_t *t)
{
	(void)t;
	volume_timer = NULL;
	lv_obj_set_hidden(volume, true);
}

static void volume_changed(uint32_t evs, void *ctx)
{
	(void)evs;
	(void)ctx;
	if (lv_obj_is_hidden(volume))
		return;
	lv_arc_set_value(vol_arc, app.sys.volume);
	lv_label_set_text_fmt(vol_label, "%s %d", app.sys.out_bt ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_VOLUME_MAX, app.sys.volume);
}

void ui_volume_overlay(void)
{
	if (!volume) {
		volume = lv_obj_create(lv_layer_top());
		lv_obj_remove_style_all(volume);
		lv_obj_set_size(volume, SCR_W, SCR_H);
		lv_obj_set_clickable(volume, false);
		vol_arc = lv_arc_create(volume);
		lv_obj_set_size(vol_arc, SCR_W - 6, SCR_H - 6);
		lv_obj_center(vol_arc);
		lv_arc_set_rotation(vol_arc, 135);
		lv_arc_set_bg_angles(vol_arc, 0, 270);
		lv_arc_set_range(vol_arc, 0, 100);
		lv_obj_set_style_arc_width(vol_arc, 10, LV_PART_MAIN);
		lv_obj_set_style_arc_width(vol_arc, 10, LV_PART_INDICATOR);
		lv_obj_set_style_arc_color(vol_arc, lv_palette_main(LV_PALETTE_AMBER), LV_PART_INDICATOR);
		lv_obj_remove_style(vol_arc, NULL, LV_PART_KNOB);
		lv_obj_set_clickable(vol_arc, false);
		vol_label = ui_label(volume, &lv_font_montserrat_24, "");
		lv_obj_set_style_bg_color(vol_label, lv_color_hex(0x202020), 0);
		lv_obj_set_style_bg_opa(vol_label, LV_OPA_90, 0);
		lv_obj_set_style_radius(vol_label, 18, 0);
		lv_obj_set_style_pad_all(vol_label, 10, 0);
		lv_obj_align(vol_label, LV_ALIGN_BOTTOM_MID, 0, -20);
		app_listen(volume, EV_VOLUME, volume_changed, NULL);
	}
	lv_obj_set_hidden(volume, false);
	lv_obj_move_foreground(volume);
	volume_changed(0, NULL);
	if (volume_timer)
		lv_timer_reset(volume_timer);
	else {
		volume_timer = lv_timer_create(volume_hide, VOLUME_MS, NULL);
		lv_timer_set_repeat_count(volume_timer, 1);
	}
}

static lv_obj_t *fullscreen_overlay(void)
{
	lv_obj_t *o = lv_obj_create(lv_layer_top());

	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, SCR_W, SCR_H);
	lv_obj_set_style_bg_color(o, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	return o;
}

void ui_shutdown_overlay(bool reboot)
{
	if (!shutdown) {
		shutdown = fullscreen_overlay();
		lv_obj_t *l = ui_label(shutdown, app.font.l, reboot ? "Restarting…" : "Powering off…");
		lv_obj_center(l);
	}
	lv_obj_move_foreground(shutdown);
	lv_refr_now(NULL);
}

void lowbat_update(void)
{
	if (app.sys.lowbat <= 0) {
		if (lowbat)
			lv_obj_set_hidden(lowbat, true);
		return;
	}
	if (!lowbat) {
		lowbat = fullscreen_overlay();
		lv_obj_set_style_bg_opa(lowbat, LV_OPA_80, 0);
		lv_obj_t *l = ui_label(lowbat, app.font.l, "");
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_center(l);
	}
	lv_label_set_text_fmt(lv_obj_get_child(lowbat, 0), LV_SYMBOL_BATTERY_EMPTY "\nBattery empty\nPowering off in %d s\n\nConnect a charger", app.sys.lowbat);
	lv_obj_set_hidden(lowbat, false);
	lv_obj_move_foreground(lowbat);
}

bool ui_modal_active(void)
{
	return n_modals > 0;
}

static void modal_free(struct modal *m)
{
	for (int i = 0; i < m->n_items; i++)
		free(m->items[i]);
	free(m->items);
	free(m);
}

static void modal_finish(struct modal *m, int result)
{
	int i;

	for (i = 0; i < n_modals && modals[i] != m; i++)
		;
	if (i == n_modals)
		return;
	memmove(modals + i, modals + i + 1, (n_modals - i - 1) * sizeof(*modals));
	n_modals--;
	lv_obj_delete_async(m->root);
	if (m->on_close)
		m->on_close(m, result);
	modal_free(m);
}

bool ui_modal_close(void)
{
	if (!n_modals)
		return false;
	modal_finish(modals[n_modals - 1], -1);
	return true;
}

void ui_modal_close_all(void)
{
	while (ui_modal_close())
		;
}

static struct modal *modal_new(const char *title)
{
	struct modal *m = calloc(1, sizeof(*m));

	m->root = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(m->root);
	lv_obj_set_size(m->root, SCR_W, SCR_H);
	lv_obj_set_style_bg_color(m->root, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(m->root, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(m->root, false);
	if (title && *title) {
		lv_obj_t *t = ui_label(m->root, app.font.title, title);
		lv_obj_set_size(t, 220, lv_font_get_line_height(app.font.title));
		lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
		lv_obj_set_style_text_color(t, app.accent, 0);
		lv_obj_align(t, LV_ALIGN_TOP_MID, 0, HEADER_Y);
	}
	if (n_modals < MAX_MODALS)
		modals[n_modals++] = m;
	return m;
}

static void menu_closed(struct modal *m, int result)
{
	if (result >= 0 && m->cb)
		((ui_pick_cb)m->cb)(result, m->ctx);
}

static int menu_count(void *ctx)
{
	return ((struct modal *)ctx)->n_items;
}

static void menu_row(void *ctx, int idx, struct vrow *r)
{
	struct modal *m = ctx;

	snprintf(r->text, sizeof(r->text), "%s", m->items[idx]);
	r->active = idx == m->val;
	if (m->val >= 0)
		r->icon = idx == m->val ? LV_SYMBOL_OK : " ";
}

static void menu_click(void *ctx, int idx)
{
	modal_finish(ctx, idx);
}

static const struct vlist_ops menu_ops = { .count = menu_count, .row = menu_row, .click = menu_click };

static void open_menu(const char *title, const char *const *items, int n, int selected, ui_pick_cb cb, void *ctx)
{
	struct modal *m = modal_new(title);
	lv_obj_t *list;

	m->cb = cb;
	m->ctx = ctx;
	m->val = selected;
	m->on_close = menu_closed;
	m->items = calloc(n ? n : 1, sizeof(char *));
	m->n_items = n;
	for (int i = 0; i < n; i++)
		m->items[i] = strdup(items[i]);
	list = vlist_create(m->root, &menu_ops, m);
	lv_obj_set_size(list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(list, 0, CONTENT_Y);
	vlist_reload(list);
	if (selected > 3)
		vlist_scroll_to(list, selected, true);
}

void ui_menu(const char *title, const char *const *items, int n, ui_pick_cb cb, void *ctx)
{
	open_menu(title, items, n, -1, cb, ctx);
}

void ui_pick(const char *title, const char *const *items, int n, int selected, ui_pick_cb cb, void *ctx)
{
	open_menu(title, items, n, selected < 0 ? n + 1 : selected, cb, ctx);
}

static void confirm_closed(struct modal *m, int result)
{
	if (result == 1 && m->cb)
		((void (*)(void *))m->cb)(m->ctx);
}

static void btn_result(lv_event_t *e)
{
	struct modal *m = lv_event_get_user_data(e);
	modal_finish(m, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e)));
}

static void two_buttons(struct modal *m, const char *no, const char *yes, int y)
{
	lv_obj_t *b;

	b = ui_text_button(m->root, no, btn_result, m);
	lv_obj_set_user_data(b, (void *)(intptr_t)0);
	lv_obj_set_size(b, 110, 48);
	lv_obj_align(b, LV_ALIGN_CENTER, -62, y);
	lv_obj_set_style_bg_color(b, lv_color_hex(0x404040), 0);
	b = ui_text_button(m->root, yes, btn_result, m);
	lv_obj_set_user_data(b, (void *)(intptr_t)1);
	lv_obj_set_size(b, 110, 48);
	lv_obj_align(b, LV_ALIGN_CENTER, 62, y);
}

void ui_confirm(const char *text, const char *yes, void (*cb)(void *ctx), void *ctx)
{
	struct modal *m = modal_new(NULL);
	lv_obj_t *l = ui_label(m->root, app.font.l, text);

	m->cb = cb;
	m->ctx = ctx;
	m->on_close = confirm_closed;
	lv_obj_set_width(l, 260);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(l, LV_ALIGN_CENTER, 0, -40);
	two_buttons(m, "Cancel", yes, 60);
}

void ui_info(const char *title, const char *text)
{
	struct modal *m = modal_new(title);
	lv_obj_t *box = lv_obj_create(m->root), *l;

	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, 240, 240);
	lv_obj_set_pos(box, (SCR_W - 240) / 2, CONTENT_Y);
	lv_obj_set_scroll_dir(box, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
	l = ui_label(box, app.font.s, text);
	lv_obj_set_width(l, 240);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
}

static void text_closed(struct modal *m, int result)
{
	if (result == 1 && m->cb)
		((void (*)(const char *, void *))m->cb)(lv_textarea_get_text(m->ta), m->ctx);
}

static void kb_event(lv_event_t *e)
{
	struct modal *m = lv_event_get_user_data(e);
	modal_finish(m, lv_event_get_code(e) == LV_EVENT_READY ? 1 : 0);
}

void ui_text_input(const char *title, const char *initial, bool secret, void (*cb)(const char *text, void *ctx), void *ctx)
{
	struct modal *m = modal_new(title);
	lv_obj_t *kb;

	m->cb = cb;
	m->ctx = ctx;
	m->on_close = text_closed;
	m->ta = lv_textarea_create(m->root);
	lv_textarea_set_one_line(m->ta, true);
	lv_textarea_set_password_mode(m->ta, secret);
	lv_textarea_set_text(m->ta, initial ? initial : "");
	lv_obj_set_width(m->ta, 240);
	lv_obj_set_style_text_font(m->ta, app.font.m, 0);
	lv_obj_align(m->ta, LV_ALIGN_TOP_MID, 0, CONTENT_Y);
	kb = lv_keyboard_create(m->root);
	lv_obj_set_size(kb, 280, 176);
	lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 112);
	lv_obj_set_style_text_font(kb, &lv_font_montserrat_16, 0);
	lv_obj_set_style_pad_all(kb, 2, 0);
	lv_obj_set_style_pad_gap(kb, 3, 0);
	lv_keyboard_set_textarea(kb, m->ta);
	lv_obj_add_event_cb(kb, kb_event, LV_EVENT_READY, m);
	lv_obj_add_event_cb(kb, kb_event, LV_EVENT_CANCEL, m);
}

static void number_show(struct modal *m)
{
	lv_label_set_text_fmt(m->value, "%d %s", m->val, m->unit);
	lv_slider_set_value(m->slider, m->val, LV_ANIM_OFF);
}

static void number_step(lv_event_t *e)
{
	struct modal *m = lv_event_get_user_data(e);
	int dir = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));

	m->val += dir * m->step;
	if (m->val < m->min)
		m->val = m->min;
	if (m->val > m->max)
		m->val = m->max;
	number_show(m);
}

static void number_slide(lv_event_t *e)
{
	struct modal *m = lv_event_get_user_data(e);
	int v = lv_slider_get_value(m->slider);

	m->val = m->min + (v - m->min) / m->step * m->step;
	lv_label_set_text_fmt(m->value, "%d %s", m->val, m->unit);
}

static void number_closed(struct modal *m, int result)
{
	if (result == 1 && m->cb)
		((void (*)(int, void *))m->cb)(m->val, m->ctx);
}

void ui_number_input(const char *title, int val, int min, int max, int step, const char *unit, void (*cb)(int v, void *ctx), void *ctx)
{
	struct modal *m = modal_new(title);
	lv_obj_t *b;

	m->cb = cb;
	m->ctx = ctx;
	m->on_close = number_closed;
	m->val = val;
	m->min = min;
	m->max = max;
	m->step = step > 0 ? step : 1;
	snprintf(m->unit, sizeof(m->unit), "%s", unit ? unit : "");
	m->value = ui_label(m->root, app.font.clock, "");
	lv_obj_align(m->value, LV_ALIGN_CENTER, 0, -50);
	b = ui_round_button(m->root, LV_SYMBOL_MINUS, 52, number_step, m);
	lv_obj_set_user_data(b, (void *)(intptr_t)-1);
	lv_obj_align(b, LV_ALIGN_CENTER, -105, -50);
	b = ui_round_button(m->root, LV_SYMBOL_PLUS, 52, number_step, m);
	lv_obj_set_user_data(b, (void *)(intptr_t)1);
	lv_obj_align(b, LV_ALIGN_CENTER, 105, -50);
	m->slider = lv_slider_create(m->root);
	lv_obj_set_width(m->slider, 240);
	lv_slider_set_range(m->slider, min, max);
	lv_obj_align(m->slider, LV_ALIGN_CENTER, 0, 20);
	lv_obj_add_event_cb(m->slider, number_slide, LV_EVENT_VALUE_CHANGED, m);
	number_show(m);
	two_buttons(m, "Cancel", "OK", 90);
}
