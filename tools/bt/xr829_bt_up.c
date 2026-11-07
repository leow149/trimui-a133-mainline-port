/*
 * xr829_bt_up - XR829 Bluetooth firmware uploader + H4 attach (userspace).
 * Protocol: docs/bluetooth-xr829-protocol.md (from the vendor rtk_hciattach "xradio" type).
 *
 *   xr829_bt_up [-d /dev/ttyS1] [-f fw.bin] [-a aa:bb:cc:dd:ee:ff] [-p]   (-p = probe only)
 *
 * Keeps the tty open for the whole sequence, then attaches it as an HCI H4 line discipline
 * and stays resident (the hci0 device lives as long as this fd is open).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>

#define N_HCI 15
#define HCIUARTSETPROTO _IOW('U', 200, int)
#define HCIUARTGETPROTO _IOR('U', 201, int)
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

static char rfkill_state[128];
static int find_rfkill(void)
{
	DIR *d = opendir("/sys/class/rfkill");
	struct dirent *e;
	if (!d) return -1;
	while ((e = readdir(d))) {
		char p[160], t[32] = "";
		snprintf(p, sizeof p, "/sys/class/rfkill/%s/type", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		if (fgets(t, sizeof t, f) && !strncmp(t, "bluetooth", 9)) {
			snprintf(rfkill_state, sizeof rfkill_state, "/sys/class/rfkill/%s/state", e->d_name);
			fclose(f); closedir(d); return 0;
		}
		fclose(f);
	}
	closedir(d);
	return -1;
}

static void power_cycle(int low_ms)
{
	write_file(rfkill_state, "0");
	msleep(low_ms);
	write_file(rfkill_state, "1");
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

/* read exactly n bytes within timeout ms total; returns bytes read */
static int rd(uint8_t *b, int n, int to_ms)
{
	int got = 0;
	while (got < n) {
		struct pollfd p = { fd, POLLIN, 0 };
		int r = poll(&p, 1, to_ms);
		if (r <= 0) break;
		r = read(fd, b + got, n - got);
		if (r > 0) got += r;
	}
	return got;
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

static int brom_sync(int tries, int cycle_after)
{
	uint8_t b[2];
	for (int i = 0; ; i++) {
		uint8_t c = 0x55;
		wr(&c, 1);
		msleep(20);
		int n = rd(b, 2, 0);
		if (n > 0) {
			if (verbose) fprintf(stderr, "sync rx %d byte(s): %02x %02x (try %d)\n", n, b[0], n > 1 ? b[1] : 0, i);
			if (n == 1) n += rd(b + 1, 1, 50);
			if (n == 2 && ((b[0] == 'O' && b[1] == 'K') || (b[0] == 'K' && b[1] == 'O'))) return 0;
		}
		if (tries && i + 1 >= tries) return -1;
		if (cycle_after && (i + 1) % cycle_after == 0) {
			fprintf(stderr, "sync: no answer after %d tries, power cycle\n", i + 1);
			power_cycle(500); msleep(20);
			tcflush(fd, TCIOFLUSH);
		}
	}
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
	return (got >= 7 && r[0] == 4 && r[1] == 0x0e && r[6] == 0) ? 0 : -1;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/ttyS1", *fw = "/lib/firmware/xr829/fw_xr829_bt.bin";
	uint8_t bd[6] = { 0 }; int have_bd = 0, probe = 0, fast = 1500000, flow = 1, postb = 115200, wake = 1, rstlow = 20, ibaud = 115200;
	int o;
	while ((o = getopt(argc, argv, "d:f:a:pqw:r:i:F:B:")) != -1) {
		switch (o) {
		case 'd': dev = optarg; break;
		case 'f': fw = optarg; break;
		case 'p': probe = 1; break;
		case 'q': verbose = 0; break;
		case 'F': flow = atoi(optarg); break;
		case 'B': postb = atoi(optarg); break;
		case 'w': wake = atoi(optarg); break;
		case 'r': rstlow = atoi(optarg); break;
		case 'i': ibaud = atoi(optarg); break;
		case 'a': {
			unsigned a[6];
			if (sscanf(optarg, "%x:%x:%x:%x:%x:%x", a, a+1, a+2, a+3, a+4, a+5) != 6) return 2;
			for (int i = 0; i < 6; i++) bd[5 - i] = a[i];	/* little endian on the wire */
			have_bd = 1; break; }
		default: return 2;
		}
	}
	if (find_rfkill()) { fprintf(stderr, "no bluetooth rfkill\n"); return 1; }
	fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) { perror(dev); return 1; }
	if (set_baud(ibaud == 115200 ? B115200 : ibaud == 921600 ? B921600 : ibaud == 1500000 ? B1500000 : B9600, 0)) { perror("termios"); return 1; }

	/* 2: reset pulse, 3: wake on, lpm off */
	power_cycle(rstlow); 
	write_file("/proc/bluetooth/sleep/btwake", wake ? "1" : "0");
	write_file("/proc/bluetooth/sleep/lpm", "0");
	{ uint8_t u[64]; int k = rd(u, 64, 150); fprintf(stderr, "unsolicited rx after reset: %d bytes\n", k); hexdump("unsol", u, k); }

	fprintf(stderr, "== BROM sync @%d\n", ibaud);
	if (brom_sync(probe ? 100 : 0, 16)) { fprintf(stderr, "sync failed\n"); return 1; }
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
	fseek(ff, 0, SEEK_END); long sz = ftell(ff); rewind(ff);
	uint8_t *img = malloc(sz);
	if (fread(img, 1, sz, ff) != (size_t)sz) return 1;
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
		for (int ri = 0; ri < 8; ri++) for (int fl = 0; fl < 1; fl++) {
			tcflush(fd, TCIOFLUSH);
			set_baud(sp[ri], fl);
			uint8_t u[64]; memset(u,0xee,64); int k = rd(u, 64, 300);
			fprintf(stderr, "dbg %d flow=%d passive rx %d\n", rates[ri], fl, k); hexdump("  rx", u, k);
			wr(rst, 4);
			memset(u,0xee,64); k = rd(u, 64, 500);
			fprintf(stderr, "dbg %d flow=%d reset rx %d\n", rates[ri], fl, k); hexdump("  rx", u, k);
		}
dbg_done: return 0;
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
	fprintf(stderr, "attached as H4; staying resident\n");
	if (!fork()) {	/* child keeps fd open */
		setsid();
		int nul = open("/dev/null", O_RDWR); dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
		for (;;) pause();
	}
	return 0;
}
