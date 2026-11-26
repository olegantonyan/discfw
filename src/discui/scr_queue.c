#include "groups.h"
#include "mpdc.h"
#include "ui.h"

#include <disc/settings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct queue {
	lv_obj_t *scr, *title, *list;
	struct items it;
	struct groups g;
	int sel;
	char **labels, **presets;
	int n_presets;
};

static int count(void *ctx)
{
	return ((struct queue *)ctx)->g.n;
}

static void row(void *ctx, int rowi, struct vrow *r)
{
	struct queue *q = ctx;
	int idx = q->g.map[rowi];
	char buf[256];

	if (idx < 0) {
		snprintf(r->text, sizeof(r->text), "%s", q->g.head[-idx - 1]);
		snprintf(r->sub, sizeof(r->sub), "%s", q->g.head_sub[-idx - 1]);
		r->header = true;
		return;
	}
	const struct item *it = &q->it.v[idx];

	snprintf(r->text, sizeof(r->text), "%s", song_label(it, buf, sizeof(buf)));
	snprintf(r->sub, sizeof(r->sub), "%s", it->artist);
	if (it->dur_ms)
		fmt_time(r->right, sizeof(r->right), it->dur_ms);
	r->active = idx == app.mpd.song_pos;
	r->icon = r->active ? (app.mpd.state == PS_PLAY ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE) : NULL;
}

static void set_title(struct queue *q)
{
	const char *o = mpdc_queue_origin(), *slash;
	char buf[300];

	if (!strncmp(o, "playlist:", 9))
		snprintf(buf, sizeof(buf), LV_SYMBOL_LIST " %s", o + 9);
	else if (!strncmp(o, "folder:", 7))
		snprintf(buf, sizeof(buf), LV_SYMBOL_DIRECTORY " %s", (slash = strrchr(o, '/')) ? slash + 1 : o + 7);
	else
		snprintf(buf, sizeof(buf), "Queue");
	lv_label_set_text(q->title, buf);
}

static void reload(struct queue *q, bool to_current)
{
	items_free(&q->it);
	if (mpdc_queue(&q->it) < 0)
		q->it.n = 0;
	set_title(q);
	groups_build(&q->g, &q->it);
	vlist_reload(q->list);
	if (to_current && app.mpd.song_pos >= 0)
		vlist_scroll_to(q->list, groups_row_of(&q->g, app.mpd.song_pos), true);
}

static void on_event(uint32_t evs, void *ctx)
{
	struct queue *q = ctx;

	if (evs & (EV_QUEUE | EV_SETTINGS))
		reload(q, false);
	else
		vlist_refresh(q->list);
}

static void click(void *ctx, int row)
{
	int idx = groups_track_at(&((struct queue *)ctx)->g, row);

	if (idx >= 0)
		mpdc_play_pos(idx);
}

enum { Q_PLAY, Q_NEXT, Q_UP, Q_DOWN, Q_REMOVE, Q_FOLDER, Q_INFO };

static void item_picked(int choice, void *ctx)
{
	struct queue *q = lv_obj_get_user_data(lv_screen_active());
	int idx = (int)(intptr_t)ctx;
	char *path, *slash;

	if (!q || idx >= q->it.n)
		return;
	switch (choice) {
	case Q_PLAY:
		mpdc_play_pos(idx);
		break;
	case Q_NEXT:
		if (app.mpd.song_pos >= 0 && idx != app.mpd.song_pos)
			mpdc_queue_move(idx, idx > app.mpd.song_pos ? app.mpd.song_pos + 1 : app.mpd.song_pos);
		break;
	case Q_UP:
		if (idx > 0)
			mpdc_queue_move(idx, idx - 1);
		break;
	case Q_DOWN:
		if (idx + 1 < q->it.n)
			mpdc_queue_move(idx, idx + 1);
		break;
	case Q_REMOVE:
		mpdc_queue_delete(idx);
		break;
	case Q_FOLDER:
		path = mpdc_path_for_uri(q->it.v[idx].uri);
		if (path && (slash = strrchr(path, '/'))) {
			*slash = 0;
			folders_open_at(path, slash + 1);
		}
		free(path);
		break;
	case Q_INFO:
		ui_info("File", q->it.v[idx].uri);
		break;
	}
}

