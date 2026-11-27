#include "mpdc.h"
#include "ui.h"

#include <disc/conn.h>
#include <disc/paths.h>
#include <disc/text.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define TIMEOUT_MS 3000
#define BATCH 500
#define IDLE_MASK (MPD_IDLE_PLAYER | MPD_IDLE_MIXER | MPD_IDLE_OPTIONS | MPD_IDLE_QUEUE | MPD_IDLE_STORED_PLAYLIST | MPD_IDLE_DATABASE | MPD_IDLE_UPDATE)

static struct mpd_connection *cmd, *idle;
static lv_timer_t *retry;
static unsigned expected_queue_version;
static char origin[1100];

static char *xstrdup(const char *s)
{
	return strdup(s ? s : "");
}

void items_free(struct items *it)
{
	for (int i = 0; i < it->n; i++) {
		free(it->v[i].uri);
		free(it->v[i].title);
		free(it->v[i].artist);
		free(it->v[i].album);
		free(it->v[i].albumartist);
	}
	free(it->v);
	it->v = NULL;
	it->n = it->cap = 0;
}

static void items_add(struct items *it, const struct mpd_song *s)
{
	const char *t;

	if (it->n == it->cap) {
		it->cap = it->cap ? it->cap * 2 : 64;
		it->v = realloc(it->v, it->cap * sizeof(*it->v));
	}
	struct item *x = &it->v[it->n++];
	x->uri = xstrdup(mpd_song_get_uri(s));
	t = mpd_song_get_tag(s, MPD_TAG_TITLE, 0);
	x->title = t ? xstrdup(t) : NULL;
	x->artist = xstrdup(mpd_song_get_tag(s, MPD_TAG_ARTIST, 0));
	x->album = xstrdup(mpd_song_get_tag(s, MPD_TAG_ALBUM, 0));
	x->albumartist = xstrdup(mpd_song_get_tag(s, MPD_TAG_ALBUM_ARTIST, 0));
	t = mpd_song_get_tag(s, MPD_TAG_DATE, 0);
	x->year = t ? year_of(t) : 0;
	x->dur_ms = mpd_song_get_duration_ms(s);
	x->id = mpd_song_get_id(s);
	t = mpd_song_get_tag(s, MPD_TAG_DISC, 0);
	x->disc = t ? atoi(t) : 0;
	t = mpd_song_get_tag(s, MPD_TAG_TRACK, 0);
	x->track = t ? atoi(t) : 0;
}

const char *song_label(const struct item *it, char *buf, size_t len)
{
	const char *slash;

	if (it->title && it->title[0])
		return it->title;
	slash = strrchr(it->uri, '/');
	snprintf(buf, len, "%s", slash ? slash + 1 : it->uri);
	return buf;
}

static void copy_tag(char *dst, size_t len, const struct mpd_song *s, enum mpd_tag_type t)
{
	const char *v = mpd_song_get_tag(s, t, 0);
	snprintf(dst, len, "%s", v ? v : "");
}

void song_fill(struct song *dst, const struct mpd_song *s)
{
	snprintf(dst->uri, sizeof(dst->uri), "%s", mpd_song_get_uri(s));
	copy_tag(dst->title, sizeof(dst->title), s, MPD_TAG_TITLE);
	copy_tag(dst->artist, sizeof(dst->artist), s, MPD_TAG_ARTIST);
	copy_tag(dst->album, sizeof(dst->album), s, MPD_TAG_ALBUM);
	copy_tag(dst->albumartist, sizeof(dst->albumartist), s, MPD_TAG_ALBUM_ARTIST);
	copy_tag(dst->date, sizeof(dst->date), s, MPD_TAG_DATE);
	copy_tag(dst->genre, sizeof(dst->genre), s, MPD_TAG_GENRE);
	copy_tag(dst->track, sizeof(dst->track), s, MPD_TAG_TRACK);
	copy_tag(dst->disc, sizeof(dst->disc), s, MPD_TAG_DISC);
	dst->duration_ms = mpd_song_get_duration_ms(s);
	dst->pos = mpd_song_get_pos(s);
	dst->id = mpd_song_get_id(s);
	if (!dst->title[0]) {
		const char *slash = strrchr(dst->uri, '/');
		snprintf(dst->title, sizeof(dst->title), "%.*s", (int)sizeof(dst->title) - 1, slash ? slash + 1 : dst->uri);
	}
}

#define IDLE_PING_MS 20000

static int64_t cmd_used;

struct mpd_connection *mpdc(void)
{
	int64_t now = mono_ms();

	if (cmd && mpd_connection_get_error(cmd) == MPD_ERROR_SUCCESS && now - cmd_used > IDLE_PING_MS && !(mpd_send_command(cmd, "ping", NULL) && mpd_response_finish(cmd))) {
		mpd_connection_free(cmd);
		cmd = NULL;
	}
	cmd_used = now;
	if (cmd && mpd_connection_get_error(cmd) == MPD_ERROR_SUCCESS)
		return cmd;
	if (cmd && mpd_connection_clear_error(cmd))
		return cmd;
	if (cmd)
		mpd_connection_free(cmd);
	cmd = mpd_connection_new(MPD_SOCK, 0, TIMEOUT_MS);
	if (cmd && mpd_connection_get_error(cmd) != MPD_ERROR_SUCCESS) {
		mpd_connection_free(cmd);
		cmd = NULL;
	}
	if (cmd)
		mpd_connection_set_keepalive(cmd, true);
	return cmd;
}

