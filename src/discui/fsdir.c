#include "fsdir.h"
#include "mpdc.h"

#include <dirent.h>
#include <disc/paths.h>
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

	bool root = !strcmp(path, disc_music_root());

	l->path = strdup(path);
	l->ok = d != NULL;
	while (d && (e = readdir(d))) {
		bool dir;
		if (hidden(e->d_name) || (root && !strcmp(e->d_name, "EQ")))
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

struct strv {
	char **v;
	int n, cap;
};

static void strv_push(struct strv *a, char *s)
{
	if (a->n == a->cap) {
		a->cap = a->cap ? a->cap * 2 : 64;
		a->v = realloc(a->v, a->cap * sizeof(char *));
	}
	a->v[a->n++] = s;
}

struct ints {
	int *v, n, cap;
};

static void ints_push(struct ints *a, int x)
{
	if (a->n == a->cap) {
		a->cap = a->cap ? a->cap * 2 : 8;
		a->v = realloc(a->v, a->cap * sizeof(int));
	}
	a->v[a->n++] = x;
}

struct tnode {
	const char *path, *name;
	size_t len;
	struct ints files, kids;
};

struct tree {
	char **paths;
	bool *is_dir;
	int n;
	struct tnode *nodes;
	int n_nodes;
};

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static bool tree_build(struct tree *t, const char *rel)
{
	int *stack, sp = 1;

	memset(t, 0, sizeof(*t));
	t->n = mpdc_listall(rel, &t->paths, &t->is_dir);
	if (t->n < 0)
		return false;
	t->nodes = calloc(t->n + 1, sizeof(*t->nodes));
	stack = malloc((t->n + 1) * sizeof(int));
	t->nodes[0] = (struct tnode){ .path = rel, .name = base_name(rel), .len = strlen(rel) };
	t->n_nodes = 1;
	stack[0] = 0;
	for (int i = 0; i < t->n; i++) {
		const char *p = t->paths[i], *slash = strrchr(p, '/');
		size_t plen = slash ? (size_t)(slash - p) : 0;
		struct tnode *top;

		while (sp > 1 && !(t->nodes[stack[sp - 1]].len == plen && !strncmp(t->nodes[stack[sp - 1]].path, p, plen)))
			sp--;
		top = &t->nodes[stack[sp - 1]];
		if (t->is_dir[i]) {
			int k = t->n_nodes++;
			t->nodes[k] = (struct tnode){ .path = p, .name = base_name(p), .len = strlen(p) };
			ints_push(&top->kids, k);
			stack[sp++] = k;
		} else {
			ints_push(&top->files, i);
		}
	}
	free(stack);
	return true;
}

static void tree_free(struct tree *t)
{
	for (int i = 0; i < t->n_nodes; i++) {
		free(t->nodes[i].files.v);
		free(t->nodes[i].kids.v);
	}
	for (int i = 0; i < t->n; i++)
		free(t->paths[i]);
	free(t->paths);
	free(t->is_dir);
	free(t->nodes);
}

static const struct tree *sort_tree;

static int cmp_file(const void *a, const void *b)
{
	return nat_cmp(base_name(sort_tree->paths[*(const int *)a]), base_name(sort_tree->paths[*(const int *)b]));
}

static int cmp_kid(const void *a, const void *b)
{
	return nat_cmp(sort_tree->nodes[*(const int *)a].name, sort_tree->nodes[*(const int *)b].name);
}

static void tree_emit(struct tree *t, int node, struct strv *out)
{
	struct tnode *nd = &t->nodes[node];

	sort_tree = t;
	if (nd->files.n > 1)
		qsort(nd->files.v, nd->files.n, sizeof(int), cmp_file);
	if (nd->kids.n > 1)
		qsort(nd->kids.v, nd->kids.n, sizeof(int), cmp_kid);
	for (int i = 0; i < nd->files.n; i++) {
		const char *p = t->paths[nd->files.v[i]], *name = base_name(p);
		if (!hidden(name) && fsdir_is_audio(name))
			strv_push(out, strdup(p));
	}
	for (int i = 0; i < nd->kids.n; i++)
		if (!hidden(t->nodes[nd->kids.v[i]].name))
			tree_emit(t, nd->kids.v[i], out);
}

static bool collect_db(const char *dir, struct strv *out)
{
	char *rel = mpdc_uri_for_path(dir);
	struct tree t;
	int before = out->n;

	if (rel && rel[0] && tree_build(&t, rel)) {
		tree_emit(&t, 0, out);
		tree_free(&t);
	}
	free(rel);
	return out->n > before;
}

static void collect(const char *dir, int depth, bool recursive, struct strv *out, int *missing)
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
		for (int i = 0; i < nf; i++)
			strv_push(out, uris[i]);
		free(uris);
	}
	fsdir_free_strv(names, nf);
	if (recursive && depth < MAX_DEPTH)
		for (int i = 0; i < nd; i++) {
			char sub[2048];
			snprintf(sub, sizeof(sub), "%s/%s", dir, subdirs[i]);
			if (!collect_db(sub, out))
				collect(sub, depth + 1, true, out, missing);
		}
	fsdir_free_strv(subdirs, nd);
}

int fsdir_collect(const char *dir, bool recursive, char ***uris_out, int *missing)
{
	struct strv out = { 0 };

	*missing = 0;
	collect(dir, 0, recursive, &out, missing);
	*uris_out = out.v;
	return out.n;
}

static char **subdirs_of(const char *dir, int *n)
{
	const struct dirlist *l = fsdir_get(dir);
	char **v = calloc(l->ndirs + 1, sizeof(char *));

	for (int i = 0; i < l->ndirs; i++)
		v[i] = strdup(l->v[i].name);
	*n = l->ndirs;
	return v;
}

static bool first_music(const char *dir, bool self, int depth, char *out, size_t len)
{
	const struct dirlist *l = fsdir_get(dir);
	char **subs, sub[2048];
	bool found = false;
	int n;

	if (self && l->n > l->ndirs) {
		snprintf(out, len, "%s", dir);
		return true;
	}
	if (depth >= MAX_DEPTH)
		return false;
	subs = subdirs_of(dir, &n);
	for (int i = 0; i < n && !found; i++) {
		snprintf(sub, sizeof(sub), "%s/%s", dir, subs[i]);
		found = first_music(sub, true, depth + 1, out, len);
	}
	fsdir_free_strv(subs, n);
	return found;
}

bool fsdir_next_music(const char *dir, bool wrap, char *out, size_t len)
{
	const char *root = disc_music_root();
	size_t rn = strlen(root);
	char cur[2048], sub[2400];

	if (strncmp(dir, root, rn) || (dir[rn] && dir[rn] != '/'))
		return false;
	if (first_music(dir, false, 0, out, len))
		return true;
	snprintf(cur, sizeof(cur), "%s", dir);
	while (strcmp(cur, root)) {
		char *slash = strrchr(cur, '/'), **subs;
		bool after = false, found = false;
		int n;

		*slash = 0;
		subs = subdirs_of(cur, &n);
		for (int i = 0; i < n && !found; i++) {
			if (after) {
				snprintf(sub, sizeof(sub), "%s/%s", cur, subs[i]);
				found = first_music(sub, true, 0, out, len);
			} else if (!strcmp(subs[i], slash + 1)) {
				after = true;
			}
		}
		fsdir_free_strv(subs, n);
		if (found)
			return true;
	}
	return wrap && first_music(root, true, 0, out, len) && strcmp(out, dir);
}
