#pragma once
#include "app.h"

#include <mpd/client.h>

struct item {
	char *uri, *title, *artist, *album, *albumartist;
	unsigned dur_ms;
	int id, disc, track, year;
};

struct items {
	struct item *v;
	int n, cap;
};

void items_free(struct items *it);
void song_fill(struct song *dst, const struct mpd_song *s);
const char *song_label(const struct item *it, char *buf, size_t len);

void mpdc_init(void);
struct mpd_connection *mpdc(void);
bool mpdc_check(const char *what);
void mpdc_refresh(void);

void mpdc_toggle(void);
void mpdc_next(void);
void mpdc_prev(void);
void mpdc_play_pos(int pos);
void mpdc_seek_ms(unsigned ms);
void mpdc_set_order(const char *order);
const char *mpdc_order(void);

char *mpdc_uri_for_path(const char *abs_path);
char *mpdc_path_for_uri(const char *uri);
int mpdc_resolve(const char *dir, char *const *names, int n, char ***uris_out);
int mpdc_play_uris(char *const *uris, int n, int start, const char *origin);
int mpdc_append_uris(char *const *uris, int n);
void mpdc_update(const char *abs_dir);
bool mpdc_folder_scanned(const char *abs_dir);

int mpdc_queue(struct items *out);
bool mpdc_queue_delete(int pos);
bool mpdc_queue_move(int from, int to);
bool mpdc_queue_clear(void);
const char *mpdc_queue_origin(void);
const char *app_loaded_playlist(void);
void items_sort(struct items *it, const char *criteria);
int sort_presets(char ***labels, char ***criteria);
void sort_presets_free(char **labels, char **criteria, int n);
bool mpdc_sort_playlist(const char *name, const char *criteria);
bool mpdc_sort_queue(const char *criteria);

int mpdc_playlists(char ***names);
int mpdc_playlist_songs(const char *name, struct items *out);
int mpdc_playlist_create(const char *name, char *const *uris, int n);
int mpdc_playlist_append(const char *name, char *const *uris, int n);
bool mpdc_playlist_rename(const char *from, const char *to);
bool mpdc_playlist_delete(const char *name);
bool mpdc_playlist_remove(const char *name, int pos);
bool mpdc_playlist_move(const char *name, int from, int to);
bool mpdc_playlist_play(const char *name, int pos);
bool mpdc_playlist_save_queue(const char *name);
void mpdc_playlist_name_for(char *out, size_t len, const char *rel_path);
void mpdc_playlist_order_set(char **names, int n);

int mpdc_list_tag(enum mpd_tag_type tag, enum mpd_tag_type filter_tag, const char *filter, char ***out);
int mpdc_find(enum mpd_tag_type t1, const char *v1, enum mpd_tag_type t2, const char *v2, struct items *out);
bool mpdc_stats(unsigned *songs, unsigned *albums, unsigned *artists, unsigned long *db_update, unsigned long *play_time);