bool mpdc_check(const char *what)
{
	if (!cmd)
		return false;
	if (mpd_connection_get_error(cmd) == MPD_ERROR_SUCCESS)
		return true;
	fprintf(stderr, "mpd %s: %s\n", what, mpd_connection_get_error_message(cmd));
	snprintf(app.mpd.error, sizeof(app.mpd.error), "%s", mpd_connection_get_error_message(cmd));
	if (!mpd_connection_clear_error(cmd)) {
		mpd_connection_free(cmd);
		cmd = NULL;
	}
	return false;
}

static enum ps map_state(enum mpd_state s)
{
	return s == MPD_STATE_PLAY ? PS_PLAY : s == MPD_STATE_PAUSE ? PS_PAUSE : s == MPD_STATE_STOP ? PS_STOP : PS_UNKNOWN;
}

void mpdc_refresh(void)
{
	struct mpd_connection *c = mpdc();
	struct mpd_status *s;
	struct mpd_song *song;
	const struct mpd_audio_format *af;
	uint32_t ev = EV_STATUS;
	int prev_id = app.mpd.song_id;
	bool prev_updating = app.mpd.updating;
	unsigned prev_qv = app.mpd.queue_version;
	bool prev_opts[4] = { app.mpd.repeat, app.mpd.random, app.mpd.single, app.mpd.consume };

	if (!c)
		return;
	s = mpd_run_status(c);
	if (!s) {
		mpdc_check("status");
		return;
	}
	app.mpd.connected = true;
	app.mpd.state = map_state(mpd_status_get_state(s));
	app.mpd.elapsed_ms = mpd_status_get_elapsed_ms(s);
	app.mpd.elapsed_at = mono_ms();
	app.mpd.total_ms = mpd_status_get_total_time(s) * 1000;
	app.mpd.song_pos = mpd_status_get_song_pos(s);
	app.mpd.song_id = mpd_status_get_song_id(s);
	app.mpd.queue_len = mpd_status_get_queue_length(s);
	app.mpd.queue_version = mpd_status_get_queue_version(s);
	app.mpd.repeat = mpd_status_get_repeat(s);
	app.mpd.random = mpd_status_get_random(s);
	app.mpd.single = mpd_status_get_single_state(s) != MPD_SINGLE_OFF;
	app.mpd.consume = mpd_status_get_consume(s);
	app.mpd.kbps = mpd_status_get_kbit_rate(s);
	app.mpd.updating = mpd_status_get_update_id(s) != 0;
	af = mpd_status_get_audio_format(s);
	app.mpd.rate = af ? af->sample_rate : 0;
	app.mpd.bits = af ? af->bits : 0;
	app.mpd.channels = af ? af->channels : 0;
	snprintf(app.mpd.error, sizeof(app.mpd.error), "%s", mpd_status_get_error(s) ? mpd_status_get_error(s) : "");
	mpd_status_free(s);
	if (app.mpd.song_id != prev_id || app.mpd.queue_version != prev_qv) {
		song = app.mpd.song_id >= 0 ? mpd_run_current_song(c) : NULL;
		char prev_uri[sizeof(app.mpd.cur.uri)];
		snprintf(prev_uri, sizeof(prev_uri), "%s", app.mpd.cur.uri);
		if (song) {
			song_fill(&app.mpd.cur, song);
			mpd_song_free(song);
		} else {
			mpdc_check("currentsong");
			memset(&app.mpd.cur, 0, sizeof(app.mpd.cur));
			app.mpd.cur.pos = app.mpd.cur.id = -1;
		}
		if (app.mpd.song_id != prev_id || strcmp(prev_uri, app.mpd.cur.uri)) {
			ev |= EV_SONG;
			cover_request(app.mpd.cur.uri);
		}
	}
	if (app.mpd.cur.duration_ms && !app.mpd.total_ms)
		app.mpd.total_ms = app.mpd.cur.duration_ms;
	if (app.mpd.queue_version != prev_qv) {
		ev |= EV_QUEUE;
		if (expected_queue_version && app.mpd.queue_version != expected_queue_version && origin[0]) {
			origin[0] = 0;
			state_set("queue.origin", "");
		}
		expected_queue_version = 0;
	}
	if (prev_updating != app.mpd.updating)
		ev |= EV_UPDATE;
	if (prev_opts[0] != app.mpd.repeat || prev_opts[1] != app.mpd.random || prev_opts[2] != app.mpd.single ||
	    prev_opts[3] != app.mpd.consume)
		ev |= EV_OPTIONS;
	app_notify(ev);
}

static void idle_lost(void);

