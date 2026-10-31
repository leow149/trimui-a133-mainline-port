/*
 * Minimal I2C register peek/poke (i2c-dev, forced address so it works next to
 * a bound kernel driver). Bench tool only: writes bypass the driver's regmap.
 *
 * Usage: i2creg <bus> <addr_hex> <reg_hex> [value_hex]
 */
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	char path[32];
	unsigned char buf[2];
	int fd;

	if (argc < 4) {
		fprintf(stderr, "usage: %s <bus> <addr_hex> <reg_hex> [value_hex]\n", argv[0]);
		return 1;
	}
	snprintf(path, sizeof(path), "/dev/i2c-%s", argv[1]);
	fd = open(path, O_RDWR);
	if (fd < 0 || ioctl(fd, I2C_SLAVE_FORCE, strtoul(argv[2], NULL, 16)) < 0) {
		perror(path);
		return 1;
	}
	buf[0] = strtoul(argv[3], NULL, 16);
	if (argc > 4) {
		buf[1] = strtoul(argv[4], NULL, 16);
		if (write(fd, buf, 2) != 2) {
			perror("write");
			return 1;
		}
	}
	if (write(fd, buf, 1) != 1 || read(fd, buf + 1, 1) != 1) {
		perror("read");
		return 1;
	}
	printf("reg 0x%02x = 0x%02x\n", buf[0], buf[1]);
	return 0;
}
