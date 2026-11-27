#include "ui.h"

#include <disc/conn.h>
#include <disc/kv.h>
#include <disc/settings.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLY 24576
#define MAX_ITEMS 64

static char *request(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static char *request(const char *fmt, ...)
{
	char cmd[512], *reply = malloc(REPLY);
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd) - 1, fmt, ap);
	va_end(ap);
	strcat(cmd, "\n");
	if (!reply || disc_request(DISCSYS_SOCK, cmd, reply, REPLY, 5000) < 0) {
		free(reply);
		return NULL;
	}
	return reply;
}

static bool reply_ok(const char *r, char *err, size_t len)
{
	const char *last = r, *p;

	for (p = r; (p = strchr(p, '\n')) && p[1]; p++)
		last = p + 1;
	if (!strncmp(last, "ok", 2))
		return true;
	kv_get(last, "msg", err, len);
	return false;
}

static void send_simple(const char *cmd)
{
	char *r = request("%s", cmd), err[160] = "";

	if (!r) {
		ui_toast("System service not running");
		return;
	}
	if (!reply_ok(r, err, sizeof(err)))
		ui_toast(err);
	free(r);
}

static void enc(char *out, size_t len, const char *key, const char *val)
{
	struct kvbuf b;

	kv_init(&b);
	kv_str(&b, key, val);
	snprintf(out, len, "%s", b.s ? b.s : "");
	kv_free(&b);
}

struct net {
	char ssid[128];
	int signal;
	bool secure, known, current, failed;
};

struct wifi {
	lv_obj_t *scr, *list;
	lv_timer_t *timer;
	struct net v[MAX_ITEMS];
	int n;
	char status[1024], pending[128];
	bool running, hidden;
	int target;
};

static void wifi_load(struct wifi *w)
{
	char *r = request("wifi status"), *line, *save = NULL;

	w->running = false;
	w->status[0] = 0;
	if (r) {
		line = strtok_r(r, "\n", &save);
		if (line && line[0] == '=') {
			snprintf(w->status, sizeof(w->status), "%s", line);
			w->running = kv_get_int(line, "running", 0);
		}
		free(r);
	}
	w->n = 0;
	if (!w->running)
		return;
	r = request("wifi list");
	save = NULL;
	for (line = r ? strtok_r(r, "\n", &save) : NULL; line && w->n < MAX_ITEMS; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] != '=')
			continue;
		struct net *x = &w->v[w->n++];
		kv_get(line, "ssid", x->ssid, sizeof(x->ssid));
		x->signal = kv_get_int(line, "signal", -200);
		x->secure = kv_get_int(line, "secure", 0);
		x->known = kv_get_int(line, "known", 0);
		x->current = kv_get_int(line, "current", 0);
		x->failed = kv_get_int(line, "failed", 0);
	}
	free(r);
}

static int wifi_count(void *ctx)
{
	struct wifi *w = ctx;
	return 1 + w->n;
}

static void wifi_row(void *ctx, int idx, struct vrow *r)
{
	struct wifi *w = ctx;
	char ssid[128], state[32], ip[32];

	if (idx == 0) {
		r->header = true;
		if (!set_get_bool("wifi.enabled") || !w->running) {
			snprintf(r->text, sizeof(r->text), "Wi-Fi is off");
			snprintf(r->sub, sizeof(r->sub), "Tap to turn on");
			return;
		}
		kv_get(w->status, "ssid", ssid, sizeof(ssid));
		kv_get(w->status, "wpa_state", state, sizeof(state));
		kv_get(w->status, "ip", ip, sizeof(ip));
		if (!strcmp(state, "COMPLETED")) {
			snprintf(r->text, sizeof(r->text), "%s", ssid);
			snprintf(r->sub, sizeof(r->sub), "%s · tap for details", ip[0] ? ip : "getting address…");
		} else {
			bool busy = strstr(state, "ASSOC") || strstr(state, "HANDSHAKE") || !strcmp(state, "AUTHENTICATING");
			snprintf(r->text, sizeof(r->text), "%s", busy ? "Connecting…" : "Not connected");
			snprintf(r->sub, sizeof(r->sub), "Tap a network to connect");
		}
		return;
	}
	struct net *x = &w->v[idx - 1];
	r->icon = LV_SYMBOL_WIFI;
	snprintf(r->text, sizeof(r->text), "%s", x->ssid);
	r->active = x->current;
	if (x->failed)
		snprintf(r->sub, sizeof(r->sub), "Couldn't connect · wrong password?");
	else if (x->signal > -200)
		snprintf(r->sub, sizeof(r->sub), "%s · %d dBm", x->current ? "Connected" : x->known ? "Saved" : x->secure ? "Secured" : "Open",
			 x->signal);
	else
		snprintf(r->sub, sizeof(r->sub), "Saved · not in range");
}

