/* Minimal mmap-based /dev/mem register reader for the stock TrimUI kernel,
 * where /dev/mem's read() path returns EFAULT for MMIO (peripheral) addresses
 * and only mmap() access works. Usage: mmapread <hex_addr> [count]
 * Prints "0x<addr>=0x<value>" per 32-bit word, page-aligned mmap under the hood. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <hex_addr> [count]\n", argv[0]);
		return 1;
	}
	uint64_t addr = strtoull(argv[1], NULL, 16);
	int count = argc > 2 ? atoi(argv[2]) : 1;
	long pagesize = sysconf(_SC_PAGESIZE);
	uint64_t page_base = addr & ~(pagesize - 1);
	uint64_t page_off = addr - page_base;

	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); return 1; }

	size_t maplen = ((page_off + count * 4 + pagesize - 1) / pagesize) * pagesize;
	void *map = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, page_base);
	if (map == MAP_FAILED) { perror("mmap"); return 1; }

	volatile uint32_t *p = (volatile uint32_t *)((char *)map + page_off);
	for (int i = 0; i < count; i++)
		printf("0x%08llx=0x%08x\n", (unsigned long long)(addr + i * 4), p[i]);

	munmap(map, maplen);
	close(fd);
	return 0;
}
