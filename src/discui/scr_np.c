#include "mpdc.h"
#include "ui.h"

#include <disc/settings.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING 350
#define BAND_IN 140
#define BAND_OUT 190
#define SEEK_MOVE 8
#define COVER 172
#define VINYL 180
#define LABEL 86
#define SRC 360
#define SCRUB_MS_PER_TURN 60000

struct np {
	lv_obj_t *scr, *ring, *time, *fmt, *box, *img, *disc, *title, *artist, *play_lbl, *seek_lbl, *full;
	lv_timer_t *anim;
	int angle, mode;
	bool seek_armed, seeking, scrub;
	lv_point_t press;
	unsigned seek_ms, scrub_start, scrub_target;
	float scrub_last, scrub_acc;
};

enum { CM_NONE, CM_STATIC, CM_ROTATING, CM_VINYL };

static unsigned total_ms(void)
{
	return app.mpd.total_ms ? app.mpd.total_ms : app.mpd.cur.duration_ms;
}

static void update_time(struct np *n)
{
	char a[16], b[16];
	unsigned e = mpd_elapsed_now(), t = total_ms();

	if (n->seeking || n->scrub)
		return;
	if (app.mpd.state == PS_STOP || app.mpd.state == PS_UNKNOWN) {
		lv_label_set_text(n->time, "");
		lv_arc_set_value(n->ring, 0);
		return;
	}
	lv_label_set_text_fmt(n->time, "%s / %s", fmt_time(a, sizeof(a), e), t ? fmt_time(b, sizeof(b), t) : "--:--");
	lv_arc_set_value(n->ring, t ? (int)((uint64_t)e * 1000 / t) : 0);
}

static void update_format(struct np *n)
{
	const char *uri = app.mpd.cur.uri, *dot = strrchr(uri, '.');
	char ext[8] = "";

	if (dot && strlen(dot) < sizeof(ext))
		for (int i = 0; dot[i + 1]; i++)
			ext[i] = (char)(dot[i + 1] >= 'a' && dot[i + 1] <= 'z' ? dot[i + 1] - 32 : dot[i + 1]), ext[i + 1] = 0;
	if (!app.mpd.rate) {
		lv_label_set_text(n->fmt, ext);
		return;
	}
	lv_label_set_text_fmt(n->fmt, "%s  %u.%u kHz / %u bit  %s", ext, app.mpd.rate / 1000, app.mpd.rate % 1000 / 100,
			      app.mpd.bits, app.sys.out_bt ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_VOLUME_MAX);
}

static bool animate(struct np *n)
{
	char key[48];

	snprintf(key, sizeof(key), "power.%s.anim", app.sys.profile[0] ? app.sys.profile : "balanced");
	return !n->scrub && (n->mode == CM_ROTATING || n->mode == CM_VINYL) && app.mpd.state == PS_PLAY && app.sys.screen_on &&
	       lv_screen_active() == n->scr && set_get_bool(key);
}

static void anim_cb(lv_timer_t *t)
{
	struct np *n = lv_timer_get_user_data(t);
	int fps = set_get_int("ui.anim_fps");
	int per_s = n->mode == CM_VINYL ? 2000 : 300;

	if (!animate(n))
		return;
	n->angle = (n->angle + per_s / (fps > 0 ? fps : 10)) % 3600;
	lv_image_set_rotation(n->img, n->angle);
}

static void update_anim(struct np *n)
{
	int fps = set_get_int("ui.anim_fps");

	if (animate(n)) {
		if (!n->anim)
			n->anim = lv_timer_create(anim_cb, 1000 / (fps > 0 ? fps : 10), n);
	} else if (n->anim) {
		lv_timer_delete(n->anim);
		n->anim = NULL;
	}
}

static void update_cover(struct np *n)
{
	const lv_image_dsc_t *c = cover_current();
	int size = n->mode == CM_VINYL ? LABEL : COVER;

	if (n->mode == CM_NONE)
		return;
	if (!c)
		c = n->mode == CM_VINYL ? vinyl_label() : cover_placeholder();
	lv_image_set_src(n->img, c);
	lv_image_set_pivot(n->img, c->header.w / 2, c->header.h / 2);
	lv_image_set_scale(n->img, 256 * size / (c->header.w ? c->header.w : SRC));
	lv_obj_center(n->img);
}