static void wifi_details(struct wifi *w)
{
	static const char *const keys[] = { "ssid", "wpa_state", "bssid", "freq", "rssi", "speed", "key_mgmt", "ip", "gw", "dns",
					    "address", "host" };
	static const char *const labels[] = { "SSID", "State", "BSSID", "Frequency (MHz)", "Signal (dBm)", "Link (Mbit/s)", "Security",
					      "IP", "Gateway", "DNS", "MAC", "Hostname" };
	char buf[1600] = "", v[160];

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		kv_get(w->status, keys[i], v, sizeof(v));
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s: %s\n", labels[i], v[0] ? v : "—");
	}
	ui_info("Wi-Fi status", buf);
}

static void wifi_refresh(struct wifi *w)
{
	int first = vlist_first(w->list);

	wifi_load(w);
	vlist_reload(w->list);
	vlist_scroll_to(w->list, first, false);
}

static void wifi_connect(struct wifi *w, const char *ssid, const char *pass, bool hidden)
{
	char a[300], b[300], *r, err[160] = "";

	enc(a, sizeof(a), "ssid", ssid);
	enc(b, sizeof(b), "pass", pass ? pass : "");
	r = request("wifi connect %s %s hidden=%d", a, b, hidden);
	if (!r) {
		ui_toast("System service not running");
		return;
	}
	if (reply_ok(r, err, sizeof(err)))
		ui_toast("Connecting…");
	else
		ui_toast(err);
	free(r);
	wifi_refresh(w);
}

static void password_entered(const char *text, void *ctx)
{
	struct wifi *w = ctx;

	wifi_connect(w, w->pending, text, w->hidden);
}

static void ask_password(struct wifi *w, const char *ssid, bool hidden)
{
	char title[200];

	snprintf(w->pending, sizeof(w->pending), "%s", ssid);
	w->hidden = hidden;
	snprintf(title, sizeof(title), hidden ? "Password (empty if open)" : "Password: %s", ssid);
	ui_text_input(title, "", false, password_entered, w);
}

static void hidden_entered(const char *text, void *ctx)
{
	if (*text)
		ask_password(ctx, text, true);
}

static void wifi_click(void *ctx, int idx)
{
	struct wifi *w = ctx;

	if (idx == 0) {
		if (!set_get_bool("wifi.enabled") || !w->running)
			sysc_set("wifi.enabled", "1");
		else
			wifi_details(w);
		return;
	}
	struct net *x = &w->v[idx - 1];
	if (x->current) {
		wifi_details(w);
		return;
	}
	w->target = idx - 1;
	if ((x->known && !x->failed) || !x->secure)
		wifi_connect(w, x->ssid, NULL, false);
	else
		ask_password(w, x->ssid, false);
}

static void wifi_forget(void *ctx)
{
	struct wifi *w = ctx;
	char a[300], cmd[320];

	if (w->target < 0 || w->target >= w->n)
		return;
	enc(a, sizeof(a), "ssid", w->v[w->target].ssid);
	snprintf(cmd, sizeof(cmd), "wifi forget %s", a);
	send_simple(cmd);
	wifi_refresh(w);
}

enum { W_CONNECT, W_PASSWORD, W_FORGET, W_DETAILS };

static int wifi_actions(const struct net *x, int *act, const char **items)
{
	int n = 0;

	act[n] = x->current ? W_DETAILS : W_CONNECT;
	items[n++] = x->current ? "Status details" : "Connect";
	if (x->secure || x->known) {
		act[n] = W_PASSWORD;
		items[n++] = x->known ? "Change password…" : "Connect with password…";
	}
	if (x->known) {
		act[n] = W_FORGET;
		items[n++] = "Forget";
	}
	return n;
}

