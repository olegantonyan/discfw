#include "mpdc.h"
#include "ui.h"

#include <disc/settings.h>
#include <disc/text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPTH 6
#define EXPR 1600
#define RECENT_SONGS 2000
#define RECENT_ALBUMS 50
#define SEARCH_ARTISTS 20
#define SEARCH_ALBUMS 30
#define SEARCH_SONGS 100

enum view { V_ROOT, V_SEARCH, V_ARTISTS, V_ALBUMS, V_GENRES, V_COMPOSERS, V_YEARS, V_RECENT, V_HIRES, V_TRACKS };
enum kind { K_ROOT, K_SECTION, K_DISC, K_NAME, K_ALBUM, K_YEAR, K_SONG };

struct frame {
	enum view v;
	enum mpd_tag_type tag;
	char title[256];
	char expr[EXPR];
	char album[256], artist[256];
	int year;
	bool by_year;
	int first;
};

struct lrow {
	enum kind k;
	int i;
};

struct lib {
	lv_obj_t *scr, *title, *list;
	struct frame st[DEPTH];
	int depth;
	struct names nm;
	struct albums al;
	struct items it;
	int *years, *year_n, n_years;
	struct lrow *rows;
	int n_rows, cap_rows;
};

static const struct {
	const char *label;
	enum view v;
} roots[] = {
	{ "Search…", V_SEARCH }, { "Artists", V_ARTISTS }, { "Albums", V_ALBUMS },
	{ "Genres", V_GENRES },   { "Composers", V_COMPOSERS }, { "Years", V_YEARS },
	{ "Recently added", V_RECENT }, { "Hi-res", V_HIRES },
};

static const char *const sections[] = { "Artists", "Albums", "Songs" };

static const char *const hires[] = {
	"(AudioFormat =~ \"88200:*:*\")",  "(AudioFormat =~ \"96000:*:*\")",  "(AudioFormat =~ \"176400:*:*\")",
	"(AudioFormat =~ \"192000:*:*\")", "(AudioFormat =~ \"352800:*:*\")", "(AudioFormat =~ \"384000:*:*\")",
	"(AudioFormat =~ \"*:24:*\")",     "(AudioFormat =~ \"*:32:*\")",
};

static char last_query[256];

static struct frame *top(struct lib *l)
{
	return &l->st[l->depth - 1];
}

static enum mpd_tag_type artist_tag(void)
{
	return strcmp(set_get("lib.artist_tag"), "artist") ? MPD_TAG_ALBUM_ARTIST : MPD_TAG_ARTIST;
}

static enum mpd_tag_type sort_tag_of(enum mpd_tag_type t)
{
	switch (t) {
	case MPD_TAG_ARTIST:
		return MPD_TAG_ARTIST_SORT;
	case MPD_TAG_ALBUM_ARTIST:
		return MPD_TAG_ALBUM_ARTIST_SORT;
	case MPD_TAG_COMPOSER:
		return MPD_TAG_COMPOSER_SORT;
	default:
		return MPD_TAG_UNKNOWN;
	}
}

static bool tag_expr(char *out, size_t len, const char *tag, const char *op, const char *v)
{
	char q[600];

	return filter_quote(q, sizeof(q), v) && snprintf(out, len, "(%s %s %s)", tag, op, q) < (int)len;
}

static bool year_expr(char *out, size_t len, int year)
{
	char y[8];

	snprintf(y, sizeof(y), "%04d", year);
	return tag_expr(out, len, "Date", "starts_with", y);
}

static bool album_expr(char *out, size_t len, const char *album, const char *artist, const char *parent)
{
	char a[600], b[600];

	return filter_quote(a, sizeof(a), album) && filter_quote(b, sizeof(b), artist) &&
	       snprintf(out, len, "((Album == %s) AND (AlbumArtist == %s)%s%s)", a, b, parent[0] ? " AND " : "", parent) <
		       (int)len;
}

static void clear(struct lib *l)
{
	names_free(&l->nm);
	albums_free(&l->al);
	items_free(&l->it);
	free(l->years);
	free(l->year_n);
	l->years = l->year_n = NULL;
	l->n_years = 0;
	l->n_rows = 0;
}

static void add_row(struct lib *l, enum kind k, int i)
{
	if (l->n_rows == l->cap_rows) {
		l->cap_rows = l->cap_rows ? l->cap_rows * 2 : 64;
		l->rows = realloc(l->rows, l->cap_rows * sizeof(*l->rows));
	}
	l->rows[l->n_rows++] = (struct lrow){ k, i };
}

