#include "ui.h"

#include <disc/settings.h>
#include <disc/text.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define POOL 12
#define ROW_PITCH 52
#define STRIP_W 40
#define MAX_LETTERS 64
#define MIN_FOR_INDEX 24
#define TAIL_PAD 96

struct vl;

struct row {
	lv_obj_t *obj, *icon, *box, *text, *sub, *right;
	int idx, left, right_w, width, press_x, drag_base;
	bool holding, dragging, has_sub;
	struct vl *vl;
};

#define DRAG_SLOP 8

struct vl {
	lv_obj_t *root, *scroller, *spacer, *strip, *bubble, *empty;
	const struct vlist_ops *ops;
	void *ctx;
	int count;
	struct row rows[POOL];
	uint32_t letters[MAX_LETTERS];
	int letter_first[MAX_LETTERS];
	int n_letters;
};

static struct vl *get(lv_obj_t *root)
{
	return lv_obj_get_user_data(root);
}

static void row_click(lv_event_t *e)
{
	struct row *r = lv_event_get_user_data(e);

	if (r->idx >= 0 && r->idx < r->vl->count && r->vl->ops->click)
		r->vl->ops->click(r->vl->ctx, r->idx);
}

static lv_label_long_mode_t text_mode(bool active)
{
	const char *m = set_get("ui.marquee");
	return !strcmp(m, "all") || (!strcmp(m, "active") && active) ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR : LV_LABEL_LONG_MODE_DOTS;
}

static int point_x(void)
{
	lv_point_t p;

	lv_indev_get_point(lv_indev_active(), &p);
	return p.x;
}

static void drag_start(struct row *r)
{
	r->dragging = true;
	lv_label_set_long_mode(r->text, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_width(r->text, LV_SIZE_CONTENT);
	lv_obj_update_layout(r->text);
	r->drag_base = lv_obj_get_style_translate_x(r->text, 0);
}

static void drag_move(struct row *r, int dx)
{
	int over = lv_obj_get_width(r->text) - lv_obj_get_width(r->box), off = r->drag_base + dx;

	if (over < 0)
		over = 0;
	if (off > 0)
		off = 0;
	if (off < -over)
		off = -over;
	lv_obj_set_style_translate_x(r->text, off, 0);
}

static void row_hold(lv_event_t *e)
{
	struct row *r = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_LONG_PRESSED) {
		r->holding = true;
		r->dragging = false;
		r->press_x = point_x();
		return;
	}
	if (!r->holding)
		return;
	if (code == LV_EVENT_PRESSING) {
		int dx = point_x() - r->press_x;
		if (!r->dragging && abs(dx) > DRAG_SLOP)
			drag_start(r);
		if (r->dragging)
			drag_move(r, dx);
		return;
	}
	r->holding = false;
	if (code == LV_EVENT_RELEASED && !r->dragging && r->idx >= 0 && r->idx < r->vl->count && r->vl->ops->hold)
		r->vl->ops->hold(r->vl->ctx, r->idx);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *f)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, f, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(l, "");
	return l;
}

