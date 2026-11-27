#include "discsys.h"

#include <disc/settings.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define AGENT_PATH "/org/discfw/agent"
#define PLAYER_PATH "/org/discfw/player"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define A2DP_SINK "0000110b-0000-1000-8000-00805f9b34fb"
#define CALL_MS 30000
#define UPDATE_MS 200
#define HEADSET_LOW_PCT 20

static GDBusObjectManager *om;
static GDBusConnection *bus;
static guint agent_id, player_id, scan_src, update_src;
static gint64 pair_until;
static char player_adapter[64];

static const char agent_xml[] = "<node><interface name='org.bluez.Agent1'>"
				"<method name='Release'/>"
				"<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
				"<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
				"<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
				"<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/>"
				"<arg type='q' direction='in'/></method>"
				"<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
				"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
				"<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
				"<method name='Cancel'/>"
				"</interface></node>";

static void agent_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *method,
		       GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
	(void)c;
	(void)sender;
	(void)path;
	(void)iface;
	(void)params;
	(void)data;
	if (strcmp(method, "Release") && strcmp(method, "Cancel") && g_get_monotonic_time() > pair_until) {
		logmsg(LOG_WARNING, "bluetooth: rejected %s outside a pairing window", method);
		g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "not pairing");
		return;
	}
	if (!strcmp(method, "RequestPinCode"))
		g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", "0000"));
	else if (!strcmp(method, "RequestPasskey"))
		g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", 0));
	else
		g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable agent_vtable = { .method_call = agent_call };

static const char player_xml[] = "<node><interface name='" PLAYER_IFACE "'>"
				 "<method name='Play'/><method name='Pause'/><method name='PlayPause'/><method name='Stop'/>"
				 "<method name='Next'/><method name='Previous'/>"
				 "</interface></node>";

static void player_call(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *method,
			GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
	bool playing = sys.play == PS_PLAY;

	(void)c;
	(void)sender;
	(void)path;
	(void)iface;
	(void)params;
	(void)data;
	logmsg(LOG_INFO, "bluetooth: headset %s", method);
	if (!strcmp(method, "Next"))
		action_run("next");
	else if (!strcmp(method, "Previous"))
		action_run("prev");
	else if (!strcmp(method, "PlayPause") || (!strcmp(method, "Play") ? !playing : playing))
		action_run("play_pause");
	g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable player_vtable = { .method_call = player_call };

static void add_status(GVariantBuilder *b)
{
	g_variant_builder_add(b, "{sv}", "PlaybackStatus",
			      g_variant_new_string(sys.play == PS_PLAY ? "Playing" : sys.play == PS_PAUSE ? "Paused" : "Stopped"));
}

static void call_done(GObject *src, GAsyncResult *res, gpointer data)
{
	GError *e = NULL;
	GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);

	if (v)
		g_variant_unref(v);
	if (e) {
		logmsg(LOG_WARNING, "bluez %s: %s", (const char *)data, e->message);
		g_error_free(e);
	}
}

static void register_agent(void)
{
	if (!bus)
		return;
	g_dbus_connection_call(bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
			       g_variant_new("(os)", AGENT_PATH, "NoInputNoOutput"), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
			       call_done, "RegisterAgent");
	g_dbus_connection_call(bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent",
			       g_variant_new("(o)", AGENT_PATH), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, call_done,
			       "RequestDefaultAgent");
}

static void register_player(const char *adapter_path)
{
	static const char *const caps[] = { "CanControl", "CanPlay", "CanPause", "CanGoNext", "CanGoPrevious" };
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	add_status(&b);
	for (size_t i = 0; i < G_N_ELEMENTS(caps); i++)
		g_variant_builder_add(&b, "{sv}", caps[i], g_variant_new_boolean(TRUE));
	g_variant_builder_add(&b, "{sv}", "Identity", g_variant_new_string("discfw"));
	g_dbus_connection_call(bus, "org.bluez", adapter_path, "org.bluez.Media1", "RegisterPlayer",
			       g_variant_new("(o@a{sv})", PLAYER_PATH, g_variant_builder_end(&b)), NULL, G_DBUS_CALL_FLAGS_NONE,
			       5000, NULL, call_done, "RegisterPlayer");
}

