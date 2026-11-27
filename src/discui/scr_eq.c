#include "ui.h"

#include <disc/conn.h>
#include <disc/eq.h>
#include <disc/kv.h>
#include <disc/settings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum orow { O_DAC, O_SPDIF, O_BT, O_BTDEV, O_PER_DEVICE, O_PRESETS };

struct btstatus {
	char name[64], dev[EQ_ID_MAX];
	bool set;
};

struct outs {
	lv_obj_t *scr, *list;
	enum orow rows[8];
	int n;
	struct btstatus bt;
	struct eq_entry *presets;
	int npresets;
	const char *pick_key;
};

static void bt_status(struct btstatus *s)
{
	char reply[1024];
	const char *line = reply;

	memset(s, 0, sizeof(*s));
	if (disc_request(DISCSYS_SOCK, "eq status\n", reply, sizeof(reply), 2000) < 0)
		return;
	while (line && strncmp(line, "=eq", 3))
		line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL;
	if (!line)
		return;
	kv_get(line, "btname", s->name, sizeof(s->name));
	kv_get(line, "btdev", s->dev, sizeof(s->dev));
	s->set = kv_get_int(line, "btset", 0);
}

static void preset_label(const char *id, char *out, size_t len)
{
	char path[512], name[EQ_NAME_MAX];

	if (!id[0]) {
		snprintf(out, len, "Off");
		return;
	}
	eq_id_name(id, name, sizeof(name));
	if (!eq_id_path(id, path, sizeof(path)) || access(path, R_OK))
		snprintf(out, len, "%s (missing)", name);
	else
		snprintf(out, len, "%s", name);
}

static const char *active_output(void)
{
	return app.sys.out_bt ? "bt" : set_get_bool("audio.spdif") ? "spdif" : "dac";
}

const char *eq_active_summary(char *buf, size_t len)
{
	const char *out = active_output();
	struct btstatus bt;

	if (!strcmp(out, "bt")) {
		bt_status(&bt);
		preset_label(bt.set ? bt.dev : set_get("eq.bt"), buf, len);
	} else {
		preset_label(set_get(!strcmp(out, "dac") ? "eq.dac" : "eq.spdif"), buf, len);
	}
	return buf;
}

static void send_kv(struct kvbuf *b)
{
	sysc_send_line(b->s);
	kv_free(b);
}

static void build(struct outs *o)
{
	bt_status(&o->bt);
	o->n = 0;
	o->rows[o->n++] = O_DAC;
	o->rows[o->n++] = O_SPDIF;
	o->rows[o->n++] = O_BT;
	if (o->bt.name[0])
		o->rows[o->n++] = O_BTDEV;
	o->rows[o->n++] = O_PER_DEVICE;
	o->rows[o->n++] = O_PRESETS;
}

static int outs_count(void *ctx)
{
	return ((struct outs *)ctx)->n;
}

static void outs_row(void *ctx, int idx, struct vrow *r)
{
	struct outs *o = ctx;
	const char *act = active_output();

	switch (o->rows[idx]) {
	case O_DAC:
		snprintf(r->text, sizeof(r->text), "Headphones");
		preset_label(set_get("eq.dac"), r->sub, sizeof(r->sub));
		r->active = !strcmp(act, "dac");
		break;
	case O_SPDIF:
		snprintf(r->text, sizeof(r->text), "S/PDIF");
		preset_label(set_get("eq.spdif"), r->sub, sizeof(r->sub));
		r->active = !strcmp(act, "spdif");
		break;
	case O_BT:
		snprintf(r->text, sizeof(r->text), "%s", o->bt.name[0] ? "Bluetooth default" : "Bluetooth");
		preset_label(set_get("eq.bt"), r->sub, sizeof(r->sub));
		r->active = !strcmp(act, "bt") && !o->bt.set;
		break;
	case O_BTDEV:
		r->icon = LV_SYMBOL_BLUETOOTH;
		snprintf(r->text, sizeof(r->text), "%s", o->bt.name);
		if (o->bt.set)
			preset_label(o->bt.dev, r->sub, sizeof(r->sub));
		else
			snprintf(r->sub, sizeof(r->sub), "Bluetooth default");
		r->active = !strcmp(act, "bt") && o->bt.set;
		break;
	case O_PER_DEVICE:
		snprintf(r->text, sizeof(r->text), "Remember per headset");
		snprintf(r->sub, sizeof(r->sub), "%s", set_get_bool("eq.bt_per_device") ? "On" : "Off");
		break;
	case O_PRESETS:
		snprintf(r->text, sizeof(r->text), "Presets");
		r->icon = LV_SYMBOL_RIGHT;
		break;
	}
}