static int cmp_album_name(const void *a, const void *b)
{
	const struct album *x = a, *y = b;
	int c = nat_cmp(x->name, y->name);
	return c ? c : nat_cmp(x->artist, y->artist);
}

static int cmp_album_year(const void *a, const void *b)
{
	const struct album *x = a, *y = b;

	if (x->year != y->year)
		return !x->year ? 1 : !y->year ? -1 : x->year < y->year ? -1 : 1;
	return cmp_album_name(a, b);
}

static int cmp_album_artist(const void *a, const void *b)
{
	const struct album *x = a, *y = b;
	int c = nat_cmp(x->artist, y->artist);
	return c ? c : cmp_album_name(a, b);
}

static void sort_albums(struct albums *al, int (*cmp)(const void *, const void *))
{
	if (al->n > 1)
		qsort(al->v, al->n, sizeof(*al->v), cmp);
}

static void albums_push(struct albums *al, const char *name, const char *artist, int year)
{
	if (al->n == al->cap) {
		al->cap = al->cap ? al->cap * 2 : 64;
		al->v = realloc(al->v, al->cap * sizeof(*al->v));
	}
	al->v[al->n++] = (struct album){ strdup(name), strdup(artist), year };
}

static void load_years(struct lib *l)
{
	struct albums all;

	if (mpdc_albums((const char *const[]){ NULL }, 1, &all) < 0)
		return;
	l->years = calloc(all.n + 1, sizeof(int));
	l->year_n = calloc(all.n + 1, sizeof(int));
	for (int i = 0; i < all.n; i++) {
		int y = all.v[i].year, j = 0;
		if (!y)
			continue;
		while (j < l->n_years && l->years[j] > y)
			j++;
		if (j < l->n_years && l->years[j] == y) {
			l->year_n[j]++;
			continue;
		}
		memmove(l->years + j + 1, l->years + j, (l->n_years - j) * sizeof(int));
		memmove(l->year_n + j + 1, l->year_n + j, (l->n_years - j) * sizeof(int));
		l->years[j] = y;
		l->year_n[j] = 1;
		l->n_years++;
	}
	albums_free(&all);
	for (int i = 0; i < l->n_years; i++)
		add_row(l, K_YEAR, i);
}

static void load_year_albums(struct lib *l, int year)
{
	struct albums all;

	if (mpdc_albums((const char *const[]){ NULL }, 1, &all) < 0)
		return;
	for (int i = 0; i < all.n; i++)
		if (all.v[i].year == year)
			albums_push(&l->al, all.v[i].name, all.v[i].artist, year);
	albums_free(&all);
	sort_albums(&l->al, cmp_album_artist);
}

static void load_recent(struct lib *l)
{
	struct items it;

	if (mpdc_songs("(added-since \"1\")", "-Added", RECENT_SONGS, &it) < 0)
		return;
	for (int i = 0; i < it.n && l->al.n < RECENT_ALBUMS; i++) {
		const char *artist = item_album_artist(&it.v[i]);
		bool seen = !it.v[i].album[0];
		for (int j = 0; j < l->al.n && !seen; j++)
			seen = !strcmp(l->al.v[j].name, it.v[i].album) && !strcmp(l->al.v[j].artist, artist);
		if (!seen)
			albums_push(&l->al, it.v[i].album, artist, it.v[i].year);
	}
	items_free(&it);
}

static void load_search(struct lib *l, const char *q)
{
	enum mpd_tag_type tag = artist_tag();
	char e[EXPR];

	if (tag_expr(e, sizeof(e), mpd_tag_name(tag), "contains_ci", q) && mpdc_list_tag(tag, sort_tag_of(tag), e, &l->nm) > 0) {
		add_row(l, K_SECTION, 0);
		for (int i = 0; i < l->nm.n && i < SEARCH_ARTISTS; i++)
			add_row(l, K_NAME, i);
	}
	if (tag_expr(e, sizeof(e), "Album", "contains_ci", q) && mpdc_albums((const char *const[]){ e }, 1, &l->al) > 0) {
		sort_albums(&l->al, cmp_album_name);
		add_row(l, K_SECTION, 1);
		for (int i = 0; i < l->al.n && i < SEARCH_ALBUMS; i++)
			add_row(l, K_ALBUM, i);
	}
	if (tag_expr(e, sizeof(e), "Title", "contains_ci", q) && mpdc_songs(e, NULL, SEARCH_SONGS, &l->it) > 0) {
		add_row(l, K_SECTION, 2);
		for (int i = 0; i < l->it.n; i++)
			add_row(l, K_SONG, i);
	}
}

