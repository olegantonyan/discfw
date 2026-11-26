#include <disc/conn.h>
#include <disc/settings.h>

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void usage(void)
{
	fputs("usage: discfw get KEY | set KEY VALUE | reset KEY | list [PREFIX] | state | watch | cfg KEY\n"
	      "              screen on|off|lock|unlock|toggle | key KEY [short|double|long] | action NAME\n"
	      "              vol PCT|+N|-N | sleep MIN | toast TEXT | poweroff | reboot | library reset\n"
	      "       discfw ui state | dump [top] | tap X Y | hold X Y [MS] | swipe X1 Y1 X2 Y2 [MS] | back | home |\n"
	      "              goto SCREEN | folder PATH | shot PATH\n"
	      "  cfg reads the settings file directly (no daemon needed); ui talks to discui's debug socket\n",
	      stderr);
}

int main(int argc, char **argv)
{
	char line[1024] = "", buf[8192];
	const char *sock = DISCSYS_SOCK;
	size_t len = 0;
	ssize_t n;
	int fd, watch, first = 1;

	if (argc < 2) {
		usage();
		return 2;
	}
	if (!strcmp(argv[1], "cfg")) {
		const char *v;
		if (argc != 3 || settings_load(SETTINGS_PATH) < 0 || !(v = set_get(argv[2])))
			return 1;
		puts(v);
		return 0;
	}
	if (!strcmp(argv[1], "ui")) {
		sock = DISCUI_SOCK;
		first = 2;
		if (argc < 3) {
			usage();
			return 2;
		}
	}
	watch = !strcmp(argv[1], "watch");
	if (watch)
		snprintf(line, sizeof(line), "hello\n");
	else
		for (int i = first; i < argc; i++)
			snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s", argv[i], i + 1 < argc ? " " : "\n");
	fd = unix_connect(sock, watch ? 0 : 5000);
	if (fd < 0 || send(fd, line, strlen(line), MSG_NOSIGNAL) < 0) {
		perror("discsys");
		return 1;
	}
	while ((n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
		len += n;
		buf[len] = 0;
		char *start = buf, *nl;
		while ((nl = strchr(start, '\n'))) {
			*nl = 0;
			if (watch || start[0] != '!')
				puts(start);
			fflush(stdout);
			if (!watch && (!strncmp(start, "ok", 2) || !strncmp(start, "err", 3)))
				return start[0] == 'e';
			start = nl + 1;
		}
		len = strlen(start);
		memmove(buf, start, len + 1);
	}
	return watch ? 0 : 1;
}
