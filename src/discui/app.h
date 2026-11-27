#pragma once
#include <lvgl/lvgl.h>
#include <stdbool.h>
#include <stdint.h>

#define SCR_W 360
#define SCR_H 360
#define SCR_R 180

enum ps { PS_UNKNOWN, PS_STOP, PS_PLAY, PS_PAUSE };

enum app_ev {
	EV_STATUS = 1 << 0,
	EV_SONG = 1 << 1,
	EV_QUEUE = 1 << 2,
	EV_PLAYLISTS = 1 << 3,
	EV_DB = 1 << 4,
	EV_OPTIONS = 1 << 5,
	EV_SYS = 1 << 6,
	EV_SETTINGS = 1 << 7,
	EV_BAT = 1 << 8,
	EV_SECOND = 1 << 9,
	EV_UPDATE = 1 << 10,
	EV_MOUNT = 1 << 11,
	EV_VOLUME = 1 << 12,
	EV_COVER = 1 << 13,
	EV_EQ = 1 << 14,
};

struct song {
	char uri[1024];
	char title[256], artist[256], album[256], albumartist[256], date[32], genre[128], track[16], disc[16];
	unsigned duration_ms;
	int pos, id;
};

struct mpdstate {
	bool connected;
	enum ps state;
	unsigned elapsed_ms, total_ms;
	int64_t elapsed_at;
	int song_pos, song_id, queue_len;
	unsigned queue_version;
	bool repeat, random, single, consume;
	unsigned rate, bits, channels, kbps;
	bool updating;
	char error[128];
	struct song cur;
};

struct sysstate {
	bool connected;
	bool screen_on, dimmed, locked;
	int volume;
	bool out_bt;
	char bt_dev[64];
	char bt_name[64];
	int bt_bat;
	bool usb, mode_always;
	int uac_rate;
	int sleep_left;
	bool powered;
	char profile[16];
	int lowbat;
	bool shutting_down;
};

struct batstate {
	bool valid, skip;
	int cap, mv, ma, temp;
	char state[16], vbus[16], stage[16];
};

struct fonts {
	const lv_font_t *s, *m, *l, *title, *clock, *icon, *icon_big;
};

struct app {
	struct mpdstate mpd;
	struct sysstate sys;
	struct batstate bat;
	struct fonts font;
	bool safe_mode;
	bool host;
	lv_color_t accent;
};

extern struct app app;

typedef void (*app_listener)(uint32_t evs, void *ctx);
void app_listen(lv_obj_t *owner, uint32_t mask, app_listener cb, void *ctx);
void app_notify(uint32_t evs);
int64_t mono_ms(void);
unsigned mpd_elapsed_now(void);

typedef void (*fd_cb)(int fd, short revents, void *ctx);
void loop_watch(int fd, short events, fd_cb cb, void *ctx);
void loop_events(int fd, short events);
void loop_unwatch(int fd);
void loop_wake(void);

lv_display_t *disp_create(void);
void disp_set_rotation(int deg);
int disp_rotation(void);
void disp_set_active(bool on);
bool disp_active(void);
int disp_shot(const char *path);

void input_init(void);
void input_inject(int x, int y, bool pressed);
void input_set_back_handler(bool (*cb)(void));

void sysc_init(void);
void sysc_send(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sysc_set(const char *key, const char *val);
void sysc_set_int(const char *key, int val);
void sysc_send_line(const char *line);

void dbg_init(void);

void fonts_init(void);
void theme_apply(lv_display_t *d);
