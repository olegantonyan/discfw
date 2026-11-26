#include "groups.h"

#include <disc/settings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum mode { G_NONE, G_ALBUM, G_FOLDER, G_BOTH };

static void dir_of(const char *uri, char *out, size_t len)
{
	const char *s = strrchr(uri, '/');
	snprintf(out, len, "%.*s", s ? (int)(s - uri) : 0, uri);
}

static const char *short_dir(const char *dir)
{
	const char *s = strrchr(dir, '/');
	return s ? s + 1 : dir;
}

static void key_of(const struct item *it, enum mode m, char *out, size_t len)
{
	char dir[1024];

	dir_of(it->uri, dir, sizeof(dir));
	if (m == G_FOLDER || ((m == G_ALBUM) && !it->album[0]))
		snprintf(out, len, "d:%s", dir);
	else if (m == G_ALBUM)
		snprintf(out, len, "a:%s\x1f%s", it->album, it->albumartist[0] ? it->albumartist : it->artist);
	else
		snprintf(out, len, "b:%s\x1f%s", it->album, dir);
}

static void add_head(struct groups *g, const struct item *it, enum mode m)
{
	char dir[1024], head[512], sub[512];

	dir_of(it->uri, dir, sizeof(dir));
	if (m == G_FOLDER || !it->album[0]) {
		snprintf(head, sizeof(head), "%.500s", dir[0] ? short_dir(dir) : "/");
		snprintf(sub, sizeof(sub), "%s", it->album[0] ? it->album : "");
	} else {
		const char *artist = it->albumartist[0] ? it->albumartist : it->artist;
		snprintf(head, sizeof(head), "%s", it->album);
		if (m == G_BOTH)
			snprintf(sub, sizeof(sub), "%.500s", short_dir(dir));
		else if (it->year)
			snprintf(sub, sizeof(sub), "%s%s%d", artist, artist[0] ? " · " : "", it->year);
		else
			snprintf(sub, sizeof(sub), "%s", artist);
	}
	g->head = realloc(g->head, (g->n_heads + 1) * sizeof(char *));
	g->head_sub = realloc(g->head_sub, (g->n_heads + 1) * sizeof(char *));
	g->head[g->n_heads] = strdup(head);
	g->head_sub[g->n_heads] = strdup(sub);
	g->map[g->n++] = -(++g->n_heads);
}

void groups_build(struct groups *g, const struct items *it)
{
	enum mode m = set_get_enum("pl.group");
	char key[2200], prev[2200] = "";

	groups_free(g);
	if (m < 0)
		m = G_NONE;
	g->map = malloc((it->n * 2 + 1) * sizeof(int));
	for (int i = 0; i < it->n; i++) {
		if (m != G_NONE) {
			key_of(&it->v[i], m, key, sizeof(key));
			if (!i || strcmp(key, prev)) {
				add_head(g, &it->v[i], m);
				snprintf(prev, sizeof(prev), "%s", key);
			}
		}
		g->map[g->n++] = i;
	}
}

void groups_free(struct groups *g)
{
	for (int i = 0; i < g->n_heads; i++) {
		free(g->head[i]);
		free(g->head_sub[i]);
	}
	free(g->head);
	free(g->head_sub);
	free(g->map);
	memset(g, 0, sizeof(*g));
}

int groups_row_of(const struct groups *g, int track)
{
	for (int i = 0; i < g->n; i++)
		if (g->map[i] == track)
			return i;
	return -1;
}

int groups_track_at(const struct groups *g, int row)
{
	if (row < 0 || row >= g->n)
		return -1;
	if (g->map[row] >= 0)
		return g->map[row];
	return row + 1 < g->n ? g->map[row + 1] : -1;
}
