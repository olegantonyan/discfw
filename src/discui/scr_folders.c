#include "fsdir.h"
#include "mpdc.h"
#include "ui.h"

#include <dirent.h>
#include <disc/paths.h>
#include <disc/settings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum pending_op { PEND_NONE, PEND_CREATE, PEND_APPEND };

struct folders {
	lv_obj_t *scr, *title, *list;
	char path[2048];
	char playing[2048];
	enum pending_op pend;
	char pend_dir[2048], pend_name[512];
	bool pend_recursive;
};

static const char *root(void)
{
	return disc_music_root();
}

static bool at_root(struct folders *f)
{
	return !strcmp(f->path, root());
}

static const struct dirlist *dl(struct folders *f)
{
	return fsdir_get(f->path);
}

static bool valid(struct folders *f, int idx)
{
	return idx >= 0 && idx < dl(f)->n;
}

static void full_path(struct folders *f, int idx, char *out, size_t len)
{
	snprintf(out, len, "%s/%s", f->path, dl(f)->v[idx].name);
}

static void dir_key(const char *path, char *key, size_t len)
{
	snprintf(key, len, "dir.%s", path);
}

static void save_pos(struct folders *f)
{
	char key[2100];

	dir_key(f->path, key, sizeof(key));
	state_set_int(key, vlist_first(f->list));
	state_set("folder.path", f->path);
}

static int count(void *ctx)
{
	return dl(ctx)->n;
}

static void row(void *ctx, int idx, struct vrow *r)
{
	struct folders *f = ctx;
	const struct dentry *e;
	char full[2100];

	if (!valid(f, idx))
		return;
	e = &dl(f)->v[idx];
	r->icon = e->dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO;
	snprintf(r->text, sizeof(r->text), "%s", e->name);
	if (!e->dir && f->playing[0]) {
		full_path(f, idx, full, sizeof(full));
		r->active = !strcmp(full, f->playing);
	} else if (e->dir && f->playing[0]) {
		full_path(f, idx, full, sizeof(full));
		size_t n = strlen(full);
		r->active = !strncmp(f->playing, full, n) && f->playing[n] == '/';
	}
}

static uint32_t letter(void *ctx, int idx)
{
	return valid(ctx, idx) ? dl(ctx)->v[idx].letter : 0;
}

static void update_playing(struct folders *f)
{
	char *p = app.mpd.cur.uri[0] ? mpdc_path_for_uri(app.mpd.cur.uri) : NULL;

	snprintf(f->playing, sizeof(f->playing), "%s", p ? p : "");
	free(p);
}

static void load(struct folders *f, const char *path, const char *select)
{
	char key[2100];
	const struct dirlist *l;
	const char *name;
	int first = 0;

	snprintf(f->path, sizeof(f->path), "%s", path);
	l = dl(f);
	name = strrchr(f->path, '/');
	lv_label_set_text(f->title, at_root(f) ? "SD card" : name ? name + 1 : f->path);
	vlist_set_empty(f->list, !l->ok ? (at_root(f) ? "No SD card" : "Folder not found") : "No music here");
	vlist_reload(f->list);
	dir_key(f->path, key, sizeof(key));
	first = state_get_int(key, 0);
	if (select) {
		for (int i = 0; i < l->n; i++)
			if (!strcmp(l->v[i].name, select)) {
				if (i < first || i > first + 4)
					first = i > 2 ? i - 2 : 0;
				break;
			}
	}
	vlist_scroll_to(f->list, first, false);
	state_set("folder.path", f->path);
}

static void enter(struct folders *f, const char *path, const char *select)
{
	save_pos(f);
	load(f, path, select);
}

static bool scan_requested(const char *dir)
{
	static char done[16][2048];
	static int next;

	for (int i = 0; i < 16; i++)
		if (!strcmp(done[i], dir))
			return true;
	snprintf(done[next++ % 16], sizeof(done[0]), "%s", dir);
	return false;
}

static int collect(const char *dir, bool recursive, char ***uris)
{
	int missing, n;

	if (recursive && fsdir_get(dir)->ndirs >= 8)
		ui_busy("Adding…");
	n = fsdir_collect(dir, recursive, uris, &missing);
	if (missing && !scan_requested(dir)) {
		mpdc_update(dir);
		ui_toast("Folder not in library yet, scanning in background");
	}
	return n;
}

