#include <disc/eq.h>
#include <disc/paths.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECKS_PER_S 20

struct eqp {
	snd_pcm_extplug_t ext;
	char path[256];
	struct eq_dsp dsp;
	enum eq_fmt fmt;
	bool exists;
	ino_t ino;
	off_t size;
	struct timespec mtime;
	snd_pcm_uframes_t since_check;
};

static void reload(struct eqp *e, bool force)
{
	struct eq_filter f[EQ_MAX_FILTERS];
	struct eq_preset p;
	struct stat st;
	bool exists = !stat(e->path, &st);
	double pre = 0;
	int n = 0;

	if (!force && exists == e->exists &&
	    (!exists || (st.st_ino == e->ino && st.st_size == e->size && st.st_mtim.tv_sec == e->mtime.tv_sec &&
			 st.st_mtim.tv_nsec == e->mtime.tv_nsec)))
		return;
	e->exists = exists;
	if (exists) {
		e->ino = st.st_ino;
		e->size = st.st_size;
		e->mtime = st.st_mtim;
	}
	if (exists && !eq_load_file(e->path, &p)) {
		n = eq_flatten(&p, true, true, f);
		pre = eq_preamp_db(&p);
	}
	eq_dsp_set(&e->dsp, f, n, pre);
}

static snd_pcm_sframes_t eq_transfer(snd_pcm_extplug_t *ext, const snd_pcm_channel_area_t *dst_areas, snd_pcm_uframes_t dst_offset,
				     const snd_pcm_channel_area_t *src_areas, snd_pcm_uframes_t src_offset, snd_pcm_uframes_t size)
{
	struct eqp *e = ext->private_data;
	const char *src[EQ_MAX_CH];
	char *dst[EQ_MAX_CH];
	int sstep[EQ_MAX_CH], dstep[EQ_MAX_CH];

	e->since_check += size;
	if (e->since_check >= ext->rate / CHECKS_PER_S) {
		e->since_check = 0;
		reload(e, false);
	}
	for (int c = 0; c < e->dsp.nch; c++) {
		src[c] = (const char *)src_areas[c].addr + (src_areas[c].first + src_areas[c].step * src_offset) / 8;
		dst[c] = (char *)dst_areas[c].addr + (dst_areas[c].first + dst_areas[c].step * dst_offset) / 8;
		sstep[c] = src_areas[c].step / 8;
		dstep[c] = dst_areas[c].step / 8;
	}
	eq_dsp_run(&e->dsp, e->fmt, src, sstep, dst, dstep, size);
	return size;
}

static int eq_hw_params(snd_pcm_extplug_t *ext, snd_pcm_hw_params_t *params)
{
	struct eqp *e = ext->private_data;

	(void)params;
	switch (ext->format) {
	case SND_PCM_FORMAT_S16_LE:
		e->fmt = EQ_S16;
		break;
	case SND_PCM_FORMAT_S24_LE:
		e->fmt = EQ_S24;
		break;
	case SND_PCM_FORMAT_S24_3LE:
		e->fmt = EQ_S24_3;
		break;
	case SND_PCM_FORMAT_S32_LE:
		e->fmt = EQ_S32;
		break;
	default:
		return -EINVAL;
	}
	eq_dsp_init(&e->dsp, ext->channels, ext->rate);
	reload(e, true);
	return 0;
}

static int eq_init(snd_pcm_extplug_t *ext)
{
	struct eqp *e = ext->private_data;

	reload(e, false);
	eq_dsp_reset(&e->dsp);
	e->since_check = 0;
	return 0;
}

static int eq_close(snd_pcm_extplug_t *ext)
{
	free(ext->private_data);
	return 0;
}

static const snd_pcm_extplug_callback_t callbacks = {
	.transfer = eq_transfer,
	.close = eq_close,
	.hw_params = eq_hw_params,
	.init = eq_init,
};

static bool output_valid(const char *s)
{
	if (!*s || strlen(s) > 16)
		return false;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_'))
			return false;
	return true;
}

SND_PCM_PLUGIN_DEFINE_FUNC(disc_eq)
{
	static const unsigned int formats[] = { SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S24_3LE,
						SND_PCM_FORMAT_S32_LE };
	snd_config_iterator_t i, next;
	snd_config_t *slave = NULL;
	const char *output = "dac";
	char file[32];
	struct eqp *e;
	int err;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;

		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (!strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint"))
			continue;
		if (!strcmp(id, "slave")) {
			slave = n;
		} else if (!strcmp(id, "output")) {
			if (snd_config_get_string(n, &output) < 0 || !output_valid(output)) {
				SNDERR("disc_eq: output must be a short lowercase name");
				return -EINVAL;
			}
		} else {
			SNDERR("disc_eq: unknown field %s", id);
			return -EINVAL;
		}
	}
	if (!slave) {
		SNDERR("disc_eq: no slave");
		return -EINVAL;
	}
	e = calloc(1, sizeof(*e));
	if (!e)
		return -ENOMEM;
	snprintf(file, sizeof(file), "eq.%s", output);
	snprintf(e->path, sizeof(e->path), "%s", disc_run_path(file));
	eq_dsp_init(&e->dsp, 2, 48000);
	e->ext.version = SND_PCM_EXTPLUG_VERSION;
	e->ext.name = "discfw equalizer";
	e->ext.callback = &callbacks;
	e->ext.private_data = e;
	err = snd_pcm_extplug_create(&e->ext, name, root, slave, stream, mode);
	if (err < 0) {
		free(e);
		return err;
	}
	snd_pcm_extplug_set_param_list(&e->ext, SND_PCM_EXTPLUG_HW_FORMAT, sizeof(formats) / sizeof(formats[0]), formats);
	snd_pcm_extplug_set_param_minmax(&e->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 1, EQ_MAX_CH);
	snd_pcm_extplug_set_param_link(&e->ext, SND_PCM_EXTPLUG_HW_FORMAT, 1);
	snd_pcm_extplug_set_param_link(&e->ext, SND_PCM_EXTPLUG_HW_CHANNELS, 1);
	*pcmp = e->ext.pcm;
	return 0;
}
SND_PCM_PLUGIN_SYMBOL(disc_eq);
