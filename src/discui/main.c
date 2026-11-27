#include "app.h"
#include "mpdc.h"
#include "ui.h"

#include <disc/paths.h>
#include <disc/settings.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FONT_PATH "/usr/share/fonts/dejavu/DejaVuSans.ttf"
#define MAX_WATCH 32
#define DARK_TICK_MS 2000

struct app app;

static struct watch {
	int fd;
	short events;
	fd_cb cb;
	void *ctx;
} watches[MAX_WATCH];
static int n_watches;
static int wake_pipe[2] = { -1, -1 };

struct listener {
	lv_obj_t *owner;
	uint32_t mask;
	app_listener cb;
	void *ctx;
	bool dead;
};
static struct listener *listeners;
static int n_listeners, cap_listeners;
static int notify_depth;

int64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static uint32_t tick_cb(void)
{
	return (uint32_t)mono_ms();
}

unsigned mpd_elapsed_now(void)
{
	unsigned e = app.mpd.elapsed_ms;

	if (app.mpd.state == PS_PLAY)
		e += mono_ms() - app.mpd.elapsed_at;
	if (app.mpd.total_ms && e > app.mpd.total_ms)
		e = app.mpd.total_ms;
	return e;
}

void loop_watch(int fd, short events, fd_cb cb, void *ctx)
{
	for (int i = 0; i < n_watches; i++)
		if (watches[i].fd == fd) {
			watches[i] = (struct watch){ fd, events, cb, ctx };
			return;
		}
	if (n_watches < MAX_WATCH)
		watches[n_watches++] = (struct watch){ fd, events, cb, ctx };
}

void loop_events(int fd, short events)
{
	for (int i = 0; i < n_watches; i++)
		if (watches[i].fd == fd)
			watches[i].events = events;
}

void loop_unwatch(int fd)
{
	for (int i = 0; i < n_watches; i++)
		if (watches[i].fd == fd) {
			watches[i] = watches[--n_watches];
			return;
		}
}

void loop_wake(void)
{
	if (wake_pipe[1] >= 0 && write(wake_pipe[1], "x", 1) < 0 && errno != EAGAIN)
		perror("wake");
}

static void on_wake(int fd, short revents, void *ctx)
{
	char buf[64];

	(void)revents;
	(void)ctx;
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	cover_poll();
}

static void owner_deleted(lv_event_t *e)
{
	lv_obj_t *owner = lv_event_get_target_obj(e);

	for (int i = 0; i < n_listeners; i++)
		if (listeners[i].owner == owner)
			listeners[i].dead = true;
}

void app_listen(lv_obj_t *owner, uint32_t mask, app_listener cb, void *ctx)
{
	if (n_listeners == cap_listeners) {
		cap_listeners = cap_listeners ? cap_listeners * 2 : 32;
		listeners = realloc(listeners, cap_listeners * sizeof(*listeners));
	}
	listeners[n_listeners++] = (struct listener){ owner, mask, cb, ctx, false };
	lv_obj_add_event_cb(owner, owner_deleted, LV_EVENT_DELETE, NULL);
}

void app_notify(uint32_t evs)
{
	int n = n_listeners;

	notify_depth++;
	for (int i = 0; i < n; i++)
		if (!listeners[i].dead && (listeners[i].mask & evs))
			listeners[i].cb(listeners[i].mask & evs, listeners[i].ctx);
	if (--notify_depth)
		return;
	int j = 0;
	for (int i = 0; i < n_listeners; i++)
		if (!listeners[i].dead)
			listeners[j++] = listeners[i];
	n_listeners = j;
}

static void second_timer(lv_timer_t *t)
{
	(void)t;
	app_notify(EV_SECOND);
}

static void *font_data;
static size_t font_size;

static const lv_font_t *ttf(int px, const lv_font_t *fallback)
{
	lv_font_t *f;

	if (!font_data)
		return fallback;
	f = lv_tiny_ttf_create_data(font_data, font_size, px);
	if (!f)
		return fallback;
	f->fallback = fallback;
	return f;
}

