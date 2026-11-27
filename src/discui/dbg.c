#include "app.h"
#include "ui.h"

#include <disc/conn.h>
#include <disc/kv.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 4
#define STEP_MS 16

struct client {
	struct lconn c;
	bool used;
};

static struct client clients[MAX_CLIENTS];

struct gesture {
	int x0, y0, x1, y1, steps, i;
	int cx, cy, r, a0, a1, hold;
	bool arc;
};

static void gesture_step(lv_timer_t *t)
{
	struct gesture *g = lv_timer_get_user_data(t);

	if (g->hold > 0) {
		g->hold--;
		input_inject(g->x0, g->y0, true);
		return;
	}
	if (g->i <= g->steps) {
		int x = g->x0 + (g->x1 - g->x0) * g->i / (g->steps ? g->steps : 1);
		int y = g->y0 + (g->y1 - g->y0) * g->i / (g->steps ? g->steps : 1);
		if (g->arc) {
			float a = (g->a0 + (float)(g->a1 - g->a0) * g->i / (g->steps ? g->steps : 1)) * 3.14159265f / 180;
			x = g->cx + (int)(g->r * cosf(a));
			y = g->cy + (int)(g->r * sinf(a));
			g->x1 = x;
			g->y1 = y;
		}
		input_inject(x, y, true);
		g->i++;
		return;
	}
	input_inject(g->x1, g->y1, false);
	lv_timer_delete(t);
	free(g);
}

static void start_gesture(int x0, int y0, int x1, int y1, int ms)
{
	struct gesture *g = calloc(1, sizeof(*g));

	*g = (struct gesture){ .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1, .steps = ms / STEP_MS };
	lv_timer_create(gesture_step, STEP_MS, g);
	sysc_send("screen activity");
}

static void reply_state(struct lconn *c)
{
	struct kvbuf b;
	char nav[256];

	kv_init(&b);
	kv_raw(&b, "ok");
	kv_str(&b, "nav", nav_describe(nav, sizeof(nav)));
	kv_int(&b, "modal", ui_modal_active());
	kv_int(&b, "shade", shade_is_open());
	kv_int(&b, "locked", app.sys.locked);
	kv_int(&b, "screen", app.sys.screen_on);
	kv_int(&b, "mpd", app.mpd.connected);
	kv_int(&b, "state", app.mpd.state);
	kv_int(&b, "pos", app.mpd.song_pos);
	kv_int(&b, "qlen", app.mpd.queue_len);
	kv_str(&b, "uri", app.mpd.cur.uri);
	kv_str(&b, "title", app.mpd.cur.title);
	kv_int(&b, "elapsed", mpd_elapsed_now());
	kv_int(&b, "vol", app.sys.volume);
	kv_int(&b, "cover", cover_current() != NULL);
	kv_end(&b);
	lc_puts(c, b.s);
	kv_free(&b);
}

static const char *kind(lv_obj_t *o)
{
	if (lv_obj_check_type(o, &lv_label_class))
		return "label";
	if (lv_obj_check_type(o, &lv_button_class))
		return "button";
	if (lv_obj_check_type(o, &lv_image_class))
		return "image";
	if (lv_obj_check_type(o, &lv_arc_class))
		return "arc";
	if (lv_obj_check_type(o, &lv_slider_class))
		return "slider";
	return "obj";
}

static void dump(struct lconn *c, lv_obj_t *o, int depth)
{
	lv_area_t a;
	char line[600];

	lv_obj_get_coords(o, &a);
	snprintf(line, sizeof(line), "= %*s%s %d,%d %dx%d%s%s%s\n", depth * 2, "", kind(o),
		 (int)a.x1, (int)a.y1, (int)lv_area_get_width(&a), (int)lv_area_get_height(&a), lv_obj_is_hidden(o) ? " hidden" : "",
		 lv_obj_check_type(o, &lv_label_class) ? " text=" : "", lv_obj_check_type(o, &lv_label_class) ? lv_label_get_text(o) : "");
	lc_puts(c, line);
	for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
		dump(c, lv_obj_get_child(o, i), depth + 1);
}

