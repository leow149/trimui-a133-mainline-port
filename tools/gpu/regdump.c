// Read 32-bit GPU registers listed as "offset name" lines on stdin, print "name value".
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long base = argc > 1 ? strtoul(argv[1], NULL, 0) : 0x01800000;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	volatile uint32_t *r = mmap(NULL, 0x80000, PROT_READ, MAP_SHARED, fd, base);
	if (r == MAP_FAILED) { perror("mmap"); return 1; }
	char name[128];
	unsigned long off;
	while (scanf("%lx %127s", &off, name) == 2)
		printf("%-48s 0x%05lx = 0x%08x\n", name, off, r[off / 4]);
	return 0;
}
