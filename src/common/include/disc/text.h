#pragma once
#include <stdbool.h>
#include <stdint.h>

uint32_t utf8_next(const char **s);
int utf8_put(char *dst, uint32_t cp);
uint32_t uc_lower(uint32_t cp);
uint32_t uc_upper(uint32_t cp);
bool uc_is_letter(uint32_t cp);
int nat_cmp(const char *a, const char *b);
uint32_t index_letter(const char *s);
bool has_suffix_ci(const char *s, const char *suffix);