static void layout_mode(struct np *n)
{
	n->mode = set_get_enum("ui.cover_mode");
	lv_obj_set_hidden(n->box, n->mode == CM_NONE);
	lv_obj_set_hidden(n->disc, n->mode != CM_VINYL);
	if (n->mode == CM_VINYL) {
		lv_obj_set_size(n->box, LABEL, LABEL);
		lv_obj_set_style_radius(n->box, LV_RADIUS_CIRCLE, 0);
		lv_image_set_src(n->disc, vinyl_disc());
		lv_image_set_scale(n->disc, 256 * VINYL / vinyl_disc()->header.w);
	} else {
		lv_obj_set_size(n->box, COVER, COVER);
		lv_obj_set_style_radius(n->box, n->mode == CM_STATIC ? 14 : LV_RADIUS_CIRCLE, 0);
	}
	lv_obj_align(n->box, LV_ALIGN_CENTER, 0, -30);
	lv_obj_align(n->disc, LV_ALIGN_CENTER, 0, -30);
	lv_obj_set_width(n->title, n->mode == CM_NONE ? 280 : 250);
	lv_obj_align(n->title, LV_ALIGN_CENTER, 0, n->mode == CM_NONE ? -30 : 72);
	lv_obj_align(n->artist, LV_ALIGN_CENTER, 0, n->mode == CM_NONE ? 4 : 98);
	n->angle = 0;
	lv_image_set_rotation(n->img, 0);
	update_cover(n);
}

static void update_song(struct np *n)
{
	char line[600];
	const struct song *s = &app.mpd.cur;

	if (!s->uri[0]) {
		lv_label_set_text(n->title, app.mpd.connected ? "Nothing playing" : "Music service starting…");
		lv_label_set_text(n->artist, "");
		return;
	}
	if (strcmp(lv_label_get_text(n->title), s->title))
		lv_label_set_text(n->title, s->title);
	snprintf(line, sizeof(line), "%s%s%s", s->artist, s->artist[0] && s->album[0] ? " — " : "", s->album);
	lv_label_set_text(n->artist, line);
}

static void update_state(struct np *n)
{
	lv_label_set_text(n->play_lbl, app.mpd.state == PS_PLAY ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	update_time(n);
	update_format(n);
	update_anim(n);
}

static void on_event(uint32_t evs, void *ctx)
{
	struct np *n = ctx;

	if (evs & EV_SETTINGS)
		layout_mode(n);
	if (evs & EV_SONG)
		update_song(n);
	if (evs & EV_COVER)
		update_cover(n);
	if (evs & (EV_STATUS | EV_SYS | EV_SETTINGS | EV_VOLUME))
		update_state(n);
	if (evs & EV_SECOND)
		update_time(n);
}

static void on_ctl(lv_event_t *e)
{
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
	}
}

static unsigned angle_ms(lv_point_t p)
{
	float a = atan2f((float)(p.x - SCR_R), (float)(SCR_R - p.y));

	if (a < 0)
		a += 2 * (float)M_PI;
	return (unsigned)(a / (2 * (float)M_PI) * total_ms());
}

static int radius(lv_point_t p)
{
	int dx = p.x - SCR_R, dy = p.y - SCR_R;
	return (int)sqrtf((float)(dx * dx + dy * dy));
}

static void on_touch(lv_event_t *e)
{
	struct np *n = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_point_t p;
	char a[16];

	lv_indev_get_point(lv_indev_active(), &p);
	if (code == LV_EVENT_PRESSED) {
		int r = radius(p);
		n->seek_armed = r >= BAND_IN && r <= BAND_OUT && total_ms() && app.mpd.state != PS_STOP;
		n->seeking = false;
		n->press = p;
		return;
	}
	if (code == LV_EVENT_PRESSING && n->seek_armed) {
		if (!n->seeking && abs(p.x - n->press.x) + abs(p.y - n->press.y) < SEEK_MOVE)
			return;
		n->seeking = true;
		n->seek_ms = angle_ms(p);
		lv_arc_set_value(n->ring, (int)((uint64_t)n->seek_ms * 1000 / total_ms()));
		lv_label_set_text(n->seek_lbl, fmt_time(a, sizeof(a), n->seek_ms));
		lv_obj_set_hidden(n->seek_lbl, false);
		return;
	}
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		if (n->seeking && code == LV_EVENT_RELEASED)
			mpdc_seek_ms(n->seek_ms);
		n->seeking = n->seek_armed = false;
		lv_obj_set_hidden(n->seek_lbl, true);
		update_time(n);
	}
}

