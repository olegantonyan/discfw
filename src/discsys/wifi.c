#include "discsys.h"

#include <arpa/inet.h>
#include <disc/paths.h>
#include <disc/settings.h>
#include <ctype.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#define CTRL_DIR "/run/wpa_supplicant/"
#define IFACE "wlan0"
#define TIMEOUT_MS 3000
#define REPLY_MAX 16384
#define REENABLE_S 20

static guint reenable_src;

static int wpa_request(const char *cmd, char *reply, size_t len)
{
	struct sockaddr_un local = { .sun_family = AF_UNIX }, dest = { .sun_family = AF_UNIX };
	struct pollfd pfd;
	ssize_t n;
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	snprintf(local.sun_path, sizeof(local.sun_path), "%s", disc_run_path("discsys-wpa"));
	unlink(local.sun_path);
	snprintf(dest.sun_path, sizeof(dest.sun_path), CTRL_DIR IFACE);
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) || connect(fd, (struct sockaddr *)&dest, sizeof(dest)) ||
	    send(fd, cmd, strlen(cmd), 0) < 0) {
		close(fd);
		unlink(local.sun_path);
		return -1;
	}
	pfd = (struct pollfd){ .fd = fd, .events = POLLIN };
	n = poll(&pfd, 1, TIMEOUT_MS) == 1 ? recv(fd, reply, len - 1, 0) : -1;
	close(fd);
	unlink(local.sun_path);
	if (n < 0)
		return -1;
	reply[n] = 0;
	return 0;
}

static bool wpa_ok(const char *cmd)
{
	char r[64];
	return wpa_request(cmd, r, sizeof(r)) == 0 && !strncmp(r, "OK", 2);
}

static void unescape(const char *s, char *out, size_t len)
{
	size_t o = 0;

	while (*s && o + 1 < len) {
		if (s[0] == '\\' && s[1] == 'x' && s[2] && s[3]) {
			char hex[3] = { s[2], s[3], 0 };
			out[o++] = (char)strtol(hex, NULL, 16);
			s += 4;
		} else if (s[0] == '\\' && s[1]) {
			out[o++] = s[1];
			s += 2;
		} else {
			out[o++] = *s++;
		}
	}
	out[o] = 0;
}

static void hex(const unsigned char *in, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++)
		sprintf(out + i * 2, "%02x", in[i]);
	out[n * 2] = 0;
}

void wifi_psk(const char *pass, const char *ssid, char out[65])
{
	unsigned char res[40], u[20], t[20];
	GHmac *base = g_hmac_new(G_CHECKSUM_SHA1, (const guchar *)pass, strlen(pass));
	size_t slen = strlen(ssid);

	for (int block = 1; block <= 2; block++) {
		unsigned char cnt[4] = { 0, 0, 0, (unsigned char)block };
		GHmac *h = g_hmac_copy(base);
		gsize dlen = sizeof(u);
		g_hmac_update(h, (const guchar *)ssid, slen);
		g_hmac_update(h, cnt, 4);
		g_hmac_get_digest(h, u, &dlen);
		g_hmac_unref(h);
		memcpy(t, u, 20);
		for (int i = 1; i < 4096; i++) {
			h = g_hmac_copy(base);
			dlen = sizeof(u);
			g_hmac_update(h, u, 20);
			g_hmac_get_digest(h, u, &dlen);
			g_hmac_unref(h);
			for (int j = 0; j < 20; j++)
				t[j] ^= u[j];
		}
		memcpy(res + (block - 1) * 20, t, 20);
	}
	g_hmac_unref(base);
	hex(res, 32, out);
}

static bool running(void)
{
	char r[64];
	return wpa_request("PING", r, sizeof(r)) == 0 && !strncmp(r, "PONG", 4);
}

struct net {
	char ssid[128];
	int signal, freq, id;
	bool secure, known, current, failed;
};

static int find(struct net *v, int n, const char *ssid)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(v[i].ssid, ssid))
			return i;
	return -1;
}