static void load_presets(struct outs *o)
{
	free(o->presets);
	o->npresets = eq_store_list(&o->presets);
}

static void output_picked(int idx, void *ctx)
{
	struct outs *o = ctx;

	sysc_set(o->pick_key, idx == 0 ? "" : o->presets[idx - 1].id);
}

static void device_picked(int idx, void *ctx)
{
	struct outs *o = ctx;
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "eq btdev");
	if (idx == 0)
		kv_int(&b, "clear", 1);
	else
		kv_str(&b, "id", idx == 1 ? "" : o->presets[idx - 2].id);
	send_kv(&b);
}

static void pick(struct outs *o, const char *title, const char *cur, bool with_default, ui_pick_cb cb)
{
	int base = with_default ? 2 : 1, n, sel = with_default ? 0 : -1;
	char (*labels)[EQ_NAME_MAX + 8];
	const char **items;

	load_presets(o);
	n = o->npresets + base;
	items = calloc(n, sizeof(*items));
	labels = calloc(o->npresets + 1, sizeof(*labels));
	if (with_default)
		items[0] = "Bluetooth default";
	items[base - 1] = "Off";
	if (cur && !cur[0])
		sel = base - 1;
	for (int i = 0; i < o->npresets; i++) {
		snprintf(labels[i], sizeof(labels[i]), "%s%s", o->presets[i].name, o->presets[i].sd ? " (SD)" : "");
		items[base + i] = labels[i];
		if (cur && !strcmp(cur, o->presets[i].id))
			sel = base + i;
	}
	ui_pick(title, items, n, sel, cb, o);
	free(items);
	free(labels);
}

static void outs_click(void *ctx, int idx)
{
	static const char *const keys[] = { [O_DAC] = "eq.dac", [O_SPDIF] = "eq.spdif", [O_BT] = "eq.bt" };
	static const char *const titles[] = { [O_DAC] = "Headphones", [O_SPDIF] = "S/PDIF", [O_BT] = "Bluetooth" };
	struct outs *o = ctx;
	enum orow r = o->rows[idx];

	switch (r) {
	case O_DAC:
	case O_SPDIF:
	case O_BT:
		o->pick_key = keys[r];
		pick(o, titles[r], set_get(keys[r]), false, output_picked);
		break;
	case O_BTDEV:
		pick(o, o->bt.name, o->bt.set ? o->bt.dev : NULL, true, device_picked);
		break;
	case O_PER_DEVICE:
		sysc_set("eq.bt_per_device", set_get_bool("eq.bt_per_device") ? "0" : "1");
		break;
	case O_PRESETS:
		nav_push(&scr_eq_presets, NULL);
		break;
	}
}

static const struct vlist_ops outs_ops = { .count = outs_count, .row = outs_row, .click = outs_click };

static void outs_refresh(uint32_t evs, void *ctx)
{
	struct outs *o = ctx;
	int first = vlist_first(o->list);

	(void)evs;
	build(o);
	vlist_reload(o->list);
	vlist_scroll_to(o->list, first, false);
}

static void outs_delete(lv_event_t *e)
{
	struct outs *o = lv_event_get_user_data(e);

	free(o->presets);
	free(o);
}

