#include <disc/mpddb.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

enum { F_TITLE, F_ARTIST, F_ALBUM, F_ALBUMARTIST, F_DATE, F_TRACK, F_DISC, F_FORMAT, N_FIELDS };

static const char *const names[N_FIELDS] = { "Title", "Artist", "Album", "AlbumArtist", "Date", "Track", "Disc", "Format" };

struct song {
	char *uri, *f[N_FIELDS];
	unsigned dur_ms;
	long added;
	bool hidden;
};

static const char *after(const char *line, const char *prefix)
{
	size_t n = strlen(prefix);
	return strncmp(line, prefix, n) ? NULL : line + n;
}

static void song_clear(struct song *s)
{
	free(s->uri);
	for (int i = 0; i < N_FIELDS; i++)
		free(s->f[i]);
	memset(s, 0, sizeof(*s));
}

static void song_line(struct song *s, char *line)
{
	char *colon = strchr(line, ':'), *v;

	if (!colon)
		return;
	*colon = 0;
	for (v = colon + 1; *v == ' '; v++)
		;
	if (!strcmp(line, "Time")) {
		s->dur_ms = (unsigned)(strtod(v, NULL) * 1000 + 0.5);
	} else if (!strcmp(line, "added")) {
		s->added = strtol(v, NULL, 10);
	} else if (!strcmp(line, "InPlaylist")) {
		s->hidden = !strcmp(v, "yes");
	} else {
		for (int i = 0; i < N_FIELDS; i++)
			if (!s->f[i] && !strcmp(line, names[i]))
				s->f[i] = strdup(v);
	}
}

static void song_emit(struct song *s, mpddb_cb cb, void *ctx)
{
	struct mpddb_song o = { s->uri,	       s->f[F_TITLE], s->f[F_ARTIST], s->f[F_ALBUM],  s->f[F_ALBUMARTIST],
				s->f[F_DATE], s->f[F_TRACK], s->f[F_DISC],   s->f[F_FORMAT], s->dur_ms,
				s->added };

	cb(ctx, &o);
}

static char *join(const char *dir, const char *name)
{
	size_t a = dir ? strlen(dir) : 0, b = strlen(name);
	char *p = malloc(a + b + 2);

	if (a) {
		memcpy(p, dir, a);
		p[a++] = '/';
	}
	memcpy(p + a, name, b + 1);
	return p;
}

int mpddb_read(FILE *f, mpddb_cb cb, void *ctx)
{
	char *line = NULL, *dirs[MAX_DEPTH];
	const char *p;
	size_t cap = 0;
	ssize_t len;
	int depth = 0, n = 0, lineno = 0;
	bool format = false, body = false, in_song = false, in_playlist = false;
	struct song s = { 0 };

	while ((len = getline(&line, &cap, f)) >= 0) {
		if (len && line[len - 1] == '\n')
			line[--len] = 0;
		if (!body) {
			if (!lineno++ && strcmp(line, "info_begin"))
				break;
			if (!strcmp(line, "format: 2"))
				format = true;
			else if (!strcmp(line, "info_end"))
				body = format;
			if (!body && !strcmp(line, "info_end"))
				break;
			continue;
		}
		if (in_song) {
			if (strcmp(line, "song_end")) {
				song_line(&s, line);
				continue;
			}
			if (!s.hidden) {
				song_emit(&s, cb, ctx);
				n++;
			}
			song_clear(&s);
			in_song = false;
		} else if (in_playlist) {
			in_playlist = strcmp(line, "playlist_end");
		} else if ((p = after(line, "song_begin: "))) {
			s.uri = join(depth ? dirs[depth - 1] : NULL, p);
			in_song = true;
		} else if ((p = after(line, "begin: "))) {
			if (depth == MAX_DEPTH)
				break;
			dirs[depth++] = strdup(p);
		} else if (after(line, "end: ")) {
			if (!depth)
				break;
			free(dirs[--depth]);
		} else if (after(line, "playlist_begin: ")) {
			in_playlist = true;
		}
	}
	bool ok = body && !in_song && !in_playlist && !depth && feof(f);
	song_clear(&s);
	while (depth)
		free(dirs[--depth]);
	free(line);
	return ok ? n : -1;
}
