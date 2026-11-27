#include "discsys.h"

#include <disc/eq.h>
#include <disc/kv.h>
#include <disc/paths.h>
#include <disc/settings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define SAVE_DELAY_MS 2000
#define TEXT_MAX 8192
#define BT_PREFIX "eq.btname."

static const char *const outputs[] = { "dac", "spdif", "bt" };
static char *written[3];

static struct eq_preset dirty;
static char dirty_name[EQ_NAME_MAX];
static guint save_src;

static void bt_key(char *key, size_t len)
{
	size_t n = snprintf(key, len, BT_PREFIX);

	for (const unsigned char *p = (const unsigned char *)sys.bt_name; *p && n + 4 < len; p++)
		n += *p <= ' ' || *p == '=' || *p == '%' || *p == 0x7f ? (size_t)snprintf(key + n, len - n, "%%%02X", *p)
								      : (size_t)snprintf(key + n, len - n, "%c", *p);
}

static bool bt_device_active(void)
{
	return set_get_bool("eq.bt_per_device") && sys.bt_name[0];
}

static const char *bt_device_id(void)
{
	char key[256];

	if (!bt_device_active())
		return NULL;
	bt_key(key, sizeof(key));
	return state_get_str(key);
}

static const char *resolve(int out)
{
	const char *dev;

	if (out == 0)
		return set_get("eq.dac");
	if (out == 1)
		return set_get("eq.spdif");
	dev = bt_device_id();
	return dev ? dev : set_get("eq.bt");
}

static bool is_dirty(const char *id)
{
	return save_src && !strncmp(id, "my:", 3) && !strcmp(id + 3, dirty_name);
}

static int load(const char *id, struct eq_preset *p)
{
	if (is_dirty(id)) {
		*p = dirty;
		return 0;
	}
	return eq_store_load(id, p);
}

void eq_apply(void)
{
	for (int i = 0; i < 3; i++) {
		const char *id = resolve(i);
		char file[16], text[4096] = "";
		struct eq_preset p;

		if (id && id[0] && !load(id, &p) && eq_format(&p, text, sizeof(text)) < 0)
			text[0] = 0;
		if (written[i] && !strcmp(written[i], text))
			continue;
		snprintf(file, sizeof(file), "eq.%s", outputs[i]);
		if (eq_write_atomic(disc_run_path(file), text)) {
			logmsg(LOG_ERR, "eq: writing %s failed", file);
			continue;
		}
		free(written[i]);
		written[i] = strdup(text);
	}
}

static void announce(void)
{
	ev_send("!eq\n");
}

static gboolean save_now(gpointer data)
{
	(void)data;
	save_src = 0;
	if (eq_store_save(dirty_name, &dirty))
		logmsg(LOG_ERR, "eq: saving preset %s failed", dirty_name);
	return G_SOURCE_REMOVE;
}

void eq_flush(void)
{
	if (save_src) {
		g_source_remove(save_src);
		save_now(NULL);
	}
}

void eq_bt_changed(void)
{
	eq_apply();
	announce();
}

static bool exists(const char *name)
{
	char id[EQ_NAME_MAX + 4];
	struct eq_preset p;

	snprintf(id, sizeof(id), "my:%s", name);
	return is_dirty(id) || !eq_store_load(id, &p);
}

static void reassign(const char *old_id, const char *new_id)
{
	static const char *const keys[] = { "eq.dac", "eq.spdif", "eq.bt" };

	for (int i = 0; i < 3; i++)
		if (!strcmp(set_get(keys[i]), old_id))
			cfg_set(keys[i], new_id ? new_id : "");
	state_rewrite(BT_PREFIX, old_id, new_id);
}

