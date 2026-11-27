#include "check.h"
#include <disc/eq.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

static const char autoeq[] = "\xEF\xBB\xBFPreamp: -6.5 dB\r\n"
			     "Filter 1: ON LSC Fc 105 Hz Gain -8.4 dB Q 0.70\r\n"
			     "Filter 2: ON PK Fc 210 Hz Gain -3.2 dB Q 1.20\n"
			     "Filter 3: OFF PK Fc 1000 Hz Gain 2 dB Q 1\n"
			     "Filter 4: ON HS Fc 10000 Hz Gain +3.5 dB Q 0.7\n"
			     "Filter 5: ON None\n"
			     "Filter: ON LPQ Fc 18000 Hz\n"
			     "Filter 7: ON PK Gain 3 dB Q 1\n"
			     "# comment\n"
			     "Channel: L\n";

static void test_parse(void)
{
	struct eq_preset p;

	CHECK(eq_parse(autoeq, &p) == 5);
	CHECK(p.nparam == 5);
	CHECK(!p.auto_preamp && NEAR(p.preamp, -6.5, 1e-9));
	CHECK(p.param[0].type == EQ_LSC && p.param[0].on && NEAR(p.param[0].fc, 105, 1e-9) && NEAR(p.param[0].gain, -8.4, 1e-9) &&
	      NEAR(p.param[0].q, 0.7, 1e-9));
	CHECK(p.param[2].type == EQ_PK && !p.param[2].on);
	CHECK(p.param[3].type == EQ_HSC && NEAR(p.param[3].gain, 3.5, 1e-9));
	CHECK(p.param[4].type == EQ_LPQ && NEAR(p.param[4].q, 0.707, 1e-9));
	for (int i = 0; i < EQ_BANDS; i++)
		CHECK(p.band[i] == 0);

	CHECK(eq_parse("Filter 1: ON PK Fc 100 Hz Gain -3 dB Q 1\n", &p) == 1);
	CHECK(p.auto_preamp);
	CHECK(eq_parse("garbage\nmore", &p) == 0);
}

static void test_roundtrip(void)
{
	struct eq_preset p, q;
	char text[4096];

	eq_preset_init(&p);
	p.nparam = 2;
	p.param[0] = (struct eq_filter){ EQ_LSC, true, 105, -8.4, 0.7 };
	p.param[1] = (struct eq_filter){ EQ_HPQ, false, 20, 0, 0.5 };
	p.band[1] = -3.5;
	p.band[9] = 2;
	CHECK(eq_format(&p, text, sizeof(text)) > 0);
	CHECK(strstr(text, "# discfw: preamp auto\n") != NULL);
	CHECK(strstr(text, "Filter 2: OFF HPQ Fc 20 Hz Q 0.5\n") != NULL);
	CHECK(strstr(text, "Filter 4: ON PK Fc 63 Hz Gain -3.5 dB Q 1.41\n") != NULL);
	CHECK(eq_parse(text, &q) == 12);
	CHECK(q.auto_preamp && q.nparam == 2);
	CHECK(q.param[0].type == EQ_LSC && NEAR(q.param[0].gain, -8.4, 1e-9));
	CHECK(q.param[1].type == EQ_HPQ && !q.param[1].on);
	CHECK(NEAR(q.band[1], -3.5, 1e-9) && NEAR(q.band[9], 2, 1e-9) && q.band[0] == 0);

	p.auto_preamp = false;
	p.preamp = -4.5;
	CHECK(eq_format(&p, text, sizeof(text)) > 0);
	CHECK(eq_parse(text, &q) == 12);
	CHECK(!q.auto_preamp && NEAR(q.preamp, -4.5, 1e-9));
	CHECK(eq_format(&p, text, 16) < 0);
}