static void wifi_item_picked(int choice, void *ctx)
{
	struct wifi *w = ctx;
	const char *items[4];
	int act[4];

	if (w->target < 0 || w->target >= w->n || choice < 0 || choice >= wifi_actions(&w->v[w->target], act, items))
		return;
	switch (act[choice]) {
	case W_CONNECT:
		wifi_connect(w, w->v[w->target].ssid, NULL, false);
		break;
	case W_PASSWORD:
		ask_password(w, w->v[w->target].ssid, false);
		break;
	case W_FORGET:
		ui_confirm("Forget this network?", "Forget", wifi_forget, w);
		break;
	case W_DETAILS:
		wifi_details(w);
		break;
	}
}

static void wifi_hold(void *ctx, int idx)
{
	struct wifi *w = ctx;
	const char *items[4];
	int act[4];

	if (idx == 0)
		return;
	w->target = idx - 1;
	ui_menu(w->v[idx - 1].ssid, items, wifi_actions(&w->v[idx - 1], act, items), wifi_item_picked, w);
}

static const struct vlist_ops wifi_ops = { .count = wifi_count, .row = wifi_row, .click = wifi_click, .hold = wifi_hold };

static void wifi_title_picked(int idx, void *ctx)
{
	struct wifi *w = ctx;

	if (idx == 0) {
		send_simple("wifi scan");
		ui_toast("Scanning…");
	} else if (idx == 1) {
		sysc_set("wifi.enabled", set_get_bool("wifi.enabled") ? "0" : "1");
	} else if (idx == 2) {
		ui_text_input("Network name", "", false, hidden_entered, w);
	} else if (idx == 3) {
		wifi_details(w);
	}
}

static void wifi_title(lv_event_t *e)
{
	const char *items[] = { "Scan now", set_get_bool("wifi.enabled") ? "Turn Wi-Fi off" : "Turn Wi-Fi on", "Add hidden network…",
				"Status details" };
	ui_menu("Wi-Fi", items, 4, wifi_title_picked, lv_event_get_user_data(e));
}

static void wifi_tick(lv_timer_t *t)
{
	struct wifi *w = lv_timer_get_user_data(t);

	if (app.sys.screen_on && lv_screen_active() == w->scr && !ui_modal_active())
		wifi_refresh(w);
}

static void wifi_settings(uint32_t evs, void *ctx)
{
	(void)evs;
	wifi_refresh(ctx);
}

static void wifi_delete(lv_event_t *e)
{
	struct wifi *w = lv_event_get_user_data(e);

	lv_timer_delete(w->timer);
	free(w);
}

static lv_obj_t *wifi_create(void *arg)
{
	struct wifi *w = calloc(1, sizeof(*w));
	lv_obj_t *title;
	char *r;

	(void)arg;
	w->target = -1;
	w->scr = ui_screen("Wi-Fi", &title);
	lv_obj_set_clickable(title, true);
	lv_obj_set_ext_click_area(title, 12);
	lv_obj_add_event_cb(title, wifi_title, LV_EVENT_CLICKED, w);
	w->list = vlist_create(w->scr, &wifi_ops, w);
	lv_obj_set_size(w->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(w->list, 0, CONTENT_Y);
	if ((r = request("wifi scan")))
		free(r);
	wifi_load(w);
	vlist_reload(w->list);
	w->timer = lv_timer_create(wifi_tick, 3000, w);
	app_listen(w->scr, EV_SETTINGS, wifi_settings, w);
	lv_obj_add_event_cb(w->scr, wifi_delete, LV_EVENT_DELETE, w);
	return w->scr;
}

const struct screen_ops scr_wifi = { .name = "wifi", .create = wifi_create };

struct dev {
	char mac[24], name[128];
	bool paired, connected, audio;
	int rssi, battery;
};

struct bt {
	lv_obj_t *scr, *list;
	lv_timer_t *timer;
	struct dev v[MAX_ITEMS];
	int n;
	bool present, powered, discovering;
	char address[24];
	int target;
};

static void bt_load(struct bt *b)
{
	char *r = request("bt list"), *line, *save = NULL, kind[16];

	b->n = 0;
	b->present = false;
	for (line = r ? strtok_r(r, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] != '=')
			continue;
		kv_word(line, 1, kind, sizeof(kind));
		if (!strcmp(kind, "adapter")) {
			b->present = kv_get_int(line, "present", 0);
			b->powered = kv_get_int(line, "powered", 0);
			b->discovering = kv_get_int(line, "discovering", 0);
			kv_get(line, "address", b->address, sizeof(b->address));
		} else if (!strcmp(kind, "device") && b->n < MAX_ITEMS) {
			struct dev *d = &b->v[b->n++];
			kv_get(line, "mac", d->mac, sizeof(d->mac));
			kv_get(line, "name", d->name, sizeof(d->name));
			d->paired = kv_get_int(line, "paired", 0);
			d->connected = kv_get_int(line, "connected", 0);
			d->audio = kv_get_int(line, "audio", 0);
			d->rssi = kv_get_int(line, "rssi", 0);
			d->battery = kv_get_int(line, "battery", -1);
		}
	}
	free(r);
}

