#include <disc/conn.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

void lc_init(struct lconn *c, int fd, size_t out_max)
{
	memset(c, 0, offsetof(struct lconn, in));
	c->fd = fd;
	c->out_max = out_max;
}

int lc_read(struct lconn *c, lc_line_cb cb, void *arg)
{
	for (;;) {
		if (c->in_len >= sizeof(c->in) - 1) {
			c->dead = true;
			return -1;
		}
		ssize_t n = read(c->fd, c->in + c->in_len, sizeof(c->in) - 1 - c->in_len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			c->dead = true;
			return -1;
		}
		if (n == 0) {
			c->eof = true;
			return -1;
		}
		c->in_len += n;
		size_t start = 0;
		for (size_t i = c->in_len - n; i < c->in_len; i++) {
			if (c->in[i] != '\n')
				continue;
			c->in[i] = 0;
			if (i > start && c->in[i - 1] == '\r')
				c->in[i - 1] = 0;
			cb(c, c->in + start, arg);
			if (c->dead)
				return -1;
			start = i + 1;
		}
		memmove(c->in, c->in + start, c->in_len - start);
		c->in_len -= start;
	}
}

int lc_flush(struct lconn *c)
{
	while (c->out_len) {
		ssize_t n = send(c->fd, c->out, c->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 1;
			c->dead = true;
			return -1;
		}
		memmove(c->out, c->out + n, c->out_len - n);
		c->out_len -= n;
	}
	return 0;
}

bool lc_send(struct lconn *c, const char *s, size_t n)
{
	if (c->dead)
		return false;
	if (c->out_len + n > c->out_max) {
		c->dead = true;
		return false;
	}
	if (c->out_len + n > c->out_cap) {
		size_t cap = c->out_cap ? c->out_cap : 1024;
		while (cap < c->out_len + n)
			cap *= 2;
		char *p = realloc(c->out, cap);
		if (!p) {
			c->dead = true;
			return false;
		}
		c->out = p;
		c->out_cap = cap;
	}
	memcpy(c->out + c->out_len, s, n);
	c->out_len += n;
	return lc_flush(c) >= 0;
}

bool lc_puts(struct lconn *c, const char *s)
{
	return lc_send(c, s, strlen(s));
}

bool lc_printf(struct lconn *c, const char *fmt, ...)
{
	char buf[1024], *p = buf;
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return false;
	if ((size_t)n >= sizeof(buf)) {
		p = malloc(n + 1);
		if (!p)
			return false;
		va_start(ap, fmt);
		vsnprintf(p, n + 1, fmt, ap);
		va_end(ap);
	}
	bool ok = lc_send(c, p, n);
	if (p != buf)
		free(p);
	return ok;
}

bool lc_pending(const struct lconn *c)
{
	return c->out_len > 0;
}

void lc_close(struct lconn *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	free(c->out);
	c->out = NULL;
	c->out_len = c->out_cap = 0;
	c->dead = true;
}

int unix_listen(const char *path, int mode)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
		close(fd);
		return -1;
	}
	chmod(path, mode);
	return fd;
}

int unix_connect(const char *path, int timeout_ms)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int disc_request(const char *sock, const char *cmd, char *reply, size_t reply_len, int timeout_ms)
{
	size_t len = strlen(cmd), off = 0;
	ssize_t n;
	int fd = unix_connect(sock, timeout_ms);

	if (fd < 0)
		return -1;
	while (off < len) {
		n = send(fd, cmd + off, len - off, MSG_NOSIGNAL);
		if (n <= 0) {
			close(fd);
			return -1;
		}
		off += n;
	}
	shutdown(fd, SHUT_WR);
	off = 0;
	while (reply && off + 1 < reply_len) {
		n = read(fd, reply + off, reply_len - 1 - off);
		if (n <= 0)
			break;
		off += n;
	}
	if (reply && reply_len)
		reply[off] = 0;
	close(fd);
	return 0;
}

int discd_request(const char *cmd, char *reply, size_t reply_len, int timeout_ms)
{
	return disc_request(DISCD_SOCK, cmd, reply, reply_len, timeout_ms);
}
