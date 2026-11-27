#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EQ_MAX_PARAM 20
#define EQ_BANDS 10
#define EQ_MAX_FILTERS (EQ_MAX_PARAM + EQ_BANDS)
#define EQ_MAX_CH 2
#define EQ_BAND_DB 12.0
#define EQ_PREAMP_MIN -30.0
#define EQ_PREAMP_MAX 12.0
#define EQ_REF_FS 48000.0
#define EQ_NAME_MAX 64
#define EQ_ID_MAX 384

enum eq_type { EQ_PK, EQ_LSC, EQ_HSC, EQ_LPQ, EQ_HPQ, EQ_NTYPES };

struct eq_filter {
	enum eq_type type;
	bool on;
	double fc, gain, q;
};

struct eq_preset {
	int nparam;
	struct eq_filter param[EQ_MAX_PARAM];
	double band[EQ_BANDS];
	bool auto_preamp;
	double preamp;
};

extern const double eq_band_fc[EQ_BANDS];
extern const char *const eq_type_names[EQ_NTYPES];

bool eq_type_has_gain(enum eq_type t);
bool eq_filter_active(const struct eq_filter *f);
void eq_preset_init(struct eq_preset *p);
void eq_band_filter(int i, double gain, struct eq_filter *f);
int eq_parse(const char *text, struct eq_preset *p);
int eq_format(const struct eq_preset *p, char *out, size_t len);
int eq_flatten(const struct eq_preset *p, bool param, bool graphic, struct eq_filter *out);
double eq_response_db(const struct eq_filter *f, int n, double fs, double hz);
double eq_auto_preamp(const struct eq_filter *f, int n);
double eq_preamp_db(const struct eq_preset *p);
double eq_step_r40(double v, int dir);

struct eq_biquad {
	double b0, b1, b2, a1, a2;
};

void eq_design(const struct eq_filter *f, double fs, struct eq_biquad *bq);

enum eq_fmt { EQ_S16, EQ_S24, EQ_S24_3, EQ_S32 };

struct eq_ramp {
	double cur, target, step;
	unsigned left;
};

struct eq_dsp {
	int nch;
	double fs;
	int nf;
	struct eq_filter f[EQ_MAX_FILTERS];
	struct eq_biquad bq[EQ_MAX_FILTERS];
	double z[EQ_MAX_CH][EQ_MAX_FILTERS][2];
	double pre_db;
	struct eq_ramp pre;
	int pend_nf;
	struct eq_filter pend_f[EQ_MAX_FILTERS];
	double pend_db;
	bool pending, fresh;
	int xf_dir;
	unsigned xf_left, xf_len;
	uint32_t rng;
};

void eq_dsp_init(struct eq_dsp *d, int nch, double fs);
void eq_dsp_set(struct eq_dsp *d, const struct eq_filter *f, int n, double preamp_db);
void eq_dsp_reset(struct eq_dsp *d);
bool eq_dsp_bypass(const struct eq_dsp *d);
void eq_dsp_run(struct eq_dsp *d, enum eq_fmt fmt, const char *const *src, const int *sstep, char *const *dst, const int *dstep,
		size_t frames);

struct eq_entry {
	char id[EQ_ID_MAX];
	char name[EQ_NAME_MAX];
	bool sd;
};

bool eq_name_valid(const char *name);
void eq_id_name(const char *id, char *out, size_t len);
const char *eq_id_path(const char *id, char *buf, size_t len);
int eq_load_file(const char *path, struct eq_preset *p);
int eq_store_load(const char *id, struct eq_preset *p);
int eq_store_list(struct eq_entry **out);
int eq_store_save(const char *name, const struct eq_preset *p);
int eq_store_delete(const char *name);
int eq_write_atomic(const char *path, const char *text);