static void hold(void *ctx, int row)
{
	static const char *const items[] = { "Play", "Play next", "Move up", "Move down", "Remove", "Go to folder", "File" };
	char buf[256];
	int idx = ((struct queue *)ctx)->g.map[row];

	if (idx < 0)
		return;

	ui_menu(song_label(&((struct queue *)ctx)->it.v[idx], buf, sizeof(buf)), items, 7, item_picked, (void *)(intptr_t)idx);
}

static const struct vlist_ops ops = { .count = count, .row = row, .click = click, .hold = hold };

static void save_named(const char *text, void *ctx)
{
	(void)ctx;
	if (!text[0] || strchr(text, '/')) {
		ui_toast("Invalid name");
		return;
	}
	if (mpdc_playlist_save_queue(text))
		ui_toast("Saved");
}

static void do_clear(void *ctx)
{
	(void)ctx;
	mpdc_queue_clear();
}

static void sort_picked(int idx, void *ctx)
{
	struct queue *q = ctx;

	if (idx < q->n_presets && mpdc_sort_queue(q->presets[idx]))
		ui_toast("Sorted");
	reload(q, true);
}

static const char *const groups_values[] = { "none", "album", "folder", "both" };
static const char *const groups_labels[] = { "No grouping", "Album", "Folder", "Album and folder" };

static void group_picked(int idx, void *ctx)
{
	(void)ctx;
	sysc_set("pl.group", groups_values[idx]);
}

static void title_picked(int idx, void *ctx)
{
	struct queue *q = ctx;

	switch (idx) {
	case 0:
		if (app.mpd.song_pos >= 0)
			vlist_scroll_to(q->list, groups_row_of(&q->g, app.mpd.song_pos), true);
		break;
	case 4:
		sort_presets_free(q->labels, q->presets, q->n_presets);
		q->n_presets = sort_presets(&q->labels, &q->presets);
		ui_menu("Sort by", (const char *const *)q->labels, q->n_presets, sort_picked, q);
		break;
	case 5:
		ui_pick("Group by", groups_labels, 4, set_get_enum("pl.group"), group_picked, NULL);
		break;
	case 1:
		ui_text_input("Save as playlist", "", false, save_named, NULL);
		break;
	case 2:
		if (mpdc() && !mpd_run_shuffle(mpdc()))
			mpdc_check("shuffle");
		break;
	case 3:
		ui_confirm("Clear the queue?", "Clear", do_clear, NULL);
		break;
	}
}

static void title_click(lv_event_t *e)
{
	static const char *const items[] = { "Go to current", "Save as playlist…", "Shuffle queue", "Clear queue", "Sort…",
					     "Group by…" };
	ui_menu("Queue", items, 6, title_picked, lv_event_get_user_data(e));
}

static void on_delete(lv_event_t *e)
{
	struct queue *q = lv_event_get_user_data(e);

	sort_presets_free(q->labels, q->presets, q->n_presets);
	groups_free(&q->g);
	items_free(&q->it);
	free(q);
}

static lv_obj_t *create(void *arg)
{
	struct queue *q = calloc(1, sizeof(*q));

	(void)arg;
	q->scr = ui_screen("Queue", &q->title);
	lv_obj_set_user_data(q->scr, q);
	lv_obj_set_clickable(q->title, true);
	lv_obj_set_ext_click_area(q->title, 12);
	lv_obj_add_event_cb(q->title, title_click, LV_EVENT_CLICKED, q);
	q->list = vlist_create(q->scr, &ops, q);
	lv_obj_set_size(q->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(q->list, 0, CONTENT_Y);
	vlist_set_empty(q->list, "Queue is empty");
	reload(q, true);
	app_listen(q->scr, EV_QUEUE | EV_SONG | EV_STATUS | EV_SETTINGS, on_event, q);
	lv_obj_add_event_cb(q->scr, on_delete, LV_EVENT_DELETE, q);
	return q->scr;
}

const struct screen_ops scr_queue = { .name = "queue", .create = create };
