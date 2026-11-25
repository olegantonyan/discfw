#include "hw.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BACKLIGHT "/sys/class/backlight/backlight/brightness"
#define BRIGHTNESS_ON 30
#define LCD_DEV "/dev/lcd_st77916"
#define LCD_SLEEP_IN _IO(0xef, 0x01)
#define LCD_SLEEP_OUT _IO(0xef, 0x02)
#define TOUCH_DEV "/dev/cst816t"
#define TOUCH_SLEEP _IO(0xef, 0x03)
#define TOUCH_WAKE _IO(0xef, 0x04)

static struct {
	int fd;
	uint8_t *mem;
	size_t len;
	uint32_t bpp, stride;
	struct fb_var_screeninfo v;
	bool rot180;
} fb;

static bool screen_on = true;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
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

lv_display_t *fbdisp_create(const char *dev, bool rot180)
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
	fprintf(stderr, "fb: %ux%u virt %ux%u bpp %u stride %u yoff %u\n", fb.v.xres, fb.v.yres, fb.v.xres_virtual,
		fb.v.yres_virtual, fb.v.bits_per_pixel, fb.stride, fb.v.yoffset);

	d = lv_display_create(fb.v.xres, fb.v.yres);
	lv_display_set_color_format(d, fb.bpp == 4 ? LV_COLOR_FORMAT_XRGB8888 : LV_COLOR_FORMAT_RGB565);
	bufsz = fb.v.xres * (fb.v.yres / 4) * fb.bpp;
	lv_display_set_buffers(d, malloc(bufsz), malloc(bufsz), bufsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(d, flush_cb);
	screen_power(true);
	return d;
}

int input_find(const char *name, char *path, size_t len)
{
	char p[300], n[64];
	struct dirent *e;
	DIR *dir = opendir("/sys/class/input");
	int found = -1;

	while (dir && (e = readdir(dir))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/sys/class/input/%s/device/name", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f)
			continue;
		if (fgets(n, sizeof(n), f) && !strncmp(n, name, strlen(name))) {
			snprintf(path, len, "/dev/input/%s", e->d_name);
			found = 0;
		}
		fclose(f);
		if (!found)
			break;
	}
	if (dir)
		closedir(dir);
	return found;
}

static void write_int(const char *path, int v)
{
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%d\n", v);
		fclose(f);
	}
}

static void dev_ioctl(const char *dev, unsigned long req)
{
	int fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		ioctl(fd, req);
		close(fd);
	}
}

void screen_power(bool on)
{
	if (on) {
		dev_ioctl(LCD_DEV, LCD_SLEEP_OUT);
		dev_ioctl(TOUCH_DEV, TOUCH_WAKE);
		usleep(120000);
		lv_display_enable_invalidation(NULL, true);
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(NULL);
		write_int(BACKLIGHT, BRIGHTNESS_ON);
	} else {
		write_int(BACKLIGHT, 0);
		lv_display_enable_invalidation(NULL, false);
		dev_ioctl(TOUCH_DEV, TOUCH_SLEEP);
		dev_ioctl(LCD_DEV, LCD_SLEEP_IN);
	}
	screen_on = on;
}

bool screen_is_on(void)
{
	return screen_on;
}