static void on_idle(int fd, short revents, void *ctx)
{
	enum mpd_idle ev;
	uint32_t out = 0;

	(void)fd;
	(void)ctx;
	if (revents & (POLLERR | POLLHUP)) {
		idle_lost();
		return;
	}
	ev = mpd_recv_idle(idle, false);
	if (!ev && mpd_connection_get_error(idle) != MPD_ERROR_SUCCESS) {
		idle_lost();
		return;
	}
	if (ev & MPD_IDLE_STORED_PLAYLIST)
		out |= EV_PLAYLISTS;
	if (ev & MPD_IDLE_DATABASE)
		out |= EV_DB;
	if (out)
		app_notify(out);
	if (ev & (MPD_IDLE_PLAYER | MPD_IDLE_MIXER | MPD_IDLE_OPTIONS | MPD_IDLE_QUEUE | MPD_IDLE_UPDATE))
		mpdc_refresh();
	if (!mpd_send_idle_mask(idle, IDLE_MASK))
		idle_lost();
}

static void idle_lost(void)
{
	if (idle) {
		loop_unwatch(mpd_connection_get_fd(idle));
		mpd_connection_free(idle);
	}
	idle = NULL;
	if (cmd)
		mpd_connection_free(cmd);
	cmd = NULL;
	app.mpd.connected = false;
	app.mpd.state = PS_UNKNOWN;
	app_notify(EV_STATUS);
	lv_timer_resume(retry);
}

static void try_connect(lv_timer_t *t)
{
	(void)t;
	idle = mpd_connection_new(MPD_SOCK, 0, TIMEOUT_MS);
	if (!idle || mpd_connection_get_error(idle) != MPD_ERROR_SUCCESS || !mpdc() || !mpd_send_idle_mask(idle, IDLE_MASK)) {
		if (idle)
			mpd_connection_free(idle);
		idle = NULL;
		return;
	}
	loop_watch(mpd_connection_get_fd(idle), POLLIN, on_idle, NULL);
	lv_timer_pause(retry);
	app.mpd.song_id = -2;
	mpdc_refresh();
	app_notify(EV_QUEUE | EV_PLAYLISTS | EV_DB);
}

void mpdc_init(void)
{
	const char *link = disc_run_path("sdfile");
	const char *o = state_get("queue.origin");

	if (access(link, F_OK) && symlink(disc_music_root(), link))
		perror(link);
	snprintf(origin, sizeof(origin), "%s", o ? o : "");
	app.mpd.song_id = -2;
	retry = lv_timer_create(try_connect, 1000, NULL);
	try_connect(NULL);
}

void mpdc_toggle(void)
{
	struct mpd_connection *c = mpdc();

	if (!c)
		return;
	if (app.mpd.state == PS_PLAY)
		mpd_run_pause(c, true);
	else if (app.mpd.state == PS_PAUSE)
		mpd_run_pause(c, false);
	else
		mpd_run_play(c);
	mpdc_check("toggle");
}

void mpdc_next(void)
{
	if (mpdc())
		mpd_run_next(cmd);
	mpdc_check("next");
}

void mpdc_prev(void)
{
	if (mpdc())
		mpd_run_previous(cmd);
	mpdc_check("previous");
}

void mpdc_play_pos(int pos)
{
	if (mpdc())
		mpd_run_play_pos(cmd, pos);
	mpdc_check("play");
}

void mpdc_seek_ms(unsigned ms)
{
	if (mpdc())
		mpd_run_seek_current(cmd, ms / 1000.0f, false);
	mpdc_check("seek");
	app.mpd.elapsed_ms = ms;
	app.mpd.elapsed_at = mono_ms();
}

const char *mpdc_order(void)
{
	if (app.mpd.single)
		return app.mpd.repeat ? "single" : "once";
	if (app.mpd.random)
		return "random";
	return app.mpd.repeat ? "seq" : "noloop";
}

void mpdc_set_order(const char *o)
{
	struct mpd_connection *c = mpdc();

	if (!c)
		return;
	mpd_command_list_begin(c, false);
	mpd_send_repeat(c, !strcmp(o, "seq") || !strcmp(o, "random") || !strcmp(o, "single"));
	mpd_send_random(c, !strcmp(o, "random"));
	mpd_send_single(c, !strcmp(o, "single") || !strcmp(o, "once"));
	mpd_command_list_end(c);
	mpd_response_finish(c);
	mpdc_check("order");
}

char *mpdc_uri_for_path(const char *abs)
{
	const char *root = disc_music_root();
	size_t n = strlen(root);

	if (!strncmp(abs, root, n) && (abs[n] == '/' || !abs[n]))
		return strdup(abs[n] ? abs + n + 1 : "");
	return NULL;
}

