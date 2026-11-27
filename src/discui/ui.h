#pragma once
#include "app.h"

#define HEADER_Y 30
#define CONTENT_Y 62
#define ROW_H 48

struct screen_ops {
	const char *name;
	lv_obj_t *(*create)(void *arg);
	bool (*back)(lv_obj_t *scr);
	void (*shown)(lv_obj_t *scr);
};

void nav_start(void);
void nav_push(const struct screen_ops *ops, void *arg);
void nav_open(const struct screen_ops *ops, void *arg);
bool nav_back(void);
void nav_home(void);
const struct screen_ops *nav_top(void);
const char *nav_describe(char *buf, size_t len);
bool nav_goto_name(const char *name);

lv_obj_t *ui_screen(const char *title, lv_obj_t **title_label);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, const char *text);
lv_obj_t *ui_round_button(lv_obj_t *parent, const char *sym, int size, lv_event_cb_t cb, void *ud);
lv_obj_t *ui_text_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);
void ui_toast(const char *text);
void ui_volume_overlay(void);
void ui_shutdown_overlay(bool reboot);
void lowbat_update(void);

typedef void (*ui_pick_cb)(int idx, void *ctx);
void ui_menu(const char *title, const char *const *items, int n, ui_pick_cb cb, void *ctx);
void ui_pick(const char *title, const char *const *items, int n, int selected, ui_pick_cb cb, void *ctx);
void ui_confirm(const char *text, const char *yes, void (*cb)(void *ctx), void *ctx);
void ui_text_input(const char *title, const char *initial, bool secret, void (*cb)(const char *text, void *ctx), void *ctx);
void ui_number_input(const char *title, int val, int min, int max, int step, const char *unit, void (*cb)(int v, void *ctx), void *ctx);
void ui_info(const char *title, const char *text);
bool ui_modal_active(void);
bool ui_modal_close(void);
void ui_modal_close_all(void);

struct vrow {
	const char *icon;
	char text[256];
	char sub[160];
	char right[32];
	bool active, dim, header;
};

struct vlist_ops {
	int (*count)(void *ctx);
	void (*row)(void *ctx, int idx, struct vrow *r);
	void (*click)(void *ctx, int idx);
	void (*hold)(void *ctx, int idx);
	uint32_t (*letter)(void *ctx, int idx);
};

lv_obj_t *vlist_create(lv_obj_t *parent, const struct vlist_ops *ops, void *ctx);
void vlist_reload(lv_obj_t *vl);
void vlist_set_empty(lv_obj_t *vl, const char *text);
void vlist_refresh(lv_obj_t *vl);
void vlist_scroll_to(lv_obj_t *vl, int idx, bool center);
int vlist_first(lv_obj_t *vl);
int vlist_count(lv_obj_t *vl);

void shade_drag(int dy, bool released);
void shade_lift(int dy, bool released);
bool shade_is_open(void);
void shade_close(void);
void lock_update(void);
void lock_init(void);
void shade_init(void);
void status_set_visible(bool on);

void state_load(void);
void state_save(void);
void state_reset(void);
const char *state_get(const char *k);
int state_get_int(const char *k, int def);
void state_set(const char *k, const char *v);
void state_set_int(const char *k, int v);

void cover_init(void);
void cover_poll(void);
void cover_request(const char *uri);
const lv_image_dsc_t *cover_current(void);
const lv_image_dsc_t *cover_placeholder(void);
const lv_image_dsc_t *vinyl_disc(void);
const lv_image_dsc_t *vinyl_label(void);

extern const struct screen_ops scr_home, scr_folders, scr_np, scr_queue, scr_playlists, scr_playlist, scr_library,
	scr_settings, scr_settings_page, scr_wifi, scr_bt, scr_mode;

void folders_open_at(const char *path, const char *select_name);
void settings_open_page(const char *page);
const char *fmt_time(char *buf, size_t len, unsigned ms);
