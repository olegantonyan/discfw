#include "fsdir.h"
#include "mpdc.h"
#include "ui.h"

#include <disc/settings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SKIP 20

static int boundary = -1;
static unsigned boundary_qv;
static char next_dir[2048];

static bool active(void)
{
	const char *order = mpdc_order();

	return set_get_bool("play.through_folders") && !strncmp(mpdc_queue_origin(), "folder:", 7) &&
	       (!strcmp(order, "seq") || !strcmp(order, "noloop"));
}

static bool trim(void)
{
	char origin[2100];
	int b = boundary;

	if (b < 0 || app.mpd.song_pos < b)
		return false;
	boundary = -1;
	if (mpdc_queue_delete_range(0, b)) {
		snprintf(origin, sizeof(origin), "folder:%s", next_dir);
		mpdc_set_origin(origin);
	}
	return true;
}

static int append_folder(const char *dir)
{
	char **uris;
	int n, missing, added;

	n = fsdir_collect(dir, set_get_bool("play.recursive"), &uris, &missing);
	if (missing)
		mpdc_update(dir);
	added = n ? mpdc_append_uris(uris, n) : 0;
	fsdir_free_strv(uris, n);
	return added;
}

static void extend(void)
{
	char *path, *slash, cur[2048], dir[2048], origin[2100];
	int len = app.mpd.queue_len, added = 0;
	bool wrap = !strcmp(mpdc_order(), "seq");

	if (boundary >= 0 || len <= 0 || app.mpd.song_pos != len - 1 || !app.mpd.cur.uri[0])
		return;
	path = mpdc_path_for_uri(app.mpd.cur.uri);
	if (!path || !(slash = strrchr(path, '/'))) {
		free(path);
		return;
	}
	*slash = 0;
	snprintf(cur, sizeof(cur), "%s", path);
	free(path);
	for (int i = 0; i < MAX_SKIP && added <= 0; i++) {
		if (!fsdir_next_music(cur, wrap, dir, sizeof(dir)))
			return;
		added = append_folder(dir);
		snprintf(cur, sizeof(cur), "%s", dir);
	}
	if (added <= 0)
		return;
	boundary = len;
	snprintf(next_dir, sizeof(next_dir), "%s", dir);
	snprintf(origin, sizeof(origin), "%s", mpdc_queue_origin());
	boundary_qv = mpdc_set_origin(origin);
}

static void on_event(uint32_t evs, void *ctx)
{
	(void)evs;
	(void)ctx;
	if (!active()) {
		boundary = -1;
		return;
	}
	if (boundary >= 0 && app.mpd.queue_version != boundary_qv)
		boundary = -1;
	if (!trim())
		extend();
}

void through_init(void)
{
	app_listen(lv_layer_top(), EV_SONG | EV_QUEUE, on_event, NULL);
}
