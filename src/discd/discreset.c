#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Stock soc_watchdog.ko: _IO('W', 0x10) runs soc_reset(), an immediate watchdog reset. The kernel's own
 * restart path halts on this board, so reboot goes through here. */
#define WDT_RESET 0x20005710

int main(void)
{
	int fd = open("/dev/jz_watchdog", O_RDWR);

	if (fd < 0) {
		system("insmod /lib/modules/4.4.94/fiio/soc_watchdog.ko 2>/dev/null");
		for (int i = 0; i < 20 && fd < 0; i++) {
			usleep(50000);
			fd = open("/dev/jz_watchdog", O_RDWR);
		}
	}
	if (fd < 0) {
		perror("discreset: /dev/jz_watchdog");
		return 1;
	}
	sync();
	ioctl(fd, WDT_RESET);
	sleep(5);
	fprintf(stderr, "discreset: still running\n");
	return 1;
}
