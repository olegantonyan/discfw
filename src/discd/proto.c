#include "proto.h"

#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

int discd_request(const char *cmd, char *reply, size_t reply_len, int timeout_ms)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
	size_t len = strlen(cmd), off = 0;
	ssize_t n;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	strncpy(addr.sun_path, DISCD_SOCK, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		goto fail;
	while (off < len) {
		n = send(fd, cmd + off, len - off, MSG_NOSIGNAL);
		if (n <= 0)
			goto fail;
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
fail:
	close(fd);
	return -1;
}
