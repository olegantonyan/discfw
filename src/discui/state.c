#include "app.h"
#include "ui.h"

#include <disc/paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DIRS 300
#define SAVE_DELAY_MS 10000

struct kvp {
	char *k, *v;
};

static struct kvp *items;
static int n_items, cap_items;
static lv_timer_t *save_timer;

static int find(const char *k)
{
	for (int i = 0; i < n_items; i++)
		if (!strcmp(items[i].k, k))
			return i;
	return -1;
}

const char *state_get(const char *k)
{
	int i = find(k);
	return i < 0 ? NULL : items[i].v;
}

int state_get_int(const char *k, int def)
{
	const char *v = state_get(k);
	return v ? atoi(v) : def;
}

static void remove_at(int i)
{
	free(items[i].k);
	free(items[i].v);
	memmove(items + i, items + i + 1, (n_items - i - 1) * sizeof(*items));
	n_items--;
}

static void trim_dirs(void)
{
	int dirs = 0;

	for (int i = 0; i < n_items; i++)
		dirs += !strncmp(items[i].k, "dir.", 4);
	for (int i = 0; i < n_items && dirs > MAX_DIRS;) {
		if (!strncmp(items[i].k, "dir.", 4)) {
			remove_at(i);
			dirs--;
		} else {
			i++;
		}
	}
}

static void save_cb(lv_timer_t *t)
{
	(void)t;
	state_save();
}

void state_set(const char *k, const char *v)
{
	int i = find(k);

	if (i >= 0 && v && !strcmp(items[i].v, v))
		return;
	if (i >= 0)
		remove_at(i);
	if (v && !strchr(v, '\n')) {
		if (n_items == cap_items) {
			cap_items = cap_items ? cap_items * 2 : 64;
			items = realloc(items, cap_items * sizeof(*items));
		}
		items[n_items++] = (struct kvp){ strdup(k), strdup(v) };
		if (!strncmp(k, "dir.", 4))
			trim_dirs();
	}
	if (!save_timer) {
		save_timer = lv_timer_create(save_cb, SAVE_DELAY_MS, NULL);
		lv_timer_set_repeat_count(save_timer, 1);
		lv_timer_set_auto_delete(save_timer, true);
	}
}

void state_set_int(const char *k, int v)
{
	char num[16];
	snprintf(num, sizeof(num), "%d", v);
	state_set(k, num);
}

void state_save(void)
{
	const char *path = disc_data_path("ui.state"), *tmp = disc_data_path("ui.state.new");
	FILE *f;

	if (save_timer) {
		lv_timer_delete(save_timer);
		save_timer = NULL;
	}
	f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < n_items; i++)
		fprintf(f, "%s=%s\n", items[i].k, items[i].v);
	if (fclose(f) == 0)
		rename(tmp, path);
}

void state_load(void)
{
	char line[2048];
	FILE *f = fopen(disc_data_path("ui.state"), "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		eq[1 + strcspn(eq + 1, "\n")] = 0;
		state_set(line, eq + 1);
	}
	fclose(f);
	if (save_timer) {
		lv_timer_delete(save_timer);
		save_timer = NULL;
	}
}

void state_reset(void)
{
	while (n_items)
		remove_at(n_items - 1);
	state_save();
}
