#pragma once
#include <stdbool.h>

enum np_state { NP_UNKNOWN, NP_STOP, NP_PLAY, NP_PAUSE };

struct np {
	enum np_state state;
	unsigned elapsed_s, total_s;
	char title[256];
	char artist[128];
	char audio[32];
};

int player_play_files(const char *dir, char **names, int n, int idx);
void player_toggle(void);
void player_next(void);
void player_prev(void);
void player_pause(void);
int player_poll(struct np *np);
int dac_cmd(const char *cmd, char *reply, int len);