void fonts_init(void)
{
	static const int sizes[3][4] = { { 15, 17, 20, 20 }, { 16, 19, 23, 22 }, { 18, 22, 26, 24 } };
	const char *path = getenv("DISCUI_FONT") ? getenv("DISCUI_FONT") : FONT_PATH;
	int sel = set_get_enum("ui.font");
	FILE *f;

	if (!font_data && (f = fopen(path, "rb"))) {
		fseek(f, 0, SEEK_END);
		font_size = ftell(f);
		rewind(f);
		font_data = malloc(font_size);
		if (!font_data || fread(font_data, 1, font_size, f) != font_size) {
			free(font_data);
			font_data = NULL;
		}
		fclose(f);
	}
	if (sel < 0 || sel > 2)
		sel = 1;
	app.font.s = ttf(sizes[sel][0], &lv_font_montserrat_16);
	app.font.m = ttf(sizes[sel][1], &lv_font_montserrat_16);
	app.font.l = ttf(sizes[sel][2], &lv_font_montserrat_20);
	app.font.title = ttf(sizes[sel][3], &lv_font_montserrat_20);
	app.font.clock = ttf(72, &lv_font_montserrat_40);
	app.font.icon = &lv_font_montserrat_28;
	app.font.icon_big = &lv_font_montserrat_40;
}

void theme_apply(lv_display_t *d)
{
	static const lv_palette_t accents[] = { LV_PALETTE_TEAL, LV_PALETTE_CYAN, LV_PALETTE_BLUE, LV_PALETTE_PURPLE, LV_PALETTE_PINK,
		LV_PALETTE_RED, LV_PALETTE_DEEP_ORANGE, LV_PALETTE_AMBER, LV_PALETTE_GREEN };
	int i = set_get_enum("ui.accent");

	app.accent = lv_palette_main(i >= 0 && i < (int)(sizeof(accents) / sizeof(accents[0])) ? accents[i] : LV_PALETTE_TEAL);
	lv_display_set_theme(d, lv_theme_default_init(d, app.accent, lv_palette_main(LV_PALETTE_AMBER), true, app.font.m));
	lv_obj_set_style_text_color(lv_layer_top(), lv_color_white(), 0);
	lv_obj_set_style_text_font(lv_layer_top(), app.font.m, 0);
}

static volatile sig_atomic_t terminate;

static void on_term(int sig)
{
	(void)sig;
	terminate = 1;
	loop_wake();
}

static bool safe_mode_check(void)
{
	const char *path = disc_run_path("discui.starts");
	int64_t now = mono_ms() / 1000, t1 = 0, t2 = 0;
	FILE *f = fopen(path, "r");
	bool safe;

	if (f) {
		if (fscanf(f, "%lld %lld", (long long *)&t1, (long long *)&t2) != 2)
			t1 = t2 = 0;
		fclose(f);
	}
	safe = t1 && now - t1 < 10;
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%lld %lld\n", (long long)now, (long long)t1);
		fclose(f);
	}
	(void)t2;
	return safe;
}

int main(int argc, char **argv)
{
	lv_display_t *d;
	struct pollfd pfd[MAX_WATCH];

	app.host = (argc > 1 && !strcmp(argv[1], "--host")) || getenv("DISCUI_HOST");
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);
	settings_load(SETTINGS_PATH);
	app.safe_mode = safe_mode_check();
	if (app.safe_mode)
		fprintf(stderr, "discui: crashed twice in 10 s, safe mode\n");
	lv_init();
	lv_tick_set_cb(tick_cb);
	d = disp_create();
	if (!d)
		return 1;
	fonts_init();
	theme_apply(d);
	if (pipe2(wake_pipe, O_NONBLOCK | O_CLOEXEC) == 0)
		loop_watch(wake_pipe[0], POLLIN, on_wake, NULL);
	input_init();
	input_set_back_handler(nav_back);
	state_load();
	cover_init();
	mpdc_init();
	sysc_init();
	dbg_init();
	nav_start();
	lv_timer_create(second_timer, 1000, NULL);
	for (;;) {
		uint32_t t = lv_timer_handler();
		int timeout = disp_active() ? (t > 1000 ? 1000 : (int)t) : DARK_TICK_MS;
		int n = n_watches;

		for (int i = 0; i < n; i++)
			pfd[i] = (struct pollfd){ .fd = watches[i].fd, .events = watches[i].events };
		int r = poll(pfd, n, timeout);
		if (terminate) {
			state_save();
			return 0;
		}
		if (r <= 0)
			continue;
		for (int i = 0; i < n; i++) {
			if (!pfd[i].revents)
				continue;
			for (int j = 0; j < n_watches; j++)
				if (watches[j].fd == pfd[i].fd) {
					watches[j].cb(pfd[i].fd, pfd[i].revents, watches[j].ctx);
					break;
				}
		}
	}
}
