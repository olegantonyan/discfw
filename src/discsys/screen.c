#include "discsys.h"

#include <disc/settings.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

#define BACKLIGHT "/sys/class/backlight/backlight/brightness"
#define BL_POWER "/sys/class/backlight/backlight/bl_power"
#define LCD_DEV "/dev/lcd_st77916"
#define LCD_SLEEP_IN _IO(0xef, 0x01)
#define LCD_SLEEP_OUT _IO(0xef, 0x02)
#define TOUCH_DEV "/dev/cst816t"
#define TOUCH_SLEEP _IO(0xef, 0x03)
#define TOUCH_WAKE _IO(0xef, 0x04)
#define PANEL_WAKE_MS 120
#define DRAWN_TIMEOUT_MS 400

static guint dim_src, off_src, wake_src, drawn_src;
static bool waiting_drawn;

static void write_int(const char *path, int v)
{
	FILE *f;

	if (sys.fake_hw)
		return;
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%d\n", v);
		fclose(f);
	}
}

static void dev_ioctl(const char *dev, unsigned long req)
{
	int fd;

	if (sys.fake_hw)
		return;
	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		ioctl(fd, req);
		close(fd);
	}
}

static void stop(guint *src)
{
	if (*src)
		g_source_remove(*src);
	*src = 0;
}

static void announce(void)
{
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "!screen");
	kv_str(&b, "state", sys.screen == SCR_ON ? "on" : sys.screen == SCR_DIM ? "dim" : "off");
	kv_int(&b, "locked", sys.locked);
	ev_kv(&b);
}

static void backlight(int level)
{
	if (level > 0) {
		write_int(BL_POWER, 0);
		write_int(BACKLIGHT, level);
	} else {
		write_int(BACKLIGHT, 0);
		write_int(BL_POWER, 4);
	}
}

static int dim_level(void)
{
	int b = set_get_int("ui.brightness") / 6;
	return b < 1 ? 1 : b;
}

static gboolean on_dim(gpointer data)
{
	(void)data;
	dim_src = 0;
	if (sys.screen != SCR_ON)
		return G_SOURCE_REMOVE;
	sys.screen = SCR_DIM;
	backlight(dim_level());
	announce();
	return G_SOURCE_REMOVE;
}

static gboolean on_off(gpointer data)
{
	(void)data;
	off_src = 0;
	screen_set(SCR_OFF, false);
	return G_SOURCE_REMOVE;
}

void screen_reschedule(void)
{
	int dim = power_profile_int("dim_s"), off = power_profile_int("off_s");

	stop(&dim_src);
	stop(&off_src);
	if (sys.screen == SCR_OFF || sys.shutting_down)
		return;
	if (dim > 0 && (off <= 0 || dim < off))
		dim_src = g_timeout_add_seconds(dim, on_dim, NULL);
	if (off > 0)
		off_src = g_timeout_add_seconds(off, on_off, NULL);
}

static void lights_on(void)
{
	stop(&drawn_src);
	waiting_drawn = false;
	if (sys.screen == SCR_ON)
		backlight(set_get_int("ui.brightness"));
}

static gboolean on_drawn_timeout(gpointer data)
{
	(void)data;
	drawn_src = 0;
	lights_on();
	return G_SOURCE_REMOVE;
}

void screen_ui_drawn(void)
{
	if (waiting_drawn)
		lights_on();
}

static gboolean on_panel_awake(gpointer data)
{
	(void)data;
	wake_src = 0;
	if (sys.screen != SCR_ON)
		return G_SOURCE_REMOVE;
	waiting_drawn = true;
	announce();
	drawn_src = g_timeout_add(server_ui_connected() ? DRAWN_TIMEOUT_MS : 1, on_drawn_timeout, NULL);
	return G_SOURCE_REMOVE;
}

void screen_set(enum screen_state s, bool lock)
{
	enum screen_state prev = sys.screen;

	if (s == SCR_OFF) {
		sys.locked = lock || set_get_bool("lock.enabled") || sys.locked;
		stop(&wake_src);
		stop(&drawn_src);
		waiting_drawn = false;
		if (prev != SCR_OFF) {
			sys.screen = SCR_OFF;
			backlight(0);
			dev_ioctl(TOUCH_DEV, TOUCH_SLEEP);
			dev_ioctl(LCD_DEV, LCD_SLEEP_IN);
		}
		screen_reschedule();
		announce();
		return;
	}
	sys.screen = SCR_ON;
	sys.last_activity = now_ms();
	screen_reschedule();
	if (prev == SCR_OFF) {
		dev_ioctl(LCD_DEV, LCD_SLEEP_OUT);
		dev_ioctl(TOUCH_DEV, TOUCH_WAKE);
		wake_src = g_timeout_add(PANEL_WAKE_MS, on_panel_awake, NULL);
		return;
	}
	if (!waiting_drawn)
		backlight(set_get_int("ui.brightness"));
	announce();
}

void screen_unlock(void)
{
	sys.locked = false;
	if (sys.screen == SCR_OFF)
		screen_set(SCR_ON, false);
	else
		announce();
}

void screen_toggle(void)
{
	screen_set(sys.screen == SCR_OFF ? SCR_ON : SCR_OFF, false);
}

void screen_activity(void)
{
	sys.last_activity = now_ms();
	if (sys.screen == SCR_OFF)
		return;
	if (sys.screen == SCR_DIM)
		screen_set(SCR_ON, false);
	else
		screen_reschedule();
}

void screen_brightness_changed(void)
{
	if (sys.screen == SCR_ON && !waiting_drawn)
		backlight(set_get_int("ui.brightness"));
}

static gboolean on_touch(gint fd, GIOCondition cond, gpointer data)
{
	struct input_event ev;
	bool any = false;

	(void)cond;
	(void)data;
	while (read(fd, &ev, sizeof(ev)) == sizeof(ev))
		any = true;
	if (any)
		screen_activity();
	return G_SOURCE_CONTINUE;
}

void screen_init(void)
{
	char path[64];
	int fd;

	sys.screen = SCR_ON;
	sys.last_activity = now_ms();
	dev_ioctl(LCD_DEV, LCD_SLEEP_OUT);
	dev_ioctl(TOUCH_DEV, TOUCH_WAKE);
	if (!sys.fake_hw && input_find("cst816t", path, sizeof(path)) == 0 &&
	    (fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) >= 0)
		g_unix_fd_add(fd, G_IO_IN, on_touch, NULL);
	backlight(set_get_int("ui.brightness"));
	screen_reschedule();
}
