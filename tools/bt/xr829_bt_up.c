/*
 * xr829_bt_up - XR829 Bluetooth firmware uploader + H4 attach (userspace).
 * Protocol: docs/bluetooth-xr829-protocol.md (from the vendor rtk_hciattach "xradio" type).
 *
 *   xr829_bt_up [-d /dev/ttyS1] [-f fw.bin] [-a aa:bb:cc:dd:ee:ff] [-p]   (-p = probe only)
 *               [-g chiplabel:line | -g none]   (BT_WAKE GPIO, default 7022000.pinctrl:4 = PL4)
 *               [-n]   (stay in the foreground once attached, for a Type=exec service)
 *               [-k rfkill-name]   (default rfkill-xr829-bt)  [-F 1]   (RTS/CTS flow control; off
 *               by default: CTS (PG9) floats on the TrimUI Smart Pro and would stall TX)
 *               [-i 115200|921600|1500000] [-B 115200|1500000]
 *
 * Keeps the tty open for the whole sequence, then attaches it as an HCI H4 line discipline
 * and stays resident (the hci0 device lives as long as this fd is open). The bring-up is
 * bounded by a 40 s alarm, so a wedged controller cannot hang the caller. BT_WAKE is
 * requested through the GPIO character device and held high for as long as the
 * resident process lives, so the controller never enters its low-power mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <linux/gpio.h>

#define N_HCI 15
#define HCIUARTSETPROTO _IOW('U', 200, int)
#define HCIUARTGETPROTO _IOR('U', 201, int)
#define HCIUARTGETDEVICE _IOR('U', 202, int)
#define HCI_UART_H4 0

static int fd = -1, verbose = 1;

static void msleep(int ms) { usleep(ms * 1000); }

static void hexdump(const char *tag, const uint8_t *b, int n)
{
	if (!verbose) return;
	fprintf(stderr, "%s [%d]:", tag, n);
	for (int i = 0; i < n && i < 32; i++) fprintf(stderr, " %02x", b[i]);
	fprintf(stderr, "\n");
}

static int write_file(const char *path, const char *s)
{
	int f = open(path, O_WRONLY);
	if (f < 0) { perror(path); return -1; }
	int r = write(f, s, strlen(s));
	close(f);
	return r < 0 ? -1 : 0;
}

/* The rfkill switch of the XR829 Bluetooth (rfkill-gpio on BT_RST_N), by label. */
static const char *rfkill_name = "rfkill-xr829-bt";
static char rfkill_dir[300];

static int find_rfkill(void)
{
	DIR *d = opendir("/sys/class/rfkill");
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char p[300], name[64] = "";
		FILE *f;

		snprintf(p, sizeof p, "/sys/class/rfkill/%s/name", e->d_name);
		f = fopen(p, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof name, f) && !strncmp(name, rfkill_name, strlen(rfkill_name)) &&
		    (name[strlen(rfkill_name)] == '\n' || !name[strlen(rfkill_name)])) {
			snprintf(rfkill_dir, sizeof rfkill_dir, "/sys/class/rfkill/%s", e->d_name);
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

	snprintf(p, sizeof p, "%s/%s", rfkill_dir, attr);
	f = fopen(p, "r");
	if (!f)
		return -1;
	if (!fgets(v, sizeof v, f))
		v[0] = 0;
	fclose(f);
	return v[0] == '1' ? 1 : v[0] == '0' ? 0 : -1;
}

/* 1 if blocked, 0 if not, -1 if the rfkill switch is gone */
static int rfkill_blocked(void)
{
	int soft = rfkill_attr("soft"), hard = rfkill_attr("hard");

	if (soft < 0 || hard < 0)
		return -1;
	return soft || hard;
}

/* Exit codes for the wrapper script */
#define EXIT_FAIL	1	/* bring-up failed */
#define EXIT_BLOCKED	3	/* blocked after the attach: not a failure */

/*
 * Request "label:line" as an output driven high; returns the line fd (keep it open to
 * keep the line high) or -1.
 */