static lv_obj_t *outs_create(void *arg)
{
	struct outs *o = calloc(1, sizeof(*o));

	(void)arg;
	o->scr = ui_screen("Equalizer", NULL);
	o->list = vlist_create(o->scr, &outs_ops, o);
	lv_obj_set_size(o->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(o->list, 0, CONTENT_Y);
	build(o);
	vlist_reload(o->list);
	app_listen(o->scr, EV_SETTINGS | EV_EQ | EV_SYS | EV_MOUNT | EV_VOLUME, outs_refresh, o);
	lv_obj_add_event_cb(o->scr, outs_delete, LV_EVENT_DELETE, o);
	return o->scr;
}

static void outs_shown(lv_obj_t *scr)
{
	(void)scr;
	app_notify(EV_EQ);
}

const struct screen_ops scr_eq = { .name = "eq", .create = outs_create, .shown = outs_shown };

enum prow { P_HEADER_MY, P_MY, P_NEW, P_HEADER_SD, P_SD, P_HINT };

struct plist {
	lv_obj_t *scr, *list;
	struct eq_entry *v;
	int n;
	struct {
		enum prow kind;
		int entry;
	} rows[512];
	int nrows;
	char sel[EQ_ID_MAX];
};

static void plist_add(struct plist *p, enum prow kind, int entry)
{
	if (p->nrows < (int)(sizeof(p->rows) / sizeof(p->rows[0]))) {
		p->rows[p->nrows].kind = kind;
		p->rows[p->nrows++].entry = entry;
	}
}

static void plist_build(struct plist *p)
{
	bool sd = false;

	free(p->v);
	p->n = eq_store_list(&p->v);
	p->nrows = 0;
	plist_add(p, P_HEADER_MY, -1);
	for (int i = 0; i < p->n; i++)
		if (!p->v[i].sd)
			plist_add(p, P_MY, i);
	plist_add(p, P_NEW, -1);
	plist_add(p, P_HEADER_SD, -1);
	for (int i = 0; i < p->n; i++)
		if (p->v[i].sd) {
			plist_add(p, P_SD, i);
			sd = true;
		}
	if (!sd)
		plist_add(p, P_HINT, -1);
}

static int plist_count(void *ctx)
{
	return ((struct plist *)ctx)->nrows;
}

static void plist_row(void *ctx, int idx, struct vrow *r)
{
	struct plist *p = ctx;
	int e = p->rows[idx].entry;

	switch (p->rows[idx].kind) {
	case P_HEADER_MY:
		snprintf(r->text, sizeof(r->text), "My presets");
		r->header = true;
		break;
	case P_HEADER_SD:
		snprintf(r->text, sizeof(r->text), "AutoEQ on SD card");
		r->header = true;
		break;
	case P_MY:
	case P_SD:
		snprintf(r->text, sizeof(r->text), "%s", p->v[e].name);
		if (p->v[e].sd && strchr(p->v[e].id + 3, '/'))
			snprintf(r->sub, sizeof(r->sub), "%.*s", (int)(strrchr(p->v[e].id, '/') - p->v[e].id - 3), p->v[e].id + 3);
		r->icon = LV_SYMBOL_RIGHT;
		break;
	case P_NEW:
		snprintf(r->text, sizeof(r->text), "New preset");
		r->icon = LV_SYMBOL_PLUS;
		break;
	case P_HINT:
		snprintf(r->text, sizeof(r->text), "Copy AutoEQ \"ParametricEQ.txt\" files to the EQ folder on the card");
		r->dim = true;
		break;
	}
}

static bool my_exists(const char *name)
{
	char id[EQ_NAME_MAX + 4], path[512];

	snprintf(id, sizeof(id), "my:%s", name);
	return eq_id_path(id, path, sizeof(path)) && !access(path, F_OK);
}

void eq_unique_name(const char *base, char *out, size_t len)
{
	char trimmed[EQ_NAME_MAX - 8];

	snprintf(trimmed, sizeof(trimmed), "%s", base);
	snprintf(out, len, "%s", trimmed);
	for (int i = 2; my_exists(out) && i < 1000; i++)
		snprintf(out, len, "%s %d", trimmed, i);
}

static bool check_name(const char *name)
{
	if (!eq_name_valid(name)) {
		ui_toast("Invalid name");
		return false;
	}
	if (my_exists(name)) {
		ui_toast("A preset with this name exists");
		return false;
	}
	return true;
}

static void new_named(const char *text, void *ctx)
{
	struct eq_preset p;
	char id[EQ_NAME_MAX + 4];

	(void)ctx;
	if (!check_name(text))
		return;
	eq_preset_init(&p);
	snprintf(id, sizeof(id), "my:%s", text);
	eq_open_new(id, &p);
}

static void renamed(const char *text, void *ctx)
{
	struct plist *p = ctx;
	struct kvbuf b;

	if (!strcmp(text, p->sel + 3) || !check_name(text))
		return;
	kv_init(&b);
	kv_raw(&b, "eq rename");
	kv_str(&b, "name", p->sel + 3);
	kv_str(&b, "to", text);
	send_kv(&b);
}

static void deleted(void *ctx)
{
	struct plist *p = ctx;
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "eq delete");
	kv_str(&b, "name", p->sel + 3);
	send_kv(&b);
}

