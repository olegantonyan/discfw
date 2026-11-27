#include "power.h"

#include <disc/kv.h>
#include <disc/settings.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

#define GAUGE "/sys/class/power_supply/cw221X-bat/"
#define TYPEC_STATE "/sys/bus/i2c/devices/2-0022/control/typec_state"
#define CHARGER_BUS "/dev/i2c-2"
#define CHARGER_ADDR 0x1a
#define REG_CTRL 0x01
#define CHG_CONFIG 0x10
#define REG_ICHG 0x02
#define ICHG_MASK 0x3f
#define ICHG_NORMAL 0x2d
#define ICHG_REDUCED 0x21
#define REG_STATUS 0x08
#define PG_STAT 0x04
#define TEMP_HYST 30

static const char *const stage_names[] = { "none", "pre", "fast", "done" };
static const char *const vbus_names[] = { "none", "sdp", "adapter", "3", "4", "5", "6", "otg" };
static int i2c = -1;
static int limit = 100, resume_drop = 5, ichg = ICHG_NORMAL, temp_max;
static bool was_powered, too_hot, full_once;
static struct {
	int cap, mv, ma, temp, status;
	bool powered;
	const char *state;
} bat = { .state = "unknown", .status = -1 };

static int reg_rw(char rw, int reg, union i2c_smbus_data *d)
{
	struct i2c_smbus_ioctl_data a = { .read_write = rw, .command = reg, .size = I2C_SMBUS_BYTE_DATA, .data = d };
	return i2c < 0 ? -1 : ioctl(i2c, I2C_SMBUS, &a);
}

static int reg_read(int reg)
{
	union i2c_smbus_data d;
	return reg_rw(I2C_SMBUS_READ, reg, &d) < 0 ? -1 : d.byte;
}

static int reg_write(int reg, int v)
{
	union i2c_smbus_data d = { .byte = v };
	return reg_rw(I2C_SMBUS_WRITE, reg, &d);
}

static int read_int(const char *path, int fallback)
{
	FILE *f = fopen(path, "r");
	int v;

	if (!f)
		return fallback;
	if (fscanf(f, "%d", &v) != 1)
		v = fallback;
	fclose(f);
	return v;
}

static void read_str(const char *path, char *out, size_t len)
{
	FILE *f = fopen(path, "r");

	out[0] = 0;
	if (!f)
		return;
	if (fgets(out, len, f))
		out[strcspn(out, "\n")] = 0;
	fclose(f);
}

void power_reload(void)
{
	limit = set_get_int("battery.limit");
	resume_drop = set_get_int("battery.resume_drop");
	ichg = !strcmp(set_get("battery.current"), "reduced") ? ICHG_REDUCED : ICHG_NORMAL;
	temp_max = set_get_int("battery.temp_max") * 10;
}

void power_charge_full(bool on)
{
	if (on != full_once)
		syslog(LOG_INFO, "charge limit %s", on ? "skipped until unplugged" : "back on");
	full_once = on;
}

void power_init(void)
{
	power_reload();
	i2c = open(CHARGER_BUS, O_RDWR | O_CLOEXEC);
	if (i2c < 0 || ioctl(i2c, I2C_SLAVE_FORCE, CHARGER_ADDR) < 0)
		syslog(LOG_ERR, "charger: %m");
	power_tick();
}

static bool want_charging(bool on)
{
	if (!bat.powered)
		return true;
	if (temp_max) {
		if (bat.temp >= temp_max)
			too_hot = true;
		else if (bat.temp <= temp_max - TEMP_HYST)
			too_hot = false;
		if (too_hot)
			return false;
	}
	if (limit >= 100 || full_once)
		return true;
	if (!was_powered || on)
		return bat.cap < limit;
	return bat.cap <= limit - resume_drop;
}

bool power_tick(void)
{
	int ctrl, status, cur, prev_cap = bat.cap, prev_status = bat.status;
	const char *prev_state = bat.state;
	bool on, want;

	bat.cap = read_int(GAUGE "capacity", -1);
	bat.mv = read_int(GAUGE "voltage_now", 0) / 1000;
	bat.ma = read_int(GAUGE "current_now", 0) / 1000;
	bat.temp = read_int(GAUGE "temp", 0);
	ctrl = reg_read(REG_CTRL);
	status = reg_read(REG_STATUS);
	if (bat.cap < 0 || ctrl < 0 || status < 0) {
		bat.state = "unknown";
		return prev_state != bat.state;
	}
	bat.status = status;
	bat.powered = status & PG_STAT;
	if (!bat.powered)
		full_once = false;
	on = ctrl & CHG_CONFIG;
	want = want_charging(on);
	was_powered = bat.powered;
	if (want != on && reg_write(REG_CTRL, want ? ctrl | CHG_CONFIG : ctrl & ~CHG_CONFIG) == 0) {
		on = want;
		if (bat.powered)
			syslog(LOG_INFO, "charging %s at %d%% %d.%d C (limit %d%%)", on ? "on" : "off", bat.cap, bat.temp / 10,
			       bat.temp % 10, limit);
	}
	cur = reg_read(REG_ICHG);
	if (cur >= 0 && (cur & ICHG_MASK) != ichg && reg_write(REG_ICHG, (cur & ~ICHG_MASK) | ichg) == 0)
		syslog(LOG_INFO, "charge current code 0x%02x", ichg);
	if (!bat.powered)
		bat.state = "battery";
	else if (!on)
		bat.state = too_hot ? "hot" : "held";
	else
		bat.state = (const char *[]){ "idle", "charging", "charging", "full" }[(status >> 3) & 3];
	return prev_cap != bat.cap || prev_state != bat.state || (prev_status ^ bat.status) & 0xf8;
}

void power_status(struct kvbuf *b, bool full)
{
	kv_int(b, "cap", bat.cap);
	kv_int(b, "mv", bat.mv);
	kv_int(b, "ma", bat.ma);
	kv_int(b, "temp", bat.temp);
	kv_int(b, "limit", limit);
	kv_int(b, "skip", full_once);
	kv_str(b, "state", bat.state);
	kv_str(b, "vbus", bat.status < 0 ? "unknown" : vbus_names[(bat.status >> 5) & 7]);
	kv_str(b, "stage", bat.status < 0 ? "unknown" : stage_names[(bat.status >> 3) & 3]);
	if (full) {
		char s[64];
		kv_int(b, "cycles", read_int(GAUGE "cycle_count", -1));
		read_str(GAUGE "health", s, sizeof(s));
		kv_str(b, "health", s);
		read_str(TYPEC_STATE, s, sizeof(s));
		kv_str(b, "typec", s);
		kv_int(b, "ichg", ichg == ICHG_REDUCED ? 600 : 1320);
	}
}
