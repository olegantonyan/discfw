#include "ui.h"

#include <string.h>

struct tile {
	const char *sym, *label;
	const struct screen_ops *ops;
};

static const struct tile tiles[] = {
	{ LV_SYMBOL_PLAY, "Playing", &scr_np },
	{ LV_SYMBOL_DIRECTORY, "Folders", &scr_folders },
	{ LV_SYMBOL_LIST, "Playlists", &scr_playlists },
	{ LV_SYMBOL_AUDIO, "Library", &scr_library },
	{ LV_SYMBOL_SHUFFLE, "Queue", &scr_queue },
	{ LV_SYMBOL_SETTINGS, "Settings", &scr_settings },
};

static void on_tile(lv_event_t *e)
{
	const struct tile *t = lv_event_get_user_data(e);
	nav_push(t->ops, NULL);
}

static lv_obj_t *create(void *arg)
{
	lv_obj_t *scr = ui_screen("", NULL), *grid;

	(void)arg;
	grid = lv_obj_create(scr);
	lv_obj_remove_style_all(grid);
	lv_obj_set_size(grid, 270, 260);
	lv_obj_align(grid, LV_ALIGN_CENTER, 0, 10);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
	lv_obj_set_scrollable(grid, false);
	for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++) {
		lv_obj_t *b = lv_button_create(grid), *icon, *label;
		lv_obj_set_size(b, 120, 74);
		lv_obj_set_style_radius(b, 18, 0);
		lv_obj_set_style_shadow_width(b, 0, 0);
		lv_obj_set_style_bg_color(b, lv_color_hex(0x262626), 0);
		lv_obj_add_event_cb(b, on_tile, LV_EVENT_CLICKED, (void *)&tiles[i]);
		icon = ui_label(b, app.font.icon, tiles[i].sym);
		lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 4);
		lv_obj_set_style_text_color(icon, app.accent, 0);
		label = ui_label(b, app.font.s, tiles[i].label);
		lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);
	}
	return scr;
}

const struct screen_ops scr_home = { .name = "home", .create = create };