static void test_response(void)
{
	struct eq_filter pk = { EQ_PK, true, 1000, 6, 1 }, ls = { EQ_LSC, true, 100, -6, 0.707 }, flat = { EQ_PK, true, 1000, 0, 1 };
	struct eq_filter f[EQ_MAX_FILTERS];
	struct eq_biquad bq;
	struct eq_preset p;

	CHECK(NEAR(eq_response_db(&pk, 1, 48000, 1000), 6, 0.01));
	CHECK(NEAR(eq_response_db(&pk, 1, 48000, 20000), 0, 0.1));
	CHECK(NEAR(eq_response_db(&ls, 1, 48000, 20), -6, 0.3));
	CHECK(NEAR(eq_response_db(&ls, 1, 48000, 10000), 0, 0.05));
	eq_design(&flat, 48000, &bq);
	CHECK(NEAR(bq.b0, 1, 1e-12) && NEAR(bq.b1, bq.a1, 1e-12) && NEAR(bq.b2, bq.a2, 1e-12));
	CHECK(!eq_filter_active(&flat));

	eq_preset_init(&p);
	CHECK(eq_preamp_db(&p) == 0);
	p.band[3] = 6;
	CHECK(eq_preamp_db(&p) <= -6 && eq_preamp_db(&p) >= -6.2);
	p.band[3] = -6;
	CHECK(eq_preamp_db(&p) == 0);
	p.nparam = 1;
	p.param[0] = (struct eq_filter){ EQ_PK, true, 3000, 4, 4 };
	CHECK(eq_flatten(&p, true, true, f) == 2);
	CHECK(eq_flatten(&p, true, false, f) == 1);
	CHECK(eq_preamp_db(&p) <= -4 && eq_preamp_db(&p) >= -4.2);
	p.auto_preamp = false;
	p.preamp = -1;
	CHECK(eq_preamp_db(&p) == -1);

	CHECK(NEAR(eq_step_r40(100, 1), 106, 1e-9));
	CHECK(NEAR(eq_step_r40(100, -1), 95, 1e-9));
	CHECK(NEAR(eq_step_r40(31.5, 1), 33.5, 1e-9));
	CHECK(NEAR(eq_step_r40(105, 1), 106, 1e-9));
	CHECK(NEAR(eq_step_r40(0.7, -1), 0.67, 1e-9));
}

#define FRAMES 48000

static void run_s32(struct eq_dsp *d, const int32_t *in, int32_t *out, size_t frames)
{
	const char *src[2] = { (const char *)in, (const char *)(in + 1) };
	char *dst[2] = { (char *)out, (char *)(out + 1) };
	int step[2] = { 8, 8 };

	eq_dsp_run(d, EQ_S32, src, step, dst, step, frames);
}

static void sine(int32_t *buf, size_t frames, double hz, double amp)
{
	for (size_t i = 0; i < frames; i++)
		buf[2 * i] = buf[2 * i + 1] = (int32_t)lrint(amp * 2147483647.0 * sin(2 * M_PI * hz * i / 48000));
}

static double peak(const int32_t *buf, size_t from, size_t to)
{
	double m = 0;

	for (size_t i = from; i < to; i++)
		m = fmax(m, fabs(buf[2 * i] / 2147483648.0));
	return m;
}