static void play_dir(struct folders *f, const char *dir, int start_file, bool shuffle)
{
	char **uris, origin[2100];
	int n;

	(void)f;
	if (!app.mpd.connected) {
		ui_toast("Music service not running");
		return;
	}
	n = collect(dir, set_get_bool("play.recursive"), &uris);
	if (!n) {
		fsdir_free_strv(uris, n);
		ui_toast("No music here");
		return;
	}
	snprintf(origin, sizeof(origin), "folder:%s", dir);
	mpdc_set_order(shuffle ? "random" : set_get("play.order"));
	if (mpdc_play_uris(uris, n, shuffle ? -1 : start_file < 0 ? 0 : start_file, origin) == 0)
		nav_push(&scr_np, NULL);
	else
		ui_toast("Could not start playback");
	fsdir_free_strv(uris, n);
}

static void rel_of(const char *dir, char *out, size_t len)
{
	char *rel = mpdc_uri_for_path(dir);
	snprintf(out, len, "%s", rel ? rel : "");
	free(rel);
}

static void run_pending(struct folders *f)
{
	char **uris;
	int missing, n, k = 0;

	n = fsdir_collect(f->pend_dir, f->pend_recursive, &uris, &missing);
	for (int i = 0; i < n; i++) {
		if (strncmp(uris[i], "file://", 7) && uris[i][0] != '/')
			uris[k++] = uris[i];
		else
			free(uris[i]);
	}
	if (missing && app.mpd.updating) {
		fsdir_free_strv(uris, k);
		return;
	}
	enum pending_op op = f->pend;
	f->pend = PEND_NONE;
	if (!k) {
		ui_toast("Nothing to add (not in library)");
	} else if (op == PEND_CREATE) {
		if (mpdc_playlist_create(f->pend_name, uris, k) >= 0) {
			char msg[600];
			snprintf(msg, sizeof(msg), "Created playlist \"%s\" (%d)", f->pend_name, k);
			ui_toast(msg);
		}
	} else if (op == PEND_APPEND) {
		char msg[600];
		int added = mpdc_playlist_append(f->pend_name, uris, k);
		snprintf(msg, sizeof(msg), "Added %d to \"%s\"", added < 0 ? 0 : added, f->pend_name);
		ui_toast(msg);
	}
	fsdir_free_strv(uris, k);
}

static void to_playlist(struct folders *f, const char *dir, enum pending_op op, const char *name)
{
	char **uris;
	int missing, n;

	f->pend = op;
	f->pend_recursive = true;
	snprintf(f->pend_dir, sizeof(f->pend_dir), "%s", dir);
	snprintf(f->pend_name, sizeof(f->pend_name), "%s", name);
	n = fsdir_collect(dir, true, &uris, &missing);
	fsdir_free_strv(uris, n);
	if (missing) {
		mpdc_update(dir);
		ui_toast("Scanning folder first…");
		return;
	}
	run_pending(f);
}

static void on_update(uint32_t evs, void *ctx)
{
	struct folders *f = ctx;

	if ((evs & EV_UPDATE) && f->pend != PEND_NONE && !app.mpd.updating)
		run_pending(f);
	if (evs & EV_SONG) {
		update_playing(f);
		vlist_refresh(f->list);
	}
	if (evs & EV_MOUNT) {
		fsdir_invalidate();
		load(f, f->path, NULL);
	}
}

struct pick_ctx {
	struct folders *f;
	char dir[2048];
	char **names;
	int n;
};

static void pick_free(struct pick_ctx *p)
{
	fsdir_free_strv(p->names, p->n);
	free(p);
}

static void new_playlist_named(const char *text, void *ctx)
{
	struct pick_ctx *p = ctx;

	if (text[0] && !strchr(text, '/'))
		to_playlist(p->f, p->dir, PEND_CREATE, text);
	else
		ui_toast("Invalid name");
	pick_free(p);
}

static void playlist_picked(int idx, void *ctx)
{
	struct pick_ctx *p = ctx;
	char rel[2048], name[512];

	if (idx == 0) {
		rel_of(p->dir, rel, sizeof(rel));
		mpdc_playlist_name_for(name, sizeof(name), rel);
		ui_text_input("New playlist", name, false, new_playlist_named, p);
		return;
	}
	to_playlist(p->f, p->dir, PEND_APPEND, p->names[idx - 1]);
	pick_free(p);
}

