#pragma once
#include <stdbool.h>
#include <stddef.h>

#include <disc/paths.h>

#define DISCD_SOCK disc_run_path("discd.sock")
#define DISCSYS_SOCK disc_run_path("discsys.sock")
#define DISCUI_SOCK disc_run_path("discui.sock")
#define MPD_SOCK disc_run_path("mpd.sock")

struct lconn {
	int fd;
	bool dead, eof;
	size_t in_len;
	char *out;
	size_t out_len, out_cap, out_max;
	void *user;
	char in[8192];
};

typedef void (*lc_line_cb)(struct lconn *c, char *line, void *arg);

void lc_init(struct lconn *c, int fd, size_t out_max);
int lc_read(struct lconn *c, lc_line_cb cb, void *arg);
bool lc_send(struct lconn *c, const char *s, size_t n);
bool lc_puts(struct lconn *c, const char *s);
bool lc_printf(struct lconn *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int lc_flush(struct lconn *c);
bool lc_pending(const struct lconn *c);
void lc_close(struct lconn *c);

int unix_listen(const char *path, int mode);
int unix_connect(const char *path, int timeout_ms);
int disc_request(const char *sock, const char *cmd, char *reply, size_t reply_len, int timeout_ms);
int discd_request(const char *cmd, char *reply, size_t reply_len, int timeout_ms);
