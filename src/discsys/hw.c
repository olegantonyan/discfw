#include "discsys.h"

#include <disc/conn.h>
#include <disc/settings.h>
#include <glib-unix.h>
#include <mpd/client.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

static struct lconn watch;
static guint watch_src, retry_src;

int hw_request(const char *cmd, char *reply, size_t len)
{
	char line[128];

	if (sys.fake_hw)
		return -1;
	snprintf(line, sizeof(line), "%s\n", cmd);
	if (discd_request(line, reply, len, 3000) < 0)
		return -1;
	return reply && strncmp(reply, "ok", 2) ? -1 : 0;
}

void hw_volpct(int pct)
{
	char cmd[32];

	if (sys.fake_hw)
		return;
	snprintf(cmd, sizeof(cmd), "volpct %d", pct);
	if (hw_request(cmd, NULL, 0))
		logmsg(LOG_WARNING, "discd volpct failed");
}

static void parse_bat(const char *line)
{
	struct bat b = { .valid = true };

	b.cap = kv_get_int(line, "cap", -1);
	b.mv = kv_get_int(line, "mv", 0);
	b.ma = kv_get_int(line, "ma", 0);
	b.temp = kv_get_int(line, "temp", 0);
	b.skip = kv_get_int(line, "skip", 0);
	kv_get(line, "state", b.state, sizeof(b.state));
	kv_get(line, "vbus", b.vbus, sizeof(b.vbus));
	kv_get(line, "stage", b.stage, sizeof(b.stage));
	b.valid = b.cap >= 0 && strcmp(b.state, "unknown");
	sys.bat = b;
	power_on_bat();
	mode_on_bat();
}

static void on_jack(bool plugged)
{
	struct mpd_connection *c;

	if (plugged || sys.mode != MODE_PLAYER || sys.output != OUT_DAC || sys.play != PS_PLAY || !set_get_bool("audio.pause_jack_out"))
		return;
	logmsg(LOG_INFO, "headphones unplugged, pausing");
	if ((c = mpdc()) && !mpd_run_pause(c, true))
		mpdc_report("pause");
}

static void on_line(struct lconn *c, char *line, void *arg)
{
	(void)c;
	(void)arg;
	if (!strncmp(line, "!jack", 5)) {
		on_jack(kv_get_int(line, "plugged", 1));
	} else if (!strncmp(line, "!bat", 4)) {
		parse_bat(line);
		struct kvbuf b;
		kv_init(&b);
		kv_raw(&b, line);
		ev_kv(&b);
	}
}

static void schedule_retry(void);

static gboolean on_watch(gint fd, GIOCondition cond, gpointer data)
{
	(void)fd;
	(void)data;
	lc_read(&watch, on_line, NULL);
	if (cond & G_IO_OUT)
		lc_flush(&watch);
	if (watch.dead || watch.eof) {
		logmsg(LOG_WARNING, "discd connection lost");
		lc_close(&watch);
		watch_src = 0;
		schedule_retry();
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static gboolean connect_watch(gpointer data)
{
	char reply[512];
	int fd;

	(void)data;
	retry_src = 0;
	fd = unix_connect(DISCD_SOCK, 1000);
	if (fd < 0) {
		schedule_retry();
		return G_SOURCE_REMOVE;
	}
	g_unix_set_fd_nonblocking(fd, TRUE, NULL);
	lc_init(&watch, fd, 4096);
	lc_puts(&watch, "watch\n");
	watch_src = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_watch, NULL);
	hw_request("reload", NULL, 0);
	volume_reapply();
	if (hw_request("battery", reply, sizeof(reply)) == 0)
		parse_bat(reply);
	return G_SOURCE_REMOVE;
}

static void schedule_retry(void)
{
	if (!retry_src)
		retry_src = g_timeout_add_seconds(2, connect_watch, NULL);
}

void hw_init(void)
{
	if (sys.fake_hw) {
		sys.bat = (struct bat){ .valid = true, .cap = 77, .mv = 3950, .ma = -30, .temp = 300 };
		strcpy(sys.bat.state, "battery");
		strcpy(sys.bat.vbus, "none");
		strcpy(sys.bat.stage, "none");
		return;
	}
	connect_watch(NULL);
}
