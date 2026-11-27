#include <disc/conn.h>

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int watch(void)
{
	char buf[4096];
	ssize_t n;
	int fd = unix_connect(DISCD_SOCK, 0);

	if (fd < 0 || send(fd, "watch\n", 6, MSG_NOSIGNAL) != 6) {
		perror("discd");
		return 1;
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		fwrite(buf, 1, n, stdout), fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	char cmd[128] = "", reply[512];

	if (argc < 2) {
		fprintf(stderr, "usage: discctl rate <hz> | mute | unmute | release | vol <0-255>|+|- | volpct <0-100> | off | status | battery | charge full|limit | reload | watch\n");
		return 2;
	}
	if (!strcmp(argv[1], "watch"))
		return watch();
	for (int i = 1; i < argc; i++) {
		strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 2);
		strncat(cmd, i + 1 < argc ? " " : "\n", sizeof(cmd) - strlen(cmd) - 1);
	}
	if (discd_request(cmd, reply, sizeof(reply), 3000) < 0) {
		perror("discd");
		return 1;
	}
	fputs(reply, stdout);
	return strncmp(reply, "ok", 2) != 0;
}
