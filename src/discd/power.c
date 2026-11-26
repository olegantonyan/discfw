#include "power.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

#define GAUGE "/sys/class/power_supply/cw221X-bat/"
#define CHARGER_BUS "/dev/i2c-2"
#define CHARGER_ADDR 0x1a
#define REG_CTRL 0x01
#define CHG_CONFIG 0x10
#define REG_STATUS 0x08
#define PG_STAT 0x04
#define LIMIT_FILE "/usr/data/discfw/charge_limit"
#define LIMIT_MIN 50
#define RESUME_DROP 5

static const char *const chrg_stat[] = { "idle", "charging", "charging", "full" };
static int i2c = -1;
static int limit = 100;
static bool was_powered;
static struct {
	int cap, mv, ma, temp;
	const char *state;
} bat = { .state = "unknown" };

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

static int gauge_read(const char *name, int fallback)
{
	char path[64];
	FILE *f;
	int v;

	snprintf(path, sizeof(path), GAUGE "%s", name);
	f = fopen(path, "r");
	if (!f)
		return fallback;
	if (fscanf(f, "%d", &v) != 1)
		v = fallback;
	fclose(f);
	return v;
}

void power_init(void)
{
	FILE *f = fopen(LIMIT_FILE, "r");

	if (f) {
		if (fscanf(f, "%d", &limit) != 1 || limit < LIMIT_MIN || limit > 100)
			limit = 100;
		fclose(f);
	}
	i2c = open(CHARGER_BUS, O_RDWR | O_CLOEXEC);
	if (i2c < 0 || ioctl(i2c, I2C_SLAVE_FORCE, CHARGER_ADDR) < 0)
		syslog(LOG_ERR, "charger: %m");
	power_tick();
}

void power_tick(void)
{
	int ctrl, status;
	bool powered, on, want;

	bat.cap = gauge_read("capacity", -1);
	bat.mv = gauge_read("voltage_now", 0) / 1000;
	bat.ma = gauge_read("current_now", 0) / 1000;
	bat.temp = gauge_read("temp", 0);
	ctrl = reg_read(REG_CTRL);
	status = reg_read(REG_STATUS);
	if (bat.cap < 0 || ctrl < 0 || status < 0) {
		bat.state = "unknown";
		return;
	}
	powered = status & PG_STAT;
	on = ctrl & CHG_CONFIG;
	if (!powered || limit >= 100)
		want = true;
	else if (!was_powered || on)
		want = bat.cap < limit;
	else
		want = bat.cap <= limit - RESUME_DROP;
	was_powered = powered;
	if (want != on && reg_write(REG_CTRL, want ? ctrl | CHG_CONFIG : ctrl & ~CHG_CONFIG) == 0) {
		on = want;
		if (powered)
			syslog(LOG_INFO, "charging %s at %d%% (limit %d%%)", on ? "on" : "off", bat.cap, limit);
	}
	if (!powered)
		bat.state = "battery";
	else if (!on)
		bat.state = "held";
	else
		bat.state = chrg_stat[(status >> 3) & 3];
}

int power_set_limit(int v)
{
	FILE *f;
	bool ok;

	if (v < LIMIT_MIN || v > 100)
		return -1;
	limit = v;
	f = fopen(LIMIT_FILE ".new", "w");
	ok = f && fprintf(f, "%d\n", v) > 0;
	if (f && fclose(f))
		ok = false;
	if (!ok || rename(LIMIT_FILE ".new", LIMIT_FILE))
		syslog(LOG_ERR, "save limit: %m");
	return 0;
}

void power_status(FILE *out)
{
	fprintf(out, "ok cap=%d mv=%d ma=%d temp=%d limit=%d state=%s\n", bat.cap, bat.mv, bat.ma, bat.temp, limit, bat.state);
}