static void load_tracks(struct lib *l, struct frame *f)
{
	char e[EXPR];
	bool discs = false;

	if (!album_expr(e, sizeof(e), f->album, f->artist, f->expr) || mpdc_songs(e, NULL, 0, &l->it) < 0)
		return;
	for (int i = 1; i < l->it.n; i++)
		discs |= l->it.v[i].disc != l->it.v[0].disc;
	for (int i = 0; i < l->it.n; i++) {
		if (discs && (!i || l->it.v[i].disc != l->it.v[i - 1].disc))
			add_row(l, K_DISC, l->it.v[i].disc);
		add_row(l, K_SONG, i);
	}
}

static const char *empty_text(enum view v)
{
	if (!app.mpd.connected)
		return "Music service not running";
	switch (v) {
	case V_SEARCH:
		return "Nothing found";
	case V_GENRES:
		return "No genres tagged";
	case V_COMPOSERS:
		return "No composers tagged";
	case V_YEARS:
		return "No years tagged";
	case V_RECENT:
		return "Nothing added yet";
	case V_HIRES:
		return "No hi-res music";
	case V_TRACKS:
		return "No songs";
	default:
		return "Library is empty.\nSettings → Library → Scan";
	}
}

static void load(struct lib *l)
{
	struct frame *f = top(l);

	clear(l);
	lv_label_set_text(l->title, f->title);
	switch (f->v) {
	case V_ROOT:
		for (int i = 0; i < (int)(sizeof(roots) / sizeof(roots[0])); i++)
			add_row(l, K_ROOT, i);
		break;
	case V_ARTISTS:
	case V_GENRES:
	case V_COMPOSERS:
		mpdc_list_tag(f->tag, sort_tag_of(f->tag), NULL, &l->nm);
		for (int i = 0; i < l->nm.n; i++)
			add_row(l, K_NAME, i);
		break;
	case V_ALBUMS:
		if (f->year)
			load_year_albums(l, f->year);
		else if (mpdc_albums((const char *const[]){ f->expr[0] ? f->expr : NULL }, 1, &l->al) >= 0)
			sort_albums(&l->al, f->by_year ? cmp_album_year : cmp_album_name);
		break;
	case V_YEARS:
		load_years(l);
		break;
	case V_RECENT:
		load_recent(l);
		break;
	case V_HIRES:
		if (mpdc_albums(hires, sizeof(hires) / sizeof(hires[0]), &l->al) >= 0)
			sort_albums(&l->al, cmp_album_name);
		break;
	case V_SEARCH:
		load_search(l, f->title);
		break;
	case V_TRACKS:
		load_tracks(l, f);
		break;
	}
	if (f->v == V_ALBUMS || f->v == V_RECENT || f->v == V_HIRES)
		for (int i = 0; i < l->al.n; i++)
			add_row(l, K_ALBUM, i);
	vlist_set_empty(l->list, empty_text(f->v));
	vlist_reload(l->list);
	vlist_scroll_to(l->list, f->first, false);
}

static int count(void *ctx)
{
	return ((struct lib *)ctx)->n_rows;
}

static void album_sub(const struct album *a, const char *hide, char *out, size_t len)
{
	bool artist = a->artist[0] && (!hide || strcmp(a->artist, hide));

	if (artist && a->year)
		snprintf(out, len, "%s · %d", a->artist, a->year);
	else if (artist)
		snprintf(out, len, "%s", a->artist);
	else if (a->year)
		snprintf(out, len, "%d", a->year);
}

static void song_row(struct lib *l, const struct item *it, struct vrow *r)
{
	char buf[256];
	const char *title = song_label(it, buf, sizeof(buf)), *aa = item_album_artist(it);

	if (top(l)->v == V_TRACKS) {
		if (it->track)
			snprintf(r->text, sizeof(r->text), "%d. %s", it->track, title);
		else
			snprintf(r->text, sizeof(r->text), "%s", title);
		if (it->artist[0] && strcmp(it->artist, aa))
			snprintf(r->sub, sizeof(r->sub), "%s", it->artist);
	} else {
		snprintf(r->text, sizeof(r->text), "%s", title);
		if (it->album[0])
			snprintf(r->sub, sizeof(r->sub), "%s · %s", it->artist, it->album);
		else
			snprintf(r->sub, sizeof(r->sub), "%s", it->artist);
	}
	if (it->dur_ms)
		fmt_time(r->right, sizeof(r->right), it->dur_ms);
	r->active = !strcmp(it->uri, app.mpd.cur.uri);
}

