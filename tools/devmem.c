/* Minimal devmem-style raw physical memory peek/poke tool, since busybox's
 * devmem applet isn't built into this rootfs. Statically linked, no deps.
 *
 * Usage: devmem <phys_addr_hex> [value_hex_to_write]
 * Always prints the value read (before any write).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <phys_addr_hex> [value_hex_to_write]\n", argv[0]);
		return 1;
	}

	uint64_t addr = strtoull(argv[1], NULL, 16);
	uint64_t page_base = addr & ~(uint64_t)(4095);
	uint64_t page_off = addr - page_base;

	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("open /dev/mem"); return 1; }

	void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page_base);
	if (map == MAP_FAILED) { perror("mmap"); return 1; }

	volatile uint32_t *reg = (volatile uint32_t *)((char *)map + page_off);

	uint32_t before = *reg;
	printf("addr=0x%llx value=0x%08x\n", (unsigned long long)addr, before);

	if (argc >= 3) {
		uint32_t val = strtoul(argv[2], NULL, 16);
		*reg = val;
		uint32_t after = *reg;
		printf("wrote 0x%08x, readback=0x%08x\n", val, after);
	}

	munmap(map, 4096);
	close(fd);
	return 0;
}