char *mpdc_path_for_uri(const char *uri)
{
	const char *link = disc_run_path("sdfile");
	char buf[2048];

	if (!strncmp(uri, "file://", 7) || uri[0] == '/') {
		if (uri[0] != '/')
			uri += 7;
		size_t n = strlen(link);
		if (!strncmp(uri, link, n) && uri[n] == '/')
			snprintf(buf, sizeof(buf), "%s%s", disc_music_root(), uri + n);
		else
			snprintf(buf, sizeof(buf), "%s", uri);
		return strdup(buf);
	}
	if (strstr(uri, "://"))
		return NULL;
	snprintf(buf, sizeof(buf), "%s/%s", disc_music_root(), uri);
	return strdup(buf);
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int db_songs_in(const char *reldir, char ***out)
{
	struct mpd_connection *c = mpdc();
	struct mpd_entity *e;
	char **v = NULL;
	int n = 0, cap = 0;

	*out = NULL;
	if (!c || !mpd_send_list_meta(c, reldir))
		return -1;
	while ((e = mpd_recv_entity(c))) {
		if (mpd_entity_get_type(e) == MPD_ENTITY_TYPE_SONG) {
			if (n == cap) {
				cap = cap ? cap * 2 : 64;
				v = realloc(v, cap * sizeof(char *));
			}
			v[n++] = strdup(mpd_song_get_uri(mpd_entity_get_song(e)));
		}
		mpd_entity_free(e);
	}
	if (!mpdc_check("lsinfo")) {
		for (int i = 0; i < n; i++)
			free(v[i]);
		free(v);
		return -1;
	}
	if (n > 1)
		qsort(v, n, sizeof(char *), cmp_str);
	*out = v;
	return n;
}

int mpdc_listall(const char *rel, char ***paths, bool **is_dir)
{
	struct mpd_connection *c = mpdc();
	struct mpd_pair *p;
	int n = 0, cap = 0;

	*paths = NULL;
	*is_dir = NULL;
	if (!c || !mpd_send_list_all(c, rel))
		return mpdc_check("listall"), -1;
	while ((p = mpd_recv_pair(c))) {
		bool d = !strcmp(p->name, "directory");
		if (d || !strcmp(p->name, "file")) {
			if (n == cap) {
				cap = cap ? cap * 2 : 256;
				*paths = realloc(*paths, cap * sizeof(char *));
				*is_dir = realloc(*is_dir, cap * sizeof(bool));
			}
			(*paths)[n] = strdup(p->value);
			(*is_dir)[n++] = d;
		}
		mpd_return_pair(c, p);
	}
	if (!mpdc_check("listall")) {
		for (int i = 0; i < n; i++)
			free((*paths)[i]);
		free(*paths);
		free(*is_dir);
		*paths = NULL;
		*is_dir = NULL;
		return -1;
	}
	return n;
}

bool mpdc_folder_scanned(const char *abs_dir)
{
	char *rel = mpdc_uri_for_path(abs_dir), **songs;
	int n;

	if (!rel)
		return false;
	n = db_songs_in(rel, &songs);
	free(rel);
	for (int i = 0; i < n; i++)
		free(songs[i]);
	free(songs);
	return n >= 0;
}

int mpdc_resolve(const char *dir, char *const *names, int n, char ***uris_out)
{
	char *rel = mpdc_uri_for_path(dir), **db, **uris, buf[2048];
	int ndb, missing = 0;

	uris = calloc(n + 1, sizeof(char *));
	ndb = rel ? db_songs_in(rel, &db) : -1;
	for (int i = 0; i < n; i++) {
		if (rel && rel[0])
			snprintf(buf, sizeof(buf), "%s/%s", rel, names[i]);
		else
			snprintf(buf, sizeof(buf), "%s", names[i]);
		const char *key = buf;
		if (ndb > 0 && bsearch(&key, db, ndb, sizeof(char *), cmp_str)) {
			uris[i] = strdup(buf);
		} else {
			missing++;
			char full[2200];
			if (rel)
				snprintf(full, sizeof(full), "file://%s/%s%s%s", disc_run_path("sdfile"), rel, rel[0] ? "/" : "", names[i]);
			else
				snprintf(full, sizeof(full), "file://%s/%s", dir, names[i]);
			uris[i] = strdup(full);
		}
	}
	for (int i = 0; i < ndb; i++)
		free(db[i]);
	if (ndb > 0)
		free(db);
	free(rel);
	*uris_out = uris;
	return missing;
}

void mpdc_update(const char *abs_dir)
{
	char *rel = mpdc_uri_for_path(abs_dir);

	if (rel && mpdc())
		mpd_run_update(cmd, rel[0] ? rel : NULL);
	mpdc_check("update");
	free(rel);
}

static int add_batch(struct mpd_connection *c, char *const *uris, int n, bool *ok)
{
	int added = 0, i = 0;

	while (i < n) {
		int m = n - i < BATCH ? n - i : BATCH, done = m;
		mpd_command_list_begin(c, false);
		for (int j = 0; j < m; j++)
			mpd_send_add(c, uris[i + j]);
		mpd_command_list_end(c);
		if (!mpd_response_finish(c)) {
			if (mpd_connection_get_error(c) != MPD_ERROR_SERVER) {
				mpdc_check("add");
				break;
			}
			done = (int)mpd_connection_get_server_error_location(c);
			if (done >= m)
				done = m - 1;
			mpdc_check("add");
		}
		for (int j = 0; j < done; j++)
			ok[i + j] = true;
		added += done;
		i += done < m ? done + 1 : m;
	}
	return added;
}

unsigned mpdc_set_origin(const char *o)
{
	struct mpd_connection *c = mpdc();
	struct mpd_status *s;

	snprintf(origin, sizeof(origin), "%s", o ? o : "");
	state_set("queue.origin", origin);
	if (c && (s = mpd_run_status(c))) {
		expected_queue_version = mpd_status_get_queue_version(s);
		mpd_status_free(s);
	}
	return expected_queue_version;
}

int mpdc_play_uris(char *const *uris, int n, int start, const char *o)
{
	struct mpd_connection *c = mpdc();
	bool *ok;
	int pos = 0;

	if (!c)
		return -1;
	ok = calloc(n ? n : 1, sizeof(bool));
	mpd_run_clear(c);
	mpdc_check("clear");
	add_batch(c, uris, n, ok);
	for (int i = 0; i < start && i < n; i++)
		pos += ok[i];
	free(ok);
	mpdc_set_origin(o);
	if (start < 0)
		mpd_run_play(c);
	else
		mpd_run_play_pos(c, pos);
	return mpdc_check("play") ? 0 : -1;
}

int mpdc_append_uris(char *const *uris, int n)
{
	struct mpd_connection *c = mpdc();
	bool *ok;
	int added;

	if (!c)
		return -1;
	ok = calloc(n ? n : 1, sizeof(bool));
	added = add_batch(c, uris, n, ok);
	free(ok);
	return added;
}

static bool playlist_rewrite(const char *name, const struct items *it)
{
	struct mpd_connection *c = mpdc();

	if (!c || (!mpd_run_playlist_clear(c, name) && !mpdc_check("playlistclear")))
		return false;
	for (int i = 0; i < it->n; i += BATCH) {
		mpd_command_list_begin(c, false);
		for (int j = i; j < it->n && j < i + BATCH; j++)
			if (strncmp(it->v[j].uri, "file://", 7) && it->v[j].uri[0] != '/')
				mpd_send_playlist_add(c, name, it->v[j].uri);
		mpd_command_list_end(c);
		if (!mpd_response_finish(c))
			return mpdc_check("playlistadd");
	}
	return true;
}

static bool queue_reorder(const struct items *it)
{
	struct mpd_connection *c = mpdc();

	if (!c)
		return false;
	for (int i = 0; i < it->n; i += BATCH) {
		mpd_command_list_begin(c, false);
		for (int j = i; j < it->n && j < i + BATCH; j++)
			mpd_send_move_id(c, it->v[j].id, j);
		mpd_command_list_end(c);
		if (!mpd_response_finish(c))
			return mpdc_check("moveid");
	}
	return true;
}

bool mpdc_sort_queue(const char *criteria)
{
	struct items q;
	char loaded[1100], o[1100];
	bool ok;

	if (mpdc_queue(&q) < 0)
		return false;
	items_sort(&q, criteria);
	snprintf(loaded, sizeof(loaded), "%s", app_loaded_playlist());
	snprintf(o, sizeof(o), "%s", origin);
	ok = queue_reorder(&q);
	if (ok && loaded[0])
		ok = playlist_rewrite(loaded, &q);
	mpdc_set_origin(o);
	items_free(&q);
	return ok;
}

bool mpdc_sort_playlist(const char *name, const char *criteria)
{
	struct items it;
	bool ok;

	if (!strcmp(name, app_loaded_playlist()))
		return mpdc_sort_queue(criteria);
	if (mpdc_playlist_songs(name, &it) < 0)
		return false;
	items_sort(&it, criteria);
	ok = playlist_rewrite(name, &it);
	items_free(&it);
	return ok;
}

const char *mpdc_queue_origin(void)
{
	return origin;
}

const char *app_loaded_playlist(void)
{
	return strncmp(origin, "playlist:", 9) ? "" : origin + 9;
}

static void remember_queue_name(const char *name)
{
	snprintf(origin, sizeof(origin), "playlist:%s", name);
	state_set("queue.origin", origin);
}

int mpdc_queue(struct items *out)
{
	struct mpd_connection *c = mpdc();
	struct mpd_song *s;

	memset(out, 0, sizeof(*out));
	if (!c || !mpd_send_list_queue_meta(c))
		return mpdc_check("playlistinfo"), -1;
	while ((s = mpd_recv_song(c))) {
		items_add(out, s);
		mpd_song_free(s);
	}
	return mpdc_check("playlistinfo") ? out->n : -1;
}

bool mpdc_queue_delete(int pos)
{
	return mpdc() && mpd_run_delete(cmd, pos) ? true : mpdc_check("delete");
}

bool mpdc_queue_delete_range(int start, int end)
{
	return mpdc() && mpd_run_delete_range(cmd, start, end) ? true : mpdc_check("delete");
}

bool mpdc_queue_move(int from, int to)
{
	return mpdc() && mpd_run_move(cmd, from, to) ? true : mpdc_check("move");
}

bool mpdc_queue_clear(void)
{
	bool r = mpdc() && mpd_run_clear(cmd) ? true : mpdc_check("clear");
	mpdc_set_origin("");
	return r;
}

static int pl_rank(const char *name, char **order, int n)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(order[i], name))
			return i;
	return n;
}

