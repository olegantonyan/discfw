#include "player.h"

#include <discd/proto.h>
#include <mpd/client.h>
#include <stdio.h>
#include <string.h>

#define MPD_SOCK "/run/mpd.sock"

static struct mpd_connection *conn;

static bool ensure(void)
{
	if (conn && mpd_connection_get_error(conn) == MPD_ERROR_SUCCESS)
		return true;
	if (conn && mpd_connection_clear_error(conn))
		return true;
	if (conn)
		mpd_connection_free(conn);
	conn = mpd_connection_new(MPD_SOCK, 0, 3000);
	if (conn && mpd_connection_get_error(conn) == MPD_ERROR_SUCCESS)
		return true;
	if (conn) {
		fprintf(stderr, "mpd: %s\n", mpd_connection_get_error_message(conn));
		mpd_connection_free(conn);
		conn = NULL;
	}
	return false;
}

static void report(const char *what)
{
	if (conn && mpd_connection_get_error(conn) != MPD_ERROR_SUCCESS)
		fprintf(stderr, "mpd %s: %s\n", what, mpd_connection_get_error_message(conn));
}

int player_play_files(const char *dir, char **names, int n, int idx)
{
	char uri[1024];

	if (!ensure())
		return -1;
	mpd_run_clear(conn);
	mpd_command_list_begin(conn, false);
	for (int i = 0; i < n; i++) {
		snprintf(uri, sizeof(uri), "file://%s/%s", dir, names[i]);
		mpd_send_add(conn, uri);
	}
	mpd_command_list_end(conn);
	mpd_response_finish(conn);
	report("add");
	if (!ensure())
		return -1;
	mpd_run_play_pos(conn, idx);
	report("play");
	return 0;
}

void player_toggle(void)
{
	struct mpd_status *s;

	if (!ensure() || !(s = mpd_run_status(conn)))
		return;
	switch (mpd_status_get_state(s)) {
	case MPD_STATE_PLAY:
		mpd_run_pause(conn, true);
		break;
	case MPD_STATE_PAUSE:
		mpd_run_pause(conn, false);
		break;
	default:
		mpd_run_play(conn);
	}
	mpd_status_free(s);
	report("toggle");
}

void player_next(void)
{
	if (ensure())
		mpd_run_next(conn);
}

void player_prev(void)
{
	if (ensure())
		mpd_run_previous(conn);
}

void player_pause(void)
{
	if (ensure())
		mpd_run_pause(conn, true);
}

static void copy_tag(char *dst, size_t len, const struct mpd_song *song, enum mpd_tag_type t)
{
	const char *v = mpd_song_get_tag(song, t, 0);
	snprintf(dst, len, "%s", v ? v : "");
}

int player_poll(struct np *np)
{
	struct mpd_status *s;
	struct mpd_song *song;
	const struct mpd_audio_format *af;

	memset(np, 0, sizeof(*np));
	if (!ensure() || !(s = mpd_run_status(conn)))
		return -1;
	switch (mpd_status_get_state(s)) {
	case MPD_STATE_PLAY:
		np->state = NP_PLAY;
		break;
	case MPD_STATE_PAUSE:
		np->state = NP_PAUSE;
		break;
	case MPD_STATE_STOP:
		np->state = NP_STOP;
		break;
	default:
		np->state = NP_UNKNOWN;
	}
	np->elapsed_s = mpd_status_get_elapsed_time(s);
	np->total_s = mpd_status_get_total_time(s);
	af = mpd_status_get_audio_format(s);
	if (af)
		snprintf(np->audio, sizeof(np->audio), "%u Hz / %u bit", af->sample_rate, af->bits);
	mpd_status_free(s);
	song = mpd_run_current_song(conn);
	if (song) {
		copy_tag(np->title, sizeof(np->title), song, MPD_TAG_TITLE);
		copy_tag(np->artist, sizeof(np->artist), song, MPD_TAG_ARTIST);
		if (!np->title[0]) {
			const char *u = mpd_song_get_uri(song), *b = strrchr(u, '/');
			snprintf(np->title, sizeof(np->title), "%s", b ? b + 1 : u);
		}
		mpd_song_free(song);
	}
	return 0;
}

int dac_cmd(const char *cmd, char *reply, int len)
{
	return discd_request(cmd, reply, len, 3000);
}
