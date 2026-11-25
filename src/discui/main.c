#include "hw.h"
#include "player.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FONT_PATH "/usr/share/fonts/dejavu/DejaVuSans.ttf"
#define MAX_ENTRIES 4000
#define SCREEN_TIMEOUT_MS 60000

enum {
	DK_PLAY = 250,
	DK_VOLUP = 251,
	DK_VOLDOWN = 252,
	DK_PWR_A = 259,
	DK_VOLDOWN_LONG = 262,
	DK_VOLUP_LONG = 263,
	DK_PWR_LONG = 264,
	DK_PWR_B = 265,
	DK_VOLUP_DOUBLE = 266,
	DK_VOLDOWN_DOUBLE = 267,
	DK_PLAY_DOUBLE = 269,
};

static const char *root_dir;
static char cur_dir[1024];
static char **entries;
static int n_entries, n_dirs;

static const lv_font_t *font_big, *font_small;
static lv_obj_t *scr_browse, *scr_play;
static lv_obj_t *lbl_dir, *list, *btn_np;
static lv_obj_t *lbl_title, *lbl_artist, *lbl_time, *lbl_info, *bar, *lbl_playbtn;
static int key_fd = -1;

static uint32_t tick_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const lv_font_t *load_font(int px, const lv_font_t *fallback)
{
	FILE *f = fopen(FONT_PATH, "rb");
	long sz;
	void *data;
	lv_font_t *font;

	if (!f)
		return fallback;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	rewind(f);
	data = malloc(sz);
	if (!data || fread(data, 1, sz, f) != (size_t)sz) {
		fclose(f);
		free(data);
		return fallback;
	}
	fclose(f);
	font = lv_tiny_ttf_create_data(data, sz, px);
	if (!font)
		return fallback;
	font->fallback = fallback;
	return font;
}

static bool is_audio(const char *name)
{
	static const char *ext[] = { "flac", "mp3", "wav", "ogg", "oga", "opus", "aif", "aiff", NULL };
	const char *dot = strrchr(name, '.');

	if (!dot)
		return false;
	for (int i = 0; ext[i]; i++)
		if (!strcasecmp(dot + 1, ext[i]))
			return true;
	return false;
}

static int cmp_entry(const void *a, const void *b)
{
	const char *x = *(char *const *)a, *y = *(char *const *)b;
	return strcasecmp(x, y);
}

static void free_entries(void)
{
	for (int i = 0; i < n_entries; i++)
		free(entries[i]);
	free(entries);
	entries = NULL;
	n_entries = n_dirs = 0;
}

static void scan_dir(const char *path)
{
	char **dirs = calloc(MAX_ENTRIES, sizeof(char *)), **files = calloc(MAX_ENTRIES, sizeof(char *));
	int nd = 0, nf = 0;
	char full[1400];
	struct dirent *e;
	struct stat st;
	DIR *d = opendir(path);

	free_entries();
	while (d && (e = readdir(d)) && nd + nf < MAX_ENTRIES) {
		bool dir;
		if (e->d_name[0] == '.')
			continue;
		if (e->d_type == DT_DIR || e->d_type == DT_REG) {
			dir = e->d_type == DT_DIR;
		} else {
			snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
			if (stat(full, &st))
				continue;
			dir = S_ISDIR(st.st_mode);
		}
		if (dir)
			dirs[nd++] = strdup(e->d_name);
		else if (is_audio(e->d_name))
			files[nf++] = strdup(e->d_name);
	}
	if (d)
		closedir(d);
	qsort(dirs, nd, sizeof(char *), cmp_entry);
	qsort(files, nf, sizeof(char *), cmp_entry);
	entries = calloc(nd + nf + 1, sizeof(char *));
	memcpy(entries, dirs, nd * sizeof(char *));
	memcpy(entries + nd, files, nf * sizeof(char *));
	n_dirs = nd;
	n_entries = nd + nf;
	free(dirs);
	free(files);
}

static void browse(const char *path);

static void on_entry(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	char path[1400];

	if (i < 0) {
		char *slash = strrchr(cur_dir, '/');
		if (strcmp(cur_dir, root_dir) && slash) {
			*slash = 0;
			browse(cur_dir);
		}
		return;
	}
	if (i < n_dirs) {
		snprintf(path, sizeof(path), "%s/%s", cur_dir, entries[i]);
		browse(path);
		return;
	}
	if (player_play_files(cur_dir, entries + n_dirs, n_entries - n_dirs, i - n_dirs) == 0)
		lv_screen_load(scr_play);
}

