#include <disc/text.h>

#include <string.h>
#include <strings.h>

uint32_t utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t cp;
	int n;

	if (!*p)
		return 0;
	if (*p < 0x80) {
		*s += 1;
		return *p;
	}
	if ((*p & 0xe0) == 0xc0) {
		cp = *p & 0x1f;
		n = 1;
	} else if ((*p & 0xf0) == 0xe0) {
		cp = *p & 0x0f;
		n = 2;
	} else if ((*p & 0xf8) == 0xf0) {
		cp = *p & 0x07;
		n = 3;
	} else {
		*s += 1;
		return 0xfffd;
	}
	for (int i = 1; i <= n; i++) {
		if ((p[i] & 0xc0) != 0x80) {
			*s += i;
			return 0xfffd;
		}
		cp = cp << 6 | (p[i] & 0x3f);
	}
	*s += n + 1;
	return cp;
}

int utf8_put(char *dst, uint32_t cp)
{
	unsigned char *d = (unsigned char *)dst;

	if (cp < 0x80) {
		d[0] = cp;
		return 1;
	}
	if (cp < 0x800) {
		d[0] = 0xc0 | cp >> 6;
		d[1] = 0x80 | (cp & 0x3f);
		return 2;
	}
	if (cp < 0x10000) {
		d[0] = 0xe0 | cp >> 12;
		d[1] = 0x80 | (cp >> 6 & 0x3f);
		d[2] = 0x80 | (cp & 0x3f);
		return 3;
	}
	d[0] = 0xf0 | cp >> 18;
	d[1] = 0x80 | (cp >> 12 & 0x3f);
	d[2] = 0x80 | (cp >> 6 & 0x3f);
	d[3] = 0x80 | (cp & 0x3f);
	return 4;
}

static bool in(uint32_t cp, uint32_t lo, uint32_t hi)
{
	return cp >= lo && cp <= hi;
}

static bool pair_upper_even(uint32_t cp)
{
	return in(cp, 0x0100, 0x012f) || in(cp, 0x0132, 0x0137) || in(cp, 0x014a, 0x0177) || in(cp, 0x0460, 0x0481) ||
	       in(cp, 0x048a, 0x04bf) || in(cp, 0x04d0, 0x04ff);
}

static bool pair_upper_odd(uint32_t cp)
{
	return in(cp, 0x0139, 0x0148) || in(cp, 0x0179, 0x017e) || in(cp, 0x04c1, 0x04ce);
}

uint32_t uc_lower(uint32_t cp)
{
	if (in(cp, 'A', 'Z'))
		return cp + 32;
	if (cp < 0xc0)
		return cp;
	if (in(cp, 0xc0, 0xde) && cp != 0xd7)
		return cp + 32;
	if (pair_upper_even(cp))
		return cp | 1;
	if (pair_upper_odd(cp))
		return cp & 1 ? cp + 1 : cp;
	if (cp == 0x0178)
		return 0xff;
	if (in(cp, 0x0391, 0x03a9) && cp != 0x03a2)
		return cp + 32;
	if (cp == 0x0386)
		return 0x03ac;
	if (in(cp, 0x0388, 0x038a))
		return cp + 37;
	if (cp == 0x038c)
		return 0x03cc;
	if (in(cp, 0x038e, 0x038f))
		return cp + 63;
	if (in(cp, 0x0410, 0x042f))
		return cp + 32;
	if (in(cp, 0x0400, 0x040f))
		return cp + 80;
	return cp;
}

uint32_t uc_upper(uint32_t cp)
{
	if (in(cp, 'a', 'z'))
		return cp - 32;
	if (cp < 0xe0)
		return cp;
	if (in(cp, 0xe0, 0xfe) && cp != 0xf7)
		return cp - 32;
	if (cp == 0xff)
		return 0x0178;
	if (pair_upper_even(cp))
		return cp & ~1u;
	if (pair_upper_odd(cp) && !(cp & 1))
		return cp - 1;
	if (in(cp, 0x03b1, 0x03c9) && cp != 0x03c2)
		return cp - 32;
	if (cp == 0x03ac)
		return 0x0386;
	if (in(cp, 0x03ad, 0x03af))
		return cp - 37;
	if (cp == 0x03cc)
		return 0x038c;
	if (in(cp, 0x03cd, 0x03ce))
		return cp - 63;
	if (in(cp, 0x0430, 0x044f))
		return cp - 32;
	if (in(cp, 0x0450, 0x045f))
		return cp - 80;
	return cp;
}

bool uc_is_letter(uint32_t cp)
{
	if (cp < 0x80)
		return in(cp, 'A', 'Z') || in(cp, 'a', 'z');
	if (in(cp, 0xc0, 0x24f))
		return cp != 0xd7 && cp != 0xf7;
	return in(cp, 0x0370, 0x03ff) || in(cp, 0x0400, 0x052f) || in(cp, 0x05d0, 0x06ff) || cp >= 0x2e80;
}

static bool is_digit(unsigned char c)
{
	return c >= '0' && c <= '9';
}

int nat_cmp(const char *a, const char *b)
{
	const char *a0 = a, *b0 = b;

	while (*a && *b) {
		if (is_digit(*a) && is_digit(*b)) {
			const char *ea, *eb;
			while (*a == '0' && is_digit(a[1]))
				a++;
			while (*b == '0' && is_digit(b[1]))
				b++;
			for (ea = a; is_digit(*ea); ea++)
				;
			for (eb = b; is_digit(*eb); eb++)
				;
			if (ea - a != eb - b)
				return ea - a < eb - b ? -1 : 1;
			int c = strncmp(a, b, ea - a);
			if (c)
				return c < 0 ? -1 : 1;
			a = ea;
			b = eb;
			continue;
		}
		uint32_t x = uc_lower(utf8_next(&a)), y = uc_lower(utf8_next(&b));
		if (x != y)
			return x < y ? -1 : 1;
	}
	if (*a || *b)
		return *a ? 1 : -1;
	int c = strcmp(a0, b0);
	return c < 0 ? -1 : c > 0;
}

uint32_t index_letter(const char *s)
{
	uint32_t cp;

	while ((cp = utf8_next(&s))) {
		if (cp < 0x80 && is_digit(cp))
			return '#';
		if (uc_is_letter(cp))
			return uc_upper(cp);
	}
	return '#';
}

bool has_suffix_ci(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && !strcasecmp(s + n - m, suffix);
}

bool filter_quote(char *out, size_t len, const char *v)
{
	size_t o = 0;

	if (len < 3)
		return false;
	out[o++] = '"';
	for (; *v; v++) {
		if (o + 4 > len)
			return false;
		if (*v == '"' || *v == '\\' || *v == '\'')
			out[o++] = '\\';
		out[o++] = *v;
	}
	out[o++] = '"';
	out[o] = 0;
	return true;
}

int year_of(const char *date)
{
	int y = 0;

	for (int i = 0; i < 4; i++) {
		if (!is_digit((unsigned char)date[i]))
			return 0;
		y = y * 10 + date[i] - '0';
	}
	return y;
}
