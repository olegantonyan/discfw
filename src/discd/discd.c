#include "cs43131_ioctl.h"
#include "jack.h"
#include "power.h"

#include <disc/conn.h>
#include <disc/kv.h>
#include <disc/settings.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define VOL_FILE "/run/discd.vol"
#define VOL_DEFAULT 0x50
#define VOL_STEP 4
#define VOL_MUTE 0xff
#define RELEASE_MS 500
#define POWER_TICK_MS 5000
#define JACK_TICK_MS 250
#define MAX_CLIENTS 16
#define CLIENT_IDLE_MS 5000
#define OUT_MAX 65536

static const unsigned rates[] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 };

static int fda = -1, fdc = -1;
static int rate_code = -1;
static int vol = VOL_DEFAULT, volpct = -1, balance;
static int muted = -1;
static int filter_applied = -1, dre_applied = -1;
static long long release_at, off_at;
static bool powered_down, spdif;

static struct client {
	struct lconn c;
	bool used, watch;
	long long last;
} clients[MAX_CLIENTS];

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int rate_to_code(unsigned hz)
{
	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
		if (rates[i] == hz)
			return i;
	return -1;
}

static int xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	if (ioctl(fd, req, arg) >= 0)
		return 0;
	syslog(LOG_ERR, "%s: %m", what);
	return -1;
}

static int set_u8(int fd, unsigned long req, int v, const char *what)
{
	unsigned char b = v;
	return xioctl(fd, req, &b, what);
}

static int dac_mute(int on)
{
	if (on == muted)
		return 0;
	int r = xioctl(fda, on ? CS43131_MUTE_ALL : CS43131_UNMUTE_ALL, NULL, on ? "mute" : "unmute");
	if (!r)
		muted = on;
	return r;
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static int dac_apply_volume(void)
{
	int left = clamp(vol + (balance > 0 ? balance : 0), 0, VOL_MUTE);
	int right = clamp(vol + (balance < 0 ? -balance : 0), 0, VOL_MUTE);

	if (powered_down)
		return 0;
	if (set_u8(fda, CS43131A_SET_VOL_A, left, "A vol a") || set_u8(fda, CS43131A_SET_VOL_B, left, "A vol b") ||
	    set_u8(fdc, CS43131C_SET_VOL_A, right, "C vol a") || set_u8(fdc, CS43131C_SET_VOL_B, right, "C vol b"))
		return -1;
	return 0;
}

static int dac_volume(int v)
{
	vol = clamp(v, 0, VOL_MUTE);
	if (dac_apply_volume())
		return -1;
	FILE *f = fopen(VOL_FILE, "w");
	if (f) {
		fprintf(f, "%d\n", vol);
		fclose(f);
	}
	return 0;
}

static int pct_to_reg(int pct)
{
	int range = set_get_int("audio.dac_range"), max = set_get_int("audio.vol_max");

	if (pct <= 0)
		return VOL_MUTE;
	pct = clamp(pct, 0, 100) * max / 100;
	return clamp((100 - pct) * range * 2 / 100, 0, VOL_MUTE - 1);
}

static void dac_filter_dre(void)
{
	int filter = set_get_enum("audio.filter"), dre = set_get_bool("audio.dre");

	if (powered_down)
		return;
	if (filter >= 0 && filter != filter_applied && (filter || filter_applied > 0)) {
		if (!set_u8(fda, CS43131A_SET_FILTER, filter, "A filter") && !set_u8(fdc, CS43131C_SET_FILTER, filter, "C filter"))
			filter_applied = filter;
	}
	if (dre != dre_applied && (dre || dre_applied > 0) && !set_u8(fda, CS43131_SET_DRE, dre, "dre"))
		dre_applied = dre;
}

static int dac_open(void)
{
	if (fda >= 0)
		close(fda);
	if (fdc >= 0)
		close(fdc);
	fda = open(CS43131_DEV_A, O_RDWR | O_CLOEXEC);
	fdc = open(CS43131_DEV_C, O_RDWR | O_CLOEXEC);
	if (fda < 0 || fdc < 0) {
		syslog(LOG_ERR, "open DAC: %m");
		return -1;
	}
	muted = -1;
	rate_code = -1;
	filter_applied = dre_applied = -1;
	return dac_mute(1);
}

static int dac_rate(unsigned hz)
{
	int code = rate_to_code(hz);
	if (code < 0)
		return -1;
	if (powered_down) {
		syslog(LOG_INFO, "waking DAC: full init");
		if (dac_open())
			return -1;
		powered_down = false;
		dac_apply_volume();
		dac_filter_dre();
	}
	if (code == rate_code)
		return 0;
	dac_mute(1);
	if (set_u8(fda, CS43131A_SET_SRATE, code, "A srate") || set_u8(fdc, CS43131C_SET_SRATE, code, "C srate"))
		return -1;
	rate_code = code;
	syslog(LOG_INFO, "rate %u (code %d)", hz, code);
	return 0;
}

static int dac_off(void)
{
	dac_mute(1);
	rate_code = -1;
	off_at = 0;
	if (xioctl(fda, CS43131_POWER_DOWN, NULL, "power down"))
		return -1;
	powered_down = true;
	return 0;
}

static void dac_status(struct kvbuf *b)
{
	kv_int(b, "rate", rate_code < 0 ? 0 : rates[rate_code]);
	kv_int(b, "vol", vol);
	kv_int(b, "volpct", volpct);
	kv_int(b, "muted", muted);
	kv_int(b, "balance", balance);
	kv_int(b, "off", powered_down);
	kv_int(b, "spdif", spdif);
	kv_int(b, "jack", jack_state());
}

static void apply_spdif(void)
{
	spdif = set_get_bool("audio.spdif");
	if (spdif && !powered_down && !dac_off())
		syslog(LOG_INFO, "S/PDIF output: DAC powered down");
}

#define PCM_PROC "/proc/asound/card0/pcm3p/sub0/"

static unsigned running_rate(void)
{
	char line[64];
	unsigned rate = 0;
	bool running = false;
	FILE *f = fopen(PCM_PROC "status", "r");

	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "state: RUNNING", 14))
			running = true;
	if (f)
		fclose(f);
	f = running ? fopen(PCM_PROC "hw_params", "r") : NULL;
	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "rate: %u", &rate) == 1)
			break;
	if (f)
		fclose(f);
	return running ? rate : 0;
}

