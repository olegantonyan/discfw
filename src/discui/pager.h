#pragma once
#include "mpdc.h"

struct pager {
	char *playlist;
	int n, n_pages;
	struct items *pages;
	unsigned char *state;
};

void pager_open(struct pager *p, const char *playlist);
const struct item *pager_at(struct pager *p, int i);
void pager_free(struct pager *p);
