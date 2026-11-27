#include "discsys.h"

#include <disc/conn.h>
#include <disc/paths.h>
#include <disc/settings.h>
#include <stdio.h>
#include <unistd.h>
#include <glib-unix.h>
#include <mpd/client.h>
#include <string.h>
#include <syslog.h>

#define TIMEOUT_MS 2000
#define IDLE_MASK (MPD_IDLE_PLAYER | MPD_IDLE_MIXER | MPD_IDLE_OUTPUT | MPD_IDLE_OPTIONS | MPD_IDLE_QUEUE)

static struct mpd_connection *cmd, *idle;
static guint idle_src, retry_src;

static struct mpd_connection *connect_mpd(void)
{
	struct mpd_connection *c = mpd_connection_new(MPD_SOCK, 0, TIMEOUT_MS);

	if (c && mpd_connection_get_error(c) == MPD_ERROR_SUCCESS)
		return c;
	if (c)
		mpd_connection_free(c);
	return NULL;
}

#define IDLE_PING_MS 20000

static gint64 cmd_used;

struct mpd_connection *mpdc(void)
{
	gint64 now = now_ms();

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
	cmd = connect_mpd();
	return cmd;
}

bool mpdc_ok(void)
{
	return cmd && mpd_connection_get_error(cmd) == MPD_ERROR_SUCCESS;
}

void mpdc_report(const char *what)
{
	if (cmd && mpd_connection_get_error(cmd) != MPD_ERROR_SUCCESS)
		logmsg(LOG_WARNING, "mpd %s: %s", what, mpd_connection_get_error_message(cmd));
}

int mpdc_output_id(const char *name)
{
	struct mpd_connection *c = mpdc();
	struct mpd_output *o;
	int id = -1;

	if (!c || !mpd_send_outputs(c))
		return -1;
	while ((o = mpd_recv_output(c))) {
		if (!strcmp(mpd_output_get_name(o), name))
			id = mpd_output_get_id(o);
		mpd_output_free(o);
	}
	mpd_response_finish(c);
	mpdc_report("outputs");
	return id;
}

void mpdc_refresh(void)
{
	struct mpd_connection *c = mpdc();
	struct mpd_status *s;
	enum play_state prev = sys.play;

	if (!c || !(s = mpd_run_status(c))) {
		mpdc_report("status");
		return;
	}
	switch (mpd_status_get_state(s)) {
	case MPD_STATE_PLAY:
		sys.play = PS_PLAY;
		break;
	case MPD_STATE_PAUSE:
		sys.play = PS_PAUSE;
		break;
	case MPD_STATE_STOP:
		sys.play = PS_STOP;
		break;
	default:
		sys.play = PS_UNKNOWN;
	}
	int vol = mpd_status_get_volume(s);
	mpd_status_free(s);
	if (vol >= 0 && vol != sys.volume && sys.outputs_enabled == 1)
		volume_on_mixer(vol);
	if (prev != sys.play) {
		power_on_play_state();
		bt_play_state();
	}
}

void mpdc_apply_order(void)
{
	const char *o = set_get("play.order");
	struct mpd_connection *c = mpdc();
	bool repeat = !strcmp(o, "seq") || !strcmp(o, "random") || !strcmp(o, "single");
	bool random = !strcmp(o, "random");
	bool single = !strcmp(o, "single") || !strcmp(o, "once");

	if (!c)
		return;
	mpd_command_list_begin(c, false);
	mpd_send_repeat(c, repeat);
	mpd_send_random(c, random);
	mpd_send_single(c, single);
	mpd_send_consume(c, false);
	mpd_command_list_end(c);
	mpd_response_finish(c);
	mpdc_report("order");
}

void mpdc_apply_replaygain(void)
{
	struct mpd_connection *c = mpdc();
	enum mpd_replay_gain_mode m = mpd_parse_replay_gain_name(set_get("play.replaygain"));

	if (c && m != MPD_REPLAY_UNKNOWN && !mpd_run_replay_gain_mode(c, m))
		mpdc_report("replay_gain_mode");
}

static void schedule_retry(void);

static void resume_policy(void)
{
	const char *flag = disc_run_path("discsys.resumed"), *mode = set_get("play.resume");
	struct mpd_connection *c = mpdc();
	FILE *f;

	if (!c || !access(flag, F_OK))
		return;
	if ((f = fopen(flag, "w")))
		fclose(f);
	if (!strcmp(mode, "off"))
		mpd_run_stop(c);
	else if (!strcmp(mode, "song"))
		mpd_run_seek_current(c, 0, false);
	mpdc_report("resume");
}

static void idle_close(void)
{
	if (idle_src)
		g_source_remove(idle_src);
	idle_src = 0;
	if (idle)
		mpd_connection_free(idle);
	idle = NULL;
}

static gboolean on_idle(gint fd, GIOCondition cond, gpointer data)
{
	enum mpd_idle ev;

	(void)fd;
	(void)data;
	if (cond & (G_IO_HUP | G_IO_ERR))
		goto lost;
	ev = mpd_recv_idle(idle, false);
	if (!ev && mpd_connection_get_error(idle) != MPD_ERROR_SUCCESS)
		goto lost;
	if (ev & MPD_IDLE_OUTPUT)
		volume_output_changed();
	if (ev & (MPD_IDLE_PLAYER | MPD_IDLE_MIXER | MPD_IDLE_OPTIONS))
		mpdc_refresh();
	if (!mpd_send_idle_mask(idle, IDLE_MASK))
		goto lost;
	return G_SOURCE_CONTINUE;
lost:
	logmsg(LOG_WARNING, "mpd idle connection lost");
	idle_src = 0;
	idle_close();
	if (cmd)
		mpd_connection_free(cmd);
	cmd = NULL;
	schedule_retry();
	return G_SOURCE_REMOVE;
}

static gboolean try_connect(gpointer data)
{
	(void)data;
	retry_src = 0;
	idle = connect_mpd();
	if (!idle || !mpdc() || !mpd_send_idle_mask(idle, IDLE_MASK)) {
		idle_close();
		schedule_retry();
		return G_SOURCE_REMOVE;
	}
	idle_src = g_unix_fd_add(mpd_connection_get_fd(idle), G_IO_IN | G_IO_HUP | G_IO_ERR, on_idle, NULL);
	logmsg(LOG_INFO, "mpd connected");
	resume_policy();
	library_on_mpd();
	volume_output_changed();
	volume_on_mpd_connect();
	mpdc_refresh();
	return G_SOURCE_REMOVE;
}

static void schedule_retry(void)
{
	if (!retry_src)
		retry_src = g_timeout_add(1000, try_connect, NULL);
}

void mpdc_init(void)
{
	try_connect(NULL);
}
