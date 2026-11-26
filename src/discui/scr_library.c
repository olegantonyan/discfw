#include "mpdc.h"
#include "ui.h"

#include <disc/settings.h>
#include <disc/text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum level { L_ROOT, L_ARTISTS, L_GENRES, L_ALBUMS, L_TRACKS };

struct frame {
	enum level lv;
	enum mpd_tag_type ftag;
	char filter[256];
	char album[256];
	int first;
};

struct lib {
	lv_obj_t *scr, *title, *list;
	struct frame st[5];
	int depth;
	char **names;
	int n;
	struct items it;
};

static const char *const root_items[] = { "Artists", "Albums", "Genres" };

static struct frame *top(struct lib *l)
{
	return &l->st[l->depth - 1];
}

static enum mpd_tag_type artist_tag(void)
{
	return strcmp(set_get("lib.artist_tag"), "artist") ? MPD_TAG_ALBUM_ARTIST : MPD_TAG_ARTIST;
}

static int cmp_name(const void *a, const void *b)
{
	return nat_cmp(*(char *const *)a, *(char *const *)b);
}

static void clear(struct lib *l)
{
	for (int i = 0; i < l->n; i++)
		free(l->names[i]);
	free(l->names);
	l->names = NULL;
	l->n = 0;
	items_free(&l->it);
}

static void load(struct lib *l)
{
	struct frame *f = top(l);

	clear(l);
	switch (f->lv) {
	case L_ROOT:
		lv_label_set_text(l->title, "Library");
		break;
	case L_ARTISTS:
		lv_label_set_text(l->title, "Artists");
		l->n = mpdc_list_tag(artist_tag(), 0, NULL, &l->names);
		break;
	case L_GENRES:
		lv_label_set_text(l->title, "Genres");
		l->n = mpdc_list_tag(MPD_TAG_GENRE, 0, NULL, &l->names);
		break;
	case L_ALBUMS:
		lv_label_set_text(l->title, f->filter[0] ? f->filter : "Albums");
		l->n = mpdc_list_tag(MPD_TAG_ALBUM, f->ftag, f->filter[0] ? f->filter : NULL, &l->names);
		break;
	case L_TRACKS:
		lv_label_set_text(l->title, f->album);
		mpdc_find(MPD_TAG_ALBUM, f->album, f->ftag, f->filter[0] ? f->filter : NULL, &l->it);
		break;
	}
	if (l->n < 0)
		l->n = 0;
	if (f->lv != L_TRACKS && f->lv != L_ROOT && l->n > 1)
		qsort(l->names, l->n, sizeof(char *), cmp_name);
	vlist_set_empty(l->list, app.mpd.connected ? "Library is empty.\nSettings → Library → Scan" : "Music service not running");
	vlist_reload(l->list);
	vlist_scroll_to(l->list, f->first, false);
}

static int count(void *ctx)
{
	struct lib *l = ctx;

	switch (top(l)->lv) {
	case L_ROOT:
		return 3;
	case L_TRACKS:
		return l->it.n;
	default:
		return l->n;
	}
}

static void row(void *ctx, int idx, struct vrow *r)
{
	struct lib *l = ctx;
	char buf[256];

	switch (top(l)->lv) {
	case L_ROOT:
		r->icon = LV_SYMBOL_RIGHT;
		snprintf(r->text, sizeof(r->text), "%s", root_items[idx]);
		break;
	case L_TRACKS:
		snprintf(r->text, sizeof(r->text), "%s", song_label(&l->it.v[idx], buf, sizeof(buf)));
		if (l->it.v[idx].dur_ms)
			fmt_time(r->right, sizeof(r->right), l->it.v[idx].dur_ms);
		r->active = !strcmp(l->it.v[idx].uri, app.mpd.cur.uri);
		break;
	default:
		snprintf(r->text, sizeof(r->text), "%s", l->names[idx]);
	}
}

static uint32_t letter(void *ctx, int idx)
{
	struct lib *l = ctx;
	return top(l)->lv == L_ROOT || top(l)->lv == L_TRACKS ? 0 : index_letter(l->names[idx]);
}

static void push(struct lib *l, struct frame f)
{
	if (l->depth == 5)
		return;
	top(l)->first = vlist_first(l->list);
	l->st[l->depth++] = f;
	load(l);
}

static void play_tracks(struct lib *l, int start)
{
	char **uris = calloc(l->it.n + 1, sizeof(char *)), origin[600];

	for (int i = 0; i < l->it.n; i++)
		uris[i] = l->it.v[i].uri;
	snprintf(origin, sizeof(origin), "album:%s", top(l)->album);
	mpdc_set_order(set_get("play.order"));
	if (mpdc_play_uris(uris, l->it.n, start, origin) == 0)
		nav_push(&scr_np, NULL);
	free(uris);
}

static void click(void *ctx, int idx)
{
	struct lib *l = ctx;
	struct frame *f = top(l), n = { 0 };

	switch (f->lv) {
	case L_ROOT:
		n.lv = idx == 0 ? L_ARTISTS : idx == 1 ? L_ALBUMS : L_GENRES;
		break;
	case L_ARTISTS:
		n.lv = L_ALBUMS;
		n.ftag = artist_tag();
		snprintf(n.filter, sizeof(n.filter), "%s", l->names[idx]);
		break;
	case L_GENRES:
		n.lv = L_ALBUMS;
		n.ftag = MPD_TAG_GENRE;
		snprintf(n.filter, sizeof(n.filter), "%s", l->names[idx]);
		break;
	case L_ALBUMS:
		n = *f;
		n.lv = L_TRACKS;
		n.first = 0;
		snprintf(n.album, sizeof(n.album), "%s", l->names[idx]);
		break;
	case L_TRACKS:
		play_tracks(l, idx);
		return;
	}
	push(l, n);
}

static const struct vlist_ops ops = { .count = count, .row = row, .click = click, .letter = letter };

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

	clear(l);
	free(l);
}

static lv_obj_t *create(void *arg)
{
	struct lib *l = calloc(1, sizeof(*l));

	(void)arg;
	l->scr = ui_screen("Library", &l->title);
	lv_obj_set_user_data(l->scr, l);
	l->list = vlist_create(l->scr, &ops, l);
	lv_obj_set_size(l->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(l->list, 0, CONTENT_Y);
	l->st[0].lv = L_ROOT;
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
