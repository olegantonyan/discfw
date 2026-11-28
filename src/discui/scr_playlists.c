#include "groups.h"
#include "mpdc.h"
#include "ui.h"

#include <disc/settings.h>

#include <disc/text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pls {
	lv_obj_t *scr, *list;
	char **names;
	int n;
	int target;
};

static void free_names(struct pls *p)
{
	for (int i = 0; i < p->n; i++)
		free(p->names[i]);
	free(p->names);
	p->names = NULL;
	p->n = 0;
}

static void reload(struct pls *p)
{
	free_names(p);
	p->n = mpdc_playlists(&p->names);
	if (p->n < 0)
		p->n = 0;
	vlist_reload(p->list);
}

static int count(void *ctx)
{
	return ((struct pls *)ctx)->n;
}

static void mode_key(char *key, size_t len, const char *name)
{
	snprintf(key, len, "pl.mode.%s", name);
}

static void row(void *ctx, int idx, struct vrow *r)
{
	struct pls *p = ctx;
	char key[300];
	const char *mode;

	r->icon = LV_SYMBOL_LIST;
	snprintf(r->text, sizeof(r->text), "%s", p->names[idx]);
	r->active = !strcmp(p->names[idx], app_loaded_playlist());
	mode_key(key, sizeof(key), p->names[idx]);
	mode = state_get(key);
	if (mode && *mode)
		snprintf(r->right, sizeof(r->right), "%s", !strcmp(mode, "random") ? LV_SYMBOL_SHUFFLE : !strcmp(mode, "noloop") ? "1×" : LV_SYMBOL_LOOP);
}

static uint32_t letter(void *ctx, int idx)
{
	return index_letter(((struct pls *)ctx)->names[idx]);
}

static void click(void *ctx, int idx)
{
	nav_push(&scr_playlist, ((struct pls *)ctx)->names[idx]);
}

static void save_order(struct pls *p)
{
	mpdc_playlist_order_set(p->names, p->n);
}

static void renamed(const char *text, void *ctx)
{
	struct pls *p = ctx;

	if (!text[0] || strchr(text, '/')) {
		ui_toast("Invalid name");
		return;
	}
	if (p->target < p->n && mpdc_playlist_rename(p->names[p->target], text)) {
		free(p->names[p->target]);
		p->names[p->target] = strdup(text);
		save_order(p);
	}
}

static void do_delete(void *ctx)
{
	struct pls *p = ctx;

	if (p->target < p->n)
		mpdc_playlist_delete(p->names[p->target]);
}

static const char *const modes[] = { "", "seq", "random", "noloop" };
static const char *const mode_labels[] = { "Default", "Sequential", "Shuffle", "Sequential, no loop" };

static void mode_picked(int idx, void *ctx)
{
	struct pls *p = ctx;
	char key[300];

	mode_key(key, sizeof(key), p->names[p->target]);
	state_set(key, idx ? modes[idx] : NULL);
	if (!strcmp(p->names[p->target], app_loaded_playlist()))
		mpdc_set_order(idx ? modes[idx] : "seq");
	vlist_refresh(p->list);
}

static void move(struct pls *p, int d)
{
	int i = p->target, j = i + d;
	char *t;

	if (j < 0 || j >= p->n)
		return;
	t = p->names[i];
	p->names[i] = p->names[j];
	p->names[j] = t;
	save_order(p);
	vlist_refresh(p->list);
}

static void item_picked(int choice, void *ctx)
{
	struct pls *p = ctx;
	char key[300];
	const char *mode;
	int cur = 0;

	if (p->target >= p->n)
		return;
	switch (choice) {
	case 0:
		if (mpdc_playlist_play(p->names[p->target], 0))
			nav_push(&scr_np, NULL);
		break;
	case 1:
		if (mpdc_playlist_play(p->names[p->target], 0)) {
			mpdc_set_order("random");
			nav_push(&scr_np, NULL);
		}
		break;
	case 2:
		ui_text_input("Rename", p->names[p->target], false, renamed, p);
		break;
	case 3:
		mode_key(key, sizeof(key), p->names[p->target]);
		mode = state_get(key);
		for (int i = 1; i < 4; i++)
			if (mode && !strcmp(mode, modes[i]))
				cur = i;
		ui_pick("Play order", mode_labels, 4, cur, mode_picked, p);
		break;
	case 4:
		move(p, -1);
		break;
	case 5:
		move(p, 1);
		break;
	case 6:
		ui_confirm("Delete this playlist?", "Delete", do_delete, p);
		break;
	}
}

