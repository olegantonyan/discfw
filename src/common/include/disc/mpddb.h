#pragma once
#include <stdio.h>

struct mpddb_song {
	const char *uri, *title, *artist, *album, *albumartist, *date, *track, *disc, *format;
	unsigned dur_ms;
	long added;
};

typedef void (*mpddb_cb)(void *ctx, const struct mpddb_song *s);

int mpddb_read(FILE *f, mpddb_cb cb, void *ctx);
