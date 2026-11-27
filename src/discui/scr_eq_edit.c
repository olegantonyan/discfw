#include "ui.h"

#include <disc/eq.h>
#include <disc/kv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PTS 64
#define CURVE_DB 15
#define COMMIT_MS 100
#define COL_W 62
#define STRIP_Y 150
#define STRIP_H 142
#define BAND_STEP 0.5
#define PRE_STEP 0.5
#define GAIN_MAX 24.0
#define FC_MIN 10.0
#define FC_MAX 22000.0
#define Q_MIN 0.1
#define Q_MAX 10.0

struct eqed {
	int refs;
	char id[EQ_ID_MAX], name[EQ_NAME_MAX];
	bool readonly, dirty;
	struct eq_preset p;
	lv_timer_t *timer;
};

struct earg {
	const char *id;
	const struct eq_preset *init;
	struct eqed *ed;
	int idx;
};

struct curve {
	lv_obj_t *chart;
	lv_chart_series_t *tot, *par;
};

static void commit(struct eqed *ed)
{
	char text[4096];
	struct kvbuf b;

	ed->dirty = false;
	if (ed->timer)
		lv_timer_pause(ed->timer);
	if (ed->readonly || eq_format(&ed->p, text, sizeof(text)) < 0)
		return;
	kv_init(&b);
	kv_raw(&b, "eq put");
	kv_str(&b, "name", ed->name);
	kv_str(&b, "text", text);
	sysc_send_line(b.s);
	kv_free(&b);
}

static void commit_cb(lv_timer_t *t)
{
	commit(lv_timer_get_user_data(t));
}

static void changed(struct eqed *ed)
{
	ed->dirty = true;
	if (!ed->timer)
		ed->timer = lv_timer_create(commit_cb, COMMIT_MS, ed);
	lv_timer_reset(ed->timer);
	lv_timer_resume(ed->timer);
}

static struct eqed *ed_get(const struct earg *a)
{
	struct eqed *ed;

	if (a->ed) {
		a->ed->refs++;
		return a->ed;
	}
	ed = calloc(1, sizeof(*ed));
	ed->refs = 1;
	snprintf(ed->id, sizeof(ed->id), "%s", a->id);
	eq_id_name(a->id, ed->name, sizeof(ed->name));
	ed->readonly = strncmp(a->id, "my:", 3) != 0;
	if (a->init) {
		ed->p = *a->init;
	} else if (eq_store_load(a->id, &ed->p)) {
		ui_toast("Cannot read the preset");
		free(ed);
		return NULL;
	}
	return ed;
}

static void ed_put(struct eqed *ed)
{
	if (--ed->refs)
		return;
	if (ed->dirty)
		commit(ed);
	if (ed->timer)
		lv_timer_delete(ed->timer);
	free(ed);
}

static void fmt_hz(double hz, char *out, size_t len)
{
	if (hz >= 1000)
		snprintf(out, len, "%.3gk", hz / 1000);
	else
		snprintf(out, len, "%.3g", hz);
}

static void fmt_db(double db, char *out, size_t len)
{
	if (fabs(db) < 0.05)
		snprintf(out, len, "0");
	else
		snprintf(out, len, "%+.1f", db);
}

static double clamp(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static double round1(double v)
{
	return round(v * 10) / 10;
}

static void curve_new(lv_obj_t *scr, struct curve *c)
{
	lv_obj_t *ch = lv_chart_create(scr);

	lv_obj_set_size(ch, 250, 80);
	lv_obj_align(ch, LV_ALIGN_TOP_MID, 0, CONTENT_Y);
	lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(ch, 0, 0);
	lv_obj_set_style_pad_all(ch, 0, 0);
	lv_obj_set_style_line_color(ch, lv_color_hex(0x383838), LV_PART_MAIN);
	lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);
	lv_obj_set_style_width(ch, 0, LV_PART_INDICATOR);
	lv_obj_set_style_height(ch, 0, LV_PART_INDICATOR);
	lv_obj_set_clickable(ch, false);
	lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
	lv_chart_set_point_count(ch, PTS);
	lv_chart_set_axis_range(ch, LV_CHART_AXIS_PRIMARY_Y, -CURVE_DB * 10, CURVE_DB * 10);
	lv_chart_set_div_line_count(ch, 3, 0);
	c->chart = ch;
	c->par = lv_chart_add_series(ch, lv_color_hex(0x6a6a6a), LV_CHART_AXIS_PRIMARY_Y);
	c->tot = lv_chart_add_series(ch, app.accent, LV_CHART_AXIS_PRIMARY_Y);
}