static void player_sync(void)
{
	GList *objs = om && player_id ? g_dbus_object_manager_get_objects(om) : NULL;
	const char *path = NULL;

	for (GList *l = objs; l && !path; l = l->next) {
		GDBusInterface *i = g_dbus_object_get_interface(l->data, "org.bluez.Media1");
		if (i) {
			path = g_dbus_object_get_object_path(l->data);
			g_object_unref(i);
		}
	}
	if (!path) {
		player_adapter[0] = 0;
	} else if (strcmp(path, player_adapter)) {
		snprintf(player_adapter, sizeof(player_adapter), "%s", path);
		logmsg(LOG_INFO, "bluetooth: registering the headset button player on %s", path);
		register_player(path);
	}
	g_list_free_full(objs, g_object_unref);
}

void bt_play_state(void)
{
	GVariantBuilder b;

	if (!bus || !player_adapter[0])
		return;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	add_status(&b);
	g_dbus_connection_emit_signal(bus, NULL, PLAYER_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
				      g_variant_new("(sa{sv}as)", PLAYER_IFACE, &b, NULL), NULL);
}

static GDBusProxy *iface_of(GDBusObject *o, const char *name)
{
	return G_DBUS_PROXY(g_dbus_object_get_interface(o, name));
}

static GDBusProxy *adapter(void)
{
	GList *objs = om ? g_dbus_object_manager_get_objects(om) : NULL;
	GDBusProxy *p = NULL;

	for (GList *l = objs; l && !p; l = l->next)
		p = iface_of(l->data, "org.bluez.Adapter1");
	g_list_free_full(objs, g_object_unref);
	return p;
}

static bool prop_bool(GDBusProxy *p, const char *name)
{
	GVariant *v = g_dbus_proxy_get_cached_property(p, name);
	bool r = v && g_variant_get_boolean(v);

	if (v)
		g_variant_unref(v);
	return r;
}

static void prop_str(GDBusProxy *p, const char *name, char *out, size_t len)
{
	GVariant *v = g_dbus_proxy_get_cached_property(p, name);

	snprintf(out, len, "%s", v ? g_variant_get_string(v, NULL) : "");
	if (v)
		g_variant_unref(v);
}

static int prop_int(GDBusProxy *p, const char *name, int def)
{
	GVariant *v = g_dbus_proxy_get_cached_property(p, name);
	int r = def;

	if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_INT16))
		r = g_variant_get_int16(v);
	else if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE))
		r = g_variant_get_byte(v);
	if (v)
		g_variant_unref(v);
	return r;
}

static bool has_a2dp_sink(GDBusProxy *p)
{
	GVariant *v = g_dbus_proxy_get_cached_property(p, "UUIDs");
	bool r = false;

	if (v) {
		const gchar **uuids = g_variant_get_strv(v, NULL);
		for (int i = 0; uuids && uuids[i]; i++)
			if (!g_ascii_strcasecmp(uuids[i], A2DP_SINK))
				r = true;
		g_free(uuids);
		g_variant_unref(v);
	}
	return r;
}

static bool is_audio(GDBusProxy *d)
{
	char icon[48];

	prop_str(d, "Icon", icon, sizeof(icon));
	return !strncmp(icon, "audio", 5) || has_a2dp_sink(d);
}

static int battery_of(GDBusObject *o)
{
	GDBusProxy *b = iface_of(o, "org.bluez.Battery1");
	int r = b ? prop_int(b, "Percentage", -1) : -1;

	if (b)
		g_object_unref(b);
	return r;
}

static void headset_set(const char *name, int bat)
{
	static bool warned;
	struct kvbuf b;
	char msg[96];

	if (!strcmp(name, sys.bt_name) && bat == sys.bt_bat)
		return;
	snprintf(sys.bt_name, sizeof(sys.bt_name), "%s", name);
	sys.bt_bat = bat;
	kv_init(&b);
	kv_raw(&b, "!bt");
	kv_str(&b, "name", name);
	kv_int(&b, "battery", bat);
	ev_kv(&b);
	if (bat < 0 || bat > HEADSET_LOW_PCT) {
		warned = false;
	} else if (!warned) {
		warned = true;
		snprintf(msg, sizeof(msg), "Headphones battery low: %d%%", bat);
		ev_toast(msg);
	}
}