static char **pl_order;
static int pl_order_n;

static int cmp_pl(const void *a, const void *b)
{
	const char *x = *(char *const *)a, *y = *(char *const *)b;
	int rx = pl_rank(x, pl_order, pl_order_n), ry = pl_rank(y, pl_order, pl_order_n);

	if (rx != ry)
		return rx < ry ? -1 : 1;
	return nat_cmp(x, y);
}

static int split_tabs(const char *s, char ***out)
{
	char **v = NULL, *dup = strdup(s ? s : ""), *save = NULL, *tok;
	int n = 0;

	for (tok = strtok_r(dup, "\t", &save); tok; tok = strtok_r(NULL, "\t", &save)) {
		v = realloc(v, (n + 1) * sizeof(char *));
		v[n++] = strdup(tok);
	}
	free(dup);
	*out = v;
	return n;
}

int mpdc_playlists(char ***names)
{
	struct mpd_connection *c = mpdc();
	struct mpd_playlist *p;
	char **v = NULL;
	int n = 0;

	*names = NULL;
	if (!c || !mpd_send_list_playlists(c))
		return mpdc_check("listplaylists"), -1;
	while ((p = mpd_recv_playlist(c))) {
		v = realloc(v, (n + 1) * sizeof(char *));
		v[n++] = strdup(mpd_playlist_get_path(p));
		mpd_playlist_free(p);
	}
	if (!mpdc_check("listplaylists"))
		return -1;
	pl_order_n = split_tabs(state_get("pl.order"), &pl_order);
	if (n > 1)
		qsort(v, n, sizeof(char *), cmp_pl);
	for (int i = 0; i < pl_order_n; i++)
		free(pl_order[i]);
	free(pl_order);
	pl_order = NULL;
	*names = v;
	return n;
}

