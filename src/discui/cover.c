#include "mpdc.h"
#include "ui.h"

#include <dirent.h>
#include <disc/conn.h>
#include <disc/text.h>
#include <stdio.h>
#include <jpeglib.h>
#include <math.h>
#include <png.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SIZE 360
#define CACHE 4
#define MAX_FILE (12 << 20)
#define MAX_PIXELS (6 << 20)
#define CHUNK (1 << 20)
#define VINYL_PX 180
#define LABEL_PX 86

struct entry {
	char *key;
	uint8_t *buf;
	lv_image_dsc_t dsc;
	unsigned age;
};

static struct entry cache[CACHE];
static unsigned age;
static const lv_image_dsc_t *current;
static char current_key[1100];

static pthread_t thread;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static char *job_uri, *job_dir, *job_key;
static char *done_key;
static uint8_t *done_buf;
static bool done;
static char *mpd_sock;

static void dsc_init(lv_image_dsc_t *d, uint8_t *buf, int w, int h)
{
	memset(d, 0, sizeof(*d));
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = LV_COLOR_FORMAT_ARGB8888;
	d->header.w = w;
	d->header.h = h;
	d->header.stride = w * 4;
	d->data_size = w * h * 4;
	d->data = buf;
}

static void resample(const uint8_t *src, int w, int h, int ch, uint8_t *dst)
{
	int s = w < h ? w : h, ox = (w - s) / 2, oy = (h - s) / 2;

	for (int y = 0; y < SIZE; y++) {
		int y0 = oy + y * s / SIZE, y1 = oy + (y + 1) * s / SIZE;
		if (y1 <= y0)
			y1 = y0 + 1;
		for (int x = 0; x < SIZE; x++) {
			int x0 = ox + x * s / SIZE, x1 = ox + (x + 1) * s / SIZE;
			unsigned r = 0, g = 0, b = 0, n = 0;
			if (x1 <= x0)
				x1 = x0 + 1;
			for (int yy = y0; yy < y1; yy++) {
				const uint8_t *p = src + ((size_t)yy * w + x0) * ch;
				for (int xx = x0; xx < x1; xx++, p += ch) {
					r += p[0];
					g += p[1];
					b += p[2];
					n++;
				}
			}
			uint8_t *d = dst + ((size_t)y * SIZE + x) * 4;
			d[0] = b / n;
			d[1] = g / n;
			d[2] = r / n;
			d[3] = 0xff;
		}
	}
}

struct jerr {
	struct jpeg_error_mgr pub;
	jmp_buf jb;
};

static void jpeg_fail(j_common_ptr c)
{
	longjmp(((struct jerr *)c->err)->jb, 1);
}

static uint8_t *decode_jpeg(const uint8_t *data, size_t len)
{
	struct jpeg_decompress_struct ci;
	struct jerr je;
	uint8_t *rgb = NULL, *out = NULL;

	ci.err = jpeg_std_error(&je.pub);
	je.pub.error_exit = jpeg_fail;
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&ci);
		free(rgb);
		free(out);
		return NULL;
	}
	jpeg_create_decompress(&ci);
	jpeg_mem_src(&ci, (unsigned char *)data, len);
	jpeg_read_header(&ci, TRUE);
	ci.out_color_space = JCS_RGB;
	ci.scale_num = 1;
	ci.scale_denom = 1;
	unsigned side = ci.image_width < ci.image_height ? ci.image_width : ci.image_height;
	while (ci.scale_denom < 8 && side / (ci.scale_denom * 2) >= SIZE)
		ci.scale_denom *= 2;
	jpeg_start_decompress(&ci);
	if ((uint64_t)ci.output_width * ci.output_height > MAX_PIXELS || ci.output_components != 3) {
		jpeg_destroy_decompress(&ci);
		return NULL;
	}
	rgb = malloc((size_t)ci.output_width * ci.output_height * 3);
	while (rgb && ci.output_scanline < ci.output_height) {
		JSAMPROW row = rgb + (size_t)ci.output_scanline * ci.output_width * 3;
		jpeg_read_scanlines(&ci, &row, 1);
	}
	jpeg_finish_decompress(&ci);
	out = rgb ? malloc(SIZE * SIZE * 4) : NULL;
	if (out)
		resample(rgb, ci.output_width, ci.output_height, 3, out);
	jpeg_destroy_decompress(&ci);
	free(rgb);
	return out;
}

