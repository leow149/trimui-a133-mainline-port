// Sample GPU registers 0xC00-0xFFF in a loop; print up to 6 distinct values per register.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define LO 0xc00
#define HI 0x1000
#define NV 6

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 200000;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	volatile uint32_t *r = mmap(NULL, 0x80000, PROT_READ, MAP_SHARED, fd, 0x01800000);
	if (r == MAP_FAILED) { perror("mmap"); return 1; }
	static uint32_t vals[(HI - LO) / 4][NV];
	static int n[(HI - LO) / 4];
	for (long it = 0; it < iters; it++)
		for (int i = 0; i < (HI - LO) / 4; i++) {
			uint32_t v = r[(LO / 4) + i];
			int j;
			for (j = 0; j < n[i]; j++)
				if (vals[i][j] == v)
					break;
			if (j == n[i] && n[i] < NV)
				vals[i][n[i]++] = v;
		}
	for (int i = 0; i < (HI - LO) / 4; i++) {
		if (n[i] == 1 && vals[i][0] == 0)
			continue;
		printf("0x%03x:", LO + i * 4);
		for (int j = 0; j < n[i]; j++)
			printf(" %08x", vals[i][j]);
		printf("\n");
	}
	return 0;
}
