#include "app.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

static struct {
	int fd;
	uint8_t *mem;
	size_t len;
	uint32_t bpp, stride;
	struct fb_var_screeninfo v;
	bool rot180;
} fb = { .fd = -1 };

static uint32_t *shadow;
static bool active = true;
static lv_display_t *display;

static void flush_fb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	int32_t w = lv_area_get_width(a);
	uint8_t *page = fb.mem + fb.v.yoffset * fb.stride + fb.v.xoffset * fb.bpp;

	for (int32_t y = a->y1; y <= a->y2; y++, px += w * fb.bpp) {
		if (!fb.rot180) {
			memcpy(page + y * fb.stride + a->x1 * fb.bpp, px, w * fb.bpp);
			continue;
		}
		uint8_t *dst = page + (fb.v.yres - 1 - y) * fb.stride + (fb.v.xres - 1 - a->x1) * fb.bpp;
		if (fb.bpp == 4) {
			uint32_t *s = (uint32_t *)px, *t = (uint32_t *)dst;
			for (int32_t x = 0; x < w; x++)
				*(t - x) = s[x];
		} else {
			uint16_t *s = (uint16_t *)px, *t = (uint16_t *)dst;
			for (int32_t x = 0; x < w; x++)
				*(t - x) = s[x];
		}
	}
	lv_display_flush_ready(d);
}

static void flush_mem(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	int32_t w = lv_area_get_width(a);

	for (int32_t y = a->y1; y <= a->y2; y++, px += w * 4)
		memcpy(shadow + y * SCR_W + a->x1, px, w * 4);
	lv_display_flush_ready(d);
}

static lv_display_t *create_fb(const char *dev, bool rot180)
{
	struct fb_fix_screeninfo f;
	lv_display_t *d;
	uint32_t bufsz;

	fb.fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fb.fd < 0 || ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.v) || ioctl(fb.fd, FBIOGET_FSCREENINFO, &f)) {
		perror(dev);
		return NULL;
	}
	fb.bpp = fb.v.bits_per_pixel / 8;
	if (fb.bpp != 4 && fb.bpp != 2) {
		fprintf(stderr, "%s: unsupported %u bpp\n", dev, fb.v.bits_per_pixel);
		return NULL;
	}
	fb.stride = f.line_length;
	fb.len = f.smem_len;
	fb.rot180 = rot180;
	fb.mem = mmap(NULL, fb.len, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
	if (fb.mem == MAP_FAILED) {
		perror("fb mmap");
		return NULL;
	}
	fb.v.xoffset = 0;
	fb.v.yoffset = 0;
	ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.v);
	ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.v);
	ioctl(fb.fd, FBIOBLANK, FB_BLANK_UNBLANK);
	d = lv_display_create(fb.v.xres, fb.v.yres);
	lv_display_set_color_format(d, fb.bpp == 4 ? LV_COLOR_FORMAT_XRGB8888 : LV_COLOR_FORMAT_RGB565);
	bufsz = fb.v.xres * (fb.v.yres / 4) * fb.bpp;
	lv_display_set_buffers(d, malloc(bufsz), malloc(bufsz), bufsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(d, flush_fb);
	return d;
}

static lv_display_t *create_mem(void)
{
	lv_display_t *d = lv_display_create(SCR_W, SCR_H);
	uint32_t bufsz = SCR_W * (SCR_H / 4) * 4;

	shadow = calloc(SCR_W * SCR_H, 4);
	lv_display_set_color_format(d, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(d, malloc(bufsz), malloc(bufsz), bufsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(d, flush_mem);
	return d;
}

lv_display_t *disp_create(void)
{
	const char *rot = getenv("DISCUI_ROTATE");

	display = app.host ? create_mem() : create_fb("/dev/fb0", !rot || strcmp(rot, "0"));
	return display;
}

void disp_set_active(bool on)
{
	if (on == active)
		return;
	active = on;
	lv_display_enable_invalidation(display, on);
	if (on) {
		lv_obj_invalidate(lv_screen_active());
		lv_obj_invalidate(lv_layer_top());
		lv_refr_now(display);
	}
}

bool disp_active(void)
{
	return active;
}

static uint32_t pixel(int x, int y)
{
	if (shadow)
		return shadow[y * SCR_W + x];
	if (fb.rot180) {
		x = fb.v.xres - 1 - x;
		y = fb.v.yres - 1 - y;
	}
	uint8_t *p = fb.mem + (fb.v.yoffset + y) * fb.stride + (fb.v.xoffset + x) * fb.bpp;
	if (fb.bpp == 4)
		return *(uint32_t *)p;
	uint16_t c = *(uint16_t *)p;
	return ((c >> 11) & 0x1f) << 19 | ((c >> 5) & 0x3f) << 10 | (c & 0x1f) << 3;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8];
	uint32_t crc = crc32(0, (const Bytef *)type, 4);

	put_be32(hdr, len);
	memcpy(hdr + 4, type, 4);
	if (len)
		crc = crc32(crc, data, len);
	uint8_t tail[4];
	put_be32(tail, crc);
	return fwrite(hdr, 8, 1, f) == 1 && (!len || fwrite(data, len, 1, f) == 1) && fwrite(tail, 4, 1, f) == 1;
}

int disp_shot(const char *path)
{
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	uint8_t ihdr[13] = { 0 };
	size_t raw_len = SCR_H * (1 + SCR_W * 3);
	uLongf z_len = compressBound(raw_len);
	uint8_t *raw = malloc(raw_len), *z = malloc(z_len);
	FILE *f = NULL;
	int r = -1;

	if (!raw || !z || (!shadow && !fb.mem))
		goto out;
	for (int y = 0; y < SCR_H; y++) {
		uint8_t *row = raw + y * (1 + SCR_W * 3);
		row[0] = 0;
		for (int x = 0; x < SCR_W; x++) {
			int dx = x - SCR_R, dy = y - SCR_R;
			uint32_t c = dx * dx + dy * dy > SCR_R * SCR_R ? 0x303030 : pixel(x, y);
			row[1 + x * 3] = c >> 16;
			row[2 + x * 3] = c >> 8;
			row[3 + x * 3] = c;
		}
	}
	if (compress2(z, &z_len, raw, raw_len, 6) != Z_OK)
		goto out;
	put_be32(ihdr, SCR_W);
	put_be32(ihdr + 4, SCR_H);
	ihdr[8] = 8;
	ihdr[9] = 2;
	f = fopen(path, "wb");
	if (f && fwrite(sig, 8, 1, f) == 1 && chunk(f, "IHDR", ihdr, 13) && chunk(f, "IDAT", z, z_len) && chunk(f, "IEND", NULL, 0))
		r = 0;
out:
	if (f)
		fclose(f);
	free(raw);
	free(z);
	return r;
}