static void row(void *ctx, int idx, struct vrow *r)
{
	struct lib *l = ctx;
	struct frame *f = top(l);
	struct lrow *w = &l->rows[idx];

	switch (w->k) {
	case K_ROOT:
		r->icon = LV_SYMBOL_RIGHT;
		snprintf(r->text, sizeof(r->text), "%s", roots[w->i].label);
		break;
	case K_SECTION:
		r->header = true;
		snprintf(r->text, sizeof(r->text), "%s", sections[w->i]);
		break;
	case K_DISC:
		r->header = true;
		snprintf(r->text, sizeof(r->text), "Disc %d", w->i);
		break;
	case K_NAME:
		snprintf(r->text, sizeof(r->text), "%s", l->nm.v[w->i]);
		break;
	case K_ALBUM:
		snprintf(r->text, sizeof(r->text), "%s", l->al.v[w->i].name);
		album_sub(&l->al.v[w->i], f->by_year ? f->title : NULL, r->sub, sizeof(r->sub));
		break;
	case K_YEAR:
		snprintf(r->text, sizeof(r->text), "%d", l->years[w->i]);
		snprintf(r->right, sizeof(r->right), "%d album%s", l->year_n[w->i], l->year_n[w->i] == 1 ? "" : "s");
		break;
	case K_SONG:
		song_row(l, &l->it.v[w->i], r);
		break;
	}
}

static uint32_t letter(void *ctx, int idx)
{
	struct lib *l = ctx;
	struct frame *f = top(l);
	struct lrow *w = &l->rows[idx];

	if (f->v == V_SEARCH)
		return '#';
	if (w->k == K_NAME)
		return index_letter(l->nm.key[w->i]);
	if (w->k == K_ALBUM && (f->v == V_HIRES || (f->v == V_ALBUMS && !f->by_year && !f->year)))
		return index_letter(l->al.v[w->i].name);
	return '#';
}

static void push(struct lib *l, const struct frame *n)
{
	if (l->depth == DEPTH)
		return;
	top(l)->first = vlist_first(l->list);
	l->st[l->depth++] = *n;
	load(l);
}

static void push_albums(struct lib *l, enum mpd_tag_type tag, const char *name)
{
	struct frame n = { .v = V_ALBUMS };

	if (!tag_expr(n.expr, sizeof(n.expr), mpd_tag_name(tag), "==", name))
		return;
	n.by_year = tag != MPD_TAG_GENRE;
	snprintf(n.title, sizeof(n.title), "%s", name);
	push(l, &n);
}

static void push_tracks(struct lib *l, const char *album, const char *artist, const char *parent)
{
	struct frame n = { .v = V_TRACKS };

	snprintf(n.title, sizeof(n.title), "%s", album);
	snprintf(n.album, sizeof(n.album), "%s", album);
	snprintf(n.artist, sizeof(n.artist), "%s", artist);
	snprintf(n.expr, sizeof(n.expr), "%s", parent);
	push(l, &n);
}

static void play(char *const *uris, int n, int start, bool shuffle, const char *origin)
{
	if (!n) {
		ui_toast("No music here");
		return;
	}
	mpdc_set_order(shuffle ? "random" : set_get("play.order"));
	if (mpdc_play_uris(uris, n, shuffle ? -1 : start, origin) == 0)
		nav_push(&scr_np, NULL);
	else
		ui_toast("Could not start playback");
}

static char **uris_of(const struct items *it)
{
	char **uris = calloc(it->n + 1, sizeof(char *));

	for (int i = 0; i < it->n; i++)
		uris[i] = it->v[i].uri;
	return uris;
}

static void play_song(struct lib *l, int item)
{
	struct frame *f = top(l);
	char **uris = uris_of(&l->it), origin[600];

	snprintf(origin, sizeof(origin), "%s:%s", f->v == V_TRACKS ? "album" : "library", f->title);
	play(uris, l->it.n, item, false, origin);
	free(uris);
}