static gboolean headset_update(gpointer data)
{
	GList *objs = om ? g_dbus_object_manager_get_objects(om) : NULL;
	char name[64] = "";
	int bat = -1;
	bool found = false;

	(void)data;
	update_src = 0;
	for (GList *l = objs; l && !found; l = l->next) {
		GDBusProxy *d = iface_of(l->data, "org.bluez.Device1");
		if (!d)
			continue;
		if (prop_bool(d, "Connected") && is_audio(d)) {
			found = true;
			prop_str(d, "Alias", name, sizeof(name));
			bat = battery_of(l->data);
		}
		g_object_unref(d);
	}
	g_list_free_full(objs, g_object_unref);
	headset_set(name, bat);
	return G_SOURCE_REMOVE;
}

static void schedule_update(void)
{
	if (!update_src)
		update_src = g_timeout_add(UPDATE_MS, headset_update, NULL);
}

static void on_object(GDBusObjectManager *m, GDBusObject *o, gpointer data)
{
	(void)m;
	(void)o;
	(void)data;
	player_sync();
	schedule_update();
}

static void on_iface(GDBusObjectManager *m, GDBusObject *o, GDBusInterface *i, gpointer data)
{
	(void)i;
	on_object(m, o, data);
}

static void on_props(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *p, GVariant *changed,
		     const gchar *const *invalidated, gpointer data)
{
	const char *name = g_dbus_proxy_get_interface_name(p);

	(void)m;
	(void)o;
	(void)changed;
	(void)invalidated;
	(void)data;
	if (!strcmp(name, "org.bluez.Device1") || !strcmp(name, "org.bluez.Battery1"))
		schedule_update();
}

static void on_owner(GObject *obj, GParamSpec *spec, gpointer data)
{
	gchar *owner = g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(obj));

	(void)spec;
	(void)data;
	if (owner) {
		logmsg(LOG_INFO, "bluetoothd up, registering agent");
		register_agent();
		player_sync();
	}
	g_free(owner);
	schedule_update();
}

static void om_ready(GObject *src, GAsyncResult *res, gpointer data)
{
	GError *e = NULL;
	GDBusNodeInfo *info;

	(void)src;
	(void)data;
	om = g_dbus_object_manager_client_new_for_bus_finish(res, &e);
	if (!om) {
		logmsg(LOG_ERR, "bluez object manager: %s", e->message);
		g_error_free(e);
		return;
	}
	bus = g_dbus_object_manager_client_get_connection(G_DBUS_OBJECT_MANAGER_CLIENT(om));
	info = g_dbus_node_info_new_for_xml(agent_xml, NULL);
	agent_id = g_dbus_connection_register_object(bus, AGENT_PATH, info->interfaces[0], &agent_vtable, NULL, NULL, &e);
	g_dbus_node_info_unref(info);
	if (!agent_id) {
		logmsg(LOG_ERR, "agent export: %s", e->message);
		g_clear_error(&e);
	}
	info = g_dbus_node_info_new_for_xml(player_xml, NULL);
	player_id = g_dbus_connection_register_object(bus, PLAYER_PATH, info->interfaces[0], &player_vtable, NULL, NULL, &e);
	g_dbus_node_info_unref(info);
	if (!player_id) {
		logmsg(LOG_ERR, "player export: %s", e->message);
		g_clear_error(&e);
	}
	g_signal_connect(om, "notify::name-owner", G_CALLBACK(on_owner), NULL);
	g_signal_connect(om, "object-added", G_CALLBACK(on_object), NULL);
	g_signal_connect(om, "object-removed", G_CALLBACK(on_object), NULL);
	g_signal_connect(om, "interface-added", G_CALLBACK(on_iface), NULL);
	g_signal_connect(om, "interface-removed", G_CALLBACK(on_iface), NULL);
	g_signal_connect(om, "interface-proxy-properties-changed", G_CALLBACK(on_props), NULL);
	on_owner(G_OBJECT(om), NULL, NULL);
}

