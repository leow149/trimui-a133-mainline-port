// SPDX-License-Identifier: GPL-2.0
/*
 * xr829_bt_up - XR829 Bluetooth firmware uploader and H4 attach (userspace).
 *
 * Protocol: docs/bluetooth-xr829-protocol.md (from the vendor rtk_hciattach
 * "xradio" type).
 *
 *   xr829_bt_up [-d /dev/ttyS1] [-f fw.bin] [-a aa:bb:cc:dd:ee:ff] [-p] [-q]
 *               [-g chiplabel:line | -g none] [-n] [-k rfkill-name] [-F 1]
 *               [-i 115200|921600|1500000] [-B 115200|1500000] [-w 0|1] [-r ms]
 *
 *   -p  probe only: reset the controller and check that the boot ROM answers
 *   -q  quiet: no packet dumps
 *   -g  BT_WAKE GPIO as "gpiochip label:line", default 7022000.pinctrl:4 (PL4)
 *   -n  stay in the foreground once attached, for a Type=exec service
 *   -k  name of the rfkill switch that owns BT_RST_N, default rfkill-xr829-bt
 *   -F  1 enables RTS/CTS flow control. Off by default: CTS (PG9) floats on
 *       the TrimUI Smart Pro and would stall TX
 *   -i  baud rate of the boot ROM after a reset, default 115200
 *   -B  baud rate of the firmware's HCI after the upload, default 115200
 *
 * The tty stays open for the whole sequence and is then attached as an HCI H4
 * line discipline; the process stays resident, since the hci0 device lives
 * as long as this fd is open. The bring-up is bounded by a 40 s alarm, so a
 * wedged controller cannot hang the caller. BT_WAKE is requested through the
 * GPIO character device and held high for as long as the resident process
 * lives, so the controller never enters its low-power mode.
 *
 * Exit codes:
 *   0  attached (the parent exits after forking), or the probe succeeded
 *   1  the bring-up failed
 *   2  usage error
 *   3  the rfkill switch got blocked: not a failure, the caller should wait
 *      for the unblock and start over
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/gpio.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Exit codes, see above */
#define EXIT_FAIL		1
#define EXIT_USAGE		2
#define EXIT_BLOCKED		3

/* HCI UART line discipline */
#define N_HCI			15
#define HCIUARTSETPROTO		_IOW('U', 200, int)
#define HCIUARTGETDEVICE	_IOR('U', 202, int)
#define HCI_UART_H4		0

/* BROM packets: command IDs */
#define BROM_CMD_WRITE_CHUNK	0x09	/* write one chunk of the image to RAM */
#define BROM_CMD_SET_UART	0x10	/* switch the UART to a new baud rate */
#define BROM_CMD_SET_PC		0x13	/* jump to the uploaded image */

/* Mode bits in the SET_UART argument, as sent by the vendor tool */
#define BROM_UART_MODE		0x03000000
#define BROM_FAST_BAUD		1500000
#define BROM_CHUNK_SIZE		1024
#define BROM_MAX_IMAGE		(4 * 1024 * 1024)

/* HCI: H4 packet type, opcodes and the vendor commands of the firmware */
#define HCI_COMMAND_PKT		0x01
#define HCI_EVENT_PKT		0x04
#define HCI_EV_CMD_COMPLETE	0x0e
#define HCI_OP_RESET		0x0c03
#define HCI_OP_XR_SET_BAUD	0xfc18	/* vendor: UART baud rate */
#define HCI_OP_XR_SET_BDADDR	0xfc0a	/* vendor: Bluetooth device address */
#define HCI_CMD_MAX_PARAMS	16

#define ALARM_SECONDS		40

static int fd = -1;
static int verbose = 1;

static void msleep(int ms)
{
	usleep(ms * 1000);
}

