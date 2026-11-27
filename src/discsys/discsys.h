#pragma once
#include <disc/kv.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

enum play_state { PS_UNKNOWN, PS_STOP, PS_PLAY, PS_PAUSE };
enum screen_state { SCR_ON, SCR_DIM, SCR_OFF };
enum output { OUT_DAC, OUT_BT };
enum boot_mode { MODE_PLAYER, MODE_STORAGE, MODE_USBDAC };

struct bat {
	bool valid;
	int cap, mv, ma, temp;
	char state[16], vbus[16], stage[16];
};

struct sys {
	GMainLoop *loop;
	bool fake_hw;
	enum boot_mode mode;
	int uac_rate;
	struct bat bat;
	bool powered;
	enum screen_state screen;
	bool locked;
	enum play_state play;
	int volume;
	enum output output;
	int outputs_enabled;
	char bt_dev[24];
	char bt_name[64];
	int bt_bat;
	int sleep_left_s;
	bool shutting_down;
	gint64 last_activity;
};

extern struct sys sys;

gint64 now_ms(void);
void logmsg(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void server_init(void);
void ev_send(const char *line);
void ev_kv(struct kvbuf *b);
void ev_toast(const char *text);
bool server_ui_connected(void);
void server_ping_ui(void);

void cfg_init(void);
int cfg_set(const char *key, const char *val);
void cfg_flush(void);
void cfg_list(struct kvbuf *out, const char *prefix);

void state_init(void);
int state_get_int(const char *key, int def);
void state_set_int(const char *key, int val);
void state_flush(void);

void mpdc_init(void);
struct mpd_connection *mpdc(void);
bool mpdc_ok(void);
void mpdc_report(const char *what);
void mpdc_refresh(void);
void mpdc_apply_order(void);
void mpdc_apply_replaygain(void);
int mpdc_output_id(const char *name);

void library_init(void);
void library_on_mpd(void);
bool library_update(const char *reason);
bool library_mounted(void);

void hw_init(void);
void bt_init(void);
int bt_command(const char *verb, const char *mac, struct kvbuf *out, char *err, size_t errlen);
int wifi_command(const char *verb, const char *ssid, const char *pass, bool hidden, struct kvbuf *out, char *err, size_t errlen);
void wifi_psk(const char *pass, const char *ssid, char out[65]);
int hw_request(const char *cmd, char *reply, size_t len);
void hw_volpct(int pct);

void mode_init(void);
const char *mode_name(void);
bool mode_usb(void);
void mode_on_bat(void);
int mode_request(const char *name, char *err, size_t errlen);
void mode_uac(int rate);

void volume_init(void);
void volume_on_mpd_connect(void);
void volume_reapply(void);
void volume_on_mixer(int pct);
void volume_step(int delta);
void volume_set(int pct);
void volume_output_changed(void);

void keys_init(void);
bool keys_power_held(void);
int input_find(const char *name, char *path, size_t len);
void keys_simulate(const char *key, const char *press);
bool action_run(const char *name);

void screen_init(void);
void screen_set(enum screen_state s, bool lock);
void screen_toggle(void);
void screen_unlock(void);
void screen_activity(void);
void screen_ui_drawn(void);
void screen_reschedule(void);
void screen_brightness_changed(void);

void power_init(void);
void power_on_bat(void);
void power_on_play_state(void);
void power_sleep_timer(int minutes);
void power_off(bool reboot);
const char *power_profile(void);
int power_profile_int(const char *field);
void power_status(struct kvbuf *b);