void bt_init(void)
{
	sys.bt_bat = -1;
	if (sys.fake_hw) {
		snprintf(sys.bt_name, sizeof(sys.bt_name), "Fake headset");
		sys.bt_bat = 60;
		return;
	}
	g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, "org.bluez", "/",
						 NULL, NULL, NULL, NULL, om_ready, NULL);
}

static GDBusProxy *device(const char *mac)
{
	GList *objs = om ? g_dbus_object_manager_get_objects(om) : NULL;
	GDBusProxy *found = NULL;
	char addr[24];

	for (GList *l = objs; l && !found; l = l->next) {
		GDBusProxy *d = iface_of(l->data, "org.bluez.Device1");
		if (!d)
			continue;
		prop_str(d, "Address", addr, sizeof(addr));
		if (!g_ascii_strcasecmp(addr, mac))
			found = d;
		else
			g_object_unref(d);
	}
	g_list_free_full(objs, g_object_unref);
	return found;
}

static bool unnamed(const char *name, const char *addr)
{
	if (!name[0])
		return true;
	for (; *name && *addr; name++, addr++)
		if (*name != *addr && !(*name == '-' && *addr == ':'))
			return false;
	return !*name && !*addr;
}

static void list(struct kvbuf *out)
{
	GList *objs = om ? g_dbus_object_manager_get_objects(om) : NULL;
	GDBusProxy *a = adapter();
	struct kvbuf b;
	char s[128];

	kv_init(&b);
	kv_raw(&b, "= adapter");
	kv_int(&b, "present", a != NULL);
	if (a) {
		kv_int(&b, "powered", prop_bool(a, "Powered"));
		kv_int(&b, "discovering", prop_bool(a, "Discovering"));
		prop_str(a, "Address", s, sizeof(s));
		kv_str(&b, "address", s);
		prop_str(a, "Alias", s, sizeof(s));
		kv_str(&b, "name", s);
		g_object_unref(a);
	}
	kv_int(&b, "enabled", set_get_bool("bt.enabled"));
	kv_end(&b);
	kv_append(out, b.s);
	kv_free(&b);
	for (GList *l = objs; l; l = l->next) {
		GDBusProxy *d = iface_of(l->data, "org.bluez.Device1");
		char addr[24], name[128], icon[48];
		bool paired, audio;
		if (!d)
			continue;
		prop_str(d, "Address", addr, sizeof(addr));
		prop_str(d, "Alias", name, sizeof(name));
		prop_str(d, "Icon", icon, sizeof(icon));
		paired = prop_bool(d, "Paired");
		audio = is_audio(d);
		if (paired || (audio && !unnamed(name, addr))) {
			kv_init(&b);
			kv_raw(&b, "= device");
			kv_str(&b, "mac", addr);
			kv_str(&b, "name", name);
			kv_str(&b, "icon", icon);
			kv_int(&b, "paired", paired);
			kv_int(&b, "connected", prop_bool(d, "Connected"));
			kv_int(&b, "trusted", prop_bool(d, "Trusted"));
			kv_int(&b, "audio", audio);
			kv_int(&b, "rssi", prop_int(d, "RSSI", 0));
			kv_int(&b, "battery", battery_of(l->data));
			kv_end(&b);
			kv_append(out, b.s);
			kv_free(&b);
		}
		g_object_unref(d);
	}
	g_list_free_full(objs, g_object_unref);
}

static void toast_result(GObject *src, GAsyncResult *res, gpointer data)
{
	GError *e = NULL;
	GVariant *v = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &e);
	char msg[200];

	if (v)
		g_variant_unref(v);
	if (e) {
		const char *m = strrchr(e->message, ':');
		snprintf(msg, sizeof(msg), "%s failed%s%s", (const char *)data, m ? ":" : "", m ? m + 1 : "");
		logmsg(LOG_WARNING, "bluetooth %s: %s", (const char *)data, e->message);
		g_error_free(e);
	} else {
		snprintf(msg, sizeof(msg), "%s: done", (const char *)data);
	}
	ev_toast(msg);
}

