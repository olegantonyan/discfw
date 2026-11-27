#include "ui.h"

#include <disc/paths.h>
#include <disc/settings.h>
#include <stdio.h>
#include <string.h>

#define ALWAYS "Starts at every power-on."

static lv_obj_t *status, *detail, *vol;
static bool storage;

static void refresh(uint32_t evs, void *ctx)
{
	char line[48];
	int r = app.sys.uac_rate;
	bool spdif = set_get_bool("audio.spdif");

	(void)evs;
	(void)ctx;
	if (storage) {
		lv_label_set_text(status, app.sys.usb ? "USB connected" : "Plug into a computer");
		lv_label_set_text_fmt(detail, "%sEject on the computer before unplugging.", app.sys.mode_always ? ALWAYS "\n" : "");
		return;
	}
	if (r % 1000)
		snprintf(line, sizeof(line), "%d.%d kHz", r / 1000, r % 1000 / 100);
	else if (r)
		snprintf(line, sizeof(line), "%d kHz", r / 1000);
	else
		snprintf(line, sizeof(line), "%s", app.sys.usb ? "Idle" : "Plug into a computer");
	lv_label_set_text(status, line);
	lv_label_set_text_fmt(detail, "%s%s", spdif ? "S/PDIF, fixed level" : "Internal DAC", app.sys.mode_always ? "\n" ALWAYS : "");
	lv_obj_set_hidden(vol, spdif);
	if (!lv_obj_has_state(vol, LV_STATE_PRESSED))
		lv_slider_set_value(vol, app.sys.volume, LV_ANIM_OFF);
}

static void leave(void *ctx)
{
	(void)ctx;
	sysc_send("mode player");
}

static void on_leave(lv_event_t *e)
{
	(void)e;
	if (storage && app.sys.usb)
		ui_confirm("Eject the drive on the computer first. Back to player?", "Restart", leave, NULL);
	else if (app.sys.mode_always)
		ui_confirm("Leave for good and restart as a player?", "Restart", leave, NULL);
	else
		ui_confirm("Restart as a player?", "Restart", leave, NULL);
}

static void vol_changed(lv_event_t *e)
{
	sysc_send("vol %d", (int)lv_slider_get_value(lv_event_get_target_obj(e)));
}

static lv_obj_t *create(void *arg)
{
	lv_obj_t *scr = ui_screen("", NULL), *icon, *title, *b;

	(void)arg;
	storage = !strcmp(disc_boot_mode(), "storage");
	icon = ui_label(scr, app.font.icon_big, storage ? LV_SYMBOL_USB : LV_SYMBOL_AUDIO);
	lv_obj_set_style_text_color(icon, app.accent, 0);
	lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 50);
	title = ui_label(scr, app.font.title, storage ? "USB storage" : "USB DAC");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 110);
	status = ui_label(scr, app.font.m, "");
	lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 146);
	detail = ui_label(scr, app.font.s, "");
	lv_obj_set_width(detail, 270);
	lv_label_set_long_mode(detail, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(detail, lv_color_hex(0x9a9a9a), 0);
	lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, 178);
	if (!storage) {
		vol = lv_slider_create(scr);
		lv_obj_set_width(vol, 200);
		lv_slider_set_range(vol, 0, 100);
		lv_obj_align(vol, LV_ALIGN_TOP_MID, 0, 232);
		lv_obj_add_event_cb(vol, vol_changed, LV_EVENT_VALUE_CHANGED, NULL);
	}
	b = ui_text_button(scr, "Back to player", on_leave, NULL);
	lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -44);
	app_listen(scr, EV_SYS | EV_VOLUME | EV_SETTINGS, refresh, NULL);
	refresh(0, NULL);
	return scr;
}

static bool back(lv_obj_t *scr)
{
	(void)scr;
	return true;
}

const struct screen_ops scr_mode = { .name = "mode", .create = create, .back = back };
