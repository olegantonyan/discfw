#include "app.h"
#include "ui.h"

#include <dirent.h>
#include <disc/settings.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define QLEN 16
#define SLOP 12
#define SIDE_LIMIT 120

struct sample {
	int16_t x, y;
	bool pressed, fresh;
};

enum gstate { G_IDLE, G_PASS, G_PEND_BACK, G_PEND_SHADE, G_PEND_UP, G_BACK, G_SHADE, G_UP };

static struct sample q[QLEN], last;
static int qhead, qtail;
static struct {
	int x, y;
	bool pressed;
} raw;
static enum gstate gs;
static int gx, gy;
static bool rot180 = true;
static bool (*back_cb)(void);
static lv_obj_t *bulge, *bulge_arrow;

static void push(int x, int y, bool pressed)
{
	int prev = (qtail + QLEN - 1) % QLEN;
	bool was_pressed = qhead != qtail ? q[prev].pressed : last.pressed;

	if (qhead != qtail && pressed && q[prev].pressed && !q[prev].fresh) {
		q[prev].x = x;
		q[prev].y = y;
		return;
	}
	if ((qtail + 1) % QLEN == qhead)
		qhead = (qhead + 1) % QLEN;
	q[qtail] = (struct sample){ x, y, pressed, pressed && !was_pressed };
	qtail = (qtail + 1) % QLEN;
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *d)
{
	(void)indev;
	if (qhead != qtail) {
		last = q[qhead];
		qhead = (qhead + 1) % QLEN;
	}
	d->point.x = last.x;
	d->point.y = last.y;
	d->state = last.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static int edge_left(int y)
{
	int dy = y - SCR_R;
	return SCR_R - (int)sqrtf((float)(SCR_R * SCR_R - dy * dy));
}

static int edge_top(int x)
{
	int dx = x - SCR_R;
	return SCR_R - (int)sqrtf((float)(SCR_R * SCR_R - dx * dx));
}

static bool in_left_zone(int x, int y)
{
	int edge = set_get_int("ui.edge_px");
	return x < SIDE_LIMIT && x - edge_left(y) < edge;
}

static bool in_top_zone(int x, int y)
{
	int edge = set_get_int("ui.edge_px");
	return abs(x - SCR_R) < SIDE_LIMIT && y - edge_top(x) < edge;
}

static void bulge_show(int dx, int y)
{
	int threshold = set_get_int("ui.back_px");
	int w = dx > threshold ? threshold : dx < 0 ? 0 : dx;

	if (!bulge) {
		bulge = lv_obj_create(lv_layer_top());
		lv_obj_remove_style_all(bulge);
		lv_obj_set_size(bulge, 80, 120);
		lv_obj_set_style_radius(bulge, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(bulge, LV_OPA_80, 0);
		lv_obj_set_clickable(bulge, false);
		bulge_arrow = lv_label_create(bulge);
		lv_label_set_text(bulge_arrow, LV_SYMBOL_LEFT);
		lv_obj_set_style_text_font(bulge_arrow, app.font.icon, 0);
		lv_obj_align(bulge_arrow, LV_ALIGN_RIGHT_MID, -12, 0);
	}
	lv_obj_set_hidden(bulge, false);
	lv_obj_set_pos(bulge, edge_left(y) - 80 + w * 3 / 4 + 16, y - 60);
	lv_obj_set_style_bg_color(bulge, dx >= threshold ? app.accent : lv_color_hex(0x404040), 0);
	lv_obj_set_style_text_color(bulge_arrow, dx >= threshold ? lv_color_black() : lv_color_white(), 0);
}

static void bulge_hide(void)
{
	if (bulge)
		lv_obj_set_hidden(bulge, true);
}

static void gesture(int x, int y, bool pressed)
{
	int dx = x - gx, dy = y - gy;

	switch (gs) {
	case G_IDLE:
		if (!pressed)
			return;
		gx = x;
		gy = y;
		if (app.sys.locked)
			gs = G_PASS;
		else if (in_left_zone(x, y))
			gs = G_PEND_BACK;
		else if (shade_is_open())
			gs = G_PEND_UP;
		else if (in_top_zone(x, y) && !ui_modal_active())
			gs = G_PEND_SHADE;
		else
			gs = G_PASS;
		if (gs == G_PASS)
			push(x, y, true);
		return;
	case G_PASS:
		push(x, y, pressed);
		if (!pressed)
			gs = G_IDLE;
		return;
	case G_PEND_BACK:
	case G_PEND_SHADE:
	case G_PEND_UP:
		if (!pressed) {
			push(gx, gy, true);
			push(gx, gy, false);
			gs = G_IDLE;
			return;
		}
		if (gs == G_PEND_BACK && dx > SLOP && dx > abs(dy)) {
			gs = G_BACK;
			bulge_show(dx, gy);
			return;
		}
		if (gs == G_PEND_SHADE && dy > SLOP && dy > abs(dx)) {
			gs = G_SHADE;
			shade_drag(dy, false);
			return;
		}
		if (gs == G_PEND_UP && -dy > SLOP && -dy > abs(dx)) {
			gs = G_UP;
			shade_lift(dy, false);
			return;
		}
		if (abs(dx) > SLOP || abs(dy) > SLOP) {
			push(gx, gy, true);
			push(x, y, true);
			gs = G_PASS;
		}
		return;
	case G_BACK:
		if (pressed) {
			bulge_show(dx, gy);
			return;
		}
		bulge_hide();
		gs = G_IDLE;
		if (dx >= set_get_int("ui.back_px") && back_cb)
			back_cb();
		return;
	case G_SHADE:
		shade_drag(dy, !pressed);
		if (!pressed)
			gs = G_IDLE;
		return;
	case G_UP:
		shade_lift(dy, !pressed);
		if (!pressed)
			gs = G_IDLE;
		return;
	}
}

void input_inject(int x, int y, bool pressed)
{
	gesture(x < 0 ? 0 : x >= SCR_W ? SCR_W - 1 : x, y < 0 ? 0 : y >= SCR_H ? SCR_H - 1 : y, pressed);
}

void input_set_back_handler(bool (*cb)(void))
{
	back_cb = cb;
}

static void on_touch(int fd, short revents, void *ctx)
{
	struct input_event ev;

	(void)revents;
	(void)ctx;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (ev.type == EV_ABS) {
			if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X)
				raw.x = ev.value;
			else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y)
				raw.y = ev.value;
			else if (ev.code == ABS_MT_TRACKING_ID)
				raw.pressed = ev.value >= 0;
		} else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
			raw.pressed = ev.value;
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			int x = rot180 ? SCR_W - raw.x : raw.x, y = rot180 ? SCR_H - raw.y : raw.y;
			input_inject(x, y, raw.pressed);
		}
	}
}

static int find_touch(char *path, size_t len)
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
		if (fgets(n, sizeof(n), f) && !strncmp(n, "cst816t", 7)) {
			snprintf(path, len, "/dev/input/%.32s", e->d_name);
			found = 0;
		}
		fclose(f);
	}
	if (dir)
		closedir(dir);
	return found;
}

void input_init(void)
{
	const char *rot = getenv("DISCUI_ROTATE");
	char path[64];
	lv_indev_t *indev = lv_indev_create();
	int fd;

	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, read_cb);
	rot180 = !rot || strcmp(rot, "0");
	if (app.host)
		return;
	if (find_touch(path, sizeof(path)) == 0 && (fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) >= 0)
		loop_watch(fd, POLLIN, on_touch, NULL);
	else
		fprintf(stderr, "touch device not found\n");
}
