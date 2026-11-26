#include "battery.h"

#include <discd/proto.h>
#include <stdio.h>

static int request(const char *cmd, struct battery *b)
{
	char reply[160];

	if (discd_request(cmd, reply, sizeof(reply), 3000))
		return -1;
	if (sscanf(reply, "ok cap=%d mv=%d ma=%d temp=%d limit=%d state=%15s", &b->cap, &b->mv, &b->ma, &b->temp, &b->limit, b->state) != 6)
		return -1;
	return b->cap < 0 ? -1 : 0;
}

int battery_get(struct battery *b)
{
	return request("battery\n", b);
}

int battery_set_limit(int limit, struct battery *b)
{
	char cmd[24];

	snprintf(cmd, sizeof(cmd), "limit %d\n", limit);
	return request(cmd, b);
}