static uint8_t *decode_png(const uint8_t *data, size_t len)
{
	png_image img = { .version = PNG_IMAGE_VERSION };
	uint8_t *rgba, *out;

	if (!png_image_begin_read_from_memory(&img, data, len))
		return NULL;
	if ((uint64_t)img.width * img.height > MAX_PIXELS) {
		png_image_free(&img);
		return NULL;
	}
	img.format = PNG_FORMAT_RGBA;
	rgba = malloc(PNG_IMAGE_SIZE(img));
	if (!rgba || !png_image_finish_read(&img, NULL, rgba, 0, NULL)) {
		png_image_free(&img);
		free(rgba);
		return NULL;
	}
	out = malloc(SIZE * SIZE * 4);
	if (out)
		resample(rgba, img.width, img.height, 4, out);
	free(rgba);
	return out;
}

static uint8_t *decode(const uint8_t *data, size_t len)
{
	if (len > 3 && data[0] == 0xff && data[1] == 0xd8)
		return decode_jpeg(data, len);
	if (len > 8 && !memcmp(data, "\x89PNG", 4))
		return decode_png(data, len);
	return NULL;
}

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf = NULL;
	long n;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < MAX_FILE && fseek(f, 0, SEEK_SET) == 0) {
		buf = malloc(n);
		if (buf && fread(buf, 1, n, f) != (size_t)n) {
			free(buf);
			buf = NULL;
		}
		*len = n;
	}
	fclose(f);
	return buf;
}