static int bt_wake_request(const char *spec)
{
	char label[GPIO_MAX_NAME_SIZE];
	const char *colon = strrchr(spec, ':');
	unsigned int line;

	if (!colon || colon == spec || (size_t)(colon - spec) >= sizeof label ||
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

		snprintf(path, sizeof path, "/dev/gpiochip%d", n);
		int c = open(path, O_RDWR | O_CLOEXEC);
		if (c < 0)
			continue;
		if (ioctl(c, GPIO_GET_CHIPINFO_IOCTL, &info) || strcmp(info.label, label)) {
			close(c);
			continue;
		}
		memset(&req, 0, sizeof req);
		req.offsets[0] = line;
		req.num_lines = 1;
		req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
		req.config.num_attrs = 1;
		req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
		req.config.attrs[0].attr.values = 1;
		req.config.attrs[0].mask = 1;
		snprintf(req.consumer, sizeof req.consumer, "xr829-bt-wake");
		int r = ioctl(c, GPIO_V2_GET_LINE_IOCTL, &req);
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

	snprintf(rfkill_state_path, sizeof rfkill_state_path, "%s/state", rfkill_dir);
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

static int set_baud(speed_t sp, int crtscts)
{
	struct termios t;
	if (tcgetattr(fd, &t)) return -1;
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
	if (crtscts) t.c_cflag |= CRTSCTS;
	t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
	cfsetispeed(&t, sp); cfsetospeed(&t, sp);
	if (tcsetattr(fd, TCSANOW, &t)) return -1;
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
		struct pollfd p = { fd, POLLIN, 0 };
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
	case 115200: return B115200;
	case 921600: return B921600;
	case 1500000: return B1500000;
	default: return B0;
	}
}

static int wr(const void *b, int n)
{
	const uint8_t *p = b; int left = n;
	while (left > 0) {
		int r = write(fd, p, left);
		if (r < 0) { if (errno == EAGAIN) { msleep(1); continue; } return -1; }
		p += r; left -= r;
	}
	tcdrain(fd);
	return n;
}

/* ---- BROM packets ---- */
static uint16_t csum_native(const uint8_t *b, int n)
{
	uint32_t s = 0;
	for (int i = 0; i + 1 < n; i += 2) s += b[i] | (b[i + 1] << 8);
	if (n & 1) s += b[n - 1];
	return ~s & 0xffff;
}

/* args: nargs of (value,nbytes) */
static int mkpkt(uint8_t *out, uint8_t cmd, const uint32_t *v, const int *nb, int na)
{
	uint8_t pay[32], nat[64];
	int L = 0;
	pay[L++] = cmd;
	int NL = 0;
	nat[NL++] = cmd;
	for (int i = 0; i < na; i++) {
		for (int k = 0; k < nb[i]; k++) pay[L++] = v[i] >> (8 * (nb[i] - 1 - k));
		for (int k = 0; k < nb[i]; k++) nat[NL++] = v[i] >> (8 * k);
	}
	uint8_t h[64] = "BROM";
	h[4] = 4; h[5] = 0; h[6] = h[7] = 0;
	h[8] = L; h[9] = L >> 8; h[10] = h[11] = 0;
	memcpy(h + 12, nat, NL);
	uint16_t c = csum_native(h, 12 + NL);
	memcpy(out, "BROM", 4);
	out[4] = 4; out[5] = 0; out[6] = c >> 8; out[7] = c;
	out[8] = 0; out[9] = 0; out[10] = L >> 8; out[11] = L;
	memcpy(out + 12, pay, L);
	return 12 + L;
}

static int brom_ack(const char *what)
{
	uint8_t r[16];
	int n = rd(r, 12, 1000);
	hexdump(what, r, n);
	if (n != 12 || memcmp(r, "BROM", 4)) return -1;
	if (r[4] & 1) { uint8_t e; rd(&e, 1, 200); fprintf(stderr, "%s: error byte %02x\n", what, e); return -1; }
	if (!(r[4] & 2)) return -1;
	return 0;
}

/* tries > 0: give up after that many 0x55 bytes (each ~20 ms, plus power cycles) */
static int brom_sync(int tries, int cycle_after)
{
	uint8_t b[2];
	for (int i = 0; i < tries; i++) {
		uint8_t c = 0x55;
		wr(&c, 1);
		msleep(20);
		int n = rd(b, 2, 0);
		if (n > 0) {
			if (verbose) fprintf(stderr, "sync rx %d byte(s): %02x %02x (try %d)\n", n, b[0], n > 1 ? b[1] : 0, i);
			if (n == 1) n += rd(b + 1, 1, 50);
			if (n == 2 && ((b[0] == 'O' && b[1] == 'K') || (b[0] == 'K' && b[1] == 'O'))) return 0;
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

static int hci_cmd(const uint8_t *cmd, int n, const char *what)
{
	uint8_t r[64];
	wr(cmd, n);
	int got = rd(r, 7, 1000);
	if (got >= 7) {
		/* Command Complete: 04 0e len ncmd opcode(2) status */
		int len = r[2];
		if (len > 4) rd(r + 7, len - 4 > 50 ? 50 : len - 4, 200);
	}
	hexdump(what, r, got);
	/* Command Complete for this opcode (cmd[1..2]) with status 0 */
	return (got >= 7 && r[0] == 4 && r[1] == 0x0e && r[4] == cmd[1] &&
		r[5] == cmd[2] && r[6] == 0) ? 0 : -1;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyS1", *fw = "/lib/firmware/xr829/fw_xr829_bt.bin";
	const char *wake_gpio = "7022000.pinctrl:4";
	int wake_fd = -1, nofork = 0;
	uint8_t bd[6] = { 0 }; int have_bd = 0, probe = 0, fast = 1500000, flow = 0, postb = 115200, wake = 1, rstlow = 20, ibaud = 115200;
	int o;
	while ((o = getopt(argc, argv, "d:f:a:pqw:r:i:F:B:g:nk:")) != -1) {
		switch (o) {
		case 'd': dev = optarg; break;
		case 'f': fw = optarg; break;
		case 'p': probe = 1; break;
		case 'q': verbose = 0; break;
		case 'F': flow = atoi(optarg); break;
		case 'B': postb = atoi(optarg); if (postb != 115200 && postb != 1500000) return 2; break;
		case 'w': wake = atoi(optarg); break;
		case 'r': rstlow = atoi(optarg); break;
		case 'i': ibaud = atoi(optarg); if (baud_speed(ibaud) == B0) return 2; break;
		case 'k': rfkill_name = optarg; break;
		case 'g': wake_gpio = optarg; break;
		case 'n': nofork = 1; break;
		case 'a': {
			unsigned a[6];
			if (sscanf(optarg, "%x:%x:%x:%x:%x:%x", a, a+1, a+2, a+3, a+4, a+5) != 6) return 2;
			for (int i = 0; i < 6; i++) bd[5 - i] = a[i];	/* little endian on the wire */
			have_bd = 1; break; }
		default: return 2;
		}
	}
	if (find_rfkill()) { fprintf(stderr, "no rfkill named %s\n", rfkill_name); return 1; }
	/* Respect a block by the user or systemd-rfkill: wait for the unblock. */
	if (rfkill_blocked() > 0) {
		fprintf(stderr, "%s is blocked, waiting\n", rfkill_name);
		while (rfkill_blocked() > 0)
			sleep(2);
	}
	if (rfkill_blocked() < 0) {
		fprintf(stderr, "%s is gone\n", rfkill_name);
		return EXIT_FAIL;
	}
	alarm(40);	/* SIGALRM ends a wedged bring-up */
	fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) { perror(dev); return 1; }
	if (set_baud(baud_speed(ibaud), 0)) { perror("termios"); return 1; }

	/* BT_WAKE high (unless -w 0 or -g none), then a reset pulse */
	if (wake && strcmp(wake_gpio, "none")) {
		wake_fd = bt_wake_request(wake_gpio);
		if (wake_fd < 0)
			fprintf(stderr, "warning: BT_WAKE not driven\n");
	}
	if (power_cycle(rstlow)) {
		fprintf(stderr, "%s got blocked\n", rfkill_name);
		return EXIT_BLOCKED;
	}
	{ uint8_t u[64]; int k = rd(u, 64, 150); fprintf(stderr, "unsolicited rx after reset: %d bytes\n", k); hexdump("unsol", u, k); }

	fprintf(stderr, "== BROM sync @%d\n", ibaud);
	if (brom_sync(probe ? 100 : 200, 16)) { fprintf(stderr, "sync failed\n"); return 1; }
	fprintf(stderr, "BROM answered OK\n");
	if (probe) return 0;

	uint8_t p[64];
	uint32_t v[3]; int nb[3]; int n;
	v[0] = 0x03000000 | fast; nb[0] = 4;
	n = mkpkt(p, 0x10, v, nb, 1);
	hexdump("SET_UART", p, n);
	wr(p, n);
	if (brom_ack("SET_UART ack")) return 1;
	msleep(5);
	if (set_baud(B1500000, 0)) { perror("termios 1.5M"); return 1; }
	fprintf(stderr, "== BROM sync @1.5M\n");
	if (brom_sync(200, 0)) { fprintf(stderr, "sync @1.5M failed\n"); return 1; }

	FILE *ff = fopen(fw, "rb");
	if (!ff) { perror(fw); return 1; }
	long sz = -1;
	if (!fseek(ff, 0, SEEK_END))
		sz = ftell(ff);
	if (sz <= 0 || sz > 4 * 1024 * 1024) { fprintf(stderr, "%s: bad size %ld\n", fw, sz); return 1; }
	rewind(ff);
	uint8_t *img = malloc(sz);
	if (!img) { perror("malloc"); return 1; }
	if (fread(img, 1, sz, ff) != (size_t)sz) { perror(fw); return 1; }
	fclose(ff);
	fprintf(stderr, "== upload %s (%ld bytes)\n", fw, sz);
	for (long off = 0; off < sz; off += 1024) {
		int len = sz - off > 1024 ? 1024 : sz - off;
		v[0] = off; nb[0] = 4; v[1] = len; nb[1] = 4; v[2] = csum_native(img + off, len); nb[2] = 2;
		n = mkpkt(p, 0x09, v, nb, 3);
		wr(p, n);
		if (brom_ack(off ? "chunk hdr ack" : "chunk0 hdr ack")) { fprintf(stderr, "fail at %ld\n", off); return 1; }
		wr(img + off, len);
		if (brom_ack(off ? "chunk data ack" : "chunk0 data ack")) { fprintf(stderr, "fail data at %ld\n", off); return 1; }
		if (!verbose) continue;
		if (off > 2048) verbose = 0;	/* quiet after first chunks */
	}
	verbose = 1;
	fprintf(stderr, "upload done\n");
	v[0] = 0; nb[0] = 4;
	n = mkpkt(p, 0x13, v, nb, 1);
	wr(p, n);
	if (brom_ack("SET_PC ack")) return 1;
	if (getenv("XR_DEBUG")) {
		static const uint8_t rst[] = { 1, 3, 0x0c, 0 };
		int rates[] = { 1500000, 115200, 921600, 3000000, 2000000, 460800, 230400, 57600 };
		speed_t sp[] = { B1500000, B115200, B921600, B3000000, B2000000, B460800, B230400, B57600 };
		/* Debug: look for the firmware's HCI at the usual baud rates, no flow control. */
		for (int ri = 0; ri < 8; ri++) {
			tcflush(fd, TCIOFLUSH);
			set_baud(sp[ri], 0);
			uint8_t u[64]; memset(u,0xee,64); int k = rd(u, 64, 300);
			fprintf(stderr, "dbg %d passive rx %d\n", rates[ri], k); hexdump("  rx", u, k);
			wr(rst, 4);
			memset(u,0xee,64); k = rd(u, 64, 500);
			fprintf(stderr, "dbg %d reset rx %d\n", rates[ri], k); hexdump("  rx", u, k);
		}
		return 0;
	}
	msleep(300);	/* firmware boot time: a reset sent after only ~15 ms is lost */
	set_baud(postb == 1500000 ? B1500000 : B115200, flow);
	msleep(10);

	static const uint8_t reset[] = { 1, 3, 0x0c, 0 };
	static const uint8_t baud[] = { 1, 0x18, 0xfc, 4, 0x60, 0xe3, 0x16, 0 };
	uint8_t addr[] = { 1, 0x0a, 0xfc, 9, 2, 0, 6, 0, 0, 0, 0, 0, 0 };
	fprintf(stderr, "== HCI\n");
	int okr = 0;
	for (int t = 0; t < 3 && !okr; t++) { if (t) fprintf(stderr, "HCI reset retry\n"); okr = !hci_cmd(reset, 4, "HCI reset"); }
	if (!okr) { fprintf(stderr, "HCI reset failed\n"); return 1; }
	hci_cmd(baud, 8, "vendor baud");
	set_baud(B1500000, flow);
	msleep(100);
	if (have_bd) {
		memcpy(addr + 7, bd, 6);
		hci_cmd(addr, 13, "set bdaddr");
	}
	if (hci_cmd(reset, 4, "HCI reset 2")) { fprintf(stderr, "HCI reset 2 failed\n"); return 1; }

	int ld = N_HCI, proto = HCI_UART_H4;
	int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
	if (ioctl(fd, TIOCSETD, &ld)) { perror("TIOCSETD"); return 1; }
	if (ioctl(fd, HCIUARTSETPROTO, (long)proto)) { perror("HCIUARTSETPROTO"); return 1; }
	int hci = -1;
	ioctl(fd, HCIUARTGETDEVICE, &hci);
	fprintf(stderr, "attached as H4 (hci%d); staying resident\n", hci);
	alarm(0);
	if (!nofork && fork())
		return 0;
	if (!nofork) {	/* child keeps the tty (and the BT_WAKE line) open */
		setsid();
		int nul = open("/dev/null", O_RDWR); dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
	}
	/*
	 * Stay until the controller goes away: a block on the rfkill switch
	 * resets it, and the HCI device is gone if the line discipline was
	 * dropped. Exit with an error then, so the service restarts and loads
	 * the firmware again once it is unblocked.
	 */
	for (;;) {
		char p[64];

		sleep(1);
		snprintf(p, sizeof p, "/sys/class/bluetooth/hci%d", hci);
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
