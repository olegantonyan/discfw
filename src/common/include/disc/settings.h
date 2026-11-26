#pragma once
#include <stdbool.h>
#include <stddef.h>

#include <disc/paths.h>

#define SETTINGS_PATH disc_data_path("settings")

enum set_type { SET_BOOL, SET_INT, SET_ENUM, SET_STR };

struct set_def {
	const char *key;
	enum set_type type;
	const char *def;
	int min, max, step;
	const char *choices;
	const char *page;
	const char *label;
	const char *unit;
};

extern const struct set_def set_defs[];
extern const int set_ndefs;

const struct set_def *set_lookup(const char *key);
int set_index(const char *key);
int set_normalize(const struct set_def *d, const char *val, char *out, size_t len);
int set_choice_count(const struct set_def *d);
bool set_choice(const struct set_def *d, int i, char *out, size_t len);
bool set_choice_label(const struct set_def *d, int i, char *out, size_t len);
int set_choice_index(const struct set_def *d, const char *val);

int settings_load(const char *path);
int settings_save(const char *path);
const char *set_get(const char *key);
int set_get_int(const char *key);
bool set_get_bool(const char *key);
int set_get_enum(const char *key);
int set_put(const char *key, const char *val);
bool set_is_default(const char *key);