static void hold(void *ctx, int idx)
{
	static const char *const items[] = { "Play", "Shuffle", "Rename", "Play order", "Move up", "Move down", "Delete" };
	struct pls *p = ctx;

	p->target = idx;
	ui_menu(p->names[idx], items, 7, item_picked, p);
}

static const struct vlist_ops ops = { .count = count, .row = row, .click = click, .hold = hold, .letter = letter };

static void new_named(const char *text, void *ctx)
{
	(void)ctx;
	if (!text[0] || strchr(text, '/')) {
		ui_toast("Invalid name");
		return;
	}
	if (mpdc_playlist_create(text, NULL, 0) >= 0)
		ui_toast("Created");
}

static void title_picked(int idx, void *ctx)
{
	(void)ctx;
	if (idx == 0)
		ui_text_input("New playlist", "", false, new_named, NULL);
	else if (idx == 1 && app_loaded_playlist()[0])
		nav_push(&scr_playlist, (void *)app_loaded_playlist());
}

static void title_click(lv_event_t *e)
{
	static const char *const items[] = { "New empty playlist…", "Open loaded playlist" };
	ui_menu("Playlists", items, app_loaded_playlist()[0] ? 2 : 1, title_picked, lv_event_get_user_data(e));
}

static void on_event(uint32_t evs, void *ctx)
{
	struct pls *p = ctx;

	if (evs & EV_PLAYLISTS)
		reload(p);
	else
		vlist_refresh(p->list);
}

static void on_delete(lv_event_t *e)
{
	struct pls *p = lv_event_get_user_data(e);

	free_names(p);
	free(p);
}