static void resume_running_stream(void)
{
	unsigned rate = running_rate();

	if (!rate)
		return;
	syslog(LOG_INFO, "stream already running at %u Hz, restoring DAC", rate);
	if (dac_rate(rate) == 0)
		dac_mute(0);
}

static void broadcast(const char *line)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].used && clients[i].watch)
			lc_puts(&clients[i].c, line);
}

static void event_bat(void)
{
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "!bat");
	power_status(&b, false);
	kv_end(&b);
	broadcast(b.s);
	kv_free(&b);
}

static void event_dac(void)
{
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "!dac");
	dac_status(&b);
	kv_end(&b);
	broadcast(b.s);
	kv_free(&b);
}

static void event_jack(void)
{
	char line[32];

	syslog(LOG_INFO, "headphones %s", jack_state() ? "plugged in" : "unplugged");
	snprintf(line, sizeof(line), "!jack plugged=%d\n", jack_state());
	broadcast(line);
}

static void reload(void)
{
	settings_load(SETTINGS_PATH);
	power_reload();
	apply_spdif();
	balance = set_get_int("audio.balance");
	if (volpct >= 0)
		vol = pct_to_reg(volpct);
	dac_apply_volume();
	dac_filter_dre();
}

static void schedule_off(void)
{
	int idle = set_get_int("audio.dac_idle_off");
	off_at = idle > 0 ? now_ms() + RELEASE_MS + idle * 1000LL : 0;
}

static void handle(struct client *cl, char *line)
{
	char cmd[16], arg[32];
	struct kvbuf b;
	int r = -1;
	bool dac_changed = false;

	kv_word(line, 0, cmd, sizeof(cmd));
	kv_word(line, 1, arg, sizeof(arg));
	kv_init(&b);
	if (!strcmp(cmd, "battery")) {
		power_tick();
		kv_raw(&b, "ok");
		power_status(&b, true);
		goto reply;
	}
	if (!strcmp(cmd, "charge") && (!strcmp(arg, "full") || !strcmp(arg, "limit"))) {
		power_charge_full(arg[0] == 'f');
		power_tick();
		event_bat();
		kv_raw(&b, "ok");
		power_status(&b, false);
		goto reply;
	}
	if (!strcmp(cmd, "watch")) {
		cl->watch = true;
		kv_raw(&b, "ok");
		goto reply;
	}
	if (!strcmp(cmd, "reload")) {
		reload();
		r = 0;
	} else if (!strcmp(cmd, "release")) {
		release_at = muted ? 0 : now_ms() + RELEASE_MS;
		schedule_off();
		r = 0;
	} else if (!strcmp(cmd, "rate")) {
		release_at = off_at = 0;
		r = spdif ? -1 : dac_rate(strtoul(arg, NULL, 10));
		dac_changed = true;
	} else if (!strcmp(cmd, "mute")) {
		release_at = 0;
		r = dac_mute(1);
		dac_changed = true;
	} else if (!strcmp(cmd, "unmute")) {
		release_at = off_at = 0;
		r = spdif || rate_code < 0 ? -1 : dac_mute(0);
		dac_changed = true;
	} else if (!strcmp(cmd, "vol")) {
		volpct = -1;
		r = dac_volume(!strcmp(arg, "+") ? vol - VOL_STEP : !strcmp(arg, "-") ? vol + VOL_STEP : (int)strtol(arg, NULL, 0));
		dac_changed = true;
	} else if (!strcmp(cmd, "volpct")) {
		volpct = clamp(atoi(arg), 0, 100);
		r = dac_volume(pct_to_reg(volpct));
		dac_changed = true;
	} else if (!strcmp(cmd, "off")) {
		release_at = 0;
		r = dac_off();
		dac_changed = true;
	} else if (!strcmp(cmd, "status")) {
		r = 0;
	}
	if (r) {
		kv_raw(&b, "err");
		kv_raw(&b, cmd[0] ? cmd : "empty");
		goto reply;
	}
	kv_raw(&b, "ok");
	dac_status(&b);
reply:
	kv_end(&b);
	lc_puts(&cl->c, b.s);
	kv_free(&b);
	if (dac_changed)
		event_dac();
}