static void build_row(struct vl *vl, struct row *r)
{
	r->vl = vl;
	r->idx = -1;
	r->obj = lv_obj_create(vl->scroller);
	lv_obj_remove_style_all(r->obj);
	lv_obj_set_height(r->obj, ROW_PITCH - 4);
	lv_obj_set_style_radius(r->obj, 12, 0);
	lv_obj_set_style_bg_color(r->obj, app.accent, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(r->obj, LV_OPA_40, LV_STATE_PRESSED);
	lv_obj_set_scrollable(r->obj, false);
	lv_obj_add_event_cb(r->obj, row_click, LV_EVENT_SHORT_CLICKED, r);
	lv_obj_add_event_cb(r->obj, row_hold, LV_EVENT_LONG_PRESSED, r);
	lv_obj_add_event_cb(r->obj, row_hold, LV_EVENT_PRESSING, r);
	lv_obj_add_event_cb(r->obj, row_hold, LV_EVENT_RELEASED, r);
	lv_obj_add_event_cb(r->obj, row_hold, LV_EVENT_PRESS_LOST, r);
	r->icon = mk_label(r->obj, &lv_font_montserrat_16);
	lv_obj_align(r->icon, LV_ALIGN_LEFT_MID, 8, 0);
	r->box = lv_obj_create(r->obj);
	lv_obj_remove_style_all(r->box);
	lv_obj_set_height(r->box, LV_PCT(100));
	lv_obj_set_scrollable(r->box, false);
	lv_obj_set_clickable(r->box, false);
	r->text = mk_label(r->box, app.font.m);
	lv_obj_set_height(r->text, lv_font_get_line_height(app.font.m));
	r->sub = mk_label(r->box, app.font.s);
	lv_obj_set_height(r->sub, lv_font_get_line_height(app.font.s));
	lv_obj_set_style_text_color(r->sub, lv_color_hex(0x9a9a9a), 0);
	r->right = mk_label(r->obj, app.font.s);
	lv_obj_set_style_text_color(r->right, lv_color_hex(0x9a9a9a), 0);
	lv_obj_set_hidden(r->obj, true);
}

static void fit(struct row *r, int width)
{
	int w = width - r->left - r->right_w - 10;

	r->width = width;
	lv_obj_set_width(r->box, w);
	lv_obj_set_x(r->box, r->left);
	if (!r->dragging)
		lv_obj_set_width(r->text, w);
	lv_obj_set_width(r->sub, w);
}

static void render(struct vl *vl, struct row *r, int idx, int width)
{
	struct vrow d = { 0 };
	int left = 40, right_w = 0;

	r->idx = idx;
	r->holding = r->dragging = false;
	vl->ops->row(vl->ctx, idx, &d);
	lv_obj_set_style_translate_x(r->text, 0, 0);
	lv_label_set_long_mode(r->text, text_mode(d.active || d.header));
	lv_label_set_text(r->icon, d.icon ? d.icon : "");
	lv_obj_set_hidden(r->icon, !d.icon);
	if (!d.icon)
		left = 12;
	lv_label_set_text(r->right, d.right);
	if (d.right[0]) {
		lv_obj_update_layout(r->right);
		right_w = lv_obj_get_width(r->right) + 8;
	}
	lv_obj_align(r->right, LV_ALIGN_RIGHT_MID, -8, 0);
	lv_label_set_text(r->text, d.text);
	lv_obj_set_style_text_color(r->text, d.header || d.active ? app.accent : d.dim ? lv_color_hex(0x8a8a8a) : lv_color_white(), 0);
	lv_obj_set_style_bg_color(r->obj, lv_color_hex(0x1c1c1c), 0);
	lv_obj_set_style_bg_opa(r->obj, d.header ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_label_set_text(r->sub, d.sub);
	lv_obj_set_hidden(r->sub, !d.sub[0]);
	r->left = left;
	r->right_w = right_w;
	fit(r, width);
	if (d.sub[0]) {
		lv_obj_align(r->text, LV_ALIGN_TOP_LEFT, 0, 4);
		lv_obj_align(r->sub, LV_ALIGN_BOTTOM_LEFT, 0, -4);
	} else {
		lv_obj_align(r->text, LV_ALIGN_LEFT_MID, 0, 0);
	}
}

static int half_width_at(int screen_y)
{
	int dy = screen_y - SCR_R;
	if (dy <= -SCR_R || dy >= SCR_R)
		return 0;
	return (int)sqrtf((float)(SCR_R * SCR_R - dy * dy));
}

static void layout(struct vl *vl, bool force)
{
	lv_area_t c;
	int sy = lv_obj_get_scroll_y(vl->scroller), h, first;
	int strip = vl->n_letters > 1 ? STRIP_W : 0;

	lv_obj_get_coords(vl->scroller, &c);
	h = lv_area_get_height(&c);
	first = sy / ROW_PITCH;
	if (first < 0)
		first = 0;
	for (int k = 0; k < POOL; k++) {
		int idx = first + k;
		struct row *r = &vl->rows[idx % POOL];
		if (idx >= vl->count || idx * ROW_PITCH - sy > h) {
			lv_obj_set_hidden(r->obj, true);
			continue;
		}
		int y = idx * ROW_PITCH;
		int center = c.y1 + y - sy + ROW_PITCH / 2;
		int hw = half_width_at(center) - 10;
		int x0 = SCR_R - hw - c.x1, x1 = SCR_R + hw - c.x1;
		if (x0 < 6)
			x0 = 6;
		if (x1 > lv_area_get_width(&c) - 6 - strip)
			x1 = lv_area_get_width(&c) - 6 - strip;
		if (x1 - x0 < 80) {
			x0 = SCR_R - 40 - c.x1;
			x1 = x0 + 80;
		}
		lv_obj_set_pos(r->obj, x0, y + 2);
		lv_obj_set_width(r->obj, x1 - x0);
		lv_obj_set_hidden(r->obj, false);
		if (force || r->idx != idx)
			render(vl, r, idx, x1 - x0);
		else if (r->width != x1 - x0)
			fit(r, x1 - x0);
	}
}

static void on_scroll(lv_event_t *e)
{
	layout(lv_event_get_user_data(e), false);
}

static void build_letters(struct vl *vl)
{
	vl->n_letters = 0;
	if (!vl->ops->letter || vl->count < MIN_FOR_INDEX || !set_get_bool("ui.index"))
		return;
	uint32_t prev = 0;
	for (int i = 0; i < vl->count && vl->n_letters < MAX_LETTERS; i++) {
		uint32_t l = vl->ops->letter(vl->ctx, i);
		if (l == prev)
			continue;
		bool seen = false;
		for (int j = 0; j < vl->n_letters; j++)
			if (vl->letters[j] == l)
				seen = true;
		if (!seen) {
			vl->letters[vl->n_letters] = l;
			vl->letter_first[vl->n_letters++] = i;
		}
		prev = l;
	}
}

static void strip_text(struct vl *vl)
{
	char buf[MAX_LETTERS * 6];
	int o = 0, step = vl->n_letters > 12 ? (vl->n_letters + 11) / 12 : 1;

	for (int i = 0; i < vl->n_letters; i += step) {
		o += utf8_put(buf + o, vl->letters[i]);
		buf[o++] = '\n';
	}
	if (o)
		o--;
	buf[o] = 0;
	lv_label_set_text(lv_obj_get_child(vl->strip, 0), buf);
}

static void strip_touch(lv_event_t *e)
{
	struct vl *vl = lv_event_get_user_data(e);
	lv_point_t p;
	lv_area_t a;
	char buf[8];

	if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_PRESS_LOST) {
		lv_obj_set_hidden(vl->bubble, true);
		return;
	}
	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(vl->strip, &a);
	int i = (p.y - a.y1) * vl->n_letters / (lv_area_get_height(&a) ? lv_area_get_height(&a) : 1);
	if (i < 0)
		i = 0;
	if (i >= vl->n_letters)
		i = vl->n_letters - 1;
	buf[utf8_put(buf, vl->letters[i])] = 0;
	lv_label_set_text(lv_obj_get_child(vl->bubble, 0), buf);
	lv_obj_set_hidden(vl->bubble, false);
	vlist_scroll_to(vl->root, vl->letter_first[i], false);
}

static void on_delete(lv_event_t *e)
{
	free(lv_event_get_user_data(e));
}

lv_obj_t *vlist_create(lv_obj_t *parent, const struct vlist_ops *ops, void *ctx)
{
	struct vl *vl = calloc(1, sizeof(*vl));

	vl->ops = ops;
	vl->ctx = ctx;
	vl->root = lv_obj_create(parent);
	lv_obj_remove_style_all(vl->root);
	lv_obj_set_scrollable(vl->root, false);
	lv_obj_set_user_data(vl->root, vl);
	lv_obj_add_event_cb(vl->root, on_delete, LV_EVENT_DELETE, vl);

	vl->scroller = lv_obj_create(vl->root);
	lv_obj_remove_style_all(vl->scroller);
	lv_obj_set_size(vl->scroller, LV_PCT(100), LV_PCT(100));
	lv_obj_set_scroll_dir(vl->scroller, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(vl->scroller, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(vl->scroller, on_scroll, LV_EVENT_SCROLL, vl);
	vl->spacer = lv_obj_create(vl->scroller);
	lv_obj_remove_style_all(vl->spacer);
	lv_obj_set_width(vl->spacer, 1);
	lv_obj_set_clickable(vl->spacer, false);
	for (int i = 0; i < POOL; i++)
		build_row(vl, &vl->rows[i]);

	vl->empty = ui_label(vl->root, app.font.m, "");
	lv_obj_set_style_text_color(vl->empty, lv_color_hex(0x8a8a8a), 0);
	lv_obj_align(vl->empty, LV_ALIGN_TOP_MID, 0, 60);
	lv_obj_set_hidden(vl->empty, true);

	vl->strip = lv_obj_create(vl->root);
	lv_obj_remove_style_all(vl->strip);
	lv_obj_set_size(vl->strip, STRIP_W, 180);
	lv_obj_align(vl->strip, LV_ALIGN_RIGHT_MID, -4, 0);
	lv_obj_set_style_bg_color(vl->strip, lv_color_hex(0x303030), 0);
	lv_obj_set_style_bg_opa(vl->strip, LV_OPA_60, 0);
	lv_obj_set_style_radius(vl->strip, 16, 0);
	lv_obj_add_event_cb(vl->strip, strip_touch, LV_EVENT_PRESSING, vl);
	lv_obj_add_event_cb(vl->strip, strip_touch, LV_EVENT_RELEASED, vl);
	lv_obj_add_event_cb(vl->strip, strip_touch, LV_EVENT_PRESS_LOST, vl);
	lv_obj_t *sl = lv_label_create(vl->strip);
	lv_obj_set_style_text_font(sl, app.font.s, 0);
	lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_line_space(sl, -2, 0);
	lv_obj_center(sl);
	lv_obj_set_hidden(vl->strip, true);

	vl->bubble = lv_obj_create(vl->root);
	lv_obj_remove_style_all(vl->bubble);
	lv_obj_set_size(vl->bubble, 96, 96);
	lv_obj_set_style_radius(vl->bubble, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(vl->bubble, app.accent, 0);
	lv_obj_set_style_bg_opa(vl->bubble, LV_OPA_90, 0);
	lv_obj_center(vl->bubble);
	lv_obj_set_clickable(vl->bubble, false);
	lv_obj_t *bl = lv_label_create(vl->bubble);
	lv_obj_set_style_text_font(bl, app.font.clock, 0);
	lv_obj_set_style_text_color(bl, lv_color_black(), 0);
	lv_obj_center(bl);
	lv_obj_set_hidden(vl->bubble, true);
	return vl->root;
}

void vlist_set_empty(lv_obj_t *root, const char *text)
{
	lv_label_set_text(get(root)->empty, text);
}

void vlist_reload(lv_obj_t *root)
{
	struct vl *vl = get(root);

	vl->count = vl->ops->count(vl->ctx);
	lv_obj_set_height(vl->spacer, vl->count * ROW_PITCH + TAIL_PAD);
	for (int i = 0; i < POOL; i++) {
		vl->rows[i].idx = -1;
		lv_obj_set_hidden(vl->rows[i].obj, true);
	}
	build_letters(vl);
	lv_obj_set_hidden(vl->strip, vl->n_letters < 2);
	if (vl->n_letters >= 2)
		strip_text(vl);
	lv_obj_set_hidden(vl->empty, vl->count > 0 || !lv_label_get_text(vl->empty)[0]);
	lv_obj_update_layout(root);
	layout(vl, true);
}

void vlist_refresh(lv_obj_t *root)
{
	layout(get(root), true);
}

void vlist_scroll_to(lv_obj_t *root, int idx, bool center)
{
	struct vl *vl = get(root);
	int h = lv_obj_get_height(vl->scroller), y = idx * ROW_PITCH;

	if (center)
		y -= h / 2 - ROW_PITCH / 2;
	if (y < 0)
		y = 0;
	lv_obj_update_layout(root);
	lv_obj_scroll_to_y(vl->scroller, y, LV_ANIM_OFF);
	layout(vl, false);
}

int vlist_first(lv_obj_t *root)
{
	int f = lv_obj_get_scroll_y(get(root)->scroller) / ROW_PITCH;
	return f < 0 ? 0 : f;
}

int vlist_count(lv_obj_t *root)
{
	return get(root)->count;
}
