#include "discsys.h"

#include <disc/settings.h>
#include <mpd/client.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define CHECK_S 30
#define LOWBAT_COUNTDOWN_S 30
#define RELEASE_WAIT_MS 10000

static gint64 idle_since, sleep_deadline;
static int lowbat_hits, lowbat_left;
static bool warned;
static guint lowbat_src, sleep_src, release_src;
static bool pending_reboot;

const char *power_profile(void)
{
	return set_get(sys.powered ? "power.charger" : "power.battery");
}

int power_profile_int(const char *field)
{
	char key[64];

	snprintf(key, sizeof(key), "power.%s.%s", power_profile(), field);
	return set_get_int(key);
}

void power_status(struct kvbuf *b)
{
	kv_int(b, "powered", sys.powered);
	kv_str(b, "profile", power_profile());
	if (sys.bat.valid) {
		kv_int(b, "cap", sys.bat.cap);
		kv_str(b, "batstate", sys.bat.state);
	}
}

static void announce(void)
{
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "!power");
	power_status(&b);
	if (lowbat_src)
		kv_int(&b, "lowbat", lowbat_left);
	ev_kv(&b);
}

static gboolean lowbat_tick(gpointer data)
{
	(void)data;
	if (sys.powered) {
		lowbat_src = 0;
		lowbat_hits = 0;
		announce();
		return G_SOURCE_REMOVE;
	}
	if (--lowbat_left <= 0) {
		lowbat_src = 0;
		logmsg(LOG_WARNING, "battery empty (%d%%, %d mV), powering off", sys.bat.cap, sys.bat.mv);
		power_off(false);
		return G_SOURCE_REMOVE;
	}
	announce();
	return G_SOURCE_CONTINUE;
}

void power_on_bat(void)
{
	bool powered = sys.bat.valid && strcmp(sys.bat.state, "battery");
	char msg[64];

	if (!sys.bat.valid)
		return;
	if (powered != sys.powered) {
		sys.powered = powered;
		warned = false;
		screen_reschedule();
		announce();
	}
	if (powered)
		return;
	if (!warned && sys.bat.cap <= set_get_int("power.lowbat_warn")) {
		warned = true;
		snprintf(msg, sizeof(msg), "Battery low: %d%%", sys.bat.cap);
		ev_toast(msg);
	}
	if (sys.bat.cap <= set_get_int("power.lowbat_off") || (sys.bat.mv > 0 && sys.bat.mv < set_get_int("power.lowbat_mv")))
		lowbat_hits++;
	else
		lowbat_hits = 0;
	if (lowbat_hits >= 2 && !lowbat_src) {
		lowbat_left = LOWBAT_COUNTDOWN_S;
		lowbat_src = g_timeout_add_seconds(1, lowbat_tick, NULL);
		screen_set(SCR_ON, false);
		announce();
	}
}

void power_on_play_state(void)
{
	if (sys.play == PS_PLAY)
		idle_since = 0;
	else if (!idle_since)
		idle_since = now_ms();
}

static gboolean idle_check(gpointer data)
{
	int mins = power_profile_int("idle_off_min");
	gint64 since = idle_since > sys.last_activity ? idle_since : sys.last_activity;

	(void)data;
	if (mins <= 0 || sys.play == PS_PLAY || sys.screen != SCR_OFF || sys.shutting_down || !idle_since)
		return G_SOURCE_CONTINUE;
	if (sys.mode != MODE_PLAYER && mode_usb())
		return G_SOURCE_CONTINUE;
	if (now_ms() - since >= mins * 60000LL) {
		logmsg(LOG_INFO, "idle for %d min, powering off", mins);
		power_off(false);
	}
	return G_SOURCE_CONTINUE;
}

static void announce_sleep(void)
{
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "!sleep");
	kv_int(&b, "left", sys.sleep_left_s);
	ev_kv(&b);
}

static gboolean sleep_tick(gpointer data)
{
	struct mpd_connection *c;

	(void)data;
	sys.sleep_left_s = (int)((sleep_deadline - now_ms()) / 1000);
	if (sys.sleep_left_s > 0) {
		if (sys.sleep_left_s % 60 == 0)
			announce_sleep();
		return G_SOURCE_CONTINUE;
	}
	sleep_src = 0;
	sys.sleep_left_s = 0;
	announce_sleep();
	logmsg(LOG_INFO, "sleep timer expired");
	if (!strcmp(set_get("play.sleep_action"), "poweroff")) {
		power_off(false);
	} else if ((c = mpdc())) {
		mpd_run_pause(c, true);
		mpdc_report("pause");
	}
	return G_SOURCE_REMOVE;
}

void power_sleep_timer(int minutes)
{
	char msg[48];

	if (sleep_src)
		g_source_remove(sleep_src);
	sleep_src = 0;
	sys.sleep_left_s = minutes > 0 ? minutes * 60 : 0;
	if (minutes > 0) {
		sleep_deadline = now_ms() + minutes * 60000LL;
		sleep_src = g_timeout_add_seconds(1, sleep_tick, NULL);
		snprintf(msg, sizeof(msg), "Sleep in %d min", minutes);
	} else {
		snprintf(msg, sizeof(msg), "Sleep timer off");
	}
	ev_toast(msg);
	announce_sleep();
}

static void finish(void)
{
	char *argv[] = { pending_reboot ? "/sbin/reboot" : "/sbin/poweroff", NULL };
	GError *e = NULL;

	if (sys.fake_hw) {
		g_main_loop_quit(sys.loop);
		return;
	}
	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &e)) {
		logmsg(LOG_ERR, "%s: %s", argv[0], e->message);
		g_error_free(e);
	}
}

static gboolean wait_release(gpointer data)
{
	static int waited;

	(void)data;
	waited += 100;
	if (keys_power_held() && waited < RELEASE_WAIT_MS)
		return G_SOURCE_CONTINUE;
	release_src = 0;
	finish();
	return G_SOURCE_REMOVE;
}

void power_off(bool reboot)
{
	struct mpd_connection *c;
	struct kvbuf b;

	if (sys.shutting_down)
		return;
	sys.shutting_down = true;
	pending_reboot = reboot;
	logmsg(LOG_INFO, "%s requested", reboot ? "reboot" : "power-off");
	kv_init(&b);
	kv_raw(&b, "!shutdown");
	kv_int(&b, "reboot", reboot);
	ev_kv(&b);
	if ((c = mpdc()) && sys.play == PS_PLAY)
		mpd_run_pause(c, true);
	cfg_flush();
	sync();
	if (keys_power_held())
		release_src = g_timeout_add(100, wait_release, NULL);
	else
		finish();
}

void power_init(void)
{
	idle_since = now_ms();
	g_timeout_add_seconds(CHECK_S, idle_check, NULL);
}
