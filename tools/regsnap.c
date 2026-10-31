/*
 * Dump AXP717 registers (i2c-dev, forced address) and the PIO/R_PIO register
 * space, for diffing stock against mainline. Built with the kernel's nolibc so
 * it stays small enough to push over the UART console.
 *
 * Usage: regsnap <i2c bus number>
 */
#include <linux/i2c-dev.h>

static void dump_mem(unsigned long base, unsigned long len)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	volatile unsigned int *p;
	unsigned long i;

	if (fd < 0) {
		printf("open /dev/mem failed\n");
		return;
	}
	p = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, base & ~0xfffUL);
	if (p == MAP_FAILED) {
		printf("mmap %lx failed\n", base);
		close(fd);
		return;
	}
	for (i = 0; i < len; i += 4)
		printf("addr=0x%lx value=0x%08x\n", base + i,
		       p[((base & 0xfff) + i) / 4]);
	munmap((void *)p, 0x1000);
	close(fd);
}

int main(int argc, char **argv)
{
	char path[32] = "/dev/i2c-";
	int n = strlen(path);
	unsigned char buf[2];
	int fd, r;

	if (argc < 2)
		return 1;
	for (r = 0; argv[1][r] && n < 31; r++)
		path[n++] = argv[1][r];
	path[n] = 0;
	printf("=== AXP717\n");
	fd = open(path, O_RDWR);
	if (fd < 0 || ioctl(fd, I2C_SLAVE_FORCE, 0x34) < 0) {
		printf("i2c open failed\n");
	} else {
		for (r = 0; r < 256; r++) {
			buf[0] = r;
			if (write(fd, buf, 1) != 1 || read(fd, buf + 1, 1) != 1)
				printf("reg 0x%02x = ERR\n", r);
			else
				printf("reg 0x%02x = 0x%02x\n", r, buf[1]);
		}
		close(fd);
	}
	printf("=== PIO\n");
	dump_mem(0x0300b000, 0x360);
	printf("=== RPIO\n");
	dump_mem(0x07022000, 0x60);
	printf("=== END\n");
	return 0;
}