static void hexdump(const char *tag, const uint8_t *b, int n)
{
	if (!verbose)
		return;
	fprintf(stderr, "%s [%d]:", tag, n);
	for (int i = 0; i < n && i < 32; i++)
		fprintf(stderr, " %02x", b[i]);
	fprintf(stderr, "\n");
}

static int write_file(const char *path, const char *s)
{
	int f = open(path, O_WRONLY);
	int r;

	if (f < 0) {
		perror(path);
		return -1;
	}
	r = write(f, s, strlen(s));
	close(f);
	return r < 0 ? -1 : 0;
}

/* Parse a non-negative decimal number; -1 on any error. */
static int parse_int(const char *s, int *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < 0 || v > INT_MAX)
		return -1;
	*out = v;
	return 0;
}

/* ---- rfkill: the switch of the XR829 Bluetooth (rfkill-gpio on BT_RST_N) ---- */

static const char *rfkill_name = "rfkill-xr829-bt";
static char rfkill_dir[300];

static int find_rfkill(void)
{
	DIR *d = opendir("/sys/class/rfkill");
	struct dirent *e;
	size_t len = strlen(rfkill_name);

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char p[300], name[64] = "";
		FILE *f;

		snprintf(p, sizeof(p), "/sys/class/rfkill/%s/name", e->d_name);
		f = fopen(p, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) && !strncmp(name, rfkill_name, len) &&
		    (name[len] == '\n' || !name[len])) {
			snprintf(rfkill_dir, sizeof(rfkill_dir), "/sys/class/rfkill/%s",
				 e->d_name);
			fclose(f);
			closedir(d);
			return 0;
		}
		fclose(f);
	}
	closedir(d);
	return -1;
}

/* Value of a 0/1 rfkill attribute ("soft", "hard"), -1 on error. */
static int rfkill_attr(const char *attr)
{
	char p[320], v[8] = "";
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", rfkill_dir, attr);
	f = fopen(p, "r");
	if (!f)
		return -1;
	if (!fgets(v, sizeof(v), f))
		v[0] = 0;
	fclose(f);
	if (v[0] == '1')
		return 1;
	if (v[0] == '0')
		return 0;
	return -1;
}

/* 1 if blocked, 0 if not, -1 if the rfkill switch is gone */
static int rfkill_blocked(void)
{
	int soft = rfkill_attr("soft");
	int hard = rfkill_attr("hard");

	if (soft < 0 || hard < 0)
		return -1;
	return soft || hard;
}

/*
 * Request "label:line" as an output driven high; returns the line fd (keep it
 * open to keep the line high) or -1.
 */
static int bt_wake_request(const char *spec)
{
	char label[GPIO_MAX_NAME_SIZE];
	const char *colon = strrchr(spec, ':');
	unsigned int line;

	if (!colon || colon == spec || (size_t)(colon - spec) >= sizeof(label) ||
	    sscanf(colon + 1, "%u", &line) != 1) {
		fprintf(stderr, "bad -g %s (want chiplabel:line)\n", spec);
		return -1;
	}
	memcpy(label, spec, colon - spec);
	label[colon - spec] = 0;

	for (int n = 0; n < 16; n++) {
		char path[32];
		struct gpiochip_info info;
		struct gpio_v2_line_request req;
		int c, r;

		snprintf(path, sizeof(path), "/dev/gpiochip%d", n);
		c = open(path, O_RDWR | O_CLOEXEC);
		if (c < 0)
			continue;
		if (ioctl(c, GPIO_GET_CHIPINFO_IOCTL, &info) || strcmp(info.label, label)) {
			close(c);
			continue;
		}
		memset(&req, 0, sizeof(req));
		req.offsets[0] = line;
		req.num_lines = 1;
		req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
		req.config.num_attrs = 1;
		req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
		req.config.attrs[0].attr.values = 1;
		req.config.attrs[0].mask = 1;
		snprintf(req.consumer, sizeof(req.consumer), "xr829-bt-wake");
		r = ioctl(c, GPIO_V2_GET_LINE_IOCTL, &req);
		close(c);
		if (r) {
			perror("BT_WAKE line request");
			return -1;
		}
		return req.fd;
	}
	fprintf(stderr, "no gpiochip labelled %s\n", label);
	return -1;
}

