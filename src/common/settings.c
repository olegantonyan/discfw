#include <disc/settings.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static char **vals;
static char **extra;
static int n_extra;

static void ensure(void)
{
	if (!vals)
		vals = calloc(set_ndefs, sizeof(char *));
}

int set_index(const char *key)
{
	for (int i = 0; i < set_ndefs; i++)
		if (!strcmp(set_defs[i].key, key))
			return i;
	return -1;
}

const struct set_def *set_lookup(const char *key)
{
	int i = set_index(key);
	return i < 0 ? NULL : &set_defs[i];
}

static const char *choice_at(const struct set_def *d, int idx, size_t *vlen, const char **label, size_t *llen)
{
	const char *p = d->choices;

	for (int i = 0; p && *p; i++) {
		const char *end = strchr(p, '|');
		size_t n = end ? (size_t)(end - p) : strlen(p);
		if (i == idx) {
			const char *eq = memchr(p, '=', n);
			*vlen = eq ? (size_t)(eq - p) : n;
			if (label) {
				*label = eq ? eq + 1 : p;
				*llen = eq ? n - (eq + 1 - p) : n;
			}
			return p;
		}
		p = end ? end + 1 : p + n;
	}
	return NULL;
}

int set_choice_count(const struct set_def *d)
{
	size_t vlen;
	int n = 0;

	while (choice_at(d, n, &vlen, NULL, NULL))
		n++;
	return n;
}

bool set_choice(const struct set_def *d, int i, char *out, size_t len)
{
	size_t vlen;
	const char *p = choice_at(d, i, &vlen, NULL, NULL);

	if (!p || vlen >= len)
		return false;
	memcpy(out, p, vlen);
	out[vlen] = 0;
	return true;
}

bool set_choice_label(const struct set_def *d, int i, char *out, size_t len)
{
	size_t vlen, llen;
	const char *label;

	if (!choice_at(d, i, &vlen, &label, &llen))
		return false;
	if (llen >= len)
		llen = len - 1;
	memcpy(out, label, llen);
	out[llen] = 0;
	return true;
}

int set_choice_index(const struct set_def *d, const char *val)
{
	size_t vlen;
	const char *p;

	for (int i = 0; (p = choice_at(d, i, &vlen, NULL, NULL)); i++)
		if (strlen(val) == vlen && !strncmp(p, val, vlen))
			return i;
	return -1;
}

int set_normalize(const struct set_def *d, const char *val, char *out, size_t len)
{
	char *end;
	long v;

	switch (d->type) {
	case SET_BOOL:
		if (!strcmp(val, "1") || !strcasecmp(val, "true") || !strcasecmp(val, "on") || !strcasecmp(val, "yes"))
			snprintf(out, len, "1");
		else if (!strcmp(val, "0") || !strcasecmp(val, "false") || !strcasecmp(val, "off") || !strcasecmp(val, "no"))
			snprintf(out, len, "0");
		else
			return -1;
		return 0;
	case SET_INT:
		errno = 0;
		v = strtol(val, &end, 10);
		if (errno || end == val || *end || v < d->min || v > d->max)
			return -1;
		snprintf(out, len, "%ld", v);
		return 0;
	case SET_ENUM:
		if (set_choice_index(d, val) < 0)
			return -1;
		snprintf(out, len, "%s", val);
		return 0;
	case SET_STR:
		if (strpbrk(val, "\n\r") || strlen(val) >= len)
			return -1;
		snprintf(out, len, "%s", val);
		return 0;
	}
	return -1;
}

static void clear(void)
{
	ensure();
	for (int i = 0; i < set_ndefs; i++) {
		free(vals[i]);
		vals[i] = NULL;
	}
	for (int i = 0; i < n_extra; i++)
		free(extra[i]);
	free(extra);
	extra = NULL;
	n_extra = 0;
}

int settings_load(const char *path)
{
	char line[1024], norm[512];
	FILE *f;
	int bad = 0;

	clear();
	f = fopen(path, "r");
	if (!f)
		return errno == ENOENT ? 0 : -1;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (!line[0] || line[0] == '#' || !eq)
			continue;
		*eq = 0;
		int i = set_index(line);
		if (i < 0) {
			*eq = '=';
			char **p = realloc(extra, (n_extra + 1) * sizeof(char *));
			if (p) {
				extra = p;
				extra[n_extra++] = strdup(line);
			}
			continue;
		}
		if (set_normalize(&set_defs[i], eq + 1, norm, sizeof(norm))) {
			bad++;
			continue;
		}
		free(vals[i]);
		vals[i] = strcmp(norm, set_defs[i].def) ? strdup(norm) : NULL;
	}
	fclose(f);
	return bad;
}

int settings_save(const char *path)
{
	char tmp[512];
	FILE *f;
	bool ok;

	ensure();
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	ok = true;
	for (int i = 0; i < set_ndefs; i++)
		if (vals[i] && fprintf(f, "%s=%s\n", set_defs[i].key, vals[i]) < 0)
			ok = false;
	for (int i = 0; i < n_extra; i++)
		if (fprintf(f, "%s\n", extra[i]) < 0)
			ok = false;
	if (fflush(f) || fsync(fileno(f)))
		ok = false;
	if (fclose(f))
		ok = false;
	if (!ok || rename(tmp, path)) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

const char *set_get(const char *key)
{
	int i = set_index(key);

	ensure();
	if (i < 0)
		return NULL;
	return vals[i] ? vals[i] : set_defs[i].def;
}

int set_get_int(const char *key)
{
	const char *v = set_get(key);
	return v ? atoi(v) : 0;
}

bool set_get_bool(const char *key)
{
	const char *v = set_get(key);
	return v && v[0] == '1';
}

int set_get_enum(const char *key)
{
	const struct set_def *d = set_lookup(key);
	return d ? set_choice_index(d, set_get(key)) : -1;
}

int set_put(const char *key, const char *val)
{
	char norm[512];
	int i = set_index(key);

	ensure();
	if (i < 0)
		return -1;
	if (set_normalize(&set_defs[i], val, norm, sizeof(norm)))
		return -2;
	free(vals[i]);
	vals[i] = strcmp(norm, set_defs[i].def) ? strdup(norm) : NULL;
	return 0;
}

bool set_is_default(const char *key)
{
	int i = set_index(key);

	ensure();
	return i < 0 || !vals[i];
}