void mpdc_playlist_order_set(char **names, int n)
{
	size_t len = 1;
	char *s;

	for (int i = 0; i < n; i++)
		len += strlen(names[i]) + 1;
	s = calloc(1, len);
	for (int i = 0; i < n; i++) {
		strcat(s, names[i]);
		if (i + 1 < n)
			strcat(s, "\t");
	}
	state_set("pl.order", s);
	free(s);
}

int mpdc_playlist_songs(const char *name, struct items *out)
{
	struct mpd_connection *c = mpdc();
	struct mpd_song *s;

	memset(out, 0, sizeof(*out));
	if (!c || !mpd_send_list_playlist_meta(c, name))
		return mpdc_check("listplaylistinfo"), -1;
	while ((s = mpd_recv_song(c))) {
		items_add(out, s);
		mpd_song_free(s);
	}
	return mpdc_check("listplaylistinfo") ? out->n : -1;
}

int mpdc_playlist_append(const char *name, char *const *uris, int n)
{
	struct mpd_connection *c = mpdc();
	int added = 0;

	if (!c)
		return -1;
	for (int i = 0; i < n; i += BATCH) {
		int m = n - i < BATCH ? n - i : BATCH;
		mpd_command_list_begin(c, false);
		for (int j = 0; j < m; j++)
			mpd_send_playlist_add(c, name, uris[i + j]);
		mpd_command_list_end(c);
		if (mpd_response_finish(c)) {
			added += m;
			continue;
		}
		mpdc_check("playlistadd");
		for (int j = 0; j < m; j++)
			if (mpd_run_playlist_add(c, name, uris[i + j]))
				added++;
			else
				mpdc_check("playlistadd");
	}
	if (!strcmp(name, app_loaded_playlist()))
		mpdc_append_uris(uris, n);
	return added;
}

int mpdc_playlist_create(const char *name, char *const *uris, int n)
{
	struct mpd_connection *c = mpdc();

	if (!c)
		return -1;
	if (!mpd_run_save(c, name) && mpd_connection_get_server_error(c) != MPD_SERVER_ERROR_EXIST && !mpdc_check("save"))
		return -1;
	mpdc_check("save");
	if (!mpd_run_playlist_clear(c, name) && !mpdc_check("playlistclear"))
		return -1;
	return n ? mpdc_playlist_append(name, uris, n) : 0;
}

bool mpdc_playlist_rename(const char *from, const char *to)
{
	bool r = mpdc() && mpd_run_rename(cmd, from, to) ? true : mpdc_check("rename");
	if (r && !strcmp(app_loaded_playlist(), from))
		remember_queue_name(to);
	return r;
}

bool mpdc_playlist_delete(const char *name)
{
	return mpdc() && mpd_run_rm(cmd, name) ? true : mpdc_check("rm");
}

bool mpdc_playlist_remove(const char *name, int pos)
{
	bool r = mpdc() && mpd_run_playlist_delete(cmd, name, pos) ? true : mpdc_check("playlistdelete");
	if (r && !strcmp(name, app_loaded_playlist()))
		mpdc_queue_delete(pos);
	return r;
}

bool mpdc_playlist_move(const char *name, int from, int to)
{
	bool r = mpdc() && mpd_run_playlist_move(cmd, name, from, to) ? true : mpdc_check("playlistmove");
	if (r && !strcmp(name, app_loaded_playlist()))
		mpdc_queue_move(from, to);
	return r;
}

