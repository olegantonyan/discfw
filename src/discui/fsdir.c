#include "fsdir.h"
#include "mpdc.h"

#include <dirent.h>
#include <disc/settings.h>
#include <disc/text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CACHE 8
#define MAX_DEPTH 8

static struct dirlist cache[CACHE];
static unsigned clock_age;

static const char *const exts[] = { ".flac", ".mp3", ".wav", ".ogg", ".oga", ".opus", ".aif", ".aiff", ".aifc", ".m4a",
				    ".mp4", ".aac", ".alac", ".ape", ".wv", ".wma", ".mka", ".tta", NULL };

bool fsdir_is_audio(const char *name)
{
	for (int i = 0; exts[i]; i++)
		if (has_suffix_ci(name, exts[i]))
			return true;
	return false;
}

static bool hidden(const char *name)
{
	if (set_get_bool("lib.hidden"))
		return !strcmp(name, ".") || !strcmp(name, "..");
	return name[0] == '.' || !strcmp(name, "System Volume Information") || !strcmp(name, "$RECYCLE.BIN") ||
	       !strcmp(name, "LOST.DIR");
}

static int cmp_entry(const void *a, const void *b)
{
	const struct dentry *x = a, *y = b;

	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	return nat_cmp(x->name, y->name);
}

static void free_list(struct dirlist *l)
{
	for (int i = 0; i < l->n; i++)
		free(l->v[i].name);
	free(l->v);
	free(l->path);
	memset(l, 0, sizeof(*l));
}

static void load(struct dirlist *l, const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	int cap = 0;

	l->path = strdup(path);
	l->ok = d != NULL;
	while (d && (e = readdir(d))) {
		bool dir;
		if (hidden(e->d_name))
			continue;
		if (e->d_type == DT_DIR || e->d_type == DT_REG) {
			dir = e->d_type == DT_DIR;
		} else {
			char full[2048];
			struct stat st;
			snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
			if (stat(full, &st))
				continue;
			dir = S_ISDIR(st.st_mode);
		}
		if (!dir && !fsdir_is_audio(e->d_name))
			continue;
		if (l->n == cap) {
			cap = cap ? cap * 2 : 64;
			l->v = realloc(l->v, cap * sizeof(*l->v));
		}
		l->v[l->n++] = (struct dentry){ strdup(e->d_name), dir, index_letter(e->d_name) };
		l->ndirs += dir;
	}
	if (d)
		closedir(d);
	if (l->n > 1)
		qsort(l->v, l->n, sizeof(*l->v), cmp_entry);
}

const struct dirlist *fsdir_get(const char *path)
{
	struct dirlist *victim = &cache[0];

	for (int i = 0; i < CACHE; i++) {
		if (cache[i].path && !strcmp(cache[i].path, path)) {
			cache[i].age = ++clock_age;
			return &cache[i];
		}
		if (cache[i].age < victim->age)
			victim = &cache[i];
	}
	free_list(victim);
	load(victim, path);
	victim->age = ++clock_age;
	return victim;
}

void fsdir_invalidate(void)
{
	for (int i = 0; i < CACHE; i++)
		free_list(&cache[i]);
}

void fsdir_free_strv(char **v, int n)
{
	for (int i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

static void collect(const char *dir, int depth, bool recursive, char ***out, int *n, int *cap, int *missing)
{
	const struct dirlist *l = fsdir_get(dir);
	char **names, **uris, **subdirs;
	int nf = l->n - l->ndirs, nd = l->ndirs;

	names = calloc(nf + 1, sizeof(char *));
	subdirs = calloc(nd + 1, sizeof(char *));
	for (int i = 0; i < nd; i++)
		subdirs[i] = strdup(l->v[i].name);
	for (int i = 0; i < nf; i++)
		names[i] = strdup(l->v[nd + i].name);
	if (nf) {
		*missing += mpdc_resolve(dir, names, nf, &uris);
		if (*n + nf > *cap) {
			*cap = (*n + nf) * 2;
			*out = realloc(*out, *cap * sizeof(char *));
		}
		memcpy(*out + *n, uris, nf * sizeof(char *));
		*n += nf;
		free(uris);
	}
	fsdir_free_strv(names, nf);
	if (recursive && depth < MAX_DEPTH)
		for (int i = 0; i < nd; i++) {
			char sub[2048];
			snprintf(sub, sizeof(sub), "%s/%s", dir, subdirs[i]);
			collect(sub, depth + 1, true, out, n, cap, missing);
		}
	fsdir_free_strv(subdirs, nd);
}

int fsdir_collect(const char *dir, bool recursive, char ***uris_out, int *missing)
{
	int n = 0, cap = 0;

	*uris_out = NULL;
	*missing = 0;
	collect(dir, 0, recursive, uris_out, &n, &cap, missing);
	return n;
}
