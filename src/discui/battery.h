#pragma once

struct battery {
	int cap, mv, ma, temp, limit;
	char state[16];
};

int battery_get(struct battery *b);
int battery_set_limit(int limit, struct battery *b);
