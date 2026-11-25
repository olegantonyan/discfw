#include "proto.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	char cmd[128] = "", reply[256];

	if (argc < 2) {
		fprintf(stderr, "usage: discctl rate <hz> | mute | unmute | vol <n>|+|- | off | status\n");
		return 2;
	}
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
