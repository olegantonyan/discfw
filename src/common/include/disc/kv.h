#pragma once
#include <stdbool.h>
#include <stddef.h>

struct kvbuf {
	char *s;
	size_t len, cap;
};

void kv_init(struct kvbuf *b);
void kv_free(struct kvbuf *b);
void kv_raw(struct kvbuf *b, const char *s);
void kv_append(struct kvbuf *b, const char *s);
void kv_str(struct kvbuf *b, const char *key, const char *val);
void kv_int(struct kvbuf *b, const char *key, long val);
void kv_end(struct kvbuf *b);

bool kv_get(const char *line, const char *key, char *out, size_t len);
long kv_get_int(const char *line, const char *key, long def);
size_t kv_decode(const char *src, size_t n, char *out, size_t len);
const char *kv_word(const char *line, int idx, char *out, size_t len);
