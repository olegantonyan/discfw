#include "discsys.h"

#include <disc/conn.h>
#include <disc/settings.h>
#include <mpd/client.h>
#include <errno.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#define MAX_CLIENTS 24
#define OUT_MAX (256 * 1024)
#define UI_MISSES 3

struct client {
	struct lconn c;
	bool used, watch, ui;
	pid_t pid;
	guint src;
	int missed;
};

static struct client clients[MAX_CLIENTS];
static unsigned ping_seq;

static void reply(struct client *cl, struct kvbuf *b)
{
	kv_end(b);
	lc_puts(&cl->c, b->s);
	kv_free(b);
}

static void ok(struct client *cl)
{
	lc_puts(&cl->c, "ok\n");
}

static void err(struct client *cl, const char *msg)
{
	struct kvbuf b;
	kv_init(&b);
	kv_raw(&b, "err");
	kv_str(&b, "msg", msg);
	reply(cl, &b);
}

void ev_send(const char *line)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].used && clients[i].watch)
			lc_puts(&clients[i].c, line);
}

void ev_kv(struct kvbuf *b)
{
	kv_end(b);
	ev_send(b->s);
	kv_free(b);
}

void ev_toast(const char *text)
{
	struct kvbuf b;
	kv_init(&b);
	kv_raw(&b, "!toast");
	kv_str(&b, "text", text);
	ev_kv(&b);
}

bool server_ui_connected(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].used && clients[i].ui)
			return true;
	return false;
}

static void snapshot(struct client *cl, const char *tag)
{
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, tag);
	kv_str(&b, "screen", sys.screen == SCR_ON ? "on" : sys.screen == SCR_DIM ? "dim" : "off");
	kv_int(&b, "locked", sys.locked);
	kv_int(&b, "vol", sys.volume);
	kv_str(&b, "out", sys.output == OUT_BT ? "bt" : "dac");
	kv_str(&b, "btdev", sys.bt_dev);
	kv_int(&b, "sleep", sys.sleep_left_s);
	power_status(&b);
	reply(cl, &b);
}

static const char *rest_of_line(const char *line, int words)
{
	const char *p = line;
	for (int i = 0; i < words; i++) {
		while (*p == ' ')
			p++;
		while (*p && *p != ' ')
			p++;
	}
	if (*p == ' ')
		p++;
	return p;
}

static void handle(struct client *cl, char *line)
{
	char cmd[32], a1[128], a2[64];
	struct kvbuf b;

	kv_word(line, 0, cmd, sizeof(cmd));
	kv_word(line, 1, a1, sizeof(a1));
	kv_word(line, 2, a2, sizeof(a2));
	kv_init(&b);

	if (!strcmp(cmd, "hello")) {
		cl->watch = true;
		if (!strcmp(a1, "ui")) {
			for (int i = 0; i < MAX_CLIENTS; i++)
				clients[i].ui = false;
			cl->ui = true;
			cl->missed = 0;
		}
		ok(cl);
		snapshot(cl, "!state");
	} else if (!strcmp(cmd, "pong")) {
		cl->missed = 0;
	} else if (!strcmp(cmd, "drawn")) {
		screen_ui_drawn();
	} else if (!strcmp(cmd, "get")) {
		const char *v = set_get(a1);
		if (!v) {
			err(cl, "unknown key");
		} else {
			kv_raw(&b, "ok");
			kv_str(&b, "key", a1);
			kv_str(&b, "val", v);
			reply(cl, &b);
		}
	} else if (!strcmp(cmd, "set") || !strcmp(cmd, "reset")) {
		const struct set_def *d = set_lookup(a1);
		int r = d ? cfg_set(a1, !strcmp(cmd, "reset") ? d->def : rest_of_line(line, 2)) : -1;
		if (r == -1)
			err(cl, "unknown key");
		else if (r)
			err(cl, "invalid value");
		else
			ok(cl);
	} else if (!strcmp(cmd, "list")) {
		cfg_list(&b, a1);
		lc_puts(&cl->c, b.s ? b.s : "");
		kv_free(&b);
		ok(cl);
	} else if (!strcmp(cmd, "state")) {
		snapshot(cl, "=");
		ok(cl);
	} else if (!strcmp(cmd, "screen")) {
		if (!strcmp(a1, "on"))
			screen_set(SCR_ON, false);
		else if (!strcmp(a1, "off"))
			screen_set(SCR_OFF, false);
		else if (!strcmp(a1, "lock"))
			screen_set(SCR_OFF, true);
		else if (!strcmp(a1, "unlock"))
			screen_unlock();
		else if (!strcmp(a1, "toggle"))
			screen_toggle();
		else if (!strcmp(a1, "activity"))
			screen_activity();
		else {
			err(cl, "usage: screen on|off|lock|unlock|toggle|activity");
			return;
		}
		ok(cl);
	} else if (!strcmp(cmd, "key")) {
		keys_simulate(a1, a2[0] ? a2 : "short");
		ok(cl);
	} else if (!strcmp(cmd, "action")) {
		if (action_run(a1))
			ok(cl);
		else
			err(cl, "unknown action");
	} else if (!strcmp(cmd, "vol")) {
		if (a1[0] == '+' || a1[0] == '-')
			volume_step(atoi(a1));
		else
			volume_set(atoi(a1));
		ok(cl);
	} else if (!strcmp(cmd, "sleep")) {
		power_sleep_timer(atoi(a1));
		ok(cl);
	} else if (!strcmp(cmd, "poweroff") || !strcmp(cmd, "reboot")) {
		ok(cl);
		lc_flush(&cl->c);
		power_off(!strcmp(cmd, "reboot"));
	} else if (!strcmp(cmd, "library") && (!strcmp(a1, "scan") || !strcmp(a1, "rescan"))) {
		struct mpd_connection *c;
		bool started = false;
		if (!strcmp(a1, "scan"))
			started = library_update("requested");
		else if ((c = mpdc()) && library_mounted())
			started = mpd_run_rescan(c, NULL) != 0;
		if (started)
			ok(cl);
		else
			err(cl, "music card not mounted or music service down");
	} else if (!strcmp(cmd, "library") && !strcmp(a1, "reset")) {
		char *argv[] = { "/usr/libexec/discfw/library", "reset", NULL };
		GError *e = NULL;
		if (sys.fake_hw || g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &e)) {
			ev_toast("Library reset, scanning…");
			ok(cl);
		} else {
			err(cl, e->message);
			g_error_free(e);
		}
	} else if (!strcmp(cmd, "wifi") || !strcmp(cmd, "bt")) {
		char ssid[160] = "", pass[160] = "", mac[32] = "", e[160] = "";
		struct kvbuf out;
		int r;
		kv_get(line, "ssid", ssid, sizeof(ssid));
		kv_get(line, "pass", pass, sizeof(pass));
		kv_get(line, "mac", mac, sizeof(mac));
		if (!mac[0])
			kv_word(line, 2, mac, sizeof(mac));
		kv_init(&out);
		r = cmd[0] == 'w' ? wifi_command(a1, ssid, pass, kv_get_int(line, "hidden", 0), &out, e, sizeof(e)) : bt_command(a1, mac, &out, e, sizeof(e));
		if (out.s)
			lc_puts(&cl->c, out.s);
		kv_free(&out);
		if (r)
			err(cl, e);
		else
			ok(cl);
	} else if (!strcmp(cmd, "toast")) {
		ev_toast(rest_of_line(line, 1));
		ok(cl);
	} else {
		err(cl, "unknown command");
	}
}

