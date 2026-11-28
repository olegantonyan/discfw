#include "check.h"
#include <disc/mpddb.h>

#include <string.h>

struct got {
	char uri[8][64], title[8][64], artist[8][64], format[8][32];
	unsigned dur[8];
	long added[8];
	int n;
};

static void add(void *ctx, const struct mpddb_song *s)
{
	struct got *g = ctx;

	if (g->n == 8)
		return;
	snprintf(g->uri[g->n], 64, "%s", s->uri);
	snprintf(g->title[g->n], 64, "%s", s->title ? s->title : "(null)");
	snprintf(g->artist[g->n], 64, "%s", s->artist ? s->artist : "(null)");
	snprintf(g->format[g->n], 32, "%s", s->format ? s->format : "(null)");
	g->added[g->n] = s->added;
	g->dur[g->n++] = s->dur_ms;
}

static int parse(const char *text, struct got *g)
{
	FILE *f = fmemopen((void *)text, strlen(text), "r");
	int n;

	memset(g, 0, sizeof(*g));
	n = mpddb_read(f, add, g);
	fclose(f);
	return n;
}

static const char db[] = "info_begin\n"
			 "format: 2\n"
			 "mpd_version: 0.24.8\n"
			 "tag: Title\n"
			 "info_end\n"
			 "directory: A\n"
			 "mtime: 1\n"
			 "begin: A\n"
			 "directory: B C\n"
			 "begin: A/B C\n"
			 "song_begin: 1.flac\n"
			 "Time: 1.5\n"
			 "Artist: One\n"
			 "Artist: Two\n"
			 "Title: Song: one\n"
			 "song_end\n"
			 "end: A/B C\n"
			 "directory: x.cue\n"
			 "type: playlist\n"
			 "begin: A/x.cue\n"
			 "song_begin: track0001\n"
			 "Title: Cue\n"
			 "Range: 0-1000\n"
			 "song_end\n"
			 "end: A/x.cue\n"
			 "song_begin: x.flac\n"
			 "InPlaylist: yes\n"
			 "song_end\n"
			 "song_begin: 2.mp3\n"
			 "Format: 44100:16:2\n"
			 "mtime: 5\n"
			 "added: 6\n"
			 "song_end\n"
			 "playlist_begin: list.m3u\n"
			 "mtime: 3\n"
			 "playlist_end\n"
			 "end: A\n"
			 "song_begin: root.ogg\n"
			 "Time: 0.0004\n"
			 "song_end\n";

int main(void)
{
	struct got g;

	CHECK(parse(db, &g) == 4);
	CHECK(g.n == 4);
	CHECK(!strcmp(g.uri[0], "A/B C/1.flac"));
	CHECK(!strcmp(g.title[0], "Song: one"));
	CHECK(!strcmp(g.artist[0], "One"));
	CHECK(g.dur[0] == 1500);
	CHECK(!strcmp(g.uri[1], "A/x.cue/track0001"));
	CHECK(!strcmp(g.title[1], "Cue"));
	CHECK(!strcmp(g.uri[2], "A/2.mp3"));
	CHECK(!strcmp(g.title[2], "(null)"));
	CHECK(!strcmp(g.format[2], "44100:16:2"));
	CHECK(g.added[2] == 6);
	CHECK(g.added[0] == 0 && !strcmp(g.format[0], "(null)"));
	CHECK(!strcmp(g.uri[3], "root.ogg"));
	CHECK(g.dur[3] == 0);

	CHECK(parse("\x1f\x8b\x08", &g) == -1);
	CHECK(parse("info_begin\nformat: 1\ninfo_end\n", &g) == -1);
	CHECK(parse("info_begin\nformat: 2\ninfo_end\n", &g) == 0);
	CHECK(parse("info_begin\nformat: 2\ninfo_end\nbegin: A\nsong_begin: a\n", &g) == -1);
	CHECK(parse("info_begin\nformat: 2\ninfo_end\nbegin: A\n", &g) == -1);
	CHECK(parse("info_begin\nformat: 2\ninfo_end\nend: A\n", &g) == -1);
	CHECK_DONE();
}
