#include "discsys.h"

#include <disc/settings.h>
#include <mpd/client.h>
#include <stdio.h>
#include <string.h>

#define DAC_DEFAULT 50
#define BT_DEFAULT 50

static void state_key(char *key, size_t len)
{
	if (sys.output == OUT_BT)
		snprintf(key, len, "vol.bt.%s", sys.bt_dev[0] ? sys.bt_dev : "any");
	else
		snprintf(key, len, "vol.dac");
}

static int stored(void)
{
	char key[48];

	state_key(key, sizeof(key));
	return state_get_int(key, sys.output == OUT_BT ? BT_DEFAULT : DAC_DEFAULT);
}

static int clamp_pct(int v)
{
	return v < 0 ? 0 : v > 100 ? 100 : v;
}

static void apply_hw(void)
{
	if (sys.output == OUT_DAC)
		hw_volpct(sys.volume);
}

static void announce(void)
{
	struct kvbuf b;

	kv_init(&b);
	kv_raw(&b, "!vol");
	kv_int(&b, "pct", sys.volume);
	kv_str(&b, "out", sys.output == OUT_BT ? "bt" : "dac");
	ev_kv(&b);
}

static void push_to_mpd(void)
{
	struct mpd_connection *c = mpdc();

	if (c && !mpd_run_set_volume(c, sys.volume))
		mpdc_report("setvol");
}

void volume_reapply(void)
{
	apply_hw();
}

void volume_on_mpd_connect(void)
{
	sys.volume = stored();
	push_to_mpd();
	apply_hw();
	announce();
}

void volume_on_mixer(int pct)
{
	char key[48];

	sys.volume = clamp_pct(pct);
	state_key(key, sizeof(key));
	state_set_int(key, sys.volume);
	apply_hw();
	announce();
}

void volume_set(int pct)
{
	sys.volume = clamp_pct(pct);
	push_to_mpd();
	volume_on_mixer(sys.volume);
}

void volume_step(int delta)
{
	volume_set(sys.volume + delta);
}

void volume_output_changed(void)
{
	struct mpd_connection *c = mpdc();
	struct mpd_output *o;
	enum output out = OUT_DAC;
	int enabled = 0;

	if (!c || !mpd_send_outputs(c))
		return;
	while ((o = mpd_recv_output(c))) {
		if (mpd_output_get_enabled(o)) {
			enabled++;
			if (!strcmp(mpd_output_get_name(o), "BT"))
				out = OUT_BT;
		}
		mpd_output_free(o);
	}
	mpd_response_finish(c);
	sys.outputs_enabled = enabled;
	if (out == sys.output || enabled != 1)
		return;
	sys.output = out;
	sys.volume = stored();
	push_to_mpd();
	apply_hw();
	announce();
}