static int bt_rank(const struct dev *d)
{
	return d->connected ? 0 : d->paired ? 1 : 2;
}

static int cmp_dev(const void *x, const void *y)
{
	const struct dev *a = x, *b = y;

	if (bt_rank(a) != bt_rank(b))
		return bt_rank(a) - bt_rank(b);
	return strcmp(a->name, b->name);
}

static void bt_refresh(struct bt *b)
{
	int first = vlist_first(b->list);

	bt_load(b);
	qsort(b->v, b->n, sizeof(b->v[0]), cmp_dev);
	vlist_reload(b->list);
	vlist_scroll_to(b->list, first, false);
}

static int bt_count(void *ctx)
{
	return 1 + ((struct bt *)ctx)->n;
}

static void bt_row(void *ctx, int idx, struct vrow *r)
{
	struct bt *b = ctx;

	if (idx == 0) {
		r->header = true;
		if (!set_get_bool("bt.enabled") || !b->present) {
			snprintf(r->text, sizeof(r->text), "Bluetooth is off");
			snprintf(r->sub, sizeof(r->sub), "Tap to turn on");
		} else {
			snprintf(r->text, sizeof(r->text), "%s", b->discovering ? "Searching…" : "Bluetooth on");
			snprintf(r->sub, sizeof(r->sub), "Tap to search for devices");
		}
		return;
	}
	struct dev *d = &b->v[idx - 1];
	r->icon = LV_SYMBOL_BLUETOOTH;
	r->active = d->connected;
	snprintf(r->text, sizeof(r->text), "%s", d->name);
	if (d->connected && d->battery >= 0)
		snprintf(r->sub, sizeof(r->sub), "Connected · battery %d%%", d->battery);
	else if (d->connected)
		snprintf(r->sub, sizeof(r->sub), "Connected");
	else if (d->paired)
		snprintf(r->sub, sizeof(r->sub), "Paired · tap to connect");
	else
		snprintf(r->sub, sizeof(r->sub), "Tap to pair");
	if (!d->connected && d->rssi)
		snprintf(r->right, sizeof(r->right), "%d", d->rssi);
}

static void bt_do(struct bt *b, const char *verb)
{
	char cmd[96];

	if (b->target < 0 || b->target >= b->n)
		return;
	snprintf(cmd, sizeof(cmd), "bt %s %s", verb, b->v[b->target].mac);
	send_simple(cmd);
	bt_refresh(b);
}

static void bt_forget(void *ctx)
{
	bt_do(ctx, "forget");
}

static void bt_details(struct bt *b)
{
	struct dev *d = &b->v[b->target];
	char buf[600], bat[16] = "unknown", rssi[16] = "—";

	if (d->battery >= 0)
		snprintf(bat, sizeof(bat), "%d %%", d->battery);
	if (d->rssi)
		snprintf(rssi, sizeof(rssi), "%d dBm", d->rssi);
	snprintf(buf, sizeof(buf), "Name: %s\nAddress: %s\nPaired: %s\nConnected: %s\nAudio output: %s\nBattery: %s\nSignal: %s", d->name,
		 d->mac, d->paired ? "yes" : "no", d->connected ? "yes" : "no", d->audio ? "yes" : "no", bat, rssi);
	ui_info("Device", buf);
}

