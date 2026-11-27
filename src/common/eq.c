#include <disc/eq.h>
#include <disc/paths.h>
#include <disc/text.h>

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIN_DB 0.005
#define XF_MS 5
#define PRE_MS 20
#define DENORMAL 1e-25
#define BLOCK 256
#define MAX_FILE (64 * 1024)
#define SD_DIR "EQ"

const double eq_band_fc[EQ_BANDS] = { 31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
const char *const eq_type_names[EQ_NTYPES] = { "PK", "LSC", "HSC", "LPQ", "HPQ" };

bool eq_type_has_gain(enum eq_type t)
{
	return t == EQ_PK || t == EQ_LSC || t == EQ_HSC;
}

bool eq_filter_active(const struct eq_filter *f)
{
	return f->on && f->fc > 0 && (!eq_type_has_gain(f->type) || fabs(f->gain) >= MIN_DB);
}

void eq_preset_init(struct eq_preset *p)
{
	memset(p, 0, sizeof(*p));
	p->auto_preamp = true;
}

void eq_band_filter(int i, double gain, struct eq_filter *f)
{
	f->type = i == 0 ? EQ_LSC : i == EQ_BANDS - 1 ? EQ_HSC : EQ_PK;
	f->on = true;
	f->fc = eq_band_fc[i];
	f->gain = gain;
	f->q = 1.41;
}

static bool map_type(const char *t, enum eq_type *out)
{
	static const struct {
		const char *name;
		enum eq_type type;
	} names[] = {
		{ "PK", EQ_PK },   { "PEQ", EQ_PK },	{ "MODAL", EQ_PK },	{ "LS", EQ_LSC },	{ "LSC", EQ_LSC },
		{ "LSQ", EQ_LSC }, { "LOWSHELF", EQ_LSC }, { "HS", EQ_HSC },	{ "HSC", EQ_HSC },	{ "HSQ", EQ_HSC },
		{ "HIGHSHELF", EQ_HSC }, { "LP", EQ_LPQ }, { "LPQ", EQ_LPQ },	{ "LPF", EQ_LPQ },	{ "HP", EQ_HPQ },
		{ "HPQ", EQ_HPQ }, { "HPF", EQ_HPQ },
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcasecmp(t, names[i].name)) {
			*out = names[i].type;
			return true;
		}
	return false;
}

static bool number(const char *s, double *v)
{
	char *end;
	const char *d = s + (*s == '+' || *s == '-');

	if (!((*d >= '0' && *d <= '9') || (*d == '.' && d[1] >= '0' && d[1] <= '9')))
		return false;
	*v = strtod(*s == '+' ? s + 1 : s, &end);
	return end != s && isfinite(*v);
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		*--e = 0;
	return s;
}

static bool parse_filter(char *rest, struct eq_filter *f)
{
	char *save = NULL, *tok, *key = NULL;
	bool have_fc = false;
	double v;
	int i = 0;

	memset(f, 0, sizeof(*f));
	f->q = 0.707;
	for (tok = strtok_r(rest, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save), i++) {
		if (i == 0) {
			if (strcasecmp(tok, "ON") && strcasecmp(tok, "OFF"))
				return false;
			f->on = !strcasecmp(tok, "ON");
		} else if (i == 1) {
			if (!map_type(tok, &f->type))
				return false;
		} else if (key) {
			if (number(tok, &v)) {
				if (!strcasecmp(key, "Fc")) {
					f->fc = v;
					have_fc = true;
				} else if (!strcasecmp(key, "Gain")) {
					f->gain = v;
				} else if (!strcasecmp(key, "Q")) {
					f->q = v > 0 ? v : 0.707;
				}
			}
			key = NULL;
		} else if (!strcasecmp(tok, "Fc") || !strcasecmp(tok, "Gain") || !strcasecmp(tok, "Q")) {
			key = tok;
		}
	}
	return have_fc && f->fc > 0;
}

static bool filter_line(char *line, char **rest)
{
	char *colon = strchr(line, ':'), *p;

	if (!colon || strncasecmp(line, "Filter", 6))
		return false;
	for (p = line + 6; p < colon; p++)
		if (!(*p >= '0' && *p <= '9') && *p != ' ' && *p != '\t')
			return false;
	*rest = colon + 1;
	return true;
}

static void marker(const char *line, bool *auto_mark, bool *graphic)
{
	line++;
	while (*line == ' ' || *line == '\t')
		line++;
	if (strncasecmp(line, "discfw:", 7))
		return;
	line += 7;
	while (*line == ' ' || *line == '\t')
		line++;
	if (!strncasecmp(line, "preamp auto", 11))
		*auto_mark = true;
	else if (!strncasecmp(line, "graphic", 7))
		*graphic = true;
}

int eq_parse(const char *text, struct eq_preset *p)
{
	bool auto_mark = false, graphic = false, have_pre = false;
	double pre = 0;
	int nband = 0, count = 0;
	const char *s = text;

	eq_preset_init(p);
	if (!strncmp(s, "\xEF\xBB\xBF", 3))
		s += 3;
	while (*s) {
		size_t n = strcspn(s, "\n");
		char buf[512], *line, *rest;
		struct eq_filter f;

		snprintf(buf, sizeof(buf), "%.*s", (int)(n < sizeof(buf) ? n : sizeof(buf) - 1), s);
		s += n + (s[n] == '\n');
		line = trim(buf);
		if (line[0] == '#') {
			marker(line, &auto_mark, &graphic);
		} else if (!strncasecmp(line, "Preamp:", 7)) {
			char *v = trim(line + 7);
			if (number(v, &pre))
				have_pre = true;
		} else if (filter_line(line, &rest) && parse_filter(rest, &f)) {
			if (graphic) {
				if (nband < EQ_BANDS)
					p->band[nband++] = eq_type_has_gain(f.type) && f.on ? f.gain : 0;
			} else if (p->nparam < EQ_MAX_PARAM) {
				p->param[p->nparam++] = f;
			}
			count++;
		}
	}
	p->auto_preamp = auto_mark || !have_pre;
	p->preamp = have_pre ? pre : 0;
	return count;
}

static void put(char *out, size_t len, size_t *pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void put(char *out, size_t len, size_t *pos, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(*pos < len ? out + *pos : NULL, *pos < len ? len - *pos : 0, fmt, ap);
	va_end(ap);
	*pos += n > 0 ? (size_t)n : 0;
}

static void put_filter(char *out, size_t len, size_t *pos, int idx, const struct eq_filter *f)
{
	put(out, len, pos, "Filter %d: %s %s Fc %.6g Hz", idx, f->on ? "ON" : "OFF", eq_type_names[f->type], f->fc);
	if (eq_type_has_gain(f->type))
		put(out, len, pos, " Gain %.6g dB", fabs(f->gain) < MIN_DB ? 0.0 : f->gain);
	put(out, len, pos, " Q %.6g\n", f->q);
}

int eq_format(const struct eq_preset *p, char *out, size_t len)
{
	size_t pos = 0;
	int idx = 1;

	if (p->auto_preamp)
		put(out, len, &pos, "# discfw: preamp auto\n");
	put(out, len, &pos, "Preamp: %.6g dB\n", eq_preamp_db(p));
	for (int i = 0; i < p->nparam; i++)
		put_filter(out, len, &pos, idx++, &p->param[i]);
	put(out, len, &pos, "# discfw: graphic\n");
	for (int i = 0; i < EQ_BANDS; i++) {
		struct eq_filter f;
		eq_band_filter(i, p->band[i], &f);
		put_filter(out, len, &pos, idx++, &f);
	}
	return pos < len ? (int)pos : -1;
}

int eq_flatten(const struct eq_preset *p, bool param, bool graphic, struct eq_filter *out)
{
	int n = 0;

	for (int i = 0; param && i < p->nparam; i++)
		if (eq_filter_active(&p->param[i]))
			out[n++] = p->param[i];
	for (int i = 0; graphic && i < EQ_BANDS; i++)
		if (fabs(p->band[i]) >= MIN_DB)
			eq_band_filter(i, p->band[i], &out[n++]);
	return n;
}

void eq_design(const struct eq_filter *f, double fs, struct eq_biquad *bq)
{
	double q = f->q > 1e-6 ? f->q : 1e-6;
	double w0, cw, sw, alpha, A, s2a, b0, b1, b2, a0, a1, a2;

	*bq = (struct eq_biquad){ .b0 = 1 };
	if (fs <= 0 || f->fc <= 0)
		return;
	w0 = 2 * M_PI * fmin(f->fc, fs * 0.5 * 0.999) / fs;
	cw = cos(w0);
	sw = sin(w0);
	alpha = sw / (2 * q);
	A = pow(10.0, f->gain / 40.0);
	s2a = 2 * sqrt(A) * alpha;
	switch (f->type) {
	case EQ_PK:
		b0 = 1 + alpha * A;
		b1 = -2 * cw;
		b2 = 1 - alpha * A;
		a0 = 1 + alpha / A;
		a1 = -2 * cw;
		a2 = 1 - alpha / A;
		break;
	case EQ_LSC:
		b0 = A * ((A + 1) - (A - 1) * cw + s2a);
		b1 = 2 * A * ((A - 1) - (A + 1) * cw);
		b2 = A * ((A + 1) - (A - 1) * cw - s2a);
		a0 = (A + 1) + (A - 1) * cw + s2a;
		a1 = -2 * ((A - 1) + (A + 1) * cw);
		a2 = (A + 1) + (A - 1) * cw - s2a;
		break;
	case EQ_HSC:
		b0 = A * ((A + 1) + (A - 1) * cw + s2a);
		b1 = -2 * A * ((A - 1) + (A + 1) * cw);
		b2 = A * ((A + 1) + (A - 1) * cw - s2a);
		a0 = (A + 1) - (A - 1) * cw + s2a;
		a1 = 2 * ((A - 1) - (A + 1) * cw);
		a2 = (A + 1) - (A - 1) * cw - s2a;
		break;
	case EQ_LPQ:
		b0 = (1 - cw) / 2;
		b1 = 1 - cw;
		b2 = (1 - cw) / 2;
		a0 = 1 + alpha;
		a1 = -2 * cw;
		a2 = 1 - alpha;
		break;
	case EQ_HPQ:
		b0 = (1 + cw) / 2;
		b1 = -(1 + cw);
		b2 = (1 + cw) / 2;
		a0 = 1 + alpha;
		a1 = -2 * cw;
		a2 = 1 - alpha;
		break;
	default:
		return;
	}
	*bq = (struct eq_biquad){ b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

static double bq_db(const struct eq_biquad *b, double c1, double s1, double c2, double s2)
{
	double nr = b->b0 + b->b1 * c1 + b->b2 * c2, ni = b->b1 * s1 + b->b2 * s2;
	double dr = 1 + b->a1 * c1 + b->a2 * c2, di = b->a1 * s1 + b->a2 * s2;
	double num = nr * nr + ni * ni, den = dr * dr + di * di;

	return num > 0 && den > 0 ? 10 * log10(num / den) : -200;
}

static double sum_db(const struct eq_biquad *bq, int n, double fs, double hz)
{
	double w = 2 * M_PI * hz / fs, db = 0;
	double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);

	if (w <= 0 || w >= M_PI)
		return 0;
	for (int k = 0; k < n; k++)
		db += bq_db(&bq[k], c1, s1, c2, s2);
	return db;
}

double eq_response_db(const struct eq_filter *f, int n, double fs, double hz)
{
	struct eq_biquad bq[EQ_MAX_FILTERS];

	if (n > EQ_MAX_FILTERS)
		n = EQ_MAX_FILTERS;
	for (int k = 0; k < n; k++)
		eq_design(&f[k], fs, &bq[k]);
	return sum_db(bq, n, fs, hz);
}

double eq_auto_preamp(const struct eq_filter *f, int n)
{
	struct eq_biquad bq[EQ_MAX_FILTERS];
	double peak = 0;

	if (n > EQ_MAX_FILTERS)
		n = EQ_MAX_FILTERS;
	for (int k = 0; k < n; k++)
		eq_design(&f[k], EQ_REF_FS, &bq[k]);
	for (double hz = 20; hz <= 20000; hz *= 1.0293022366434921)
		peak = fmax(peak, sum_db(bq, n, EQ_REF_FS, hz));
	for (int k = 0; k < n; k++)
		if (f[k].fc >= 20 && f[k].fc <= 20000)
			peak = fmax(peak, sum_db(bq, n, EQ_REF_FS, f[k].fc));
	return peak > 0 ? -ceil(peak * 10 - 1e-9) / 10 : 0;
}

double eq_preamp_db(const struct eq_preset *p)
{
	struct eq_filter f[EQ_MAX_FILTERS];

	if (!p->auto_preamp)
		return p->preamp;
	return eq_auto_preamp(f, eq_flatten(p, true, true, f));
}

double eq_step_r40(double v, int dir)
{
	static const double r40[] = { 1.00, 1.06, 1.12, 1.18, 1.25, 1.32, 1.40, 1.50, 1.60, 1.70, 1.80, 1.90, 2.00, 2.12,
				      2.24, 2.36, 2.50, 2.65, 2.80, 3.00, 3.15, 3.35, 3.55, 3.75, 4.00, 4.25, 4.50, 4.75,
				      5.00, 5.30, 5.60, 6.00, 6.30, 6.70, 7.10, 7.50, 8.00, 8.50, 9.00, 9.50 };
	const int n = sizeof(r40) / sizeof(r40[0]);
	double dec, m;

	if (v <= 0)
		return 1;
	dec = pow(10.0, floor(log10(v) + 1e-9));
	m = v / dec;
	if (dir > 0) {
		for (int i = 0; i < n; i++)
			if (r40[i] > m * (1 + 1e-9))
				return r40[i] * dec;
		return 10 * dec;
	}
	for (int i = n - 1; i >= 0; i--)
		if (r40[i] < m * (1 - 1e-9))
			return r40[i] * dec;
	return r40[n - 1] * dec / 10;
}

static double db_lin(double db)
{
	return pow(10.0, db / 20.0);
}

static void ramp_to(struct eq_ramp *r, double target, unsigned len)
{
	r->target = target;
	if (!len || fabs(target - r->cur) < 1e-12) {
		r->cur = target;
		r->left = 0;
		return;
	}
	r->step = (target - r->cur) / len;
	r->left = len;
}

static void design_all(struct eq_dsp *d)
{
	for (int k = 0; k < d->nf; k++)
		eq_design(&d->f[k], d->fs, &d->bq[k]);
}

static void apply_pending(struct eq_dsp *d)
{
	d->nf = d->pend_nf;
	memcpy(d->f, d->pend_f, sizeof(d->f[0]) * d->nf);
	d->pre_db = d->pend_db;
	design_all(d);
	ramp_to(&d->pre, db_lin(d->pre_db), 0);
	memset(d->z, 0, sizeof(d->z));
	d->pending = false;
}

void eq_dsp_init(struct eq_dsp *d, int nch, double fs)
{
	struct eq_filter f[EQ_MAX_FILTERS];
	int nf = d->nf;
	double pre = d->pre_db;

	if (d->pending) {
		nf = d->pend_nf;
		pre = d->pend_db;
		memcpy(f, d->pend_f, sizeof(f[0]) * nf);
	} else {
		memcpy(f, d->f, sizeof(f[0]) * nf);
	}
	memset(d, 0, sizeof(*d));
	d->nch = nch < 1 ? 1 : nch > EQ_MAX_CH ? EQ_MAX_CH : nch;
	d->fs = fs;
	d->xf_len = fs * XF_MS / 1000 > 1 ? (unsigned)(fs * XF_MS / 1000) : 1;
	d->rng = 0x9E3779B9u;
	d->pre.cur = d->pre.target = 1;
	d->fresh = true;
	eq_dsp_set(d, f, nf, pre);
}

void eq_dsp_set(struct eq_dsp *d, const struct eq_filter *f, int n, double preamp_db)
{
	if (n > EQ_MAX_FILTERS)
		n = EQ_MAX_FILTERS;
	if (n < 0)
		n = 0;
	d->pend_nf = n;
	if (n)
		memcpy(d->pend_f, f, sizeof(f[0]) * n);
	d->pend_db = preamp_db;
	d->pending = true;
	if (d->fresh) {
		apply_pending(d);
		return;
	}
	if (d->xf_left) {
		if (d->xf_dir > 0) {
			d->xf_dir = -1;
			d->xf_left = d->xf_len - d->xf_left;
		}
		return;
	}
	if (n == d->nf) {
		memcpy(d->f, d->pend_f, sizeof(f[0]) * n);
		d->pre_db = preamp_db;
		design_all(d);
		ramp_to(&d->pre, db_lin(preamp_db), d->fs * PRE_MS / 1000);
		d->pending = false;
		return;
	}
	d->xf_dir = -1;
	d->xf_left = d->xf_len;
}

void eq_dsp_reset(struct eq_dsp *d)
{
	if (d->pending)
		apply_pending(d);
	memset(d->z, 0, sizeof(d->z));
	d->xf_left = 0;
	ramp_to(&d->pre, d->pre.target, 0);
	d->fresh = true;
}

bool eq_dsp_bypass(const struct eq_dsp *d)
{
	return !d->nf && !d->pending && !d->xf_left && !d->pre.left && d->pre.cur == 1.0;
}

static inline double next_pre(struct eq_dsp *d)
{
	double g = d->pre.cur;

	if (d->pre.left && !--d->pre.left)
		d->pre.cur = d->pre.target;
	else if (d->pre.left)
		d->pre.cur += d->pre.step;
	return g;
}

static inline double next_fade(struct eq_dsp *d)
{
	double lvl;

	if (!d->xf_left)
		return 1;
	lvl = (double)--d->xf_left / d->xf_len;
	return d->xf_dir > 0 ? 1 - lvl : lvl;
}

static void filter_block(struct eq_dsp *d, double (*x)[BLOCK], size_t n)
{
	for (int k = 0; k < d->nf; k++) {
		const struct eq_biquad b = d->bq[k];
		double *l = x[0], *r = x[1];
		double l1 = d->z[0][k][0], l2 = d->z[0][k][1], r1 = d->z[1][k][0], r2 = d->z[1][k][1];

		if (d->nch == 2) {
			for (size_t i = 0; i < n; i++) {
				double xl = l[i], xr = r[i];
				double yl = b.b0 * xl + l1, yr = b.b0 * xr + r1;
				l1 = b.b1 * xl - b.a1 * yl + l2;
				r1 = b.b1 * xr - b.a1 * yr + r2;
				l2 = b.b2 * xl - b.a2 * yl;
				r2 = b.b2 * xr - b.a2 * yr;
				l[i] = yl;
				r[i] = yr;
			}
		} else {
			for (size_t i = 0; i < n; i++) {
				double xl = l[i], yl = b.b0 * xl + l1;
				l1 = b.b1 * xl - b.a1 * yl + l2;
				l2 = b.b2 * xl - b.a2 * yl;
				l[i] = yl;
			}
		}
		d->z[0][k][0] = l1;
		d->z[0][k][1] = l2;
		d->z[1][k][0] = r1;
		d->z[1][k][1] = r2;
	}
}

static inline int32_t quant(double v, double max)
{
	v += v < 0 ? -0.5 : 0.5;
	if (v > max)
		v = max;
	else if (v < -max - 1)
		v = -max - 1;
	return (int32_t)v;
}

static inline double tpdf(uint32_t *s)
{
	double r[2];

	for (int i = 0; i < 2; i++) {
		*s ^= *s << 13;
		*s ^= *s >> 17;
		*s ^= *s << 5;
		r[i] = (*s >> 8) * (1.0 / 16777216.0);
	}
	return r[0] - r[1];
}

static inline double ld_s16(const char *p)
{
	int16_t v;
	memcpy(&v, p, 2);
	return v * (1.0 / 32768.0);
}

static inline double ld_s24(const char *p)
{
	int32_t v;
	memcpy(&v, p, 4);
	return (int32_t)((uint32_t)v << 8) * (1.0 / 2147483648.0);
}

static inline double ld_s24_3(const char *p)
{
	const unsigned char *u = (const unsigned char *)p;
	uint32_t v = (uint32_t)u[0] << 8 | (uint32_t)u[1] << 16 | (uint32_t)u[2] << 24;
	return (int32_t)v * (1.0 / 2147483648.0);
}

static inline double ld_s32(const char *p)
{
	int32_t v;
	memcpy(&v, p, 4);
	return v * (1.0 / 2147483648.0);
}

#define ST_S16(p, x)                                                        \
	do {                                                                \
		int16_t v_ = (int16_t)quant((x) * 32768.0 + tpdf(&d->rng), 32767.0); \
		memcpy(p, &v_, 2);                                          \
	} while (0)
#define ST_S24(p, x)                                         \
	do {                                                 \
		int32_t v_ = quant((x) * 8388608.0, 8388607.0); \
		memcpy(p, &v_, 4);                           \
	} while (0)
#define ST_S24_3(p, x)                                       \
	do {                                                 \
		int32_t v_ = quant((x) * 8388608.0, 8388607.0); \
		(p)[0] = (char)(v_ & 0xff);                  \
		(p)[1] = (char)(v_ >> 8 & 0xff);             \
		(p)[2] = (char)(v_ >> 16 & 0xff);            \
	} while (0)
#define ST_S32(p, x)                                               \
	do {                                                       \
		int32_t v_ = quant((x) * 2147483648.0, 2147483647.0); \
		memcpy(p, &v_, 4);                                 \
	} while (0)

#define LOAD(LD)                                           \
	for (size_t i = 0; i < n; i++) {                   \
		double g = next_pre(d);                    \
		for (int c = 0; c < nch; c++) {            \
			x[c][i] = LD(sp[c]) * g;           \
			sp[c] += sstep[c];                 \
		}                                          \
		fade[i] = next_fade(d);                    \
	}
#define STORE(ST)                                          \
	for (size_t i = 0; i < n; i++)                     \
		for (int c = 0; c < nch; c++) {            \
			ST(dp[c], x[c][i] * fade[i]);      \
			dp[c] += dstep[c];                 \
		}

static const int widths[] = { [EQ_S16] = 2, [EQ_S24] = 4, [EQ_S24_3] = 3, [EQ_S32] = 4 };

static void copy(int nch, int w, const char *const *src, const int *sstep, char *const *dst, const int *dstep, size_t frames)
{
	bool packed = true;

	for (int c = 0; c < nch; c++)
		packed = packed && src[c] == src[0] + c * w && dst[c] == dst[0] + c * w && sstep[c] == nch * w && dstep[c] == nch * w;
	if (packed) {
		memcpy(dst[0], src[0], frames * nch * w);
		return;
	}
	for (int c = 0; c < nch; c++)
		for (size_t i = 0; i < frames; i++)
			memcpy(dst[c] + i * dstep[c], src[c] + i * sstep[c], w);
}

static void flush_denormals(struct eq_dsp *d)
{
	for (int c = 0; c < d->nch; c++)
		for (int k = 0; k < d->nf; k++)
			for (int j = 0; j < 2; j++)
				if (fabs(d->z[c][k][j]) < DENORMAL)
					d->z[c][k][j] = 0;
}

void eq_dsp_run(struct eq_dsp *d, enum eq_fmt fmt, const char *const *src, const int *sstep, char *const *dst, const int *dstep,
		size_t frames)
{
	const char *sp[EQ_MAX_CH];
	char *dp[EQ_MAX_CH];
	double x[EQ_MAX_CH][BLOCK], fade[BLOCK];
	int nch = d->nch;

	if (!frames)
		return;
	d->fresh = false;
	if (eq_dsp_bypass(d)) {
		copy(nch, widths[fmt], src, sstep, dst, dstep, frames);
		return;
	}
	for (int c = 0; c < nch; c++) {
		sp[c] = src[c];
		dp[c] = dst[c];
	}
	while (frames) {
		size_t n = frames < BLOCK ? frames : BLOCK;

		if (d->xf_left && d->xf_dir < 0 && n > d->xf_left)
			n = d->xf_left;
		switch (fmt) {
		case EQ_S16:
			LOAD(ld_s16);
			break;
		case EQ_S24:
			LOAD(ld_s24);
			break;
		case EQ_S24_3:
			LOAD(ld_s24_3);
			break;
		case EQ_S32:
			LOAD(ld_s32);
			break;
		}
		filter_block(d, x, n);
		switch (fmt) {
		case EQ_S16:
			STORE(ST_S16);
			break;
		case EQ_S24:
			STORE(ST_S24);
			break;
		case EQ_S24_3:
			STORE(ST_S24_3);
			break;
		case EQ_S32:
			STORE(ST_S32);
			break;
		}
		if (d->xf_dir < 0 && !d->xf_left) {
			apply_pending(d);
			d->xf_dir = 1;
			d->xf_left = d->xf_len;
		}
		frames -= n;
	}
	flush_denormals(d);
}

bool eq_name_valid(const char *name)
{
	size_t n = strlen(name);

	if (!n || n >= EQ_NAME_MAX - 4 || name[0] == '.' || name[0] == ' ' || name[n - 1] == ' ')
		return false;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++)
		if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\')
			return false;
	return true;
}

static bool rel_valid(const char *rel)
{
	if (!*rel || rel[0] == '/' || !has_suffix_ci(rel, ".txt"))
		return false;
	for (const char *p = rel; *p; p = strchr(p, '/') ? strchr(p, '/') + 1 : p + strlen(p))
		if (p[0] == '.')
			return false;
	return true;
}

static void strip_suffix_ci(char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);

	if (n > m && !strcasecmp(s + n - m, suffix))
		s[n - m] = 0;
}

void eq_id_name(const char *id, char *out, size_t len)
{
	const char *base;

	if (!strncmp(id, "my:", 3)) {
		snprintf(out, len, "%s", id + 3);
		return;
	}
	if (strncmp(id, "sd:", 3)) {
		snprintf(out, len, "%s", id);
		return;
	}
	base = strrchr(id + 3, '/');
	snprintf(out, len, "%s", base ? base + 1 : id + 3);
	strip_suffix_ci(out, ".txt");
	strip_suffix_ci(out, " ParametricEQ");
}

const char *eq_id_path(const char *id, char *buf, size_t len)
{
	if (!strncmp(id, "my:", 3) && eq_name_valid(id + 3))
		snprintf(buf, len, "%s/%s.txt", disc_data_path("eq"), id + 3);
	else if (!strncmp(id, "sd:", 3) && rel_valid(id + 3))
		snprintf(buf, len, "%s/" SD_DIR "/%s", disc_music_root(), id + 3);
	else
		return NULL;
	return buf;
}

int eq_load_file(const char *path, struct eq_preset *p)
{
	FILE *f = fopen(path, "r");
	char *text;
	size_t n;
	int r;

	if (!f)
		return -1;
	text = malloc(MAX_FILE + 1);
	if (!text) {
		fclose(f);
		return -1;
	}
	n = fread(text, 1, MAX_FILE, f);
	fclose(f);
	text[n] = 0;
	r = eq_parse(text, p);
	free(text);
	return r > 0 || !p->auto_preamp ? 0 : -1;
}

int eq_store_load(const char *id, struct eq_preset *p)
{
	char path[512];

	if (!eq_id_path(id, path, sizeof(path)))
		return -1;
	return eq_load_file(path, p);
}

struct list {
	struct eq_entry *v;
	int n, cap;
};

static void add(struct list *l, const char *id, bool sd)
{
	struct eq_entry *e;

	if (l->n == l->cap) {
		int cap = l->cap ? l->cap * 2 : 16;
		struct eq_entry *v = realloc(l->v, sizeof(*v) * cap);
		if (!v)
			return;
		l->v = v;
		l->cap = cap;
	}
	e = &l->v[l->n++];
	snprintf(e->id, sizeof(e->id), "%s", id);
	eq_id_name(id, e->name, sizeof(e->name));
	e->sd = sd;
}

static bool preset_file(const char *name)
{
	return name[0] != '.' && has_suffix_ci(name, ".txt") && !has_suffix_ci(name, "GraphicEQ.txt");
}

static void scan_sd(struct list *l, const char *dir, const char *rel, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d))) {
		char path[512], sub[EQ_ID_MAX];
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		snprintf(sub, sizeof(sub), "%s%s%s", rel, *rel ? "/" : "", e->d_name);
		if (stat(path, &st))
			continue;
		if (S_ISDIR(st.st_mode) && depth > 0) {
			scan_sd(l, path, sub, depth - 1);
		} else if (S_ISREG(st.st_mode) && preset_file(e->d_name)) {
			char id[EQ_ID_MAX + 4];
			snprintf(id, sizeof(id), "sd:%s", sub);
			if (strlen(id) < EQ_ID_MAX)
				add(l, id, true);
		}
	}
	closedir(d);
}

