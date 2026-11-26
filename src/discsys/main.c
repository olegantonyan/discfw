#include "discsys.h"

#include <glib-unix.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define PING_S 5

struct sys sys;

gint64 now_ms(void)
{
	return g_get_monotonic_time() / 1000;
}

void logmsg(int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(prio, fmt, ap);
	va_end(ap);
}

static gboolean on_term(gpointer data)
{
	(void)data;
	cfg_flush();
	g_main_loop_quit(sys.loop);
	return G_SOURCE_REMOVE;
}

static gboolean on_ping(gpointer data)
{
	(void)data;
	server_ping_ui();
	return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
	const char *fake = getenv("DISC_FAKE_HW");

	sys.fake_hw = (fake && *fake && strcmp(fake, "0")) || (argc > 1 && !strcmp(argv[1], "--fake-hw"));
	openlog("discsys", LOG_PERROR, LOG_DAEMON);
	signal(SIGPIPE, SIG_IGN);
	sys.loop = g_main_loop_new(NULL, FALSE);
	state_init();
	cfg_init();
	server_init();
	hw_init();
	screen_init();
	keys_init();
	power_init();
	library_init();
	mpdc_init();
	g_unix_signal_add(SIGTERM, on_term, NULL);
	g_unix_signal_add(SIGINT, on_term, NULL);
	g_timeout_add_seconds(PING_S, on_ping, NULL);
	logmsg(LOG_INFO, "ready%s", sys.fake_hw ? " (fake hardware)" : "");
	g_main_loop_run(sys.loop);
	cfg_flush();
	return 0;
}