static int collect(struct net *v, int max)
{
	char *buf = malloc(REPLY_MAX), *save = NULL, *line;
	int n = 0;

	if (!buf)
		return 0;
	if (wpa_request("LIST_NETWORKS", buf, REPLY_MAX) == 0) {
		for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char *f[4] = { 0 }, *p = line;
			if (!isdigit((unsigned char)line[0]) || n >= max)
				continue;
			for (int i = 0; i < 4 && p; i++) {
				f[i] = p;
				p = strchr(p, '\t');
				if (p)
					*p++ = 0;
			}
			struct net *x = &v[n++];
			memset(x, 0, sizeof(*x));
			x->id = atoi(f[0]);
			unescape(f[1] ? f[1] : "", x->ssid, sizeof(x->ssid));
			x->known = true;
			x->current = f[3] && strstr(f[3], "CURRENT");
			x->failed = f[3] && strstr(f[3], "TEMP-DISABLED");
			x->signal = -200;
			x->secure = true;
		}
	}
	save = NULL;
	if (wpa_request("SCAN_RESULTS", buf, REPLY_MAX) == 0) {
		for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char *f[5] = { 0 }, *p = line, ssid[128];
			if (!strncmp(line, "bssid", 5))
				continue;
			for (int i = 0; i < 5 && p; i++) {
				f[i] = p;
				p = strchr(p, '\t');
				if (p)
					*p++ = 0;
			}
			if (!f[4])
				continue;
			unescape(f[4], ssid, sizeof(ssid));
			if (!ssid[0])
				continue;
			int i = find(v, n, ssid);
			if (i < 0) {
				if (n >= max)
					continue;
				i = n++;
				memset(&v[i], 0, sizeof(v[i]));
				snprintf(v[i].ssid, sizeof(v[i].ssid), "%s", ssid);
				v[i].id = -1;
				v[i].signal = -200;
			}
			int sig = atoi(f[2]);
			if (sig > v[i].signal) {
				v[i].signal = sig;
				v[i].freq = atoi(f[1]);
			}
			v[i].secure = strstr(f[3], "WPA") || strstr(f[3], "WEP") || strstr(f[3], "RSN") || strstr(f[3], "SAE");
		}
	}
	free(buf);
	return n;
}

static int cmp_net(const void *a, const void *b)
{
	const struct net *x = a, *y = b;

	if (x->current != y->current)
		return x->current ? -1 : 1;
	if ((x->signal > -200) != (y->signal > -200))
		return x->signal > -200 ? -1 : 1;
	if (x->known != y->known)
		return x->known ? -1 : 1;
	return y->signal - x->signal;
}

static void reply_line(struct kvbuf *out, struct kvbuf *b)
{
	kv_end(b);
	kv_append(out, b->s);
	kv_free(b);
}

static void ipv4(const char *ifname, char *out, size_t len)
{
	struct ifreq ifr = { 0 };
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	out[0] = 0;
	if (fd < 0)
		return;
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	if (ioctl(fd, SIOCGIFADDR, &ifr) == 0)
		snprintf(out, len, "%s", inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));
	close(fd);
}

static void gateway(char *out, size_t len)
{
	char line[256], ifname[32];
	unsigned dest, gw;
	FILE *f = fopen("/proc/net/route", "r");

	out[0] = 0;
	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "%31s %x %x", ifname, &dest, &gw) == 3 && dest == 0 && gw) {
			struct in_addr a = { .s_addr = gw };
			snprintf(out, len, "%s", inet_ntoa(a));
			break;
		}
	if (f)
		fclose(f);
}

static void dns(char *out, size_t len)
{
	char line[256], ip[64];
	FILE *f = fopen("/etc/resolv.conf", "r");

	out[0] = 0;
	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "nameserver %63s", ip) == 1)
			snprintf(out + strlen(out), len - strlen(out), "%s%s", out[0] ? " " : "", ip);
	if (f)
		fclose(f);
}

static void status(struct kvbuf *out)
{
	char *buf = malloc(4096), *save = NULL, *line, v[128];
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "=");
	kv_int(&b, "enabled", set_get_bool("wifi.enabled"));
	kv_int(&b, "running", running());
	if (buf && wpa_request("STATUS", buf, 4096) == 0)
		for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			char *eq = strchr(line, '=');
			if (!eq)
				continue;
			*eq = 0;
			if (!strcmp(line, "ssid")) {
				unescape(eq + 1, v, sizeof(v));
				kv_str(&b, "ssid", v);
			} else if (!strcmp(line, "bssid") || !strcmp(line, "freq") || !strcmp(line, "wpa_state") ||
				   !strcmp(line, "key_mgmt") || !strcmp(line, "address")) {
				kv_str(&b, line, eq + 1);
			}
		}
	if (buf && wpa_request("SIGNAL_POLL", buf, 4096) == 0) {
		save = NULL;
		for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
			if (!strncmp(line, "RSSI=", 5))
				kv_str(&b, "rssi", line + 5);
			else if (!strncmp(line, "LINKSPEED=", 10))
				kv_str(&b, "speed", line + 10);
		}
	}
	free(buf);
	ipv4(IFACE, v, sizeof(v));
	kv_str(&b, "ip", v);
	gateway(v, sizeof(v));
	kv_str(&b, "gw", v);
	dns(v, sizeof(v));
	kv_str(&b, "dns", v);
	gethostname(v, sizeof(v));
	kv_str(&b, "host", v);
	reply_line(out, &b);
}

