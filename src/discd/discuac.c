#include <disc/conn.h>
#include <disc/settings.h>

#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define SRATE_FILE "/sys/kernel/config/usb_gadget/dac/functions/uac2.usb0/c_srate"
#define CHUNK 4096
#define RING_FRAMES (384000 + CHUNK)
#define CAP_PERIOD 1024
#define CAP_AVAIL_MIN 256
#define PLAY_PERIOD_US 10000
#define PLAY_BUFFER_US 40000
#define TARGET_MS 30
#define MARGIN_MS 5
#define MAX_PPM 500
#define IDLE_MS 150
#define FIRST_WINDOW_MS 300
#define WINDOW_MS 1000
#define LOG_EVERY_MS 60000

static const unsigned rates[] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 };

static snd_pcm_t *cap, *play;
static volatile sig_atomic_t quit;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;
static int32_t ring[RING_FRAMES * 2];
static size_t head, tail;
static long long last_data;
static unsigned stream_id, measured;
static long overruns;

static int32_t buf[(CHUNK + 1) * 2];
static unsigned rate, cur_stream;
static bool spdif;
static double level, credit;
static long long last_log, retry_at;
static long dropped, repeated, underruns, overrun_count;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static unsigned snap(double hz)
{
	for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
		if (hz > rates[i] * 0.97 && hz < rates[i] * 1.03)
			return rates[i];
	return 0;
}

static void report(unsigned r)
{
	char cmd[32];

	snprintf(cmd, sizeof(cmd), "uac rate=%u\n", r);
	disc_request(DISCSYS_SOCK, cmd, NULL, 0, 1000);
}

static int find_card(void)
{
	int card = -1;
	char *name;

	while (snd_card_next(&card) == 0 && card >= 0) {
		if (snd_card_get_name(card, &name) < 0)
			continue;
		bool hit = strstr(name, "UAC2") != NULL;
		free(name);
		if (hit)
			return card;
	}
	return -1;
}

static unsigned kernel_rate(void)
{
	FILE *f = fopen(SRATE_FILE, "r");
	unsigned r = 0;

	if (f) {
		if (fscanf(f, "%u", &r) != 1)
			r = 0;
		fclose(f);
	}
	return snap(r);
}