static void searched(const char *text, void *ctx)
{
	struct lib *l = ctx;
	struct frame n = { .v = V_SEARCH };

	if (!text[0])
		return;
	snprintf(last_query, sizeof(last_query), "%s", text);
	snprintf(n.title, sizeof(n.title), "%s", text);
	push(l, &n);
}

static void click(void *ctx, int idx)
{
	struct lib *l = ctx;
	struct frame *f = top(l), n = { 0 };
	struct lrow *w = &l->rows[idx];
	struct album *a;

	switch (w->k) {
	case K_ROOT:
		n.v = roots[w->i].v;
		if (n.v == V_SEARCH) {
			ui_text_input("Search", last_query, false, searched, l);
			return;
		}
		if (n.v == V_ARTISTS)
			n.tag = artist_tag();
		else if (n.v == V_GENRES)
			n.tag = MPD_TAG_GENRE;
		else if (n.v == V_COMPOSERS)
			n.tag = MPD_TAG_COMPOSER;
		snprintf(n.title, sizeof(n.title), "%s", roots[w->i].label);
		push(l, &n);
		break;
	case K_NAME:
		push_albums(l, f->v == V_SEARCH ? artist_tag() : f->tag, l->nm.v[w->i]);
		break;
	case K_YEAR:
		n.v = V_ALBUMS;
		n.year = l->years[w->i];
		snprintf(n.title, sizeof(n.title), "%d", n.year);
		push(l, &n);
		break;
	case K_ALBUM:
		a = &l->al.v[w->i];
		push_tracks(l, a->name, a->artist, f->v == V_ALBUMS && !f->year ? f->expr : "");
		break;
	case K_SONG:
		play_song(l, w->i);
		break;
	default:
		break;
	}
}

enum { A_PLAY, A_SHUFFLE, A_QUEUE, A_PLAYLIST, A_FOLDER, A_ALBUM };

static struct {
	struct lib *l;
	char expr[EXPR], label[256], album[256], artist[256];
	int item;
	bool current;
	int map[8];
} menu;

static int menu_songs(struct items *tmp, char ***uris)
{
	struct lib *l = menu.l;

	memset(tmp, 0, sizeof(*tmp));
	if (menu.current) {
		*uris = uris_of(&l->it);
		return l->it.n;
	}
	if (menu.item >= 0) {
		*uris = calloc(2, sizeof(char *));
		(*uris)[0] = l->it.v[menu.item].uri;
		return 1;
	}
	if (mpdc_songs(menu.expr, NULL, 0, tmp) < 0) {
		*uris = calloc(1, sizeof(char *));
		return 0;
	}
	*uris = uris_of(tmp);
	return tmp->n;
}

static void menu_picked(int idx, void *ctx)
{
	struct lib *l = menu.l;
	struct items tmp;
	char **uris, msg[600];
	int n, added;

	(void)ctx;
	if (!l)
		return;
	if (menu.map[idx] == A_ALBUM) {
		push_tracks(l, menu.album, menu.artist, "");
		return;
	}
	n = menu_songs(&tmp, &uris);
	switch (menu.map[idx]) {
	case A_PLAY:
	case A_SHUFFLE:
		snprintf(msg, sizeof(msg), "library:%s", menu.label);
		play(uris, n, 0, menu.map[idx] == A_SHUFFLE, msg);
		break;
	case A_QUEUE:
		added = n ? mpdc_append_uris(uris, n) : 0;
		snprintf(msg, sizeof(msg), "Added %d to queue", added < 0 ? 0 : added);
		ui_toast(msg);
		break;
	case A_PLAYLIST:
		playlist_add_uris(uris, n, menu.label);
		break;
	case A_FOLDER:
		if (n)
			folders_open_uri(uris[0]);
		break;
	}
	free(uris);
	items_free(&tmp);
}

static void open_menu(struct lib *l, const char *title, bool song, bool folder, bool album)
{
	const char *items[8];
	int n = 0;

	menu.l = l;
#define ADD(id, text) (menu.map[n] = (id), items[n++] = (text))
	if (!song) {
		ADD(A_PLAY, "Play");
		ADD(A_SHUFFLE, "Shuffle");
	}
	ADD(A_QUEUE, "Add to queue");
	ADD(A_PLAYLIST, "Add to playlist…");
	if (folder)
		ADD(A_FOLDER, "Open folder");
	if (album)
		ADD(A_ALBUM, "Go to album");
#undef ADD
	ui_menu(title, items, n, menu_picked, NULL);
}