static void paired(GObject *src, GAsyncResult *res, gpointer data)
{
	GDBusProxy *d = G_DBUS_PROXY(src);
	GError *e = NULL;
	GVariant *v = g_dbus_proxy_call_finish(d, res, &e);

	(void)data;
	if (v)
		g_variant_unref(v);
	if (e && !strstr(e->message, "AlreadyExists")) {
		char msg[200];
		const char *m = strrchr(e->message, ':');
		snprintf(msg, sizeof(msg), "Pairing failed%s%s", m ? ":" : "", m ? m + 1 : "");
		ev_toast(msg);
		g_error_free(e);
		g_object_unref(d);
		return;
	}
	g_clear_error(&e);
	g_dbus_proxy_call(d, "org.freedesktop.DBus.Properties.Set",
			  g_variant_new("(ssv)", "org.bluez.Device1", "Trusted", g_variant_new_boolean(TRUE)), G_DBUS_CALL_FLAGS_NONE,
			  5000, NULL, NULL, NULL);
	ev_toast("Paired, connecting…");
	g_dbus_proxy_call(d, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, CALL_MS, NULL, toast_result, "Connect");
	g_object_unref(d);
}

static gboolean scan_stop(gpointer data)
{
	GDBusProxy *a = adapter();

	(void)data;
	scan_src = 0;
	if (a) {
		g_dbus_proxy_call(a, "StopDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
		g_object_unref(a);
	}
	return G_SOURCE_REMOVE;
}

static void scan(int secs)
{
	GDBusProxy *a = adapter();
	GVariantBuilder f;

	if (!a)
		return;
	g_variant_builder_init(&f, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&f, "{sv}", "Transport", g_variant_new_string("bredr"));
	g_dbus_proxy_call(a, "SetDiscoveryFilter", g_variant_new("(a{sv})", &f), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
	g_dbus_proxy_call(a, "StartDiscovery", NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
	if (scan_src)
		g_source_remove(scan_src);
	scan_src = g_timeout_add_seconds(secs > 0 ? secs : 20, scan_stop, NULL);
	g_object_unref(a);
}

int bt_command(const char *verb, const char *mac, struct kvbuf *out, char *err, size_t errlen)
{
	GDBusProxy *d, *a;

	if (!strcmp(verb, "list")) {
		list(out);
		return 0;
	}
	if (!om || !(a = adapter())) {
		snprintf(err, errlen, "Bluetooth is off");
		return -1;
	}
	if (!strcmp(verb, "scan")) {
		g_object_unref(a);
		scan(atoi(mac ? mac : "0"));
		return 0;
	}
	if (!mac || !(d = device(mac))) {
		g_object_unref(a);
		snprintf(err, errlen, "unknown device");
		return -1;
	}
	if (!strcmp(verb, "pair")) {
		pair_until = g_get_monotonic_time() + (gint64)(CALL_MS + 5000) * 1000;
		scan_stop(NULL);
		if (prop_bool(d, "Paired"))
			g_dbus_proxy_call(d, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, CALL_MS, NULL, toast_result, "Connect");
		else
			g_dbus_proxy_call(d, "Pair", NULL, G_DBUS_CALL_FLAGS_NONE, CALL_MS, NULL, paired, g_object_ref(d));
	} else if (!strcmp(verb, "connect")) {
		g_dbus_proxy_call(d, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, CALL_MS, NULL, toast_result, "Connect");
	} else if (!strcmp(verb, "disconnect")) {
		g_dbus_proxy_call(d, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, CALL_MS, NULL, toast_result, "Disconnect");
	} else if (!strcmp(verb, "forget")) {
		g_dbus_proxy_call(a, "RemoveDevice", g_variant_new("(o)", g_dbus_proxy_get_object_path(d)), G_DBUS_CALL_FLAGS_NONE,
				  5000, NULL, toast_result, "Forget");
	} else {
		snprintf(err, errlen, "usage: bt list|scan [SECS]|pair|connect|disconnect|forget MAC");
		g_object_unref(d);
		g_object_unref(a);
		return -1;
	}
	g_object_unref(d);
	g_object_unref(a);
	return 0;
}