static void bt_item_picked(int choice, void *ctx)
{
	struct bt *b = ctx;
	struct dev *d;

	if (b->target < 0 || b->target >= b->n)
		return;
	d = &b->v[b->target];
	switch (choice) {
	case 0:
		bt_do(b, d->connected ? "disconnect" : d->paired ? "connect" : "pair");
		break;
	case 1:
		if (d->paired)
			ui_confirm("Forget this device?", "Forget", bt_forget, b);
		else
			bt_details(b);
		break;
	case 2:
		bt_details(b);
		break;
	}
}

static void bt_hold(void *ctx, int idx)
{
	struct bt *b = ctx;
	struct dev *d;
	const char *items[3];
	int n = 0;

	if (idx == 0)
		return;
	b->target = idx - 1;
	d = &b->v[b->target];
	items[n++] = d->connected ? "Disconnect" : d->paired ? "Connect" : "Pair and connect";
	if (d->paired)
		items[n++] = "Forget";
	items[n++] = "Details";
	ui_menu(d->name, items, n, bt_item_picked, b);
}

static void bt_click(void *ctx, int idx)
{
	struct bt *b = ctx;
	struct dev *d;

	if (idx == 0) {
		if (!set_get_bool("bt.enabled") || !b->present)
			sysc_set("bt.enabled", "1");
		else {
			send_simple("bt scan 20");
			ui_toast("Searching for 20 s…");
		}
		return;
	}
	b->target = idx - 1;
	d = &b->v[b->target];
	if (d->connected) {
		bt_hold(b, idx);
		return;
	}
	if (!d->paired)
		ui_toast("Pairing…");
	bt_do(b, d->paired ? "connect" : "pair");
}

static const struct vlist_ops bt_ops = { .count = bt_count, .row = bt_row, .click = bt_click, .hold = bt_hold };

static void bt_title_picked(int idx, void *ctx)
{
	(void)ctx;
	if (idx == 0) {
		send_simple("bt scan 20");
		ui_toast("Searching for 20 s…");
	} else {
		sysc_set("bt.enabled", set_get_bool("bt.enabled") ? "0" : "1");
	}
}

static void bt_title(lv_event_t *e)
{
	const char *items[] = { "Search for devices", set_get_bool("bt.enabled") ? "Turn Bluetooth off" : "Turn Bluetooth on" };
	ui_menu("Bluetooth", items, 2, bt_title_picked, lv_event_get_user_data(e));
}

static void bt_tick(lv_timer_t *t)
{
	struct bt *b = lv_timer_get_user_data(t);

	if (app.sys.screen_on && lv_screen_active() == b->scr && !ui_modal_active())
		bt_refresh(b);
}

static void bt_settings(uint32_t evs, void *ctx)
{
	(void)evs;
	bt_refresh(ctx);
}

static void bt_delete(lv_event_t *e)
{
	struct bt *b = lv_event_get_user_data(e);

	lv_timer_delete(b->timer);
	free(b);
}

static lv_obj_t *bt_create(void *arg)
{
	struct bt *b = calloc(1, sizeof(*b));
	lv_obj_t *title;

	(void)arg;
	b->target = -1;
	b->scr = ui_screen("Bluetooth", &title);
	lv_obj_set_clickable(title, true);
	lv_obj_set_ext_click_area(title, 12);
	lv_obj_add_event_cb(title, bt_title, LV_EVENT_CLICKED, b);
	b->list = vlist_create(b->scr, &bt_ops, b);
	lv_obj_set_size(b->list, SCR_W, SCR_H - CONTENT_Y);
	lv_obj_set_pos(b->list, 0, CONTENT_Y);
	bt_refresh(b);
	b->timer = lv_timer_create(bt_tick, 2000, b);
	app_listen(b->scr, EV_SETTINGS, bt_settings, b);
	lv_obj_add_event_cb(b->scr, bt_delete, LV_EVENT_DELETE, b);
	return b->scr;
}

const struct screen_ops scr_bt = { .name = "bluetooth", .create = bt_create };