static int put(const char *line, char *e, size_t elen)
{
	char name[EQ_NAME_MAX], *text = malloc(TEXT_MAX);
	struct eq_preset p;
	bool fresh;

	if (!text)
		return -1;
	if (!kv_get(line, "name", name, sizeof(name)) || !eq_name_valid(name)) {
		snprintf(e, elen, "invalid preset name");
		free(text);
		return -1;
	}
	if (!kv_get(line, "text", text, TEXT_MAX)) {
		snprintf(e, elen, "missing preset");
		free(text);
		return -1;
	}
	eq_parse(text, &p);
	free(text);
	if (save_src && strcmp(dirty_name, name))
		eq_flush();
	fresh = !exists(name);
	snprintf(dirty_name, sizeof(dirty_name), "%s", name);
	dirty = p;
	if (save_src)
		g_source_remove(save_src);
	save_src = g_timeout_add(SAVE_DELAY_MS, save_now, NULL);
	if (fresh)
		eq_flush();
	eq_apply();
	if (fresh)
		announce();
	return 0;
}

static int del(const char *line, char *e, size_t elen)
{
	char name[EQ_NAME_MAX], id[EQ_NAME_MAX + 4];

	if (!kv_get(line, "name", name, sizeof(name)) || !exists(name)) {
		snprintf(e, elen, "no such preset");
		return -1;
	}
	if (save_src && !strcmp(dirty_name, name)) {
		g_source_remove(save_src);
		save_src = 0;
	}
	if (eq_store_delete(name)) {
		snprintf(e, elen, "cannot delete");
		return -1;
	}
	snprintf(id, sizeof(id), "my:%s", name);
	reassign(id, NULL);
	eq_apply();
	announce();
	return 0;
}

static int rename_preset(const char *line, char *e, size_t elen)
{
	char from[EQ_NAME_MAX], to[EQ_NAME_MAX], old_id[EQ_NAME_MAX + 4], new_id[EQ_NAME_MAX + 4];
	struct eq_preset p;

	if (!kv_get(line, "name", from, sizeof(from)) || !kv_get(line, "to", to, sizeof(to)) || !eq_name_valid(to)) {
		snprintf(e, elen, "invalid preset name");
		return -1;
	}
	if (exists(to)) {
		snprintf(e, elen, "a preset with this name exists");
		return -1;
	}
	eq_flush();
	snprintf(old_id, sizeof(old_id), "my:%s", from);
	snprintf(new_id, sizeof(new_id), "my:%s", to);
	if (eq_store_load(old_id, &p) || eq_store_save(to, &p) || eq_store_delete(from)) {
		snprintf(e, elen, "cannot rename");
		return -1;
	}
	reassign(old_id, new_id);
	eq_apply();
	announce();
	return 0;
}

static int btdev(const char *line, char *e, size_t elen)
{
	char key[256], id[EQ_ID_MAX];

	if (!bt_device_active()) {
		snprintf(e, elen, "no Bluetooth headphones");
		return -1;
	}
	bt_key(key, sizeof(key));
	if (kv_get_int(line, "clear", 0))
		state_del(key);
	else if (kv_get(line, "id", id, sizeof(id)))
		state_set_str(key, id);
	else {
		snprintf(e, elen, "missing id");
		return -1;
	}
	eq_apply();
	announce();
	return 0;
}

int eq_command(const char *verb, const char *line, struct kvbuf *out, char *e, size_t elen)
{
	if (!strcmp(verb, "status")) {
		const char *dev = bt_device_id();
		kv_raw(out, "=eq");
		kv_str(out, "btname", bt_device_active() ? sys.bt_name : "");
		kv_int(out, "btset", dev != NULL);
		kv_str(out, "btdev", dev ? dev : "");
		kv_end(out);
		return 0;
	}
	if (!strcmp(verb, "put"))
		return put(line, e, elen);
	if (!strcmp(verb, "delete"))
		return del(line, e, elen);
	if (!strcmp(verb, "rename"))
		return rename_preset(line, e, elen);
	if (!strcmp(verb, "btdev"))
		return btdev(line, e, elen);
	snprintf(e, elen, "unknown eq command");
	return -1;
}

void eq_init(void)
{
	eq_apply();
}