/*
 * Reset pulse on BT_RST_N through the rfkill switch, which owns that GPIO.
 * The pulse is skipped if someone else blocked the switch, and a signal
 * during the low phase unblocks it again before exiting. A block issued by
 * someone else during the low phase itself is still undone.
 */
static char rfkill_state_path[320];
static volatile sig_atomic_t in_pulse;

static void pulse_signal(int sig)
{
	if (in_pulse)
		write_file(rfkill_state_path, "1");
	_exit(128 + sig);
}

static int power_cycle(int low_ms)
{
	if (rfkill_blocked())
		return -1;

	snprintf(rfkill_state_path, sizeof(rfkill_state_path), "%s/state", rfkill_dir);
	signal(SIGTERM, pulse_signal);
	signal(SIGINT, pulse_signal);
	signal(SIGALRM, pulse_signal);
	in_pulse = 1;
	write_file(rfkill_state_path, "0");
	msleep(low_ms);
	write_file(rfkill_state_path, "1");
	in_pulse = 0;
	return 0;
}

/* ---- tty I/O ---- */

static int set_baud(speed_t sp, int crtscts)
{
	struct termios t;

	if (tcgetattr(fd, &t))
		return -1;
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
	if (crtscts)
		t.c_cflag |= CRTSCTS;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	cfsetispeed(&t, sp);
	cfsetospeed(&t, sp);
	if (tcsetattr(fd, TCSANOW, &t))
		return -1;
	tcflush(fd, TCIOFLUSH);
	return 0;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* Read up to n bytes, waiting at most to_ms in total; returns the bytes read. */
static int rd(uint8_t *b, int n, int to_ms)
{
	long end = now_ms() + to_ms;
	int got = 0;

	while (got < n) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		long left = end - now_ms();
		int r;

		if (left < 0)
			left = 0;
		r = poll(&p, 1, left);
		if (r <= 0)
			break;
		r = read(fd, b + got, n - got);
		if (r > 0)
			got += r;
		else if (r == 0 || errno != EAGAIN)
			break;
	}
	return got;
}

/* termios speed for the supported rates, B0 for anything else */
static speed_t baud_speed(int rate)
{
	switch (rate) {
	case 115200:
		return B115200;
	case 921600:
		return B921600;
	case 1500000:
		return B1500000;
	default:
		return B0;
	}
}

static int wr(const void *b, int n)
{
	const uint8_t *p = b;
	int left = n;

	while (left > 0) {
		int r = write(fd, p, left);

		if (r < 0) {
			if (errno == EAGAIN) {
				msleep(1);
				continue;
			}
			return -1;
		}
		p += r;
		left -= r;
	}
	tcdrain(fd);
	return n;
}

/* ---- BROM packets ---- */

static uint16_t csum_native(const uint8_t *b, int n)
{
	uint32_t s = 0;

	for (int i = 0; i + 1 < n; i += 2)
		s += b[i] | (b[i + 1] << 8);
	if (n & 1)
		s += b[n - 1];
	return ~s & 0xffff;
}

/*
 * Build a BROM command packet from nargs arguments of nb[i] bytes each. The
 * payload has them big endian, the checksum is taken over a header with the
 * arguments in native (little endian) order.
 */
