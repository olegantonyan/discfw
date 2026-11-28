#include "pager.h"

#include <stdlib.h>
#include <string.h>

#define PAGE 100

enum { P_EMPTY, P_LOADED, P_FAILED };

void pager_free(struct pager *p)
{
	for (int i = 0; i < p->n_pages; i++)
		items_free(&p->pages[i]);
	free(p->pages);
	free(p->state);
	free(p->playlist);
	memset(p, 0, sizeof(*p));
}

void pager_open(struct pager *p, const char *playlist)
{
	char *name = playlist ? strdup(playlist) : NULL;

	pager_free(p);
	p->playlist = name;
	p->n = mpdc_list_len(name);
	if (p->n < 0)
		p->n = 0;
	p->n_pages = (p->n + PAGE - 1) / PAGE;
	p->pages = calloc(p->n_pages + 1, sizeof(*p->pages));
	p->state = calloc(p->n_pages + 1, 1);
}

const struct item *pager_at(struct pager *p, int i)
{
	int k = i / PAGE, start = k * PAGE;

	if (i < 0 || i >= p->n)
		return NULL;
	if (p->state[k] == P_EMPTY) {
		int end = start + PAGE < p->n ? start + PAGE : p->n;
		p->state[k] = mpdc_list_range(p->playlist, start, end, &p->pages[k]) < 0 ? P_FAILED : P_LOADED;
	}
	return i - start < p->pages[k].n ? &p->pages[k].v[i - start] : NULL;
}