static void handle(struct lconn *c, char *line)
{
	char cmd[16], a[5][256];
	int v[5];

	kv_word(line, 0, cmd, sizeof(cmd));
	for (int i = 0; i < 5; i++) {
		kv_word(line, i + 1, a[i], sizeof(a[i]));
		v[i] = atoi(a[i]);
	}
	if (!strcmp(cmd, "state")) {
		reply_state(c);
		return;
	}
	if (!strcmp(cmd, "dump")) {
		dump(c, !strcmp(a[0], "top") ? lv_layer_top() : lv_screen_active(), 0);
		lc_puts(c, "ok\n");
		return;
	}
	if (!strcmp(cmd, "tap"))
		start_gesture(v[0], v[1], v[0], v[1], 48);
	else if (!strcmp(cmd, "hold"))
		start_gesture(v[0], v[1], v[0], v[1], a[2][0] ? v[2] : 800);
	else if (!strcmp(cmd, "swipe"))
		start_gesture(v[0], v[1], v[2], v[3], a[4][0] ? v[4] : 250);
	else if (!strcmp(cmd, "holddrag")) {
		struct gesture *g = calloc(1, sizeof(*g));
		*g = (struct gesture){ .x0 = v[0], .y0 = v[1], .x1 = v[2], .y1 = v[3], .hold = v[4] / STEP_MS, .steps = 30 };
		lv_timer_create(gesture_step, STEP_MS, g);
		sysc_send("screen activity");
	} else if (!strcmp(cmd, "rotate")) {
		struct gesture *g = calloc(1, sizeof(*g));
		float a = v[3] * 3.14159265f / 180;
		*g = (struct gesture){ .cx = v[0], .cy = v[1], .r = v[2], .a0 = v[3], .a1 = v[4], .steps = 40, .hold = 40, .arc = true };
		g->x0 = g->x1 = v[0] + (int)(v[2] * cosf(a));
		g->y0 = g->y1 = v[1] + (int)(v[2] * sinf(a));
		lv_timer_create(gesture_step, STEP_MS, g);
		sysc_send("screen activity");
	}
	else if (!strcmp(cmd, "wake"))
		sysc_send("screen on");
	else if (!strcmp(cmd, "back"))
		nav_back();
	else if (!strcmp(cmd, "home"))
		nav_home();
	else if (!strcmp(cmd, "goto")) {
		if (!nav_goto_name(a[0])) {
			lc_puts(c, "err unknown screen\n");
			return;
		}
	} else if (!strcmp(cmd, "folder"))
		folders_open_at(a[0], a[1][0] ? a[1] : NULL);
	else if (!strcmp(cmd, "page"))
		settings_open_page(a[0]);
	else if (!strcmp(cmd, "shot")) {
		lv_refr_now(NULL);
		if (disp_shot(a[0][0] ? a[0] : "/tmp/discui.png")) {
			lc_puts(c, "err shot failed\n");
			return;
		}
	} else if (!strcmp(cmd, "toast"))
		ui_toast(line + 6);
	else {
		lc_puts(c, "err unknown command\n");
		return;
	}
	lc_puts(c, "ok\n");
}

static void on_line(struct lconn *c, char *line, void *arg)
{
	(void)arg;
	if (line[0])
		handle(c, line);
}

static void on_client(int fd, short revents, void *ctx)
{
	struct client *cl = ctx;

	(void)fd;
	if (revents & (POLLIN | POLLHUP | POLLERR))
		lc_read(&cl->c, on_line, NULL);
	if (revents & POLLOUT)
		lc_flush(&cl->c);
	if (cl->c.dead || (cl->c.eof && !lc_pending(&cl->c))) {
		loop_unwatch(cl->c.fd);
		lc_close(&cl->c);
		cl->used = false;
		return;
	}
	loop_events(cl->c.fd, POLLIN | (lc_pending(&cl->c) ? POLLOUT : 0));
}

static void on_accept(int lfd, short revents, void *ctx)
{
	int fd;

	(void)revents;
	(void)ctx;
	while ((fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
		int i;
		for (i = 0; i < MAX_CLIENTS && clients[i].used; i++)
			;
		if (i == MAX_CLIENTS) {
			close(fd);
			continue;
		}
		clients[i].used = true;
		lc_init(&clients[i].c, fd, 65536);
		loop_watch(fd, POLLIN, on_client, &clients[i]);
	}
}

void dbg_init(void)
{
	int fd = unix_listen(DISCUI_SOCK, 0600);

	if (fd >= 0)
		loop_watch(fd, POLLIN, on_accept, NULL);
	else
		perror("discui debug socket");
}