static gboolean reenable_all(gpointer data)
{
	(void)data;
	reenable_src = 0;
	wpa_ok("ENABLE_NETWORK all");
	wpa_ok("SAVE_CONFIG");
	return G_SOURCE_REMOVE;
}

static void select_net(int id)
{
	char cmd[48];

	snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", id);
	wpa_ok(cmd);
	if (reenable_src)
		g_source_remove(reenable_src);
	reenable_src = g_timeout_add_seconds(REENABLE_S, reenable_all, NULL);
}

static bool set_key(int id, const char *ssid, const char *pass)
{
	char cmd[128], psk[65];

	if (pass && *pass) {
		wifi_psk(pass, ssid, psk);
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk %s", id, psk);
		if (!wpa_ok(cmd))
			return false;
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt WPA-PSK", id);
	} else {
		snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", id);
	}
	return wpa_ok(cmd);
}

static int add_network(const char *ssid, const char *pass, bool hidden)
{
	char r[32], cmd[400], ssid_hex[260];
	int id;

	if (wpa_request("ADD_NETWORK", r, sizeof(r)) || !isdigit((unsigned char)r[0]))
		return -1;
	id = atoi(r);
	hex((const unsigned char *)ssid, strlen(ssid) > 32 ? 32 : strlen(ssid), ssid_hex);
	snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid %s", id, ssid_hex);
	if (!wpa_ok(cmd) || !set_key(id, ssid, pass))
		goto fail;
	snprintf(cmd, sizeof(cmd), "SET_NETWORK %d scan_ssid 1", id);
	if (hidden && !wpa_ok(cmd))
		goto fail;
	snprintf(cmd, sizeof(cmd), "ENABLE_NETWORK %d", id);
	wpa_ok(cmd);
	return id;
fail:
	snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", id);
	wpa_ok(cmd);
	return -1;
}

int wifi_command(const char *verb, const char *ssid, const char *pass, bool hidden, struct kvbuf *out, char *err, size_t errlen)
{
	struct net v[64];
	int n, i;

	if (!strcmp(verb, "status")) {
		status(out);
		return 0;
	}
	if (!running()) {
		snprintf(err, errlen, "Wi-Fi is off");
		return -1;
	}
	if (!strcmp(verb, "scan")) {
		char r[32];
		wpa_request("SCAN", r, sizeof(r));
		return 0;
	}
	n = collect(v, 64);
	if (!strcmp(verb, "list")) {
		qsort(v, n, sizeof(v[0]), cmp_net);
		for (i = 0; i < n; i++) {
			struct kvbuf b;
			kv_init(&b);
			kv_raw(&b, "=");
			kv_str(&b, "ssid", v[i].ssid);
			kv_int(&b, "signal", v[i].signal);
			kv_int(&b, "freq", v[i].freq);
			kv_int(&b, "secure", v[i].secure);
			kv_int(&b, "known", v[i].known);
			kv_int(&b, "current", v[i].current);
			kv_int(&b, "failed", v[i].failed);
			reply_line(out, &b);
		}
		return 0;
	}
	i = find(v, n, ssid ? ssid : "");
	if (!strcmp(verb, "connect")) {
		int id = i >= 0 && v[i].known ? v[i].id : -1;
		size_t plen = pass ? strlen(pass) : 0;
		if (!ssid || !*ssid || strlen(ssid) > 32) {
			snprintf(err, errlen, "network name must be 1–32 bytes");
			return -1;
		}
		if (plen && (plen < 8 || plen > 63)) {
			snprintf(err, errlen, "password must be 8–63 characters");
			return -1;
		}
		if (id >= 0 && plen && !set_key(id, ssid, pass)) {
			snprintf(err, errlen, "could not set password");
			return -1;
		}
		if (id < 0) {
			if (i >= 0 && v[i].secure && !plen) {
				snprintf(err, errlen, "password needed");
				return -1;
			}
			id = add_network(ssid, pass, hidden || i < 0);
			if (id < 0) {
				snprintf(err, errlen, "could not add network");
				return -1;
			}
		}
		select_net(id);
		logmsg(LOG_INFO, "wifi: connecting to network %d", id);
		return 0;
	}
	if (!strcmp(verb, "forget")) {
		char cmd[48];
		if (i < 0 || !v[i].known) {
			snprintf(err, errlen, "not a saved network");
			return -1;
		}
		snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", v[i].id);
		if (!wpa_ok(cmd) || !wpa_ok("SAVE_CONFIG")) {
			snprintf(err, errlen, "could not forget");
			return -1;
		}
		return 0;
	}
	snprintf(err, errlen, "usage: wifi status|scan|list|connect SSID [PASS]|forget SSID");
	return -1;
}