bool mpdc_playlist_play(const char *name, int pos)
{
	struct mpd_connection *c = mpdc();
	char key[300], buf[1100];
	const char *mode;

	if (!c)
		return false;
	mpd_run_clear(c);
	if (!mpd_run_load(c, name))
		return mpdc_check("load");
	snprintf(buf, sizeof(buf), "playlist:%s", name);
	mpdc_set_origin(buf);
	snprintf(key, sizeof(key), "pl.mode.%s", name);
	mode = state_get(key);
	if (mode && *mode)
		mpdc_set_order(mode);
	mpd_run_play_pos(c, pos < 0 ? 0 : pos);
	return mpdc_check("play");
}

bool mpdc_playlist_save_queue(const char *name)
{
	struct items q;
	char **uris;
	int n = mpdc_queue(&q), k = 0;

	if (n < 0)
		return false;
	uris = calloc(n + 1, sizeof(char *));
	for (int i = 0; i < n; i++)
		if (strncmp(q.v[i].uri, "file://", 7) && q.v[i].uri[0] != '/')
			uris[k++] = q.v[i].uri;
	bool ok = mpdc_playlist_create(name, uris, k) >= 0;
	free(uris);
	items_free(&q);
	return ok;
}

void mpdc_playlist_name_for(char *out, size_t len, const char *rel)
{
	char base[512];
	char **names;
	size_t o = 0;
	int n, suffix = 1;

	for (const char *p = rel; *p && o + 4 < sizeof(base); p++) {
		if (*p == '/') {
			memcpy(base + o, "∕", 3);
			o += 3;
		} else {
			base[o++] = *p;
		}
	}
	base[o] = 0;
	if (!base[0])
		snprintf(base, sizeof(base), "SD card");
	n = mpdc_playlists(&names);
	snprintf(out, len, "%s", base);
	for (bool clash = true; clash; suffix++) {
		clash = false;
		for (int i = 0; i < n; i++)
			if (!strcmp(names[i], out))
				clash = true;
		if (clash)
			snprintf(out, len, "%s (%d)", base, suffix + 1);
	}
	for (int i = 0; i < n; i++)
		free(names[i]);
	free(names);
}

void names_free(struct names *nm)
{
	for (int i = 0; i < nm->n; i++) {
		free(nm->v[i]);
		free(nm->key[i]);
	}
	free(nm->v);
	free(nm->key);
	memset(nm, 0, sizeof(*nm));
}

static const struct names *sort_names;

static int cmp_name_key(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b, c = strcmp(sort_names->v[x], sort_names->v[y]);
	return c ? c : nat_cmp(sort_names->key[x], sort_names->key[y]);
}

static int cmp_key(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b, c = nat_cmp(sort_names->key[x], sort_names->key[y]);
	return c ? c : nat_cmp(sort_names->v[x], sort_names->v[y]);
}

static void names_order(struct names *nm)
{
	int *idx = malloc((nm->n + 1) * sizeof(int)), k = 0;
	char **v = malloc((nm->n + 1) * sizeof(char *)), **key = malloc((nm->n + 1) * sizeof(char *));

	for (int i = 0; i < nm->n; i++)
		idx[i] = i;
	sort_names = nm;
	qsort(idx, nm->n, sizeof(int), cmp_name_key);
	for (int i = 0; i < nm->n; i++) {
		if (k && !strcmp(nm->v[idx[i]], nm->v[idx[k - 1]])) {
			free(nm->v[idx[i]]);
			free(nm->key[idx[i]]);
			continue;
		}
		idx[k++] = idx[i];
	}
	qsort(idx, k, sizeof(int), cmp_key);
	for (int i = 0; i < k; i++) {
		v[i] = nm->v[idx[i]];
		key[i] = nm->key[idx[i]];
	}
	free(nm->v);
	free(nm->key);
	free(idx);
	nm->v = v;
	nm->key = key;
	nm->n = k;
}

int mpdc_list_tag(enum mpd_tag_type tag, enum mpd_tag_type sort_tag, const char *expr, struct names *out)
{
	struct mpd_connection *c = mpdc();
	struct mpd_pair *p;
	const char *name = mpd_tag_name(tag);
	char key[512] = "";
	int cap = 0;

	memset(out, 0, sizeof(*out));
	if (!c || !mpd_search_db_tags(c, tag))
		return mpdc_check("list"), -1;
	if (expr)
		mpd_search_add_expression(c, expr);
	if (sort_tag != MPD_TAG_UNKNOWN)
		mpd_search_add_group_tag(c, sort_tag);
	if (!mpd_search_commit(c))
		return mpdc_check("list"), -1;
	while ((p = mpd_recv_pair(c))) {
		if (strcasecmp(p->name, name)) {
			snprintf(key, sizeof(key), "%s", p->value);
		} else if (p->value[0]) {
			if (out->n == cap) {
				cap = cap ? cap * 2 : 64;
				out->v = realloc(out->v, cap * sizeof(char *));
				out->key = realloc(out->key, cap * sizeof(char *));
			}
			out->v[out->n] = strdup(p->value);
			out->key[out->n++] = strdup(key[0] ? key : p->value);
		}
		mpd_return_pair(c, p);
	}
	if (!mpdc_check("list")) {
		names_free(out);
		return -1;
	}
	if (out->n)
		names_order(out);
	return out->n;
}