static void curve_update(struct curve *c, const struct eq_preset *p)
{
	struct eq_filter all[EQ_MAX_FILTERS], par[EQ_MAX_FILTERS];
	int na = eq_flatten(p, true, true, all), np = eq_flatten(p, true, false, par);
	bool layered = np && np != na;

	for (int i = 0; i < PTS; i++) {
		double hz = 20 * pow(1000.0, (double)i / (PTS - 1));
		lv_chart_set_series_value_by_id(c->chart, c->tot, i, lround(clamp(eq_response_db(all, na, EQ_REF_FS, hz), -CURVE_DB, CURVE_DB) * 10));
		if (layered)
			lv_chart_set_series_value_by_id(c->chart, c->par, i,
							lround(clamp(eq_response_db(par, np, EQ_REF_FS, hz), -CURVE_DB, CURVE_DB) * 10));
	}
	lv_chart_hide_series(c->chart, c->par, !layered);
	lv_chart_refresh(c->chart);
}

static lv_obj_t *strip_new(lv_obj_t *scr)
{
	lv_obj_t *s = lv_obj_create(scr);

	lv_obj_remove_style_all(s);
	lv_obj_set_size(s, SCR_W, STRIP_H);
	lv_obj_set_pos(s, 0, STRIP_Y);
	lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
	lv_obj_set_scroll_dir(s, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_style_pad_hor(s, 40, 0);
	lv_obj_set_style_pad_column(s, 4, 0);
	return s;
}

static void on_delete(lv_event_t *e)
{
	void **view = lv_event_get_user_data(e);

	ed_put(*view);
	free(view);
}

static void attach(lv_obj_t *scr, void *view)
{
	lv_obj_set_user_data(scr, view);
	lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, view);
}

struct gview {
	struct eqed *ed;
	lv_obj_t *scr, *pre, *band[EQ_BANDS], *param;
	struct curve curve;
};