static void browse(const char *path)
{
	const char *name = strrchr(path, '/');
	lv_obj_t *b;

	snprintf(cur_dir, sizeof(cur_dir), "%s", path);
	scan_dir(cur_dir);
	lv_label_set_text(lbl_dir, strcmp(cur_dir, root_dir) && name ? name + 1 : "SD card");
	lv_obj_clean(list);
	if (strcmp(cur_dir, root_dir)) {
		b = lv_list_add_button(list, LV_SYMBOL_UP, "..");
		lv_obj_add_event_cb(b, on_entry, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
	}
	for (int i = 0; i < n_entries; i++) {
		b = lv_list_add_button(list, i < n_dirs ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO, entries[i]);
		lv_obj_add_event_cb(b, on_entry, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_label_set_long_mode(lv_obj_get_child(b, 1), LV_LABEL_LONG_MODE_DOTS);
	}
	if (!n_entries)
		lv_list_add_text(list, access(root_dir, R_OK) ? "No SD card" : "No music here");
}

static void on_goto_play(lv_event_t *e)
{
	(void)e;
	lv_screen_load(scr_play);
}

static void on_goto_browse(lv_event_t *e)
{
	(void)e;
	lv_screen_load(scr_browse);
}

static void on_ctl(lv_event_t *e)
{
	switch ((int)(intptr_t)lv_event_get_user_data(e)) {
	case 0:
		player_prev();
		break;
	case 1:
		player_toggle();
		break;
	case 2:
		player_next();
		break;
	}
}

static lv_obj_t *round_button(lv_obj_t *parent, const char *sym, int size, lv_event_cb_t cb, intptr_t arg)
{
	lv_obj_t *b = lv_button_create(parent), *l = lv_label_create(b);

	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_label_set_text(l, sym);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
	lv_obj_center(l);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)arg);
	return b;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, int w, lv_align_t align, int y)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_width(l, w);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(l, "");
	lv_obj_align(l, align, 0, y);
	return l;
}

static void build_ui(void)
{
	lv_obj_t *row;

	scr_browse = lv_obj_create(NULL);
	lbl_dir = label(scr_browse, font_big, 220, LV_ALIGN_TOP_MID, 26);
	list = lv_list_create(scr_browse);
	lv_obj_set_size(list, 290, 250);
	lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
	lv_obj_set_style_text_font(list, font_small, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
	btn_np = round_button(scr_browse, LV_SYMBOL_PLAY, 44, on_goto_play, 0);
	lv_obj_align(btn_np, LV_ALIGN_BOTTOM_MID, 0, -6);
	lv_obj_add_flag(btn_np, LV_OBJ_FLAG_HIDDEN);

	scr_play = lv_obj_create(NULL);
	lv_obj_align(round_button(scr_play, LV_SYMBOL_LIST, 48, on_goto_browse, 0), LV_ALIGN_TOP_MID, 0, 22);
	lbl_title = label(scr_play, font_big, 270, LV_ALIGN_CENTER, -60);
	lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
	lbl_artist = label(scr_play, font_small, 250, LV_ALIGN_CENTER, -28);
	bar = lv_bar_create(scr_play);
	lv_obj_set_size(bar, 230, 6);
	lv_obj_align(bar, LV_ALIGN_CENTER, 0, 2);
	lbl_time = label(scr_play, font_small, 230, LV_ALIGN_CENTER, 24);
	lbl_info = label(scr_play, font_small, 230, LV_ALIGN_CENTER, 128);
	row = lv_obj_create(scr_play);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, 250, 80);
	lv_obj_align(row, LV_ALIGN_CENTER, 0, 78);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	round_button(row, LV_SYMBOL_PREV, 60, on_ctl, 0);
	lbl_playbtn = lv_obj_get_child(round_button(row, LV_SYMBOL_PLAY, 72, on_ctl, 1), 0);
	round_button(row, LV_SYMBOL_NEXT, 60, on_ctl, 2);
}

static void show_volume(void)
{
	char reply[128];
	const char *p;
	int vol;

	if (dac_cmd("status\n", reply, sizeof(reply)) == 0 && (p = strstr(reply, "vol=")) && sscanf(p, "vol=%d", &vol) == 1)
		lv_label_set_text_fmt(lbl_info, "Volume -%d.%d dB", vol / 2, (vol & 1) * 5);
}