void albums_free(struct albums *a)
{
	for (int i = 0; i < a->n; i++) {
		free(a->v[i].name);
		free(a->v[i].artist);
	}
	free(a->v);
	memset(a, 0, sizeof(*a));
}

static int cmp_album_id(const void *a, const void *b)
{
	const struct album *x = a, *y = b;
	int c = strcmp(x->name, y->name);
	return c ? c : strcmp(x->artist, y->artist);
}

static bool list_albums(struct mpd_connection *c, const char *expr, struct albums *out)
{
	struct mpd_pair *p;
	char artist[512] = "";
	int year = 0;

	if (!mpd_search_db_tags(c, MPD_TAG_ALBUM))
		return false;
	if (expr)
		mpd_search_add_expression(c, expr);
	mpd_search_add_group_tag(c, MPD_TAG_DATE);
	mpd_search_add_group_tag(c, MPD_TAG_ALBUM_ARTIST);
	if (!mpd_search_commit(c))
		return false;
	while ((p = mpd_recv_pair(c))) {
		if (!strcasecmp(p->name, "AlbumArtist")) {
			snprintf(artist, sizeof(artist), "%s", p->value);
		} else if (!strcasecmp(p->name, "Date")) {
			year = year_of(p->value);
		} else if (!strcasecmp(p->name, "Album") && p->value[0]) {
			if (out->n == out->cap) {
				out->cap = out->cap ? out->cap * 2 : 64;
				out->v = realloc(out->v, out->cap * sizeof(*out->v));
			}
			out->v[out->n++] = (struct album){ strdup(p->value), strdup(artist), year };
		}
		mpd_return_pair(c, p);
	}
	return mpd_connection_get_error(c) == MPD_ERROR_SUCCESS;
}

int mpdc_albums(const char *const *exprs, int n_exprs, struct albums *out)
{
	struct mpd_connection *c = mpdc();
	int k = 0;

	memset(out, 0, sizeof(*out));
	for (int i = 0; i < n_exprs; i++)
		if (!c || !list_albums(c, exprs[i], out)) {
			mpdc_check("list");
			albums_free(out);
			return -1;
		}
	if (out->n > 1)
		qsort(out->v, out->n, sizeof(*out->v), cmp_album_id);
	for (int i = 0; i < out->n; i++) {
		struct album *a = &out->v[i];
		if (k && !cmp_album_id(a, &out->v[k - 1])) {
			struct album *keep = &out->v[k - 1];
			if (a->year && (!keep->year || a->year < keep->year))
				keep->year = a->year;
			free(a->name);
			free(a->artist);
			continue;
		}
		out->v[k++] = *a;
	}
	out->n = k;
	return k;
}

const char *item_album_artist(const struct item *it)
{
	return it->albumartist[0] ? it->albumartist : it->artist;
}

static int cmp_album_order(const void *a, const void *b)
{
	const struct item *x = a, *y = b;
	int c = nat_cmp(item_album_artist(x), item_album_artist(y));

	if (c)
		return c;
	if (x->year != y->year)
		return x->year < y->year ? -1 : 1;
	if ((c = nat_cmp(x->album, y->album)))
		return c;
	if (x->disc != y->disc)
		return x->disc < y->disc ? -1 : 1;
	if (x->track != y->track)
		return x->track < y->track ? -1 : 1;
	return nat_cmp(x->uri, y->uri);
}

int mpdc_songs(const char *expr, const char *sort, unsigned window, struct items *out)
{
	struct mpd_connection *c = mpdc();
	struct mpd_song *s;

	memset(out, 0, sizeof(*out));
	if (!c || !mpd_search_db_songs(c, true))
		return mpdc_check("find"), -1;
	mpd_search_add_expression(c, expr);
	if (sort)
		mpd_search_add_sort_name(c, sort[0] == '-' ? sort + 1 : sort, sort[0] == '-');
	if (window)
		mpd_search_add_window(c, 0, window);
	if (!mpd_search_commit(c))
		return mpdc_check("find"), -1;
	while ((s = mpd_recv_song(c))) {
		items_add(out, s);
		mpd_song_free(s);
	}
	if (!mpdc_check("find")) {
		items_free(out);
		return -1;
	}
	if (!sort && out->n > 1)
		qsort(out->v, out->n, sizeof(struct item), cmp_album_order);
	return out->n;
}

bool mpdc_stats(unsigned *songs, unsigned *albums, unsigned *artists, unsigned long *db_update, unsigned long *play_time)
{
	struct mpd_connection *c = mpdc();
	struct mpd_stats *s;

	if (!c || !(s = mpd_run_stats(c)))
		return mpdc_check("stats");
	*songs = mpd_stats_get_number_of_songs(s);
	*albums = mpd_stats_get_number_of_albums(s);
	*artists = mpd_stats_get_number_of_artists(s);
	*db_update = mpd_stats_get_db_update_time(s);
	*play_time = mpd_stats_get_db_play_time(s);
	mpd_stats_free(s);
	return true;
}