static int cmp_entry(const void *a, const void *b)
{
	const struct eq_entry *x = a, *y = b;

	if (x->sd != y->sd)
		return x->sd - y->sd;
	return nat_cmp(x->name, y->name);
}

int eq_store_list(struct eq_entry **out)
{
	struct list l = { 0 };
	char dir[512];
	DIR *d = opendir(disc_data_path("eq"));
	struct dirent *e;

	if (d) {
		while ((e = readdir(d))) {
			char name[EQ_NAME_MAX], id[EQ_NAME_MAX + 4];
			size_t n = strlen(e->d_name);
			if (!preset_file(e->d_name) || n >= sizeof(name))
				continue;
			memcpy(name, e->d_name, n + 1);
			strip_suffix_ci(name, ".txt");
			if (!eq_name_valid(name))
				continue;
			snprintf(id, sizeof(id), "my:%s", name);
			add(&l, id, false);
		}
		closedir(d);
	}
	snprintf(dir, sizeof(dir), "%s/" SD_DIR, disc_music_root());
	scan_sd(&l, dir, "", 1);
	if (l.n)
		qsort(l.v, l.n, sizeof(l.v[0]), cmp_entry);
	*out = l.v;
	return l.n;
}

int eq_write_atomic(const char *path, const char *text)
{
	char tmp[520];
	FILE *f;
	int ok;

	snprintf(tmp, sizeof(tmp), "%s.new", path);
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	ok = fputs(text, f) >= 0;
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path)) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int eq_store_save(const char *name, const struct eq_preset *p)
{
	char path[512], text[4096];

	if (!eq_name_valid(name) || eq_format(p, text, sizeof(text)) < 0)
		return -1;
	if (mkdir(disc_data_path("eq"), 0755) && errno != EEXIST)
		return -1;
	snprintf(path, sizeof(path), "%s/%s.txt", disc_data_path("eq"), name);
	return eq_write_atomic(path, text);
}

int eq_store_delete(const char *name)
{
	char path[512];

	if (!eq_name_valid(name))
		return -1;
	snprintf(path, sizeof(path), "%s/%s.txt", disc_data_path("eq"), name);
	return unlink(path) && errno != ENOENT ? -1 : 0;
}