static void test_dsp(void)
{
	static int32_t in[FRAMES * 2], out[FRAMES * 2];
	static struct eq_dsp d;
	struct eq_filter pk = { EQ_PK, true, 1000, -6, 1 }, pk2 = { EQ_PK, true, 1000, -3, 1 };
	struct eq_filter two[2] = { pk, { EQ_PK, true, 4000, 2, 1 } };
	double z;

	srand(1);
	for (size_t i = 0; i < FRAMES * 2; i++)
		in[i] = (int32_t)((uint32_t)rand() << 16 ^ (uint32_t)rand());
	memset(&d, 0, sizeof(d));
	eq_dsp_init(&d, 2, 48000);
	CHECK(eq_dsp_bypass(&d));
	run_s32(&d, in, out, FRAMES);
	CHECK(!memcmp(in, out, sizeof(in)));

	sine(in, FRAMES, 1000, 0.5);
	eq_dsp_set(&d, &pk, 1, 0);
	eq_dsp_reset(&d);
	CHECK(!eq_dsp_bypass(&d));
	run_s32(&d, in, out, FRAMES);
	CHECK(NEAR(peak(out, FRAMES - 4800, FRAMES), 0.5 * pow(10, -6 / 20.0), 0.005));
	CHECK(out[2 * 100] == out[2 * 100 + 1]);

	z = d.z[0][0][0];
	eq_dsp_set(&d, &pk2, 1, 0);
	CHECK(!d.pending && d.z[0][0][0] == z && d.f[0].gain == -3);
	eq_dsp_set(&d, two, 2, -2);
	CHECK(d.pending && d.xf_left == d.xf_len && d.xf_dir < 0);
	run_s32(&d, in, out, d.xf_len);
	CHECK(!d.pending && d.nf == 2 && d.xf_dir > 0 && d.xf_left == d.xf_len);
	CHECK(out[2 * (d.xf_len - 1)] == 0);
	run_s32(&d, in, out, d.xf_len);
	CHECK(!d.xf_left);

	memset(d.z, 0, sizeof(d.z));
	d.z[0][0][0] = 1e-30;
	memset(in, 0, 16);
	run_s32(&d, in, out, 1);
	CHECK(d.z[0][0][0] == 0);

	eq_dsp_set(&d, NULL, 0, 12);
	eq_dsp_reset(&d);
	sine(in, FRAMES, 100, 0.9);
	run_s32(&d, in, out, FRAMES);
	CHECK(peak(out, 0, FRAMES) == 1.0);
	for (size_t i = 0; i < FRAMES; i++)
		CHECK(!in[2 * i] || (in[2 * i] > 0) == (out[2 * i] > 0));

	eq_dsp_set(&d, NULL, 0, 0);
	eq_dsp_reset(&d);
	CHECK(eq_dsp_bypass(&d));
}

static void test_formats(void)
{
	static struct eq_dsp d;
	int16_t s16[4] = { 1000, -1000, 32767, -32768 }, o16[4];
	int32_t s24[4] = { 1000000, -1000000, 8388606, -8388608 }, o24[4];
	unsigned char s243[12] = { 0x40, 0x42, 0x0f, 0xc0, 0xbd, 0xf0, 0xfe, 0xff, 0x7f, 0x00, 0x00, 0x80 }, o243[12];
	const char *src[2];
	char *dst[2];
	int st16[2] = { 4, 4 }, st24[2] = { 8, 8 }, st243[2] = { 6, 6 };

	memset(&d, 0, sizeof(d));
	eq_dsp_init(&d, 2, 44100);
	eq_dsp_set(&d, NULL, 0, -20 * log10(2));
	eq_dsp_reset(&d);

	src[0] = (char *)s16, src[1] = (char *)(s16 + 1), dst[0] = (char *)o16, dst[1] = (char *)(o16 + 1);
	eq_dsp_run(&d, EQ_S16, src, st16, dst, st16, 2);
	CHECK(abs(o16[0] - 500) <= 1 && abs(o16[1] + 500) <= 1 && abs(o16[2] - 16384) <= 1 && abs(o16[3] + 16384) <= 1);

	src[0] = (char *)s24, src[1] = (char *)(s24 + 1), dst[0] = (char *)o24, dst[1] = (char *)(o24 + 1);
	eq_dsp_run(&d, EQ_S24, src, st24, dst, st24, 2);
	CHECK(o24[0] == 500000 && o24[1] == -500000 && o24[2] == 4194303 && o24[3] == -4194304);

	src[0] = (char *)s243, src[1] = (char *)(s243 + 3), dst[0] = (char *)o243, dst[1] = (char *)(o243 + 3);
	eq_dsp_run(&d, EQ_S24_3, src, st243, dst, st243, 2);
	CHECK(o243[0] == 0x20 && o243[1] == 0xa1 && o243[2] == 0x07);
	CHECK(o243[3] == 0xe0 && o243[4] == 0x5e && o243[5] == 0xf8);
	CHECK(o243[6] == 0xff && o243[7] == 0xff && o243[8] == 0x3f);
	CHECK(o243[9] == 0x00 && o243[10] == 0x00 && o243[11] == 0xc0);
}