static const char *const band_names[EQ_BANDS] = { "31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };

static void gview_refresh(struct gview *v)
{
	struct eq_preset *p = &v->ed->p;
	char buf[32];

	fmt_db(eq_preamp_db(p), buf, sizeof(buf));
	ui_spin_set(v->pre, buf, p->auto_preamp ? "Auto" : "Preamp");
	for (int i = 0; i < EQ_BANDS; i++) {
		fmt_db(p->band[i], buf, sizeof(buf));
		ui_spin_set(v->band[i], buf, band_names[i]);
	}
	lv_label_set_text_fmt(lv_obj_get_child(v->param, 0), "Parametric (%d)", p->nparam);
	curve_update(&v->curve, p);
}

static void pre_spin(lv_obj_t *spin, int steps, void *ctx)
{
	struct gview *v = ctx;
	struct eq_preset *p = &v->ed->p;

	(void)spin;
	if (!steps && p->auto_preamp)
		return;
	if (steps)
		p->preamp = clamp(round1(eq_preamp_db(p) + steps * PRE_STEP), EQ_PREAMP_MIN, EQ_PREAMP_MAX);
	p->auto_preamp = !steps;
	changed(v->ed);
	gview_refresh(v);
}

static void band_spin(lv_obj_t *spin, int steps, void *ctx)
{
	struct gview *v = ctx;
	int i = 0;

	while (i < EQ_BANDS - 1 && v->band[i] != spin)
		i++;
	v->ed->p.band[i] = clamp(round1(v->ed->p.band[i] + steps * BAND_STEP), -EQ_BAND_DB, EQ_BAND_DB);
	changed(v->ed);
	gview_refresh(v);
}

static void open_param(lv_event_t *e)
{
	struct gview *v = lv_event_get_user_data(e);
	struct earg a = { .ed = v->ed };

	nav_push(&scr_eq_param, &a);
}

static lv_obj_t *gview_create(void *arg)
{
	struct gview *v = calloc(1, sizeof(*v));
	lv_obj_t *strip;

	v->ed = ed_get(arg);
	if (!v->ed) {
		free(v);
		return NULL;
	}
	v->scr = ui_screen(v->ed->name, NULL);
	attach(v->scr, v);
	curve_new(v->scr, &v->curve);
	strip = strip_new(v->scr);
	v->pre = ui_spin(strip, COL_W, true, pre_spin, v);
	for (int i = 0; i < EQ_BANDS; i++)
		v->band[i] = ui_spin(strip, COL_W, false, band_spin, v);
	v->param = ui_text_button(v->scr, "", open_param, v);
	lv_obj_set_size(v->param, 180, 40);
	lv_obj_align(v->param, LV_ALIGN_TOP_MID, 0, STRIP_Y + STRIP_H + 4);
	if (((const struct earg *)arg)->init)
		commit(v->ed);
	gview_refresh(v);
	return v->scr;
}

static void gview_shown(lv_obj_t *scr)
{
	gview_refresh(lv_obj_get_user_data(scr));
}

const struct screen_ops scr_eq_edit = { .name = "eq_edit", .create = gview_create, .shown = gview_shown };

void eq_open_editor(const char *id)
{
	struct earg a = { .id = id };

	nav_push(&scr_eq_edit, &a);
}

void eq_open_new(const char *id, const struct eq_preset *p)
{
	struct earg a = { .id = id, .init = p };

	nav_push(&scr_eq_edit, &a);
}

struct pview {
	struct eqed *ed;
	lv_obj_t *scr, *list;
	int sel;
};

static int pview_base(const struct pview *v)
{
	return v->ed->readonly ? 2 : 0;
}

static int pview_count(void *ctx)
{
	struct pview *v = ctx;

	return pview_base(v) + v->ed->p.nparam + (!v->ed->readonly && v->ed->p.nparam < EQ_MAX_PARAM);
}

static void pview_row(void *ctx, int idx, struct vrow *r)
{
	struct pview *v = ctx;
	const struct eq_preset *p = &v->ed->p;
	const struct eq_filter *f;
	char hz[16], db[16];
	int i = idx - pview_base(v);

	if (v->ed->readonly && idx == 0) {
		snprintf(r->text, sizeof(r->text), "Customize…");
		r->icon = LV_SYMBOL_EDIT;
		return;
	}
	if (v->ed->readonly && idx == 1) {
		fmt_db(eq_preamp_db(p), db, sizeof(db));
		snprintf(r->text, sizeof(r->text), "Preamp %s dB%s", db, p->auto_preamp ? " (auto)" : "");
		r->dim = true;
		return;
	}
	if (i >= p->nparam) {
		snprintf(r->text, sizeof(r->text), "Add filter");
		r->icon = LV_SYMBOL_PLUS;
		return;
	}
	f = &p->param[i];
	fmt_hz(f->fc, hz, sizeof(hz));
	snprintf(r->text, sizeof(r->text), "%s  %s Hz", eq_type_names[f->type], hz);
	if (eq_type_has_gain(f->type)) {
		fmt_db(f->gain, db, sizeof(db));
		snprintf(r->sub, sizeof(r->sub), "%s dB · Q %.2f", db, f->q);
	} else {
		snprintf(r->sub, sizeof(r->sub), "Q %.2f", f->q);
	}
	if (!f->on) {
		snprintf(r->right, sizeof(r->right), "off");
		r->dim = true;
	}
}

static void open_filter(struct pview *v, int i)
{
	struct earg a = { .ed = v->ed, .idx = i };

	nav_push(&scr_eq_filter, &a);
}

static void customize(struct pview *v)
{
	struct eq_preset p;
	char name[EQ_NAME_MAX], id[EQ_NAME_MAX + 4];

	eq_preset_init(&p);
	p.nparam = v->ed->p.nparam;
	memcpy(p.param, v->ed->p.param, sizeof(p.param[0]) * p.nparam);
	eq_unique_name(v->ed->name, name, sizeof(name));
	snprintf(id, sizeof(id), "my:%s", name);
	eq_open_new(id, &p);
}

static void pview_click(void *ctx, int idx)
{
	struct pview *v = ctx;
	struct eq_preset *p = &v->ed->p;
	int i = idx - pview_base(v);

	if (v->ed->readonly) {
		if (idx == 0)
			customize(v);
		return;
	}
	if (i == p->nparam && p->nparam < EQ_MAX_PARAM) {
		p->param[p->nparam++] = (struct eq_filter){ EQ_PK, true, 1000, 0, 1.0 };
		changed(v->ed);
	}
	if (i < p->nparam)
		open_filter(v, i);
}

static void pview_action(int choice, void *ctx)
{
	struct pview *v = ctx;
	struct eq_preset *p = &v->ed->p;
	int i = v->sel;

	if (i < 0 || i >= p->nparam)
		return;
	if (choice == 0) {
		p->param[i].on = !p->param[i].on;
	} else if (choice == 1 && p->nparam < EQ_MAX_PARAM) {
		memmove(&p->param[i + 1], &p->param[i], sizeof(p->param[0]) * (p->nparam - i));
		p->nparam++;
	} else if (choice == 2) {
		memmove(&p->param[i], &p->param[i + 1], sizeof(p->param[0]) * (p->nparam - i - 1));
		p->nparam--;
	} else {
		return;
	}
	changed(v->ed);
	vlist_reload(v->list);
}

static void pview_hold(void *ctx, int idx)
{
	struct pview *v = ctx;
	const char *items[3];
	char title[32];

	v->sel = idx - pview_base(v);
	if (v->ed->readonly || v->sel < 0 || v->sel >= v->ed->p.nparam)
		return;
	items[0] = v->ed->p.param[v->sel].on ? "Turn off" : "Turn on";
	items[1] = "Duplicate";
	items[2] = "Delete";
	snprintf(title, sizeof(title), "Filter %d", v->sel + 1);
	ui_menu(title, items, 3, pview_action, v);
}

static const struct vlist_ops pview_ops = { .count = pview_count, .row = pview_row, .click = pview_click, .hold = pview_hold };

static lv_obj_t *pview_create(void *arg)
{
	struct pview *v = calloc(1, sizeof(*v));

	v->ed = ed_get(arg);
	if (!v->ed) {
		free(v);
		return NULL;
	}
	v->scr = ui_screen(v->ed->readonly ? v->ed->name : "Parametric", NULL);
	attach(v->scr, v);
	v->list = vlist_create(v->scr, &pview_ops, v);
	lv_obj_set_size(v->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(v->list, 0, CONTENT_Y);
	vlist_reload(v->list);
	return v->scr;
}

static void pview_shown(lv_obj_t *scr)
{
	struct pview *v = lv_obj_get_user_data(scr);

	vlist_reload(v->list);
}

const struct screen_ops scr_eq_param = { .name = "eq_param", .create = pview_create, .shown = pview_shown };

void eq_open_view(const char *id)
{
	struct earg a = { .id = id };

	nav_push(&scr_eq_param, &a);
}

enum fcol { F_TYPE, F_FC, F_GAIN, F_Q, F_ON, F_N };

struct fview {
	struct eqed *ed;
	int idx;
	lv_obj_t *scr, *col[F_N];
	struct curve curve;
};

static struct eq_filter *fview_filter(struct fview *v)
{
	return &v->ed->p.param[v->idx];
}

static void fview_refresh(struct fview *v)
{
	struct eq_filter *f = fview_filter(v);
	char buf[32];

	ui_spin_set(v->col[F_TYPE], eq_type_names[f->type], "Type");
	fmt_hz(f->fc, buf, sizeof(buf));
	ui_spin_set(v->col[F_FC], buf, "Hz");
	if (eq_type_has_gain(f->type))
		snprintf(buf, sizeof(buf), "%+.1f", f->gain);
	else
		snprintf(buf, sizeof(buf), "–");
	ui_spin_set(v->col[F_GAIN], buf, "dB");
	snprintf(buf, sizeof(buf), "%.2f", f->q);
	ui_spin_set(v->col[F_Q], buf, "Q");
	ui_spin_set(v->col[F_ON], f->on ? "On" : "Off", "");
	curve_update(&v->curve, &v->ed->p);
}

static double r40_steps(double x, int steps, double lo, double hi)
{
	for (int i = 0; i < abs(steps); i++)
		x = eq_step_r40(x, steps);
	return clamp(x, lo, hi);
}

static void fview_spin(lv_obj_t *spin, int steps, void *ctx)
{
	struct fview *v = ctx;
	struct eq_filter *f = fview_filter(v);
	int c = 0;

	while (c < F_N - 1 && v->col[c] != spin)
		c++;
	switch (c) {
	case F_TYPE:
		f->type = (f->type + (steps < 0 ? EQ_NTYPES - 1 : 1)) % EQ_NTYPES;
		break;
	case F_FC:
		f->fc = r40_steps(f->fc, steps, FC_MIN, FC_MAX);
		break;
	case F_GAIN:
		if (!eq_type_has_gain(f->type))
			return;
		f->gain = clamp(round1(f->gain + steps * 0.1), -GAIN_MAX, GAIN_MAX);
		break;
	case F_Q:
		f->q = r40_steps(f->q, steps, Q_MIN, Q_MAX);
		break;
	case F_ON:
		f->on = !f->on;
		break;
	}
	changed(v->ed);
	fview_refresh(v);
}

static lv_obj_t *fview_create(void *arg)
{
	const struct earg *a = arg;
	struct fview *v = calloc(1, sizeof(*v));
	char title[32];
	lv_obj_t *strip;

	v->ed = ed_get(a);
	v->idx = a->idx;
	snprintf(title, sizeof(title), "Filter %d", v->idx + 1);
	v->scr = ui_screen(title, NULL);
	attach(v->scr, v);
	curve_new(v->scr, &v->curve);
	strip = strip_new(v->scr);
	for (int i = 0; i < F_N; i++)
		v->col[i] = ui_spin(strip, COL_W, i == F_TYPE || i == F_ON, fview_spin, v);
	fview_refresh(v);
	return v->scr;
}

const struct screen_ops scr_eq_filter = { .name = "eq_filter", .create = fview_create };
