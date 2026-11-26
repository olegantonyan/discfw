#pragma once
#include <stdbool.h>
#include <stdint.h>

struct dentry {
	char *name;
	bool dir;
	uint32_t letter;
};

struct dirlist {
	char *path;
	struct dentry *v;
	int n, ndirs;
	bool ok;
	unsigned age;
};

const struct dirlist *fsdir_get(const char *path);
void fsdir_invalidate(void);
bool fsdir_is_audio(const char *name);
int fsdir_collect(const char *dir, bool recursive, char ***uris_out, int *missing);
void fsdir_free_strv(char **v, int n);