static int brom_mkpkt(uint8_t *out, uint8_t cmd, const uint32_t *v, const int *nb, int na)
{
	uint8_t pay[32], nat[64];
	uint8_t h[64] = "BROM";
	int len = 0;
	int nlen = 0;
	uint16_t c;

	pay[len++] = cmd;
	nat[nlen++] = cmd;
	for (int i = 0; i < na; i++) {
		for (int k = 0; k < nb[i]; k++)
			pay[len++] = v[i] >> (8 * (nb[i] - 1 - k));
		for (int k = 0; k < nb[i]; k++)
			nat[nlen++] = v[i] >> (8 * k);
	}

	h[4] = 4;
	h[5] = 0;
	h[6] = 0;
	h[7] = 0;
	h[8] = len;
	h[9] = len >> 8;
	h[10] = 0;
	h[11] = 0;
	memcpy(h + 12, nat, nlen);
	c = csum_native(h, 12 + nlen);

	memcpy(out, "BROM", 4);
	out[4] = 4;
	out[5] = 0;
	out[6] = c >> 8;
	out[7] = c;
	out[8] = 0;
	out[9] = 0;
	out[10] = len >> 8;
	out[11] = len;
	memcpy(out + 12, pay, len);
	return 12 + len;
}

/* Build and send one BROM command, dumping it if what is set. */
static int brom_send(uint8_t cmd, const uint32_t *v, const int *nb, int na, const char *what)
{
	uint8_t p[64];
	int n = brom_mkpkt(p, cmd, v, nb, na);

	if (what)
		hexdump(what, p, n);
	return wr(p, n);
}

static int brom_ack(const char *what)
{
	uint8_t r[16];
	int n = rd(r, 12, 1000);

	hexdump(what, r, n);
	if (n != 12 || memcmp(r, "BROM", 4))
		return -1;
	if (r[4] & 1) {
		uint8_t e;

		rd(&e, 1, 200);
		fprintf(stderr, "%s: error byte %02x\n", what, e);
		return -1;
	}
	if (!(r[4] & 2))
		return -1;
	return 0;
}

/* tries > 0: give up after that many 0x55 bytes (each ~20 ms, plus power cycles) */
static int brom_sync(int tries, int cycle_after)
{
	uint8_t b[2];

	for (int i = 0; i < tries; i++) {
		uint8_t c = 0x55;
		int n;

		wr(&c, 1);
		msleep(20);
		n = rd(b, 2, 0);
		if (n > 0) {
			if (verbose)
				fprintf(stderr, "sync rx %d byte(s): %02x %02x (try %d)\n",
					n, b[0], n > 1 ? b[1] : 0, i);
			if (n == 1)
				n += rd(b + 1, 1, 50);
			if (n == 2 && ((b[0] == 'O' && b[1] == 'K') ||
				       (b[0] == 'K' && b[1] == 'O')))
				return 0;
		}
		if (cycle_after && (i + 1) % cycle_after == 0) {
			fprintf(stderr, "sync: no answer after %d tries, power cycle\n", i + 1);
			if (power_cycle(500))
				return -1;
			msleep(20);
			tcflush(fd, TCIOFLUSH);
		}
	}
	return -1;
}

