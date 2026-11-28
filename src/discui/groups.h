#pragma once
#include "pager.h"

struct groups {
	int n, n_heads;
	int *map;
	char **head, **head_sub;
};

void groups_build(struct groups *g, struct pager *p);
void groups_free(struct groups *g);
int groups_row_of(const struct groups *g, int track);
int groups_track_at(const struct groups *g, int row);