static void do_poweroff(void)
{
	player_pause();
	dac_cmd("off\n", NULL, 0);
	screen_power(false);
	system("poweroff");
}

static void on_key(int code, int value)
{
	if (value == 0)
		return;
	lv_display_trigger_activity(NULL);
	switch (code) {
	case DK_PLAY:
		player_toggle();
		break;
	case DK_VOLUP:
	case DK_VOLUP_LONG:
		dac_cmd("vol +\n", NULL, 0);
		show_volume();
		break;
	case DK_VOLDOWN:
	case DK_VOLDOWN_LONG:
		dac_cmd("vol -\n", NULL, 0);
		show_volume();
		break;
	case DK_VOLUP_DOUBLE:
	case DK_PLAY_DOUBLE:
		player_next();
		break;
	case DK_VOLDOWN_DOUBLE:
		player_prev();
		break;
	case DK_PWR_A:
	case DK_PWR_B:
		screen_power(!screen_is_on());
		break;
	case DK_PWR_LONG:
		do_poweroff();
		break;
	}
}

static void keys_timer(lv_timer_t *t)
{
	struct {
		uint32_t sec, usec;
		uint16_t type, code;
		int32_t value;
	} ev;

	(void)t;
	while (key_fd >= 0 && read(key_fd, &ev, sizeof(ev)) == sizeof(ev))
		if (ev.type == EV_KEY)
			on_key(ev.code, ev.value);
}

static void np_timer(lv_timer_t *t)
{
	struct np np;

	(void)t;
	if (!screen_is_on())
		return;
	if (lv_display_get_inactive_time(NULL) > SCREEN_TIMEOUT_MS) {
		screen_power(false);
		return;
	}
	if (player_poll(&np) || np.state == NP_STOP || np.state == NP_UNKNOWN) {
		lv_obj_add_flag(btn_np, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(lbl_title, "Stopped");
		lv_label_set_text(lbl_artist, "");
		lv_label_set_text(lbl_time, "");
		lv_label_set_text(lbl_playbtn, LV_SYMBOL_PLAY);
		lv_bar_set_value(bar, 0, LV_ANIM_OFF);
		return;
	}
	lv_obj_remove_flag(btn_np, LV_OBJ_FLAG_HIDDEN);
	if (strcmp(lv_label_get_text(lbl_title), np.title))
		lv_label_set_text(lbl_title, np.title);
	lv_label_set_text(lbl_artist, np.artist);
	lv_label_set_text_fmt(lbl_time, "%u:%02u / %u:%02u   %s", np.elapsed_s / 60, np.elapsed_s % 60, np.total_s / 60, np.total_s % 60, np.audio);
	lv_label_set_text(lbl_playbtn, np.state == NP_PLAY ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	lv_bar_set_range(bar, 0, np.total_s ? np.total_s : 1);
	lv_bar_set_value(bar, np.elapsed_s, LV_ANIM_OFF);
}

int main(void)
{
	const char *rot = getenv("DISCUI_ROTATE");
	char path[64];
	lv_display_t *disp;
	lv_indev_t *touch;

	signal(SIGPIPE, SIG_IGN);
	root_dir = getenv("DISCUI_ROOT") ? getenv("DISCUI_ROOT") : "/media/sd";
	lv_init();
	lv_tick_set_cb(tick_ms);
	disp = fbdisp_create("/dev/fb0", !rot || strcmp(rot, "0"));
	if (!disp)
		return 1;
	font_small = load_font(18, &lv_font_montserrat_20);
	font_big = load_font(22, &lv_font_montserrat_20);
	lv_display_set_theme(disp, lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_TEAL), lv_palette_main(LV_PALETTE_AMBER), true, font_small));
	if (input_find("cst816t", path, sizeof(path)) == 0) {
		touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, path);
		if (touch && (!rot || strcmp(rot, "0")))
			lv_evdev_set_calibration(touch, 360, 360, 0, 0);
	} else {
		fprintf(stderr, "touch device not found\n");
	}
	if (input_find("x2000_key", path, sizeof(path)) == 0)
		key_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	build_ui();
	browse(root_dir);
	lv_screen_load(scr_browse);
	show_volume();
	lv_timer_create(keys_timer, 20, NULL);
	lv_timer_create(np_timer, 500, NULL);
	for (;;) {
		uint32_t t = lv_timer_handler();
		usleep((t > 20 ? 20 : t) * 1000);
	}
}