static void choose_playlist(struct folders *f, const char *dir)
{
	struct pick_ctx *p = calloc(1, sizeof(*p));
	const char **items;

	p->f = f;
	snprintf(p->dir, sizeof(p->dir), "%s", dir);
	p->n = mpdc_playlists(&p->names);
	if (p->n < 0)
		p->n = 0;
	items = calloc(p->n + 1, sizeof(char *));
	items[0] = LV_SYMBOL_PLUS " New playlist…";
	for (int i = 0; i < p->n; i++)
		items[i + 1] = p->names[i];
	ui_menu("Add to playlist", items, p->n + 1, playlist_picked, p);
	free(items);
}

struct menu_ctx {
	struct folders *f;
	char path[2048];
	int idx;
	bool dir;
};

enum { M_PLAY, M_SHUFFLE, M_QUEUE, M_CREATE, M_APPEND, M_APPEND_LOADED, M_RESCAN, M_INFO, M_GOTO_PLAYING };

static int menu_map[12];

static void append_queue(const char *dir, bool recursive, const char *file)
{
	char **uris;
	int n, added;
	char msg[64];

	if (file) {
		char *dir_copy = strdup(file), *slash = strrchr(dir_copy, '/'), **one;
		*slash = 0;
		char *name = slash + 1;
		mpdc_resolve(dir_copy, &name, 1, &one);
		added = mpdc_append_uris(one, 1);
		fsdir_free_strv(one, 1);
		free(dir_copy);
	} else {
		n = collect(dir, recursive, &uris);
		added = mpdc_append_uris(uris, n);
		fsdir_free_strv(uris, n);
	}
	snprintf(msg, sizeof(msg), "Added %d to queue", added < 0 ? 0 : added);
	ui_toast(msg);
}

static void menu_picked(int idx, void *ctx)
{
	struct menu_ctx *m = ctx;
	struct folders *f = m->f;
	char rel[2048], name[512];
	const char *loaded = app_loaded_playlist();

	switch (menu_map[idx]) {
	case M_PLAY:
		if (m->dir)
			play_dir(f, m->path, 0, false);
		else
			play_dir(f, f->path, m->idx - dl(f)->ndirs, false);
		break;
	case M_SHUFFLE:
		play_dir(f, m->path, 0, true);
		break;
	case M_QUEUE:
		append_queue(m->path, true, m->dir ? NULL : m->path);
		break;
	case M_CREATE:
		rel_of(m->path, rel, sizeof(rel));
		mpdc_playlist_name_for(name, sizeof(name), rel);
		to_playlist(f, m->path, PEND_CREATE, name);
		break;
	case M_APPEND:
		choose_playlist(f, m->path);
		break;
	case M_APPEND_LOADED:
		to_playlist(f, m->path, PEND_APPEND, loaded);
		break;
	case M_RESCAN:
		mpdc_update(m->path);
		ui_toast("Scanning…");
		break;
	case M_INFO:
		ui_info("Path", m->path);
		break;
	case M_GOTO_PLAYING:
		if (f->playing[0]) {
			char *dir = strdup(f->playing), *slash = strrchr(dir, '/');
			if (slash) {
				*slash = 0;
				enter(f, dir, slash + 1);
			}
			free(dir);
		}
		break;
	}
	free(m);
}

static void open_menu(struct folders *f, const char *path, bool dir, int idx)
{
	struct menu_ctx *m = calloc(1, sizeof(*m));
	const char *items[12], *loaded = app_loaded_playlist();
	char append_loaded[300];
	int n = 0;

	m->f = f;
	m->dir = dir;
	m->idx = idx;
	snprintf(m->path, sizeof(m->path), "%s", path);
#define ADD(id, text) (menu_map[n] = (id), items[n++] = (text))
	ADD(M_PLAY, dir ? "Play" : "Play from here");
	if (dir)
		ADD(M_SHUFFLE, "Shuffle");
	ADD(M_QUEUE, "Add to queue");
	if (dir) {
		ADD(M_CREATE, "Create playlist");
		ADD(M_APPEND, "Add to playlist…");
		if (loaded[0]) {
			snprintf(append_loaded, sizeof(append_loaded), "Add to \"%s\"", loaded);
			ADD(M_APPEND_LOADED, append_loaded);
		}
		ADD(M_RESCAN, "Rescan folder");
	}
	if (idx < 0 && f->playing[0])
		ADD(M_GOTO_PLAYING, "Go to playing track");
	ADD(M_INFO, "Path");
#undef ADD
	const char *slash = strrchr(path, '/');
	ui_menu(idx < 0 && !strcmp(path, root()) ? "SD card" : slash ? slash + 1 : path, items, n, menu_picked, m);
}

