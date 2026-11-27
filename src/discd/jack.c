#include "jack.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

#define GPIO_BASE 0x10010000
#define GPIO_SIZE 0x1000
#define PB_PIN_WORD (0x100 / 4)
#define JACK_35_BIT 20

static volatile uint32_t *gpio;
static int state = -1, pending = -1;

static int read_pin(void)
{
	return (gpio[PB_PIN_WORD] >> JACK_35_BIT) & 1;
}

void jack_init(void)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
	void *p;

	if (fd < 0) {
		syslog(LOG_WARNING, "jack detect off: /dev/mem: %m");
		return;
	}
	p = mmap(NULL, GPIO_SIZE, PROT_READ, MAP_SHARED, fd, GPIO_BASE);
	close(fd);
	if (p == MAP_FAILED) {
		syslog(LOG_WARNING, "jack detect off: mmap: %m");
		return;
	}
	gpio = p;
	state = read_pin();
}

bool jack_tick(void)
{
	int v;

	if (!gpio)
		return false;
	v = read_pin();
	if (v == state) {
		pending = -1;
		return false;
	}
	if (v != pending) {
		pending = v;
		return false;
	}
	state = v;
	pending = -1;
	return true;
}

int jack_state(void)
{
	return state;
}