static lv_obj_t *create(void *arg)
{
	struct pls *p = calloc(1, sizeof(*p));
	lv_obj_t *title;

	(void)arg;
	p->scr = ui_screen("Playlists", &title);
	lv_obj_set_clickable(title, true);
	lv_obj_set_ext_click_area(title, 12);
	lv_obj_add_event_cb(title, title_click, LV_EVENT_CLICKED, p);
	p->list = vlist_create(p->scr, &ops, p);
	lv_obj_set_size(p->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(p->list, 0, CONTENT_Y);
	vlist_set_empty(p->list, "No playlists.\nHold a folder → Create playlist");
	reload(p);
	app_listen(p->scr, EV_PLAYLISTS | EV_QUEUE, on_event, p);
	lv_obj_add_event_cb(p->scr, on_delete, LV_EVENT_DELETE, p);
	return p->scr;
}

const struct screen_ops scr_playlists = { .name = "playlists", .create = create };

struct pl {
	lv_obj_t *scr, *list;
	char name[512];
	struct pager z;
	struct groups g;
	int target;
	char **labels, **presets;
	int n_presets;
};

static void pl_reload(struct pl *p)
{
	pager_open(&p->z, p->name);
	groups_build(&p->g, &p->z);
	vlist_reload(p->list);
}

static int pl_count(void *ctx)
{
	return ((struct pl *)ctx)->g.n;
}

static void pl_row(void *ctx, int row, struct vrow *r)
{
	struct pl *p = ctx;
	char buf[256];
	int idx = p->g.map[row];

	if (idx < 0) {
		snprintf(r->text, sizeof(r->text), "%s", p->g.head[-idx - 1]);
		snprintf(r->sub, sizeof(r->sub), "%s", p->g.head_sub[-idx - 1]);
		r->header = true;
		return;
	}

	const struct item *it = pager_at(&p->z, idx);

	r->active = !strcmp(p->name, app_loaded_playlist()) && idx == app.mpd.song_pos;
	if (!it)
		return;
	snprintf(r->text, sizeof(r->text), "%s", song_label(it, buf, sizeof(buf)));
	snprintf(r->sub, sizeof(r->sub), "%s", it->artist);
	if (it->dur_ms)
		fmt_time(r->right, sizeof(r->right), it->dur_ms);
}

static void pl_click(void *ctx, int row)
{
	struct pl *p = ctx;
	int idx = groups_track_at(&p->g, row);

	if (idx < 0)
		return;

	if (!strcmp(p->name, app_loaded_playlist()) && app.mpd.queue_len == p->z.n)
		mpdc_play_pos(idx);
	else
		mpdc_playlist_play(p->name, idx);
	nav_push(&scr_np, NULL);
}

static void pl_picked(int choice, void *ctx)
{
	struct pl *p = ctx;
	int i = p->target;
	char *path, *slash;

	const struct item *it = pager_at(&p->z, i);

	if (!it)
		return;
	switch (choice) {
	case 0:
		pl_click(p, i);
		break;
	case 1:
		if (i > 0)
			mpdc_playlist_move(p->name, i, i - 1);
		break;
	case 2:
		if (i + 1 < p->z.n)
			mpdc_playlist_move(p->name, i, i + 1);
		break;
	case 3:
		mpdc_playlist_remove(p->name, i);
		break;
	case 4:
		path = mpdc_path_for_uri(it->uri);
		if (path && (slash = strrchr(path, '/'))) {
			*slash = 0;
			folders_open_at(path, slash + 1);
		}
		free(path);
		break;
	}
}

static void pl_hold(void *ctx, int row)
{
	static const char *const items[] = { "Play from here", "Move up", "Move down", "Remove", "Go to folder" };
	struct pl *p = ctx;
	const struct item *it = pager_at(&p->z, p->g.map[row]);
	char buf[256];

	if (!it)
		return;
	p->target = p->g.map[row];
	ui_menu(song_label(it, buf, sizeof(buf)), items, 5, pl_picked, p);
}

static const struct vlist_ops pl_ops = { .count = pl_count, .row = pl_row, .click = pl_click, .hold = pl_hold };

static void pl_event(uint32_t evs, void *ctx)
{
	struct pl *p = ctx;

	if (evs & (EV_PLAYLISTS | EV_SETTINGS))
		pl_reload(p);
	else
		vlist_refresh(p->list);
}

static void pl_sort_picked(int idx, void *ctx)
{
	struct pl *p = ctx;

	if (idx < p->n_presets && mpdc_sort_playlist(p->name, p->presets[idx]))
		ui_toast("Sorted");
	pl_reload(p);
	if (!strcmp(p->name, app_loaded_playlist()) && app.mpd.song_pos >= 0)
		vlist_scroll_to(p->list, groups_row_of(&p->g, app.mpd.song_pos), true);
	else
		vlist_scroll_to(p->list, 0, false);
}

static const char *const groups_values[] = { "none", "album", "folder", "both" };
static const char *const groups_labels[] = { "No grouping", "Album", "Folder", "Album and folder" };

static void group_picked(int idx, void *ctx)
{
	(void)ctx;
	sysc_set("pl.group", groups_values[idx]);
}

static void pl_title_picked(int idx, void *ctx)
{
	struct pl *p = ctx;

	switch (idx) {
	case 0:
		if (mpdc_playlist_play(p->name, 0))
			nav_push(&scr_np, NULL);
		break;
	case 1:
		if (p->z.n > SORT_MAX) {
			ui_toast("Too many songs to sort");
			break;
		}
		sort_presets_free(p->labels, p->presets, p->n_presets);
		p->n_presets = sort_presets(&p->labels, &p->presets);
		ui_menu("Sort by", (const char *const *)p->labels, p->n_presets, pl_sort_picked, p);
		break;
	case 2:
		ui_pick("Group by", groups_labels, 4, set_get_enum("pl.group"), group_picked, NULL);
		break;
	}
}

static void pl_title_click(lv_event_t *e)
{
	static const char *const items[] = { "Play", "Sort…", "Group by…" };
	struct pl *p = lv_event_get_user_data(e);

	ui_menu(p->name, items, 3, pl_title_picked, p);
}

static void pl_delete(lv_event_t *e)
{
	struct pl *p = lv_event_get_user_data(e);

	sort_presets_free(p->labels, p->presets, p->n_presets);
	groups_free(&p->g);
	pager_free(&p->z);
	free(p);
}

static lv_obj_t *pl_create(void *arg)
{
	struct pl *p = calloc(1, sizeof(*p));

	lv_obj_t *title;

	snprintf(p->name, sizeof(p->name), "%s", arg ? (const char *)arg : "");
	p->scr = ui_screen(p->name, &title);
	lv_obj_set_clickable(title, true);
	lv_obj_set_ext_click_area(title, 12);
	lv_obj_add_event_cb(title, pl_title_click, LV_EVENT_CLICKED, p);
	p->list = vlist_create(p->scr, &pl_ops, p);
	lv_obj_set_size(p->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(p->list, 0, CONTENT_Y);
	vlist_set_empty(p->list, "Empty playlist");
	pl_reload(p);
	if (!strcmp(p->name, app_loaded_playlist()) && app.mpd.song_pos >= 0 && app.mpd.song_pos < p->z.n)
		vlist_scroll_to(p->list, groups_row_of(&p->g, app.mpd.song_pos), true);
	app_listen(p->scr, EV_PLAYLISTS | EV_SONG | EV_QUEUE | EV_SETTINGS, pl_event, p);
	lv_obj_add_event_cb(p->scr, pl_delete, LV_EVENT_DELETE, p);
	return p->scr;
}

const struct screen_ops scr_playlist = { .name = "playlist", .create = pl_create };

struct add_ctx {
	char **uris, **names;
	int n, n_names;
	char suggest[256];
};

static struct add_ctx *pending_add;

static void add_free(struct add_ctx *a)
{
	if (a == pending_add)
		pending_add = NULL;
	for (int i = 0; i < a->n; i++)
		free(a->uris[i]);
	for (int i = 0; i < a->n_names; i++)
		free(a->names[i]);
	free(a->uris);
	free(a->names);
	free(a);
}

static void add_named(const char *text, void *ctx)
{
	struct add_ctx *a = ctx;
	char msg[600];
	int added;

	if (text[0] && !strchr(text, '/')) {
		added = mpdc_playlist_append(text, a->uris, a->n);
		snprintf(msg, sizeof(msg), "Added %d to \"%s\"", added < 0 ? 0 : added, text);
		ui_toast(msg);
	} else if (text[0]) {
		ui_toast("Invalid name");
	}
	add_free(a);
}

static void add_picked(int idx, void *ctx)
{
	struct add_ctx *a = ctx;

	if (idx == 0)
		ui_text_input("New playlist", a->suggest, false, add_named, a);
	else
		add_named(a->names[idx - 1], a);
}

void playlist_add_uris(char *const *uris, int n, const char *suggest)
{
	struct add_ctx *a;
	const char **items;

	if (pending_add)
		add_free(pending_add);
	a = calloc(1, sizeof(*a));
	a->uris = calloc(n + 1, sizeof(char *));
	for (int i = 0; i < n; i++)
		if (strncmp(uris[i], "file://", 7) && uris[i][0] != '/')
			a->uris[a->n++] = strdup(uris[i]);
	if (!a->n) {
		ui_toast("Nothing to add (not in library)");
		add_free(a);
		return;
	}
	snprintf(a->suggest, sizeof(a->suggest), "%s", suggest ? suggest : "");
	a->n_names = mpdc_playlists(&a->names);
	if (a->n_names < 0)
		a->n_names = 0;
	items = calloc(a->n_names + 1, sizeof(char *));
	items[0] = LV_SYMBOL_PLUS " New playlist…";
	for (int i = 0; i < a->n_names; i++)
		items[i + 1] = a->names[i];
	pending_add = a;
	ui_menu("Add to playlist", items, a->n_names + 1, add_picked, a);
	free(items);
}