static float touch_angle(struct np *n)
{
	lv_point_t p;
	lv_area_t a;

	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(n->box, &a);
	return atan2f(p.y - (a.y1 + a.y2) / 2.0f, p.x - (a.x1 + a.x2) / 2.0f);
}

static void scrub_end(struct np *n, bool commit)
{
	if (!n->scrub)
		return;
	n->scrub = false;
	if (commit)
		mpdc_seek_ms(n->scrub_target);
	lv_image_set_rotation(n->img, n->angle);
	lv_obj_set_hidden(n->seek_lbl, true);
	update_state(n);
}

static void scrub(lv_event_t *e)
{
	struct np *n = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	char a[16], b[24];

	if (code == LV_EVENT_LONG_PRESSED) {
		if (!total_ms() || app.mpd.state == PS_STOP || app.mpd.state == PS_UNKNOWN)
			return;
		n->scrub = true;
		n->scrub_last = touch_angle(n);
		n->scrub_acc = 0;
		n->scrub_start = n->scrub_target = mpd_elapsed_now();
		lv_label_set_text(n->seek_lbl, fmt_time(a, sizeof(a), n->scrub_target));
		lv_obj_set_hidden(n->seek_lbl, false);
		update_anim(n);
		return;
	}
	if (!n->scrub)
		return;
	if (code == LV_EVENT_PRESSING) {
		float ang = touch_angle(n), d = ang - n->scrub_last;
		int64_t t;
		if (d > (float)M_PI)
			d -= 2 * (float)M_PI;
		else if (d < -(float)M_PI)
			d += 2 * (float)M_PI;
		n->scrub_last = ang;
		n->scrub_acc += d;
		t = (int64_t)n->scrub_start + (int64_t)(n->scrub_acc / (2 * (float)M_PI) * SCRUB_MS_PER_TURN);
		if (t < 0)
			t = 0;
		if (t > (int64_t)total_ms() - 1000)
			t = total_ms() > 1000 ? total_ms() - 1000 : 0;
		n->scrub_target = (unsigned)t;
		lv_image_set_rotation(n->img, n->angle + (int)(n->scrub_acc * 1800 / (float)M_PI));
		lv_label_set_text(n->seek_lbl, fmt_time(a, sizeof(a), n->scrub_target));
		unsigned delta = n->scrub_target > n->scrub_start ? n->scrub_target - n->scrub_start : n->scrub_start - n->scrub_target;
		snprintf(b, sizeof(b), "%s%s", n->scrub_target >= n->scrub_start ? ">> +" : "<< -", fmt_time(a, sizeof(a), delta));
		lv_label_set_text(n->time, b);
		lv_arc_set_value(n->ring, (int)((uint64_t)n->scrub_target * 1000 / total_ms()));
	} else if (code == LV_EVENT_RELEASED) {
		scrub_end(n, true);
	} else if (code == LV_EVENT_PRESS_LOST) {
		scrub_end(n, false);
	}
}

static void full_close(lv_event_t *e)
{
	struct np *n = lv_event_get_user_data(e);

	lv_obj_delete(n->full);
	n->full = NULL;
}