static void duplicate(struct plist *p)
{
	struct eq_preset pre;
	char name[EQ_NAME_MAX], id[EQ_NAME_MAX + 4];

	if (eq_store_load(p->sel, &pre)) {
		ui_toast("Cannot read the preset");
		return;
	}
	eq_unique_name(p->sel + 3, name, sizeof(name));
	snprintf(id, sizeof(id), "my:%s", name);
	eq_open_new(id, &pre);
}

static void my_action(int idx, void *ctx)
{
	struct plist *p = ctx;
	char text[160];

	if (idx == 0) {
		ui_text_input("Rename", p->sel + 3, false, renamed, p);
	} else if (idx == 1) {
		duplicate(p);
	} else if (idx == 2) {
		snprintf(text, sizeof(text), "Delete \"%s\"?", p->sel + 3);
		ui_confirm(text, "Delete", deleted, p);
	}
}

static void plist_click(void *ctx, int idx)
{
	struct plist *p = ctx;
	int e = p->rows[idx].entry;

	switch (p->rows[idx].kind) {
	case P_MY:
		eq_open_editor(p->v[e].id);
		break;
	case P_SD:
		eq_open_view(p->v[e].id);
		break;
	case P_NEW:
		ui_text_input("Preset name", "", false, new_named, p);
		break;
	default:
		break;
	}
}

static void plist_hold(void *ctx, int idx)
{
	static const char *const items[] = { "Rename…", "Duplicate", "Delete…" };
	struct plist *p = ctx;
	int e = p->rows[idx].entry;

	if (p->rows[idx].kind != P_MY)
		return;
	snprintf(p->sel, sizeof(p->sel), "%s", p->v[e].id);
	ui_menu(p->v[e].name, items, 3, my_action, p);
}

static const struct vlist_ops plist_ops = { .count = plist_count, .row = plist_row, .click = plist_click, .hold = plist_hold };

static void plist_refresh(uint32_t evs, void *ctx)
{
	struct plist *p = ctx;
	int first = vlist_first(p->list);

	(void)evs;
	plist_build(p);
	vlist_reload(p->list);
	vlist_scroll_to(p->list, first, false);
}

static void plist_delete(lv_event_t *e)
{
	struct plist *p = lv_event_get_user_data(e);

	free(p->v);
	free(p);
}

static lv_obj_t *plist_create(void *arg)
{
	struct plist *p = calloc(1, sizeof(*p));

	(void)arg;
	p->scr = ui_screen("Presets", NULL);
	lv_obj_set_user_data(p->scr, p);
	p->list = vlist_create(p->scr, &plist_ops, p);
	lv_obj_set_size(p->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(p->list, 0, CONTENT_Y);
	plist_build(p);
	vlist_reload(p->list);
	app_listen(p->scr, EV_EQ | EV_MOUNT, plist_refresh, p);
	lv_obj_add_event_cb(p->scr, plist_delete, LV_EVENT_DELETE, p);
	return p->scr;
}

static void plist_shown(lv_obj_t *scr)
{
	plist_refresh(0, lv_obj_get_user_data(scr));
}

const struct screen_ops scr_eq_presets = { .name = "eq_presets", .create = plist_create, .shown = plist_shown };
