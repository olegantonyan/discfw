#pragma once
#include <lvgl/lvgl.h>
#include <stdbool.h>

lv_display_t *fbdisp_create(const char *dev, bool rot180);
int input_find(const char *name, char *path, size_t len);
void screen_power(bool on);
bool screen_is_on(void);
