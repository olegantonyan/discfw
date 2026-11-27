#include <disc/paths.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 16

static const char *join(const char *env, const char *def, const char *name)
{
	static char bufs[SLOTS][256];
	static int next;
	const char *dir = getenv(env);
	char *b = bufs[next++ % SLOTS];

	snprintf(b, sizeof(bufs[0]), "%s/%s", dir && *dir ? dir : def, name);
	return b;
}

const char *disc_run_path(const char *name)
{
	return join("DISC_RUN_DIR", "/run", name);
}

const char *disc_data_path(const char *name)
{
	return join("DISC_DATA_DIR", "/usr/data/discfw", name);
}

const char *disc_music_root(void)
{
	const char *r = getenv("DISC_MUSIC_ROOT");
	return r && *r ? r : "/media/sd";
}

const char *disc_boot_mode(void)
{
	static char mode[16];
	FILE *f = fopen(disc_run_path("discfw.mode"), "r");

	mode[0] = 0;
	if (f) {
		if (fgets(mode, sizeof(mode), f))
			mode[strcspn(mode, "\n")] = 0;
		fclose(f);
	}
	return strcmp(mode, "storage") && strcmp(mode, "usbdac") ? "player" : mode;
}
