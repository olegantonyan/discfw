#include "cs43131_ioctl.h"
#include "proto.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define VOL_FILE "/run/discd.vol"
#define VOL_DEFAULT 0x50
#define VOL_STEP 4
#define RELEASE_MS 500

static const unsigned rates[] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 };

static int fda = -1, fdc = -1;
static int rate_code = -1;
static int vol = VOL_DEFAULT;
static int muted = -1;
static long long release_at;

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int rate_to_code(unsigned hz)
{
	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
		if (rates[i] == hz)
			return i;
	return -1;
}

static int xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	if (ioctl(fd, req, arg) >= 0)
		return 0;
	syslog(LOG_ERR, "%s: %m", what);
	return -1;
}

static int set_u8(int fd, unsigned long req, int v, const char *what)
{
	unsigned char b = v;
	return xioctl(fd, req, &b, what);
}

static int dac_mute(int on)
{
	if (on == muted)
		return 0;
	int r = xioctl(fda, on ? CS43131_MUTE_ALL : CS43131_UNMUTE_ALL, NULL, on ? "mute" : "unmute");
	if (!r)
		muted = on;
	return r;
}

static int dac_volume(int v)
{
	if (v < 0)
		v = 0;
	if (v > 255)
		v = 255;
	if (set_u8(fda, CS43131A_SET_VOL_A, v, "A vol a") || set_u8(fda, CS43131A_SET_VOL_B, v, "A vol b") ||
	    set_u8(fdc, CS43131C_SET_VOL_A, v, "C vol a") || set_u8(fdc, CS43131C_SET_VOL_B, v, "C vol b"))
		return -1;
	vol = v;
	FILE *f = fopen(VOL_FILE, "w");
	if (f) {
		fprintf(f, "%d\n", vol);
		fclose(f);
	}
	return 0;
}

static int dac_rate(unsigned hz)
{
	int code = rate_to_code(hz);
	if (code < 0)
		return -1;
	if (code == rate_code)
		return 0;
	dac_mute(1);
	if (set_u8(fda, CS43131A_SET_SRATE, code, "A srate") || set_u8(fdc, CS43131C_SET_SRATE, code, "C srate"))
		return -1;
	rate_code = code;
	syslog(LOG_INFO, "rate %u (code %d)", hz, code);
	return 0;
}

static int dac_off(void)
{
	dac_mute(1);
	rate_code = -1;
	return xioctl(fda, CS43131_POWER_DOWN, NULL, "power down");
}

static void handle(const char *line, FILE *out)
{
	char arg[32] = "";
	char cmd[16] = "";
	int r = -1;

	sscanf(line, "%15s %31s", cmd, arg);
	if (strcmp(cmd, "status") && strcmp(cmd, "vol"))
		release_at = 0;
	if (!strcmp(cmd, "release")) {
		if (!muted)
			release_at = now_ms() + RELEASE_MS;
		r = 0;
	} else if (!strcmp(cmd, "rate"))
		r = dac_rate(strtoul(arg, NULL, 10));
	else if (!strcmp(cmd, "mute"))
		r = dac_mute(1);
	else if (!strcmp(cmd, "unmute"))
		r = rate_code < 0 ? -1 : dac_mute(0);
	else if (!strcmp(cmd, "vol"))
		r = dac_volume(!strcmp(arg, "+") ? vol - VOL_STEP : !strcmp(arg, "-") ? vol + VOL_STEP : (int)strtol(arg, NULL, 0));
	else if (!strcmp(cmd, "off"))
		r = dac_off();
	else if (!strcmp(cmd, "status"))
		r = 0;
	if (r)
		fprintf(out, "err %s\n", cmd);
	else
		fprintf(out, "ok rate=%u vol=%d muted=%d\n", rate_code < 0 ? 0 : rates[rate_code], vol, muted);
}

static void serve(int lfd)
{
	struct timeval tv = { .tv_sec = 2 };
	char line[128];

	for (;;) {
		struct pollfd pfd = { .fd = lfd, .events = POLLIN };
		long long wait = release_at ? release_at - now_ms() : -1;
		if (release_at && wait <= 0) {
			release_at = 0;
			dac_mute(1);
			continue;
		}
		if (poll(&pfd, 1, (int)wait) <= 0)
			continue;
		int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
		if (cfd < 0) {
			if (errno != EINTR)
				syslog(LOG_ERR, "accept: %m");
			continue;
		}
		setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		FILE *in = fdopen(cfd, "r");
		FILE *out = in ? fdopen(dup(cfd), "w") : NULL;
		if (!out) {
			in ? fclose(in) : close(cfd);
			continue;
		}
		while (fgets(line, sizeof(line), in)) {
			handle(line, out);
			fflush(out);
		}
		fclose(out);
		fclose(in);
	}
}

static int listen_sock(void)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	strncpy(addr.sun_path, DISCD_SOCK, sizeof(addr.sun_path) - 1);
	unlink(DISCD_SOCK);
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0)
		return -1;
	chmod(DISCD_SOCK, 0666);
	return fd;
}

int main(void)
{
	FILE *f;
	int lfd;

	openlog("discd", LOG_PERROR, LOG_DAEMON);
	signal(SIGPIPE, SIG_IGN);
	fda = open(CS43131_DEV_A, O_RDWR | O_CLOEXEC);
	fdc = open(CS43131_DEV_C, O_RDWR | O_CLOEXEC);
	if (fda < 0 || fdc < 0) {
		syslog(LOG_ERR, "open DAC: %m");
		sleep(5);
		return 1;
	}
	dac_mute(1);
	f = fopen(VOL_FILE, "r");
	if (f) {
		if (fscanf(f, "%d", &vol) != 1)
			vol = VOL_DEFAULT;
		fclose(f);
	}
	dac_volume(vol);
	lfd = listen_sock();
	if (lfd < 0) {
		syslog(LOG_ERR, "socket: %m");
		return 1;
	}
	syslog(LOG_INFO, "ready, vol %d", vol);
	serve(lfd);
	return 0;
}
