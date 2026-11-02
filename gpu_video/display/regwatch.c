// Count how often 32-bit registers change: regwatch <seconds> <addr>...
// Polls every 200 us; all addresses must be in the same 64 KB window.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int secs = atoi(argv[1]), n = argc - 2, i;
	unsigned long a0 = strtoul(argv[2], NULL, 0) & ~0xffffUL;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	volatile uint32_t *m = mmap(NULL, 0x10000, PROT_READ, MAP_SHARED, fd, a0);
	uint32_t last[8], changes[8] = { 0 };
	unsigned off[8];

	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	for (i = 0; i < n; i++) {
		off[i] = (strtoul(argv[i + 2], NULL, 0) - a0) / 4;
		last[i] = m[off[i]];
	}
	time_t end = time(NULL) + secs;
	while (time(NULL) < end) {
		usleep(200);
		for (i = 0; i < n; i++) {
			uint32_t v = m[off[i]];
			if (v != last[i]) { changes[i]++; last[i] = v; }
		}
	}
	for (i = 0; i < n; i++)
		printf("%s: %u changes in %d s (last 0x%08x)\n", argv[i + 2], changes[i], secs, last[i]);
	return 0;
}