static int rank(const char *name)
{
	static const struct {
		const char *stem;
		int score;
	} names[] = { { "front", 100 }, { "folder", 80 }, { "albumart", 80 }, { "cover", 60 }, { "album", 30 } };
	const char *dot = strrchr(name, '.');
	size_t stem;

	if (!dot || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") && strcasecmp(dot, ".png")))
		return 0;
	stem = dot - name;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (stem == strlen(names[i].stem) && !strncasecmp(name, names[i].stem, stem))
			return names[i].score;
	return 5;
}

static char *folder_image(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char best[512] = "", *out = NULL;
	int best_score = 0;

	while (d && (e = readdir(d))) {
		int s = e->d_name[0] == '.' ? 0 : rank(e->d_name);
		if (s > best_score) {
			best_score = s;
			snprintf(best, sizeof(best), "%s", e->d_name);
		}
	}
	if (d)
		closedir(d);
	if (best_score && asprintf(&out, "%s/%s", dir, best) < 0)
		out = NULL;
	return out;
}

static uint8_t *embedded(struct mpd_connection **c, const char *uri, size_t *len)
{
	uint8_t *buf = NULL;
	size_t n = 0;
	int r;

	if (!*c) {
		*c = mpd_connection_new(mpd_sock, 0, 5000);
		if (*c && (mpd_connection_get_error(*c) != MPD_ERROR_SUCCESS || !mpd_run_binarylimit(*c, CHUNK))) {
			mpd_connection_free(*c);
			*c = NULL;
		}
	}
	if (!*c)
		return NULL;
	for (;;) {
		uint8_t *nb = realloc(buf, n + CHUNK);
		if (!nb)
			break;
		buf = nb;
		r = mpd_run_readpicture(*c, uri, n, buf + n, CHUNK);
		if (r <= 0)
			break;
		n += r;
		if (n >= MAX_FILE)
			break;
	}
	if (mpd_connection_get_error(*c) != MPD_ERROR_SUCCESS && !mpd_connection_clear_error(*c)) {
		mpd_connection_free(*c);
		*c = NULL;
	}
	if (!n) {
		free(buf);
		return NULL;
	}
	*len = n;
	return buf;
}

static void *worker(void *arg)
{
	struct mpd_connection *c = NULL;
	char *last_img = NULL;
	uint8_t *last_buf = NULL;

	(void)arg;
	for (;;) {
		char *uri, *dir, *key, *img;
		uint8_t *data = NULL, *out = NULL;
		size_t len = 0;

		pthread_mutex_lock(&mu);
		while (!job_key)
			pthread_cond_wait(&cv, &mu);
		uri = job_uri;
		dir = job_dir;
		key = job_key;
		job_uri = job_dir = job_key = NULL;
		pthread_mutex_unlock(&mu);

		img = dir ? folder_image(dir) : NULL;
		if (img && last_img && !strcmp(img, last_img) && last_buf) {
			out = malloc(SIZE * SIZE * 4);
			if (out)
				memcpy(out, last_buf, SIZE * SIZE * 4);
		} else if (img && (data = read_file(img, &len))) {
			out = decode(data, len);
			free(data);
			if (out) {
				free(last_img);
				free(last_buf);
				last_img = strdup(img);
				last_buf = malloc(SIZE * SIZE * 4);
				if (last_buf)
					memcpy(last_buf, out, SIZE * SIZE * 4);
			}
		}
		if (!out && uri && (data = embedded(&c, uri, &len))) {
			out = decode(data, len);
			free(data);
		}
		free(img);
		free(uri);
		free(dir);
		pthread_mutex_lock(&mu);
		free(done_key);
		free(done_buf);
		done_key = key;
		done_buf = out;
		done = true;
		pthread_mutex_unlock(&mu);
		loop_wake();
	}
	return NULL;
}

static struct entry *lookup(const char *key)
{
	for (int i = 0; i < CACHE; i++)
		if (cache[i].key && !strcmp(cache[i].key, key)) {
			cache[i].age = ++age;
			return &cache[i];
		}
	return NULL;
}

static void set_current(const lv_image_dsc_t *d)
{
	if (current == d)
		return;
	current = d;
	app_notify(EV_COVER);
}

static void insert(const char *key, uint8_t *buf)
{
	struct entry *victim = NULL;

	for (int i = 0; i < CACHE; i++) {
		if (&cache[i].dsc == current)
			continue;
		if (!victim || cache[i].age < victim->age)
			victim = &cache[i];
	}
	free(victim->key);
	free(victim->buf);
	victim->key = strdup(key);
	victim->buf = buf;
	victim->age = ++age;
	dsc_init(&victim->dsc, buf, SIZE, SIZE);
	if (!strcmp(key, current_key))
		set_current(&victim->dsc);
}

void cover_poll(void)
{
	char *key;
	uint8_t *buf;

	pthread_mutex_lock(&mu);
	if (!done) {
		pthread_mutex_unlock(&mu);
		return;
	}
	key = done_key;
	buf = done_buf;
	done_key = NULL;
	done_buf = NULL;
	done = false;
	pthread_mutex_unlock(&mu);
	if (buf)
		insert(key, buf);
	else if (key && !strcmp(key, current_key))
		set_current(NULL);
	free(key);
}

void cover_request(const char *uri)
{
	struct entry *e;
	char *path, *slash;

	snprintf(current_key, sizeof(current_key), "%s", uri ? uri : "");
	if (!uri || !uri[0] || app.safe_mode) {
		set_current(NULL);
		return;
	}
	if ((e = lookup(uri))) {
		set_current(&e->dsc);
		return;
	}
	path = mpdc_path_for_uri(uri);
	if (path && (slash = strrchr(path, '/')))
		*slash = 0;
	pthread_mutex_lock(&mu);
	free(job_uri);
	free(job_dir);
	free(job_key);
	job_uri = strdup(uri);
	job_dir = path;
	job_key = strdup(uri);
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
}

const lv_image_dsc_t *cover_current(void)
{
	return current;
}

static uint32_t argb(float a, float r, float g, float b)
{
	return (uint32_t)(a * 255) << 24 | (uint32_t)(r * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(b * 255);
}

static float clampf(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float mix(float a, float b, float t)
{
	return a + (b - a) * t;
}

const lv_image_dsc_t *cover_placeholder(void)
{
	static lv_image_dsc_t d;
	static uint32_t *px;

	if (px)
		return &d;
	px = malloc(SIZE * SIZE * 4);
	for (int y = 0; y < SIZE; y++)
		for (int x = 0; x < SIZE; x++) {
			float t = clampf((x + y) / (2.0f * SIZE));
			px[y * SIZE + x] = argb(1, mix(0.05f, 0.12f, t), mix(0.22f, 0.10f, t), mix(0.25f, 0.18f, t));
		}
	dsc_init(&d, (uint8_t *)px, SIZE, SIZE);
	return &d;
}

const lv_image_dsc_t *vinyl_disc(void)
{
	static lv_image_dsc_t d;
	static uint32_t *px;
	const int n = VINYL_PX;
	const float R = n / 2.0f;

	if (px)
		return &d;
	px = calloc(n * n, 4);
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			float dx = x + 0.5f - R, dy = y + 0.5f - R, r = sqrtf(dx * dx + dy * dy) / R;
			float edge = clampf((1.0f - r) * R);
			if (edge <= 0)
				continue;
			float t = clampf(r);
			float cr = mix(0.25f, 0.07f, t), cg = mix(0.09f, 0.02f, t), cb = mix(0.16f, 0.05f, t);
			for (int g = 0; g < 4; g++) {
				float gr = 0.91f - g * 0.08f, w = clampf(1.0f - fabsf(r - gr) * R / 1.2f) * 0.45f;
				cr = mix(cr, 0.85f, w);
				cg = mix(cg, 0.65f, w);
				cb = mix(cb, 0.30f, w);
			}
			float ang = atan2f(dy, dx);
			if (ang > -1.2f && ang < -0.35f) {
				float w = clampf(1.0f - fabsf(r - 0.82f) * R / 3.0f) * 0.18f;
				cr = mix(cr, 1, w);
				cg = mix(cg, 1, w);
				cb = mix(cb, 1, w);
			}
			px[y * n + x] = argb(edge, cr, cg, cb);
		}
	dsc_init(&d, (uint8_t *)px, n, n);
	return &d;
}

static float seg_dist(float px_, float py, float ax, float ay, float bx, float by)
{
	float vx = bx - ax, vy = by - ay, t = ((px_ - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy);
	t = clampf(t);
	float dx = px_ - (ax + t * vx), dy = py - (ay + t * vy);
	return sqrtf(dx * dx + dy * dy);
}

const lv_image_dsc_t *vinyl_label(void)
{
	static lv_image_dsc_t d;
	static uint32_t *px;
	const int n = LABEL_PX;
	const float R = n / 2.0f, a = -0.7625f, ca = cosf(a), sa = sinf(a);
	const float bars[3][2] = { { -0.32f, 0.42f }, { 0.0f, 0.26f }, { 0.32f, 0.46f } };

	if (px)
		return &d;
	px = calloc(n * n, 4);
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			float dx = x + 0.5f - R, dy = y + 0.5f - R, r = sqrtf(dx * dx + dy * dy) / R;
			float cr = 1.0f, cg = mix(0.94f, 0.64f, r), cb = mix(0.36f, 0.0f, r);
			for (int i = 0; i < 3; i++) {
				float bx = bars[i][0] * R, h = bars[i][1] * R;
				float ax = bx * ca - (-h) * sa, ay = bx * sa + (-h) * ca, ex = bx * ca - h * sa, ey = bx * sa + h * ca;
				float w = clampf(0.10f * R - seg_dist(dx, dy, ax, ay, ex, ey) + 0.5f);
				cr = mix(cr, 0.33f, w);
				cg = mix(cg, 0.13f, w);
				cb = mix(cb, 0.25f, w);
			}
			px[y * n + x] = argb(1, cr, cg, cb);
		}
	dsc_init(&d, (uint8_t *)px, n, n);
	return &d;
}

void cover_init(void)
{
	mpd_sock = strdup(MPD_SOCK);
	pthread_create(&thread, NULL, worker, NULL);
}