static void put_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	fputs(text, f);
	fclose(f);
}

static void test_store(void)
{
	char data[] = "/tmp/disc-eq-data-XXXXXX", music[] = "/tmp/disc-eq-music-XXXXXX", path[512], name[EQ_NAME_MAX];
	struct eq_entry *v;
	struct eq_preset p;
	int n;

	CHECK(mkdtemp(data) && mkdtemp(music));
	setenv("DISC_DATA_DIR", data, 1);
	setenv("DISC_MUSIC_ROOT", music, 1);

	CHECK(eq_name_valid("Ugreen fix"));
	CHECK(!eq_name_valid("") && !eq_name_valid(".x") && !eq_name_valid("a/b") && !eq_name_valid(" a") && !eq_name_valid("a\n"));
	eq_id_name("sd:Sony WH-1000XM4/Sony WH-1000XM4 ParametricEQ.txt", name, sizeof(name));
	CHECK(!strcmp(name, "Sony WH-1000XM4"));
	eq_id_name("my:Ugreen fix", name, sizeof(name));
	CHECK(!strcmp(name, "Ugreen fix"));
	CHECK(!eq_id_path("sd:../etc/passwd.txt", path, sizeof(path)) && !eq_id_path("sd:a/../b.txt", path, sizeof(path)));
	CHECK(!eq_id_path("sd:x.wav", path, sizeof(path)) && !eq_id_path("my:a/b", path, sizeof(path)) && !eq_id_path("x", path, sizeof(path)));

	eq_preset_init(&p);
	p.band[0] = -6;
	CHECK(eq_store_save("Ugreen fix", &p) == 0);
	snprintf(path, sizeof(path), "%s/EQ", music);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/EQ/Sub", music);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/EQ/A ParametricEQ.txt", music);
	put_file(path, autoeq);
	snprintf(path, sizeof(path), "%s/EQ/Sub/B ParametricEQ.txt", music);
	put_file(path, autoeq);
	snprintf(path, sizeof(path), "%s/EQ/Sub/B GraphicEQ.txt", music);
	put_file(path, "GraphicEQ: 20 -1; 30 -2\n");
	snprintf(path, sizeof(path), "%s/EQ/.hidden.txt", music);
	put_file(path, autoeq);

	n = eq_store_list(&v);
	CHECK(n == 3);
	if (n == 3) {
		CHECK(!strcmp(v[0].id, "my:Ugreen fix") && !v[0].sd);
		CHECK(!strcmp(v[1].id, "sd:A ParametricEQ.txt") && v[1].sd && !strcmp(v[1].name, "A"));
		CHECK(!strcmp(v[2].id, "sd:Sub/B ParametricEQ.txt") && !strcmp(v[2].name, "B"));
	}
	free(v);

	CHECK(eq_store_load("my:Ugreen fix", &p) == 0 && p.band[0] == -6 && p.auto_preamp);
	CHECK(eq_store_load("sd:Sub/B ParametricEQ.txt", &p) == 0 && p.nparam == 5);
	CHECK(eq_store_load("sd:Sub/B GraphicEQ.txt", &p) == -1);
	CHECK(eq_store_load("my:none", &p) == -1);
	CHECK(eq_store_delete("Ugreen fix") == 0);
	CHECK(eq_store_load("my:Ugreen fix", &p) == -1);

	snprintf(path, sizeof(path), "rm -rf '%s' '%s'", data, music);
	CHECK(system(path) == 0);
}

int main(void)
{
	test_parse();
	test_roundtrip();
	test_response();
	test_dsp();
	test_formats();
	test_store();
	CHECK_DONE();
}
