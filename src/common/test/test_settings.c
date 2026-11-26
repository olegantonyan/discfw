#include "check.h"
#include <disc/settings.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
	char path[] = "/tmp/disc-settings-test-XXXXXX", buf[512];
	int fd = mkstemp(path);
	FILE *f = fdopen(fd, "w");

	for (int i = 0; i < set_ndefs; i++) {
		const struct set_def *d = &set_defs[i];
		CHECK(set_normalize(d, d->def, buf, sizeof(buf)) == 0);
		for (int j = i + 1; j < set_ndefs; j++)
			CHECK(strcmp(d->key, set_defs[j].key));
	}

	fprintf(f, "battery.limit=80\nui.font=huge\nfuture.key=x y\n# comment\nui.index=off\n");
	fclose(f);
	CHECK(settings_load(path) == 1);
	CHECK(set_get_int("battery.limit") == 80);
	CHECK(!strcmp(set_get("ui.font"), "medium"));
	CHECK(!set_get_bool("ui.index"));
	CHECK(set_get_enum("audio.filter") == 0);
	CHECK(set_get("nope") == NULL);

	CHECK(set_put("battery.limit", "45") == -2);
	CHECK(set_put("battery.limit", "85") == 0);
	CHECK(set_put("nope", "1") == -1);
	CHECK(set_put("ui.cover_mode", "vinyl") == 0);
	CHECK(set_put("ui.index", "1") == 0);
	CHECK(set_is_default("ui.index"));
	CHECK(set_put("net.mpd_password", "a b=c") == 0);
	CHECK(set_put("net.mpd_password", "x\ny") == -2);
	CHECK(settings_save(path) == 0);

	CHECK(settings_load(path) == 0);
	CHECK(set_get_int("battery.limit") == 85);
	CHECK(!strcmp(set_get("ui.cover_mode"), "vinyl"));
	CHECK(!strcmp(set_get("net.mpd_password"), "a b=c"));
	f = fopen(path, "r");
	char all[512] = "";
	fread(all, 1, sizeof(all) - 1, f);
	fclose(f);
	CHECK(strstr(all, "future.key=x y\n"));
	CHECK(!strstr(all, "ui.index"));

	const struct set_def *d = set_lookup("audio.filter");
	CHECK(set_choice_count(d) == 6);
	CHECK(set_choice(d, 4, buf, sizeof(buf)) && !strcmp(buf, "nos"));
	CHECK(set_choice_label(d, 4, buf, sizeof(buf)) && !strcmp(buf, "Non-oversampling"));
	CHECK(set_choice_index(d, "wideband") == 5);
	unlink(path);
	CHECK_DONE();
}
