#include "proto.h"

#include <alsa/asoundlib.h>
#include <stdio.h>

static unsigned slave_rate(snd_pcm_t *pcm)
{
	snd_pcm_info_t *info;
	char path[96], line[64];
	unsigned rate = 0;
	FILE *f;

	snd_pcm_info_alloca(&info);
	if (snd_pcm_info(pcm, info) < 0)
		return 0;
	snprintf(path, sizeof(path), "/proc/asound/card%d/pcm%dp/sub%u/hw_params", snd_pcm_info_get_card(info), snd_pcm_info_get_device(info), snd_pcm_info_get_subdevice(info));
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "rate: %u", &rate) == 1)
			break;
	fclose(f);
	return rate;
}

static int on_hw_params(snd_pcm_hook_t *hook)
{
	unsigned rate = slave_rate(snd_pcm_hook_get_pcm(hook));
	char cmd[64], reply[128];

	if (!rate) {
		SNDERR("disc hook: cannot read slave rate");
		return 0;
	}
	snprintf(cmd, sizeof(cmd), "rate %u\nunmute\n", rate);
	if (discd_request(cmd, reply, sizeof(reply), 2000) < 0)
		SNDERR("disc hook: discd unreachable");
	return 0;
}

static int on_release(snd_pcm_hook_t *hook)
{
	(void)hook;
	discd_request("release\n", NULL, 0, 2000);
	return 0;
}

int disc_hook_install(snd_pcm_t *pcm, snd_config_t *conf)
{
	snd_pcm_hook_t *h;
	int err;

	(void)conf;
	err = snd_pcm_hook_add(&h, pcm, SND_PCM_HOOK_TYPE_HW_PARAMS, on_hw_params, NULL);
	if (err >= 0)
		err = snd_pcm_hook_add(&h, pcm, SND_PCM_HOOK_TYPE_HW_FREE, on_release, NULL);
	if (err >= 0)
		err = snd_pcm_hook_add(&h, pcm, SND_PCM_HOOK_TYPE_CLOSE, on_release, NULL);
	return err < 0 ? err : 0;
}
SND_DLSYM_BUILD_VERSION(disc_hook_install, SND_PCM_DLSYM_VERSION);