static void on_line(struct lconn *c, char *line, void *arg)
{
	(void)c;
	if (line[0])
		handle(arg, line);
}

static void drop(struct client *cl)
{
	if (cl->src)
		g_source_remove(cl->src);
	cl->src = 0;
	lc_close(&cl->c);
	cl->used = cl->watch = cl->ui = false;
}

static gboolean on_client(gint fd, GIOCondition cond, gpointer data)
{
	struct client *cl = data;

	(void)fd;
	if (cond & (G_IO_IN | G_IO_HUP | G_IO_ERR))
		lc_read(&cl->c, on_line, cl);
	if (cond & G_IO_OUT)
		lc_flush(&cl->c);
	if (cl->c.dead || (cl->c.eof && !lc_pending(&cl->c))) {
		cl->src = 0;
		lc_close(&cl->c);
		cl->used = cl->watch = cl->ui = false;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static gboolean flush_all(gpointer data)
{
	(void)data;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		struct client *cl = &clients[i];
		if (!cl->used)
			continue;
		if (lc_pending(&cl->c))
			lc_flush(&cl->c);
		if (cl->c.dead)
			drop(cl);
	}
	return G_SOURCE_CONTINUE;
}

static gboolean on_accept(gint lfd, GIOCondition cond, gpointer data)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);
	int fd;

	(void)cond;
	(void)data;
	while ((fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
		int i;
		for (i = 0; i < MAX_CLIENTS && clients[i].used; i++)
			;
		if (i == MAX_CLIENTS) {
			close(fd);
			continue;
		}
		struct client *cl = &clients[i];
		lc_init(&cl->c, fd, OUT_MAX);
		cl->used = true;
		cl->watch = cl->ui = false;
		cl->missed = 0;
		cl->pid = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) ? 0 : cred.pid;
		cl->src = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_client, cl);
	}
	return G_SOURCE_CONTINUE;
}

void server_ping_ui(void)
{
	char line[32];

	snprintf(line, sizeof(line), "!ping n=%u\n", ++ping_seq);
	for (int i = 0; i < MAX_CLIENTS; i++) {
		struct client *cl = &clients[i];
		if (!cl->used || !cl->ui)
			continue;
		if (++cl->missed > UI_MISSES) {
			logmsg(LOG_ERR, "UI pid %d stopped answering, killing it", (int)cl->pid);
			if (cl->pid > 1)
				kill(cl->pid, SIGKILL);
			drop(cl);
			continue;
		}
		lc_puts(&cl->c, line);
	}
}

void server_init(void)
{
	int lfd = unix_listen(DISCSYS_SOCK, 0666);

	if (lfd < 0) {
		logmsg(LOG_ERR, "listen %s: %s", DISCSYS_SOCK, strerror(errno));
		exit(1);
	}
	g_unix_fd_add(lfd, G_IO_IN, on_accept, NULL);
	g_timeout_add(100, flush_all, NULL);
}
