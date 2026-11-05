// Count TCON0 LCD_TRI_UNDERFLOW events (GINT0 bit 9, write-0-to-clear) for a
// number of seconds, polling every millisecond. Optionally write one TCON
// register first: uflow <seconds> [offset value]
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 10;
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint32_t *t = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x06511000);
	if (t == MAP_FAILED) { perror("mmap"); return 1; }
	if (argc > 3) {
		unsigned off = strtoul(argv[2], NULL, 0);
		uint32_t val = strtoul(argv[3], NULL, 0);
		printf("write +0x%03x: 0x%08x -> 0x%08x\n", off, t[off / 4], val);
		t[off / 4] = val;
	}
	unsigned events = 0, polls = 0;
	time_t end = time(NULL) + secs;
	t[1] = t[1] & ~0x200u;
	while (time(NULL) < end) {
		usleep(1000);
		uint32_t v = t[1];
		polls++;
		if (v & 0x200) {
			events++;
			t[1] = v & ~0x200u;
		}
	}
	printf("underflow events: %u in %d s (%u polls)\n", events, secs, polls);
	return 0;
}
