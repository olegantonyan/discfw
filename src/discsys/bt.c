#include "discsys.h"

#include <disc/settings.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define AGENT_PATH "/org/discfw/agent"
#define A2DP_SINK "0000110b-0000-1000-8000-00805f9b34fb"
#define CALL_MS 30000

static GDBusObjectManager *om;
static GDBusConnection *bus;
static guint agent_id, scan_src;
static gint64 pair_until;

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

static void on_owner(GObject *obj, GParamSpec *spec, gpointer data)
{
	gchar *owner = g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(obj));

	(void)spec;
	(void)data;
	if (owner) {
		logmsg(LOG_INFO, "bluetoothd up, registering agent");
		register_agent();
	}
	g_free(owner);
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
	g_signal_connect(om, "notify::name-owner", G_CALLBACK(on_owner), NULL);
	on_owner(G_OBJECT(om), NULL, NULL);
}

void bt_init(void)
{
	if (sys.fake_hw)
		return;
	g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, "org.bluez", "/",
						 NULL, NULL, NULL, NULL, om_ready, NULL);
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
		GDBusProxy *d = iface_of(l->data, "org.bluez.Device1"), *bat;
		char addr[24], name[128], icon[48];
		bool paired, audio;
		if (!d)
			continue;
		prop_str(d, "Address", addr, sizeof(addr));
		prop_str(d, "Alias", name, sizeof(name));
		prop_str(d, "Icon", icon, sizeof(icon));
		paired = prop_bool(d, "Paired");
		audio = !strncmp(icon, "audio", 5) || has_a2dp_sink(d);
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
			bat = iface_of(l->data, "org.bluez.Battery1");
			kv_int(&b, "battery", bat ? prop_int(bat, "Percentage", -1) : -1);
			if (bat)
				g_object_unref(bat);
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