/* Read the firmware image into a malloc'ed buffer; the file is always closed. */
static uint8_t *load_firmware(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *img;
	long sz = -1;

	if (!f) {
		perror(path);
		return NULL;
	}
	if (!fseek(f, 0, SEEK_END))
		sz = ftell(f);
	if (sz <= 0 || sz > BROM_MAX_IMAGE) {
		fprintf(stderr, "%s: bad size %ld\n", path, sz);
		fclose(f);
		return NULL;
	}
	rewind(f);
	img = malloc(sz);
	if (!img) {
		perror("malloc");
		fclose(f);
		return NULL;
	}
	if (fread(img, 1, sz, f) != (size_t)sz) {
		perror(path);
		free(img);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size = sz;
	return img;
}

/* Send the image in chunks, each with a header packet and the data, both acked. */
static int brom_upload(const char *fw)
{
	uint32_t v[3];
	int nb[3] = { 4, 4, 2 };
	uint8_t *img;
	long sz;

	img = load_firmware(fw, &sz);
	if (!img)
		return -1;

	fprintf(stderr, "== upload %s (%ld bytes)\n", fw, sz);
	for (long off = 0; off < sz; off += BROM_CHUNK_SIZE) {
		int len = sz - off > BROM_CHUNK_SIZE ? BROM_CHUNK_SIZE : sz - off;

		v[0] = off;
		v[1] = len;
		v[2] = csum_native(img + off, len);
		brom_send(BROM_CMD_WRITE_CHUNK, v, nb, 3, NULL);
		if (brom_ack(off ? "chunk hdr ack" : "chunk0 hdr ack")) {
			fprintf(stderr, "fail at %ld\n", off);
			free(img);
			return -1;
		}
		wr(img + off, len);
		if (brom_ack(off ? "chunk data ack" : "chunk0 data ack")) {
			fprintf(stderr, "fail data at %ld\n", off);
			free(img);
			return -1;
		}
		if (off > 2048)
			verbose = 0;	/* quiet after the first chunks */
	}
	verbose = 1;
	free(img);
	fprintf(stderr, "upload done\n");
	return 0;
}

/* Switch the boot ROM to the fast baud rate and sync again there. */
static int brom_set_fast_uart(void)
{
	uint32_t v[1] = { BROM_UART_MODE | BROM_FAST_BAUD };
	int nb[1] = { 4 };

	brom_send(BROM_CMD_SET_UART, v, nb, 1, "SET_UART");
	if (brom_ack("SET_UART ack"))
		return -1;
	msleep(5);
	if (set_baud(B1500000, 0)) {
		perror("termios 1.5M");
		return -1;
	}
	fprintf(stderr, "== BROM sync @1.5M\n");
	if (brom_sync(200, 0)) {
		fprintf(stderr, "sync @1.5M failed\n");
		return -1;
	}
	return 0;
}

/* Start the uploaded firmware. */
static int brom_set_pc(void)
{
	uint32_t v[1] = { 0 };
	int nb[1] = { 4 };

	brom_send(BROM_CMD_SET_PC, v, nb, 1, NULL);
	return brom_ack("SET_PC ack");
}

/* ---- HCI ---- */

/* Send an HCI command and wait for its Command Complete with status 0. */
static int hci_cmd(uint16_t opcode, const uint8_t *params, int plen, const char *what)
{
	uint8_t cmd[4 + HCI_CMD_MAX_PARAMS];
	uint8_t r[64];
	int got;

	cmd[0] = HCI_COMMAND_PKT;
	cmd[1] = opcode & 0xff;
	cmd[2] = opcode >> 8;
	cmd[3] = plen;
	if (plen)
		memcpy(cmd + 4, params, plen);
	wr(cmd, 4 + plen);

	got = rd(r, 7, 1000);
	if (got >= 7) {
		/* Command Complete: 04 0e len ncmd opcode(2) status */
		int len = r[2];

		if (len > 4)
			rd(r + 7, len - 4 > 50 ? 50 : len - 4, 200);
	}
	hexdump(what, r, got);
	return (got >= 7 && r[0] == HCI_EVENT_PKT && r[1] == HCI_EV_CMD_COMPLETE &&
		r[4] == cmd[1] && r[5] == cmd[2] && r[6] == 0) ? 0 : -1;
}

struct opts {
	const char *dev;
	const char *fw;
	const char *wake_gpio;
	uint8_t bdaddr[6];
	int have_bdaddr;
	int probe;
	int nofork;
	int flow;
	int post_baud;
	int wake;
	int reset_low_ms;
	int rom_baud;
};

static int parse_bdaddr(const char *s, uint8_t *bd)
{
	unsigned int a[6];

	if (sscanf(s, "%x:%x:%x:%x:%x:%x", a, a + 1, a + 2, a + 3, a + 4, a + 5) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		bd[5 - i] = a[i];	/* little endian on the wire */
	return 0;
}

/* Returns 0, or -1 on a usage error. */
static int parse_args(int argc, char **argv, struct opts *o)
{
	int c;

	while ((c = getopt(argc, argv, "d:f:a:pqw:r:i:F:B:g:nk:")) != -1) {
		switch (c) {
		case 'd':
			o->dev = optarg;
			break;
		case 'f':
			o->fw = optarg;
			break;
		case 'p':
			o->probe = 1;
			break;
		case 'q':
			verbose = 0;
			break;
		case 'F':
			if (parse_int(optarg, &o->flow))
				goto bad_value;
			break;
		case 'B':
			if (parse_int(optarg, &o->post_baud) ||
			    (o->post_baud != 115200 && o->post_baud != 1500000))
				goto bad_value;
			break;
		case 'w':
			if (parse_int(optarg, &o->wake))
				goto bad_value;
			break;
		case 'r':
			if (parse_int(optarg, &o->reset_low_ms))
				goto bad_value;
			break;
		case 'i':
			if (parse_int(optarg, &o->rom_baud) || baud_speed(o->rom_baud) == B0)
				goto bad_value;
			break;
		case 'k':
			rfkill_name = optarg;
			break;
		case 'g':
			o->wake_gpio = optarg;
			break;
		case 'n':
			o->nofork = 1;
			break;
		case 'a':
			if (parse_bdaddr(optarg, o->bdaddr))
				goto bad_value;
			o->have_bdaddr = 1;
			break;
		default:
			return -1;
		}
	}
	return 0;

bad_value:
	fprintf(stderr, "bad value for -%c: %s\n", c, optarg);
	return -1;
}

/* Respect a block by the user or systemd-rfkill: wait for the unblock. */
static int wait_for_unblock(void)
{
	if (rfkill_blocked() > 0) {
		fprintf(stderr, "%s is blocked, waiting\n", rfkill_name);
		while (rfkill_blocked() > 0)
			sleep(2);
	}
	if (rfkill_blocked() < 0) {
		fprintf(stderr, "%s is gone\n", rfkill_name);
		return -1;
	}
	return 0;
}

/* Boot ROM: sync at the ROM baud rate. */
static int brom_start(const struct opts *o)
{
	uint8_t u[64];
	int k;

	k = rd(u, 64, 150);
	fprintf(stderr, "unsolicited rx after reset: %d bytes\n", k);
	hexdump("unsol", u, k);

	fprintf(stderr, "== BROM sync @%d\n", o->rom_baud);
	if (brom_sync(o->probe ? 100 : 200, 16)) {
		fprintf(stderr, "sync failed\n");
		return -1;
	}
	fprintf(stderr, "BROM answered OK\n");
	return 0;
}

/* Talk HCI to the freshly started firmware and set the baud rate and address. */
static int hci_bringup(const struct opts *o)
{
	static const uint8_t baud[] = { 0x60, 0xe3, 0x16, 0x00 };	/* 1500000, LE */
	uint8_t addr[9] = { 2, 0, 6 };
	int okr = 0;

	msleep(300);	/* firmware boot time: a reset sent after only ~15 ms is lost */
	set_baud(o->post_baud == 1500000 ? B1500000 : B115200, o->flow);
	msleep(10);

	fprintf(stderr, "== HCI\n");
	for (int t = 0; t < 3 && !okr; t++) {
		if (t)
			fprintf(stderr, "HCI reset retry\n");
		okr = !hci_cmd(HCI_OP_RESET, NULL, 0, "HCI reset");
	}
	if (!okr) {
		fprintf(stderr, "HCI reset failed\n");
		return -1;
	}
	hci_cmd(HCI_OP_XR_SET_BAUD, baud, sizeof(baud), "vendor baud");
	set_baud(B1500000, o->flow);
	msleep(100);
	if (o->have_bdaddr) {
		memcpy(addr + 3, o->bdaddr, 6);
		hci_cmd(HCI_OP_XR_SET_BDADDR, addr, sizeof(addr), "set bdaddr");
	}
	if (hci_cmd(HCI_OP_RESET, NULL, 0, "HCI reset 2")) {
		fprintf(stderr, "HCI reset 2 failed\n");
		return -1;
	}
	return 0;
}

/* Attach the tty as an HCI H4 line discipline and get the hci number. */
static int attach_h4(int *hci)
{
	int ld = N_HCI;
	int proto = HCI_UART_H4;
	int fl = fcntl(fd, F_GETFL);

	fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
	if (ioctl(fd, TIOCSETD, &ld)) {
		perror("TIOCSETD");
		return -1;
	}
	if (ioctl(fd, HCIUARTSETPROTO, (long)proto)) {
		perror("HCIUARTSETPROTO");
		return -1;
	}
	*hci = -1;
	ioctl(fd, HCIUARTGETDEVICE, hci);
	fprintf(stderr, "attached as H4 (hci%d); staying resident\n", *hci);
	return 0;
}

/*
 * Stay until the controller goes away: a block on the rfkill switch resets
 * it, and the HCI device is gone if the line discipline was dropped. Exit
 * with an error then, so the service restarts and loads the firmware again
 * once it is unblocked.
 */
static int stay_resident(int hci)
{
	for (;;) {
		char p[64];

		sleep(1);
		snprintf(p, sizeof(p), "/sys/class/bluetooth/hci%d", hci);
		if (rfkill_blocked() > 0) {
			fprintf(stderr, "controller blocked, exiting\n");
			return EXIT_BLOCKED;
		}
		if (rfkill_blocked() < 0 || (hci >= 0 && access(p, F_OK))) {
			fprintf(stderr, "controller gone, exiting\n");
			return EXIT_FAIL;
		}
	}
}

int main(int argc, char **argv)
{
	struct opts o = {
		.dev = "/dev/ttyS1",
		.fw = "/lib/firmware/xr829/fw_xr829_bt.bin",
		.wake_gpio = "7022000.pinctrl:4",
		.post_baud = 115200,
		.wake = 1,
		.reset_low_ms = 20,
		.rom_baud = 115200,
	};
	int wake_fd = -1;
	int hci;

	if (parse_args(argc, argv, &o))
		return EXIT_USAGE;

	if (find_rfkill()) {
		fprintf(stderr, "no rfkill named %s\n", rfkill_name);
		return EXIT_FAIL;
	}
	if (wait_for_unblock())
		return EXIT_FAIL;

	alarm(ALARM_SECONDS);	/* SIGALRM ends a wedged bring-up */
	fd = open(o.dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		perror(o.dev);
		return EXIT_FAIL;
	}
	if (set_baud(baud_speed(o.rom_baud), 0)) {
		perror("termios");
		return EXIT_FAIL;
	}

	/* BT_WAKE high (unless -w 0 or -g none), then a reset pulse */
	if (o.wake && strcmp(o.wake_gpio, "none")) {
		wake_fd = bt_wake_request(o.wake_gpio);
		if (wake_fd < 0)
			fprintf(stderr, "warning: BT_WAKE not driven\n");
	}
	if (power_cycle(o.reset_low_ms)) {
		fprintf(stderr, "%s got blocked\n", rfkill_name);
		return EXIT_BLOCKED;
	}

	if (brom_start(&o))
		return EXIT_FAIL;
	if (o.probe)
		return 0;

	if (brom_set_fast_uart() || brom_upload(o.fw) || brom_set_pc())
		return EXIT_FAIL;
	if (hci_bringup(&o))
		return EXIT_FAIL;

	if (attach_h4(&hci))
		return EXIT_FAIL;
	alarm(0);

	if (!o.nofork && fork())
		return 0;
	if (!o.nofork) {	/* the child keeps the tty (and the BT_WAKE line) open */
		int nul;

		setsid();
		nul = open("/dev/null", O_RDWR);
		dup2(nul, 0);
		dup2(nul, 1);
		dup2(nul, 2);
	}
	return stay_resident(hci);
}
