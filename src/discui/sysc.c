#include "app.h"
#include "fsdir.h"
#include "ui.h"

#include <disc/conn.h>
#include <disc/kv.h>
#include <disc/settings.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct lconn conn = { .fd = -1 };
static lv_timer_t *retry;

static void update_events(void)
{
	if (conn.fd >= 0)
		loop_events(conn.fd, POLLIN | (lc_pending(&conn) ? POLLOUT : 0));
}

void sysc_send(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	if (conn.fd < 0)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	strcat(buf, "\n");
	lc_puts(&conn, buf);
	update_events();
}

void sysc_set(const char *key, const char *val)
{
	if (conn.fd < 0) {
		ui_toast("System service not running");
		return;
	}
	sysc_send("set %s %s", key, val);
}

void sysc_set_int(const char *key, int val)
{
	char num[16];
	snprintf(num, sizeof(num), "%d", val);
	sysc_set(key, num);
}

static void parse_screen(const char *line, const char *key)
{
	char st[8];
	bool was_on = app.sys.screen_on;

	kv_get(line, key, st, sizeof(st));
	app.sys.screen_on = strcmp(st, "off") != 0;
	app.sys.dimmed = !strcmp(st, "dim");
	app.sys.locked = kv_get_int(line, "locked", 0);
	disp_set_active(app.sys.screen_on);
	lock_update();
	if (app.sys.screen_on && !was_on) {
		app_notify(EV_SYS | EV_STATUS | EV_SECOND);
		lv_refr_now(NULL);
	}
	if (app.sys.screen_on)
		sysc_send("drawn");
	else
		state_save();
}

static void parse_bat(const char *line)
{
	app.bat.valid = true;
	app.bat.cap = kv_get_int(line, "cap", -1);
	app.bat.mv = kv_get_int(line, "mv", 0);
	app.bat.ma = kv_get_int(line, "ma", 0);
	app.bat.temp = kv_get_int(line, "temp", 0);
	kv_get(line, "state", app.bat.state, sizeof(app.bat.state));
	kv_get(line, "vbus", app.bat.vbus, sizeof(app.bat.vbus));
	kv_get(line, "stage", app.bat.stage, sizeof(app.bat.stage));
	app.bat.valid = app.bat.cap >= 0;
	app_notify(EV_BAT);
}

static void parse_bt(const char *line, const char *name_key, const char *bat_key)
{
	kv_get(line, name_key, app.sys.bt_name, sizeof(app.sys.bt_name));
	app.sys.bt_bat = kv_get_int(line, bat_key, -1);
}

static void parse_power(const char *line)
{
	app.sys.powered = kv_get_int(line, "powered", 0);
	kv_get(line, "profile", app.sys.profile, sizeof(app.sys.profile));
	app.sys.lowbat = kv_get_int(line, "lowbat", 0);
	if (kv_get_int(line, "cap", -1) >= 0 && !app.bat.valid) {
		app.bat.valid = true;
		app.bat.cap = kv_get_int(line, "cap", -1);
		kv_get(line, "batstate", app.bat.state, sizeof(app.bat.state));
	}
	lowbat_update();
	app_notify(EV_SYS | EV_BAT);
}

static void on_line(struct lconn *c, char *line, void *arg)
{
	char tag[16], v[512];

	(void)c;
	(void)arg;
	kv_word(line, 0, tag, sizeof(tag));
	if (!strcmp(tag, "!ping")) {
		sysc_send("pong %ld", kv_get_int(line, "n", 0));
	} else if (!strcmp(tag, "!state")) {
		parse_screen(line, "screen");
		app.sys.volume = kv_get_int(line, "vol", 0);
		kv_get(line, "out", v, sizeof(v));
		app.sys.out_bt = !strcmp(v, "bt");
		kv_get(line, "btdev", app.sys.bt_dev, sizeof(app.sys.bt_dev));
		parse_bt(line, "btname", "btbat");
		app.sys.sleep_left = kv_get_int(line, "sleep", 0);
		parse_power(line);
	} else if (!strcmp(tag, "!screen")) {
		parse_screen(line, "state");
	} else if (!strcmp(tag, "!vol")) {
		app.sys.volume = kv_get_int(line, "pct", 0);
		kv_get(line, "out", v, sizeof(v));
		app.sys.out_bt = !strcmp(v, "bt");
		app_notify(EV_VOLUME);
	} else if (!strcmp(tag, "!bat")) {
		parse_bat(line);
	} else if (!strcmp(tag, "!bt")) {
		parse_bt(line, "name", "battery");
		app_notify(EV_SYS);
	} else if (!strcmp(tag, "!power")) {
		parse_power(line);
	} else if (!strcmp(tag, "!sleep")) {
		app.sys.sleep_left = kv_get_int(line, "left", 0);
		app_notify(EV_SYS);
	} else if (!strcmp(tag, "!toast")) {
		kv_get(line, "text", v, sizeof(v));
		ui_toast(v);
	} else if (!strcmp(tag, "!set")) {
		char key[64];
		kv_get(line, "key", key, sizeof(key));
		kv_get(line, "val", v, sizeof(v));
		set_put(key, v);
		if (!strcmp(key, "ui.font")) {
			state_save();
			exit(0);
		}
		app_notify(EV_SETTINGS);
	} else if (!strcmp(tag, "!mount")) {
		fsdir_invalidate();
		app_notify(EV_MOUNT);
	} else if (!strcmp(tag, "!shutdown")) {
		app.sys.shutting_down = true;
		state_save();
		ui_shutdown_overlay(kv_get_int(line, "reboot", 0));
	} else if (!strcmp(tag, "!key")) {
		kv_get(line, "action", v, sizeof(v));
		if (!strncmp(v, "vol_", 4))
			ui_volume_overlay();
	} else if (!strcmp(tag, "err")) {
		kv_get(line, "msg", v, sizeof(v));
		ui_toast(v);
	}
}

static void disconnect(void);

static void on_fd(int fd, short revents, void *ctx)
{
	(void)fd;
	(void)ctx;
	if (revents & POLLOUT)
		lc_flush(&conn);
	if (revents & (POLLIN | POLLHUP | POLLERR))
		lc_read(&conn, on_line, NULL);
	if (conn.dead || conn.eof)
		disconnect();
	else
		update_events();
}

static void try_connect(lv_timer_t *t)
{
	int fd = unix_connect(DISCSYS_SOCK, 1000);

	(void)t;
	if (fd < 0)
		return;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	lc_init(&conn, fd, 256 * 1024);
	loop_watch(fd, POLLIN, on_fd, NULL);
	app.sys.connected = true;
	lv_timer_pause(retry);
	sysc_send("hello ui");
}

static void disconnect(void)
{
	loop_unwatch(conn.fd);
	lc_close(&conn);
	conn.fd = -1;
	app.sys.connected = false;
	app.sys.bt_bat = -1;
	if (!app.sys.screen_on) {
		app.sys.screen_on = true;
		disp_set_active(true);
	}
	lv_timer_resume(retry);
}

void sysc_init(void)
{
	app.sys.screen_on = true;
	app.sys.bt_bat = -1;
	retry = lv_timer_create(try_connect, 2000, NULL);
	try_connect(NULL);
}