static void click(void *ctx, int idx)
{
	struct folders *f = ctx;
	char full[2100];

	if (!valid(f, idx))
		return;
	full_path(f, idx, full, sizeof(full));
	if (dl(f)->v[idx].dir) {
		enter(f, full, NULL);
		return;
	}
	save_pos(f);
	play_dir(f, f->path, idx - dl(f)->ndirs, false);
}

static void hold(void *ctx, int idx)
{
	struct folders *f = ctx;
	char full[2100];

	if (!valid(f, idx))
		return;
	full_path(f, idx, full, sizeof(full));
	open_menu(f, full, dl(f)->v[idx].dir, idx);
}

static const struct vlist_ops ops = { .count = count, .row = row, .click = click, .hold = hold, .letter = letter };

static void title_click(lv_event_t *e)
{
	struct folders *f = lv_event_get_user_data(e);
	open_menu(f, f->path, true, -1);
}

static bool path_ok(const char *path)
{
	DIR *d = opendir(path);

	if (d)
		closedir(d);
	return d != NULL;
}

static void on_delete(lv_event_t *e)
{
	struct folders *f = lv_event_get_user_data(e);

	save_pos(f);
	free(f);
}

static lv_obj_t *create(void *arg)
{
	struct folders *f = calloc(1, sizeof(*f));
	char start[2048], select[512] = "";
	const char *saved = state_get("folder.path");

	snprintf(start, sizeof(start), "%s", arg ? (const char *)arg : saved && *saved ? saved : root());
	if (strncmp(start, root(), strlen(root())))
		snprintf(start, sizeof(start), "%s", root());
	while (!path_ok(start) && strcmp(start, root())) {
		char *slash = strrchr(start, '/');
		if (!slash)
			break;
		*slash = 0;
	}
	if (arg && ((const char *)arg)[strlen(arg) + 1])
		snprintf(select, sizeof(select), "%s", (const char *)arg + strlen(arg) + 1);
	f->scr = ui_screen("", &f->title);
	lv_obj_set_user_data(f->scr, f);
	lv_obj_set_clickable(f->title, true);
	lv_obj_set_ext_click_area(f->title, 12);
	lv_obj_add_event_cb(f->title, title_click, LV_EVENT_CLICKED, f);
	f->list = vlist_create(f->scr, &ops, f);
	lv_obj_set_size(f->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(f->list, 0, CONTENT_Y);
	update_playing(f);
	load(f, start, select[0] ? select : NULL);
	app_listen(f->scr, EV_UPDATE | EV_SONG | EV_MOUNT, on_update, f);
	lv_obj_add_event_cb(f->scr, on_delete, LV_EVENT_DELETE, f);
	return f->scr;
}

static bool back(lv_obj_t *scr)
{
	struct folders *f = lv_obj_get_user_data(scr);
	char parent[2048], *slash;

	if (at_root(f))
		return false;
	snprintf(parent, sizeof(parent), "%s", f->path);
	slash = strrchr(parent, '/');
	if (!slash)
		return false;
	*slash = 0;
	enter(f, parent, slash + 1);
	return true;
}

void folders_open_at(const char *path, const char *select_name)
{
	static char arg[2600];
	size_t n = strlen(path);

	snprintf(arg, sizeof(arg), "%s", path);
	snprintf(arg + n + 1, sizeof(arg) - n - 1, "%s", select_name ? select_name : "");
	nav_open(&scr_folders, arg);
}

const struct screen_ops scr_folders = { .name = "folders", .create = create, .back = back };

void folders_open_uri(const char *uri)
{
	char *path = uri[0] ? mpdc_path_for_uri(uri) : NULL, *slash;

	if (!path)
		return;
	slash = strrchr(path, '/');
	if (slash) {
		*slash = 0;
		folders_open_at(path, slash + 1);
	}
	free(path);
}