static snd_pcm_t *open_capture(int card)
{
	char dev[16];
	snd_pcm_t *p;
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t period = CAP_PERIOD, size = 0;
	unsigned r = 0;
	int dir = 0, err;

	snprintf(dev, sizeof(dev), "hw:%d,0", card);
	if ((err = snd_pcm_open(&p, dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK)) < 0) {
		syslog(LOG_ERR, "open %s: %s", dev, snd_strerror(err));
		return NULL;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(p, hw);
	if ((err = snd_pcm_hw_params_set_access(p, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(p, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(p, hw, 2)) < 0 || (err = snd_pcm_hw_params_set_rate_first(p, hw, &r, &dir)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(p, hw, &period, &dir)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_last(p, hw, &size)) < 0 || (err = snd_pcm_hw_params(p, hw)) < 0) {
		syslog(LOG_ERR, "capture params: %s", snd_strerror(err));
		snd_pcm_close(p);
		return NULL;
	}
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(p, sw);
	snd_pcm_sw_params_set_avail_min(p, sw, CAP_AVAIL_MIN);
	snd_pcm_sw_params(p, sw);
	snd_pcm_start(p);
	syslog(LOG_INFO, "capture %s: %u Hz, period %lu, buffer %lu", dev, r, period, size);
	return p;
}

static void measure(unsigned m, bool first)
{
	static unsigned pending;

	if (!m)
		return;
	if (first || m == pending || m == measured) {
		measured = m;
		pending = 0;
	} else {
		pending = m;
	}
}

static void *capture_loop(void *arg)
{
	static int32_t tmp[CHUNK * 2];
	long long win_start = 0, t;
	long win_frames = 0;
	bool active = false, first = true;

	(void)arg;
	while (!quit) {
		snd_pcm_sframes_t n;

		snd_pcm_wait(cap, 50);
		n = snd_pcm_readi(cap, tmp, CHUNK);
		t = now_ms();
		if (n == -EAGAIN) {
			n = 0;
		} else if (n < 0) {
			pthread_mutex_lock(&mu);
			overruns++;
			pthread_mutex_unlock(&mu);
			if (snd_pcm_prepare(cap) < 0 || snd_pcm_start(cap) < 0) {
				syslog(LOG_ERR, "capture: %s", snd_strerror((int)n));
				quit = 1;
			}
			continue;
		}
		pthread_mutex_lock(&mu);
		if (!n) {
			if (active && t - last_data > IDLE_MS)
				active = false;
			pthread_mutex_unlock(&mu);
			continue;
		}
		if (!active) {
			active = first = true;
			stream_id++;
			measured = 0;
			win_start = t;
			win_frames = 0;
		} else {
			win_frames += n;
			if (t - win_start >= (first ? FIRST_WINDOW_MS : WINDOW_MS)) {
				measure(snap(win_frames * 1000.0 / (t - win_start)), first);
				first = false;
				win_start = t;
				win_frames = 0;
			}
		}
		for (snd_pcm_sframes_t i = 0; i < n; i++, head++) {
			ring[head % RING_FRAMES * 2] = tmp[i * 2];
			ring[head % RING_FRAMES * 2 + 1] = tmp[i * 2 + 1];
		}
		if (head - tail > RING_FRAMES)
			tail = head - RING_FRAMES;
		last_data = t;
		pthread_cond_signal(&cv);
		pthread_mutex_unlock(&mu);
	}
	pthread_mutex_lock(&mu);
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
	return NULL;
}

static snd_pcm_t *open_play(unsigned r)
{
	const char *dev = !spdif ? "disc" : r == 352800 ? "disc_spdif176" : r == 384000 ? "disc_spdif192" : "disc_spdif24";
	unsigned period = PLAY_PERIOD_US, buffer = PLAY_BUFFER_US;
	snd_pcm_uframes_t size = 0;
	snd_pcm_t *p;
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	int dir = 0, err;

	if ((err = snd_pcm_open(&p, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		syslog(LOG_ERR, "open %s: %s", dev, snd_strerror(err));
		return NULL;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(p, hw);
	if ((err = snd_pcm_hw_params_set_access(p, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(p, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(p, hw, 2)) < 0 || (err = snd_pcm_hw_params_set_rate(p, hw, r, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_time_near(p, hw, &period, &dir)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_time_near(p, hw, &buffer, &dir)) < 0 || (err = snd_pcm_hw_params(p, hw)) < 0) {
		syslog(LOG_ERR, "%s at %u Hz: %s", dev, r, snd_strerror(err));
		snd_pcm_close(p);
		return NULL;
	}
	snd_pcm_hw_params_get_buffer_size(hw, &size);
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(p, sw);
	snd_pcm_sw_params_set_start_threshold(p, sw, r * TARGET_MS / 1000 < size ? r * TARGET_MS / 1000 : size);
	snd_pcm_sw_params(p, sw);
	return p;
}

static void stop(void)
{
	if (!play)
		return;
	snd_pcm_close(play);
	play = NULL;
	syslog(LOG_INFO, "stream stopped");
	report(0);
}

static void start(unsigned r, long long t)
{
	stop();
	settings_load(SETTINGS_PATH);
	spdif = set_get_bool("audio.spdif");
	rate = r;
	level = credit = 0;
	if (!(play = open_play(r))) {
		retry_at = t + 1000;
		return;
	}
	syslog(LOG_INFO, "stream at %u Hz to %s", r, spdif ? "S/PDIF" : "the DAC");
	report(r);
}

static void adjust(snd_pcm_sframes_t *n, size_t queued)
{
	snd_pcm_sframes_t delay = 0;
	double target = rate * TARGET_MS / 1000.0, margin = rate * MARGIN_MS / 1000.0, now;

	if (snd_pcm_state(play) != SND_PCM_STATE_RUNNING || snd_pcm_delay(play, &delay) < 0)
		return;
	now = (double)queued + delay + *n;
	level = level ? level + (now - level) / 32 : now;
	credit += *n * MAX_PPM / 1e6;
	if (credit < 1)
		return;
	if (level > target + margin && *n > 1) {
		(*n)--;
		dropped++;
		credit -= 1;
	} else if (level < target - margin) {
		buf[*n * 2] = buf[*n * 2 - 2];
		buf[*n * 2 + 1] = buf[*n * 2 - 1];
		(*n)++;
		repeated++;
		credit -= 1;
	} else if (credit > 2) {
		credit = 2;
	}
}

static void write_frames(snd_pcm_sframes_t n)
{
	int32_t *p = buf;

	while (n > 0 && play) {
		snd_pcm_sframes_t w = snd_pcm_writei(play, p, n);
		if (w == -EAGAIN || w == -EINTR)
			continue;
		if (w < 0) {
			underruns++;
			syslog(LOG_WARNING, "playback: %s, reopening", snd_strerror((int)w));
			snd_pcm_close(play);
			play = open_play(rate);
			level = credit = 0;
			return;
		}
		p += w * 2;
		n -= w;
	}
}

static void log_stats(long long t)
{
	if (t - last_log < LOG_EVERY_MS)
		return;
	if (dropped || repeated || underruns || overrun_count)
		syslog(LOG_INFO, "last minute: %ld frames dropped, %ld repeated, %ld underruns, %ld overruns", dropped, repeated, underruns, overrun_count);
	dropped = repeated = underruns = overrun_count = 0;
	last_log = t;
}

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

int main(void)
{
	pthread_condattr_t ca;
	pthread_t th;
	int card;

	openlog("discuac", LOG_PERROR, LOG_DAEMON);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&cv, &ca);
	report(0);
	while ((card = find_card()) < 0 && !quit)
		sleep(1);
	if (quit || !(cap = open_capture(card)) || pthread_create(&th, NULL, capture_loop, NULL)) {
		sleep(2);
		return 1;
	}
	while (!quit) {
		struct timespec ts;
		snd_pcm_sframes_t n = 0;
		unsigned id, m;
		size_t queued;
		long long t, last;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_nsec += 50000000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		pthread_mutex_lock(&mu);
		if (head == tail && !quit)
			pthread_cond_timedwait(&cv, &mu, &ts);
		while (tail < head && n < CHUNK) {
			buf[n * 2] = ring[tail % RING_FRAMES * 2];
			buf[n * 2 + 1] = ring[tail % RING_FRAMES * 2 + 1];
			tail++;
			n++;
		}
		queued = head - tail;
		id = stream_id;
		m = measured;
		last = last_data;
		overrun_count += overruns;
		overruns = 0;
		pthread_mutex_unlock(&mu);
		t = now_ms();
		log_stats(t);
		if (!n) {
			if (play && t - last > IDLE_MS)
				stop();
			continue;
		}
		if (!play || id != cur_stream) {
			unsigned r = m ? m : kernel_rate();
			if (t < retry_at)
				continue;
			cur_stream = id;
			start(r ? r : 44100, t);
		} else if (m && m != rate) {
			syslog(LOG_INFO, "stream is %u Hz, not %u", m, rate);
			start(m, t);
		}
		if (!play)
			continue;
		adjust(&n, queued);
		write_frames(n);
	}
	stop();
	pthread_join(th, NULL);
	snd_pcm_close(cap);
	return 0;
}