static void on_line(struct lconn *c, char *line, void *arg)
{
	(void)c;
	handle(arg, line);
}

static void drop(struct client *cl)
{
	lc_close(&cl->c);
	cl->used = false;
}

static void accept_client(int lfd)
{
	int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

	if (fd < 0)
		return;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].used)
			continue;
		clients[i].used = true;
		clients[i].watch = false;
		clients[i].last = now_ms();
		lc_init(&clients[i].c, fd, OUT_MAX);
		return;
	}
	syslog(LOG_WARNING, "too many clients");
	close(fd);
}

static void timers(void)
{
	long long now = now_ms();

	if (release_at && release_at <= now) {
		release_at = 0;
		dac_mute(1);
		event_dac();
	}
	if (off_at && off_at <= now) {
		off_at = 0;
		if (muted == 1) {
			syslog(LOG_INFO, "idle power-down");
			dac_off();
			event_dac();
		}
	}
}

static void serve(int lfd)
{
	struct pollfd pfd[MAX_CLIENTS + 1];
	long long tick_at = now_ms() + POWER_TICK_MS, jack_at = now_ms() + JACK_TICK_MS;

	for (;;) {
		struct client *map[MAX_CLIENTS + 1];
		long long now = now_ms(), wait = tick_at - now;
		int n = 1;

		if (jack_state() >= 0 && jack_at - now < wait)
			wait = jack_at - now;
		if (release_at && release_at - now < wait)
			wait = release_at - now;
		if (off_at && off_at - now < wait)
			wait = off_at - now;
		pfd[0] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		for (int i = 0; i < MAX_CLIENTS; i++) {
			struct client *cl = &clients[i];
			if (!cl->used)
				continue;
			if (!cl->watch && now - cl->last > CLIENT_IDLE_MS) {
				drop(cl);
				continue;
			}
			pfd[n] = (struct pollfd){ .fd = cl->c.fd, .events = (cl->c.eof ? 0 : POLLIN) | (lc_pending(&cl->c) ? POLLOUT : 0) };
			map[n++] = cl;
		}
		if (poll(pfd, n, wait < 0 ? 0 : wait > CLIENT_IDLE_MS ? CLIENT_IDLE_MS : (int)wait) < 0 && errno != EINTR)
			syslog(LOG_ERR, "poll: %m");
		for (int i = 1; i < n; i++) {
			struct client *cl = map[i];
			if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) {
				cl->last = now_ms();
				lc_read(&cl->c, on_line, cl);
			}
			if (pfd[i].revents & POLLOUT)
				lc_flush(&cl->c);
			if (cl->c.dead || (cl->c.eof && !lc_pending(&cl->c)))
				drop(cl);
		}
		if (pfd[0].revents & POLLIN)
			accept_client(lfd);
		timers();
		if (now_ms() >= jack_at) {
			if (jack_tick())
				event_jack();
			jack_at = now_ms() + JACK_TICK_MS;
		}
		if (now_ms() >= tick_at) {
			if (power_tick())
				event_bat();
			tick_at = now_ms() + POWER_TICK_MS;
		}
	}
}

int main(void)
{
	FILE *f;
	int lfd;

	openlog("discd", LOG_PERROR, LOG_DAEMON);
	signal(SIGPIPE, SIG_IGN);
	if (dac_open()) {
		sleep(5);
		return 1;
	}
	settings_load(SETTINGS_PATH);
	balance = set_get_int("audio.balance");
	f = fopen(VOL_FILE, "r");
	if (f) {
		if (fscanf(f, "%d", &vol) != 1)
			vol = VOL_DEFAULT;
		fclose(f);
	}
	dac_volume(vol);
	dac_filter_dre();
	resume_running_stream();
	apply_spdif();
	power_init();
	jack_init();
	lfd = unix_listen(DISCD_SOCK, 0666);
	if (lfd < 0) {
		syslog(LOG_ERR, "socket: %m");
		return 1;
	}
	syslog(LOG_INFO, "ready, vol %d", vol);
	serve(lfd);
	return 0;
}