static void cover_click(lv_event_t *e)
{
	struct np *n = lv_event_get_user_data(e);
	const lv_image_dsc_t *c = cover_current();
	lv_obj_t *img;

	if (!c || n->full)
		return;
	n->full = lv_obj_create(n->scr);
	lv_obj_remove_style_all(n->full);
	lv_obj_set_size(n->full, SCR_W, SCR_H);
	lv_obj_set_style_bg_color(n->full, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(n->full, LV_OPA_COVER, 0);
	img = lv_image_create(n->full);
	lv_image_set_src(img, c);
	lv_obj_center(img);
	lv_obj_set_clickable(img, false);
	lv_obj_add_event_cb(n->full, full_close, LV_EVENT_CLICKED, n);
}

static const char *const orders[] = { "seq", "random", "noloop", "single", "once" };
static const char *const order_labels[] = { "Sequential", "Shuffle", "Sequential, no loop", "Repeat one", "Single track" };

static void order_picked(int idx, void *ctx)
{
	(void)ctx;
	mpdc_set_order(orders[idx]);
}

static const char *const cover_modes[] = { "none", "static", "rotating", "vinyl" };
static const char *const cover_labels[] = { "None", "Static", "Rotating", "Vinyl" };

static void cover_picked(int idx, void *ctx)
{
	(void)ctx;
	sysc_set("ui.cover_mode", cover_modes[idx]);
}

static const int sleep_mins[] = { 0, 15, 30, 45, 60, 90, 120 };

static void sleep_picked(int idx, void *ctx)
{
	(void)ctx;
	sysc_send("sleep %d", sleep_mins[idx]);
}

static void song_info(void)
{
	const struct song *s = &app.mpd.cur;
	char buf[2600], d[16];

	snprintf(buf, sizeof(buf),
		 "Title: %s\nArtist: %s\nAlbum: %s\nAlbum artist: %s\nDate: %s\nGenre: %s\nTrack: %s  Disc: %s\n"
		 "Length: %s\nFormat: %u Hz, %u bit, %u ch\nBitrate: %u kbps\nOutput: %s\n\n%s",
		 s->title, s->artist, s->album, s->albumartist, s->date, s->genre, s->track, s->disc,
		 fmt_time(d, sizeof(d), total_ms()), app.mpd.rate, app.mpd.bits, app.mpd.channels, app.mpd.kbps,
		 app.sys.out_bt ? "Bluetooth" : "DAC", s->uri);
	ui_info("Song info", buf);
}

static void add_to_playlist(void)
{
	char *u = app.mpd.cur.uri;

	if (!u[0] || !strncmp(u, "file://", 7) || u[0] == '/') {
		ui_toast("Song not in library yet");
		return;
	}
	playlist_add_uris(&u, 1, "");
}

static void menu_picked(int idx, void *ctx)
{
	const char *const sleep_labels[] = { "Off", "15 min", "30 min", "45 min", "1 hour", "1.5 hours", "2 hours" };
	int cur = 0;

	(void)ctx;
	switch (idx) {
	case 0:
		for (int i = 0; i < 5; i++)
			if (!strcmp(orders[i], mpdc_order()))
				cur = i;
		ui_pick("Play order", order_labels, 5, cur, order_picked, NULL);
		break;
	case 1:
		nav_push(&scr_queue, NULL);
		break;
	case 2:
		add_to_playlist();
		break;
	case 3:
		folders_open_uri(app.mpd.cur.uri);
		break;
	case 4:
		song_info();
		break;
	case 5:
		ui_pick("Cover", cover_labels, 4, set_get_enum("ui.cover_mode"), cover_picked, NULL);
		break;
	case 6:
		ui_pick("Sleep timer", sleep_labels, 7, -1, sleep_picked, NULL);
		break;
	}
}

static void open_menu(lv_event_t *e)
{
	static const char *const items[] = { "Play order", "Queue", "Add to playlist…", "Go to folder", "Song info", "Cover style",
					     "Sleep timer" };
	(void)e;
	ui_menu("Now playing", items, 7, menu_picked, NULL);
}

static void open_queue(lv_event_t *e)
{
	(void)e;
	nav_push(&scr_queue, NULL);
}

static void on_delete(lv_event_t *e)
{
	struct np *n = lv_event_get_user_data(e);

	if (n->anim)
		lv_timer_delete(n->anim);
	free(n);
}

static lv_obj_t *create(void *arg)
{
	struct np *n = calloc(1, sizeof(*n));
	lv_obj_t *row, *b;

	(void)arg;
	n->scr = ui_screen("", NULL);
	lv_obj_set_user_data(n->scr, n);
	lv_obj_set_clickable(n->scr, true);
	lv_obj_add_event_cb(n->scr, on_touch, LV_EVENT_PRESSED, n);
	lv_obj_add_event_cb(n->scr, on_touch, LV_EVENT_PRESSING, n);
	lv_obj_add_event_cb(n->scr, on_touch, LV_EVENT_RELEASED, n);
	lv_obj_add_event_cb(n->scr, on_touch, LV_EVENT_PRESS_LOST, n);

	n->ring = lv_arc_create(n->scr);
	lv_obj_set_size(n->ring, RING, RING);
	lv_obj_center(n->ring);
	lv_arc_set_rotation(n->ring, 270);
	lv_arc_set_bg_angles(n->ring, 0, 360);
	lv_arc_set_range(n->ring, 0, 1000);
	lv_obj_set_style_arc_width(n->ring, 6, LV_PART_MAIN);
	lv_obj_set_style_arc_width(n->ring, 6, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(n->ring, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
	lv_obj_set_style_pad_all(n->ring, 3, LV_PART_KNOB);
	lv_obj_set_clickable(n->ring, false);

	n->time = ui_label(n->scr, app.font.s, "");
	lv_obj_align(n->time, LV_ALIGN_TOP_MID, 0, 26);
	n->fmt = ui_label(n->scr, &lv_font_montserrat_14, "");
	lv_obj_set_style_text_color(n->fmt, lv_color_hex(0x8a8a8a), 0);
	lv_obj_align(n->fmt, LV_ALIGN_TOP_MID, 0, 46);

	n->disc = lv_image_create(n->scr);
	lv_obj_set_clickable(n->disc, false);
	n->box = lv_obj_create(n->scr);
	lv_obj_remove_style_all(n->box);
	lv_obj_set_style_clip_corner(n->box, true, 0);
	lv_obj_set_style_bg_color(n->box, lv_color_hex(0x202020), 0);
	lv_obj_set_style_bg_opa(n->box, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(n->box, false);
	lv_obj_add_event_cb(n->box, cover_click, LV_EVENT_SHORT_CLICKED, n);
	lv_obj_set_press_lock(n->box, true);
	lv_obj_add_event_cb(n->box, scrub, LV_EVENT_LONG_PRESSED, n);
	lv_obj_add_event_cb(n->box, scrub, LV_EVENT_PRESSING, n);
	lv_obj_add_event_cb(n->box, scrub, LV_EVENT_RELEASED, n);
	lv_obj_add_event_cb(n->box, scrub, LV_EVENT_PRESS_LOST, n);
	n->img = lv_image_create(n->box);
	lv_obj_set_clickable(n->img, false);

	n->title = ui_label(n->scr, app.font.l, "");
	lv_obj_set_style_text_align(n->title, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(n->title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	n->artist = ui_label(n->scr, app.font.s, "");
	lv_obj_set_size(n->artist, 240, lv_font_get_line_height(app.font.s));
	lv_obj_set_style_text_align(n->artist, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(n->artist, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lv_obj_set_style_text_color(n->artist, lv_color_hex(0xb0b0b0), 0);

	row = lv_obj_create(n->scr);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, 210, 64);
	lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -14);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(row, false);
	ui_round_button(row, LV_SYMBOL_PREV, 48, on_ctl, (void *)0);
	b = ui_round_button(row, LV_SYMBOL_PLAY, 62, on_ctl, (void *)1);
	n->play_lbl = lv_obj_get_child(b, 0);
	ui_round_button(row, LV_SYMBOL_NEXT, 48, on_ctl, (void *)2);

	b = ui_round_button(n->scr, LV_SYMBOL_LIST, 40, open_queue, n);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_align(b, LV_ALIGN_CENTER, -128, -30);
	b = ui_round_button(n->scr, LV_SYMBOL_BARS, 40, open_menu, n);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_align(b, LV_ALIGN_CENTER, 128, -30);

	n->seek_lbl = ui_label(n->scr, app.font.clock, "");
	lv_obj_set_style_bg_color(n->seek_lbl, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(n->seek_lbl, LV_OPA_80, 0);
	lv_obj_set_style_radius(n->seek_lbl, 16, 0);
	lv_obj_set_style_pad_hor(n->seek_lbl, 12, 0);
	lv_obj_center(n->seek_lbl);
	lv_obj_set_hidden(n->seek_lbl, true);

	layout_mode(n);
	update_song(n);
	update_state(n);
	app_listen(n->scr, EV_STATUS | EV_SONG | EV_COVER | EV_SECOND | EV_SETTINGS | EV_SYS | EV_VOLUME, on_event, n);
	lv_obj_add_event_cb(n->scr, on_delete, LV_EVENT_DELETE, n);
	return n->scr;
}

static void shown(lv_obj_t *scr)
{
	struct np *n = lv_obj_get_user_data(scr);
	update_state(n);
}

const struct screen_ops scr_np = { .name = "nowplaying", .create = create, .shown = shown };