static void menu_reset(const char *label)
{
	menu.expr[0] = menu.album[0] = menu.artist[0] = 0;
	menu.item = -1;
	menu.current = false;
	snprintf(menu.label, sizeof(menu.label), "%s", label);
}

static void hold(void *ctx, int idx)
{
	struct lib *l = ctx;
	struct frame *f = top(l);
	struct lrow *w = &l->rows[idx];
	struct album *a;
	struct item *it;
	char label[256];

	switch (w->k) {
	case K_NAME:
		menu_reset(l->nm.v[w->i]);
		if (!tag_expr(menu.expr, sizeof(menu.expr), mpd_tag_name(f->v == V_SEARCH ? artist_tag() : f->tag), "==", l->nm.v[w->i]))
			return;
		open_menu(l, menu.label, false, false, false);
		break;
	case K_YEAR:
		snprintf(label, sizeof(label), "%d", l->years[w->i]);
		menu_reset(label);
		if (year_expr(menu.expr, sizeof(menu.expr), l->years[w->i]))
			open_menu(l, label, false, false, false);
		break;
	case K_ALBUM:
		a = &l->al.v[w->i];
		menu_reset(a->name);
		if (album_expr(menu.expr, sizeof(menu.expr), a->name, a->artist, f->v == V_ALBUMS && !f->year ? f->expr : ""))
			open_menu(l, a->name, false, true, false);
		break;
	case K_SONG:
		it = &l->it.v[w->i];
		menu_reset(it->album[0] ? it->album : "");
		menu.item = w->i;
		snprintf(menu.album, sizeof(menu.album), "%s", it->album);
		snprintf(menu.artist, sizeof(menu.artist), "%s", item_album_artist(it));
		open_menu(l, song_label(it, label, sizeof(label)), true, true, f->v == V_SEARCH && it->album[0]);
		break;
	default:
		break;
	}
}

static void title_click(lv_event_t *e)
{
	struct lib *l = lv_event_get_user_data(e);
	struct frame *f = top(l);

	menu_reset(f->title);
	if (f->v == V_TRACKS) {
		menu.current = true;
		open_menu(l, f->title, false, true, false);
		return;
	}
	if (f->v != V_ALBUMS || (!f->expr[0] && !f->year))
		return;
	if (f->year && !year_expr(menu.expr, sizeof(menu.expr), f->year))
		return;
	if (!f->year)
		snprintf(menu.expr, sizeof(menu.expr), "%s", f->expr);
	open_menu(l, f->title, false, false, false);
}

static const struct vlist_ops ops = { .count = count, .row = row, .click = click, .hold = hold, .letter = letter };

static void on_event(uint32_t evs, void *ctx)
{
	struct lib *l = ctx;

	if (evs & EV_DB) {
		top(l)->first = vlist_first(l->list);
		load(l);
	} else {
		vlist_refresh(l->list);
	}
}

static void on_delete(lv_event_t *e)
{
	struct lib *l = lv_event_get_user_data(e);

	if (menu.l == l)
		menu.l = NULL;
	clear(l);
	free(l->rows);
	free(l);
}

static lv_obj_t *create(void *arg)
{
	struct lib *l = calloc(1, sizeof(*l));

	(void)arg;
	l->scr = ui_screen("Library", &l->title);
	lv_obj_set_user_data(l->scr, l);
	lv_obj_set_clickable(l->title, true);
	lv_obj_set_ext_click_area(l->title, 12);
	lv_obj_add_event_cb(l->title, title_click, LV_EVENT_CLICKED, l);
	l->list = vlist_create(l->scr, &ops, l);
	lv_obj_set_size(l->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(l->list, 0, CONTENT_Y);
	l->st[0].v = V_ROOT;
	snprintf(l->st[0].title, sizeof(l->st[0].title), "Library");
	l->depth = 1;
	load(l);
	app_listen(l->scr, EV_DB | EV_SONG, on_event, l);
	lv_obj_add_event_cb(l->scr, on_delete, LV_EVENT_DELETE, l);
	return l->scr;
}

static bool back(lv_obj_t *scr)
{
	struct lib *l = lv_obj_get_user_data(scr);

	if (l->depth <= 1)
		return false;
	l->depth--;
	load(l);
	return true;
}

const struct screen_ops scr_library = { .name = "library", .create = create, .back = back };
