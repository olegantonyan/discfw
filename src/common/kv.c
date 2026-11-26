#include <disc/kv.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void kv_init(struct kvbuf *b)
{
	b->s = NULL;
	b->len = b->cap = 0;
}

void kv_free(struct kvbuf *b)
{
	free(b->s);
	kv_init(b);
}

static void put(struct kvbuf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 128;
		while (cap < b->len + n + 1)
			cap *= 2;
		char *p = realloc(b->s, cap);
		if (!p)
			abort();
		b->s = p;
		b->cap = cap;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = 0;
}

void kv_raw(struct kvbuf *b, const char *s)
{
	if (b->len)
		put(b, " ", 1);
	put(b, s, strlen(s));
}

void kv_append(struct kvbuf *b, const char *s)
{
	put(b, s, strlen(s));
}

void kv_str(struct kvbuf *b, const char *key, const char *val)
{
	char esc[4];

	if (b->len)
		put(b, " ", 1);
	put(b, key, strlen(key));
	put(b, "=", 1);
	for (const unsigned char *p = (const unsigned char *)(val ? val : ""); *p; p++) {
		if (*p <= ' ' || *p == '%' || *p == 0x7f) {
			snprintf(esc, sizeof(esc), "%%%02X", *p);
			put(b, esc, 3);
		} else {
			put(b, (const char *)p, 1);
		}
	}
}

void kv_int(struct kvbuf *b, const char *key, long val)
{
	char num[24];

	snprintf(num, sizeof(num), "%ld", val);
	kv_str(b, key, num);
}

void kv_end(struct kvbuf *b)
{
	put(b, "\n", 1);
}

static int hex(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

size_t kv_decode(const char *src, size_t n, char *out, size_t len)
{
	size_t o = 0;

	for (size_t i = 0; i < n && o + 1 < len; i++) {
		if (src[i] == '%' && i + 2 < n && hex(src[i + 1]) >= 0 && hex(src[i + 2]) >= 0) {
			out[o++] = hex(src[i + 1]) << 4 | hex(src[i + 2]);
			i += 2;
		} else {
			out[o++] = src[i];
		}
	}
	if (len)
		out[o] = 0;
	return o;
}

bool kv_get(const char *line, const char *key, char *out, size_t len)
{
	size_t kl = strlen(key);
	const char *p = line;

	while (*p) {
		while (*p == ' ')
			p++;
		const char *e = p;
		while (*e && *e != ' ' && *e != '\n')
			e++;
		if ((size_t)(e - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
			kv_decode(p + kl + 1, e - p - kl - 1, out, len);
			return true;
		}
		p = *e == '\n' ? e + 1 : e;
		if (*e == '\n')
			break;
	}
	if (len)
		out[0] = 0;
	return false;
}

long kv_get_int(const char *line, const char *key, long def)
{
	char v[24], *end;

	if (!kv_get(line, key, v, sizeof(v)) || !v[0])
		return def;
	long r = strtol(v, &end, 0);
	return *end ? def : r;
}

const char *kv_word(const char *line, int idx, char *out, size_t len)
{
	const char *p = line;

	for (int i = 0;; i++) {
		while (*p == ' ')
			p++;
		if (!*p || *p == '\n')
			break;
		const char *e = p;
		while (*e && *e != ' ' && *e != '\n')
			e++;
		if (i == idx) {
			kv_decode(p, e - p, out, len);
			return e;
		}
		p = e;
	}
	if (len)
		out[0] = 0;
	return NULL;
}
