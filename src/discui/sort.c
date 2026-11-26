#include "mpdc.h"

#include <disc/settings.h>
#include <disc/text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_KEYS 12

enum field { F_ARTIST, F_ALBUMARTIST, F_ALBUM, F_TITLE, F_YEAR, F_FILENAME, F_TRACK, F_DISC, F_DIR };

static const char *const names[] = { "ARTIST", "ALBUMARTIST", "ALBUM", "TITLE", "YEAR", "FILENAME", "TRACKNUMBER",
				     "DISCNUMBER", "DIRECTORY" };

struct key {
	enum field f;
	int order;
};

struct entry {
	const struct item *it;
	int idx;
};

static struct key keys[MAX_KEYS];
static int n_keys;

static int parse(const char *criteria)
{
	char buf[512], *save = NULL, *tok;

	const char *eq = criteria ? strchr(criteria, '=') : NULL;

	n_keys = 0;
	snprintf(buf, sizeof(buf), "%s", eq ? eq + 1 : criteria ? criteria : "");
	for (tok = strtok_r(buf, "/", &save); tok && n_keys < MAX_KEYS; tok = strtok_r(NULL, "/", &save)) {
		int order = 1;
		while (*tok == ' ')
			tok++;
		if (*tok == '-') {
			order = -1;
			tok++;
		}
		size_t len = strlen(tok);
		while (len && tok[len - 1] == ' ')
			tok[--len] = 0;
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if (!strcasecmp(tok, names[i]))
				keys[n_keys++] = (struct key){ i, order };
	}
	return n_keys;
}

static const char *base(const char *uri)
{
	const char *s = strrchr(uri, '/');
	return s ? s + 1 : uri;
}

static int dir_cmp(const char *a, const char *b)
{
	const char *ea = strrchr(a, '/'), *eb = strrchr(b, '/');
	size_t la = ea ? (size_t)(ea - a) : 0, lb = eb ? (size_t)(eb - b) : 0;

	while (la || lb) {
		const char *sa = memchr(a, '/', la), *sb = memchr(b, '/', lb);
		size_t pa = sa ? (size_t)(sa - a) : la, pb = sb ? (size_t)(sb - b) : lb;
		char x[512], y[512];
		snprintf(x, sizeof(x), "%.*s", (int)pa, a);
		snprintf(y, sizeof(y), "%.*s", (int)pb, b);
		int c = nat_cmp(x, y);
		if (c)
			return c;
		a += pa + (pa < la);
		b += pb + (pb < lb);
		la -= pa + (pa < la);
		lb -= pb + (pb < lb);
	}
	return 0;
}

static int num(int a, int b)
{
	return a < b ? -1 : a > b;
}

static int str(const char *a, const char *b)
{
	return nat_cmp(a ? a : "", b ? b : "");
}

static int field_cmp(const struct item *a, const struct item *b, enum field f)
{
	switch (f) {
	case F_ARTIST:
		return str(a->artist, b->artist);
	case F_ALBUMARTIST:
		return str(a->albumartist, b->albumartist);
	case F_ALBUM:
		return str(a->album, b->album);
	case F_TITLE:
		return str(a->title, b->title);
	case F_YEAR:
		return num(a->year, b->year);
	case F_FILENAME:
		return nat_cmp(base(a->uri), base(b->uri));
	case F_TRACK:
		return num(a->track, b->track);
	case F_DISC:
		return num(a->disc, b->disc);
	case F_DIR:
		return dir_cmp(a->uri, b->uri);
	}
	return 0;
}

static int cmp(const void *pa, const void *pb)
{
	const struct entry *a = pa, *b = pb;

	for (int i = 0; i < n_keys; i++) {
		int c = field_cmp(a->it, b->it, keys[i].f);
		if (c)
			return c * keys[i].order;
	}
	return num(a->idx, b->idx);
}

int sort_presets(char ***labels, char ***criteria)
{
	char buf[1024], *save = NULL, *tok;
	int n = 0;

	*labels = *criteria = NULL;
	snprintf(buf, sizeof(buf), "%s", set_get("pl.sort_presets"));
	for (tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
		char *eq;
		while (*tok == ' ')
			tok++;
		if (!*tok)
			continue;
		*labels = realloc(*labels, (n + 1) * sizeof(char *));
		*criteria = realloc(*criteria, (n + 1) * sizeof(char *));
		(*criteria)[n] = strdup(tok);
		eq = strchr(tok, '=');
		if (eq)
			*eq = 0;
		(*labels)[n] = strdup(tok);
		n++;
	}
	return n;
}

void sort_presets_free(char **labels, char **criteria, int n)
{
	for (int i = 0; i < n; i++) {
		free(labels[i]);
		free(criteria[i]);
	}
	free(labels);
	free(criteria);
}

void items_sort(struct items *it, const char *criteria)
{
	struct entry *e;
	struct item *sorted;

	if (it->n < 2 || !parse(criteria))
		return;
	e = malloc(it->n * sizeof(*e));
	sorted = malloc(it->n * sizeof(*sorted));
	for (int i = 0; i < it->n; i++)
		e[i] = (struct entry){ &it->v[i], i };
	qsort(e, it->n, sizeof(*e), cmp);
	for (int i = 0; i < it->n; i++)
		sorted[i] = *e[i].it;
	memcpy(it->v, sorted, it->n * sizeof(*sorted));
	free(sorted);
	free(e);
}
