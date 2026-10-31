/*
 * padview: show the TrimUI Smart Pro gamepad state on the framebuffer.
 *
 * Draws a landscape controller view (buttons light up, stick dots move, last
 * events listed) straight to /dev/fb0, in the same orientation as fbcon
 * (/sys/class/graphics/fbcon/rotate). Hold SELECT+START for a second to quit.
 *
 * Usage: padview [--shot FILE]
 *   --shot FILE  write what is on /dev/fb0 as a half-size landscape PPM and exit
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PAD_NAME	"TrimUI Smart Pro Gamepad"

#define C_BG		0x0f1216
#define C_PANEL		0x1b2027
#define C_IDLE		0x2b323b
#define C_EDGE		0x4a535e
#define C_ON		0x3fb950
#define C_DOT		0xf0b429
#define C_TEXT		0xe6edf3
#define C_DIM		0x8b949e

static int W, H;		/* logical (landscape) canvas */
static uint32_t *canvas;
static uint32_t *fbmem;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static int rot;
static int tty = -1;
static volatile sig_atomic_t quit;

/* ---- 5x7 font, rows top to bottom, bit 4 = leftmost column ---- */
static const char font_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-:=/_.";
static const uint8_t font[][7] = {
	{0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
	{0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}, {0x1f,0x02,0x04,0x02,0x01,0x11,0x0e},
	{0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e},
	{0x06,0x08,0x10,0x1e,0x11,0x11,0x0e}, {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
	{0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}, {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c},
	{0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
	{0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}, {0x1c,0x12,0x11,0x11,0x11,0x12,0x1c},
	{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
	{0x0e,0x11,0x10,0x17,0x11,0x11,0x0f}, {0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
	{0x0e,0x04,0x04,0x04,0x04,0x04,0x0e}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0c},
	{0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
	{0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
	{0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}, {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
	{0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
	{0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}, {0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
	{0x11,0x11,0x11,0x11,0x11,0x11,0x0e}, {0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
	{0x11,0x11,0x11,0x15,0x15,0x15,0x0a}, {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
	{0x11,0x11,0x11,0x0a,0x04,0x04,0x04}, {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f},
	{0x00,0x04,0x04,0x1f,0x04,0x04,0x00}, {0x00,0x00,0x00,0x1f,0x00,0x00,0x00},
	{0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00}, {0x00,0x00,0x1f,0x00,0x1f,0x00,0x00},
	{0x00,0x01,0x02,0x04,0x08,0x10,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x1f},
	{0x00,0x00,0x00,0x00,0x00,0x0c,0x0c},
};

/* ---- drawing on the logical canvas ---- */
static void px(int x, int y, uint32_t c)
{
	if (x >= 0 && y >= 0 && x < W && y < H)
		canvas[y * W + x] = c;
}

static void rect(int x, int y, int w, int h, uint32_t c)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			px(i, j, c);
}

static void rrect(int x, int y, int w, int h, int r, uint32_t c)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			int dx = i < r ? r - i : (i >= w - r ? i - (w - r - 1) : 0);
			int dy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
			if (dx * dx + dy * dy <= r * r)
				px(x + i, y + j, c);
		}
}

static void disc(int cx, int cy, int r, uint32_t c)
{
	for (int j = -r; j <= r; j++)
		for (int i = -r; i <= r; i++)
			if (i * i + j * j <= r * r)
				px(cx + i, cy + j, c);
}

static void ring(int cx, int cy, int r, int t, uint32_t c)
{
	for (int j = -r; j <= r; j++)
		for (int i = -r; i <= r; i++) {
			int d = i * i + j * j;
			if (d <= r * r && d >= (r - t) * (r - t))
				px(cx + i, cy + j, c);
		}
}

static int text_w(const char *s, int scale)
{
	return (int)strlen(s) * 6 * scale - scale;
}

static void text(int x, int y, const char *s, int scale, uint32_t c)
{
	for (; *s; s++, x += 6 * scale) {
		char ch = *s >= 'a' && *s <= 'z' ? *s - 32 : *s;
		const char *p = strchr(font_chars, ch);

		if (ch == ' ' || !p)
			continue;
		for (int row = 0; row < 7; row++)
			for (int col = 0; col < 5; col++)
				if (font[p - font_chars][row] & (0x10 >> col))
					rect(x + col * scale, y + row * scale, scale, scale, c);
	}
}

static void text_c(int cx, int y, const char *s, int scale, uint32_t c)
{
	text(cx - text_w(s, scale) / 2, y, s, scale, c);
}

/* ---- framebuffer ---- */
static size_t fb_off(int lx, int ly)
{
	int x, y;

	switch (rot) {
	case 1: x = var.xres - 1 - ly; y = lx; break;
	case 2: x = var.xres - 1 - lx; y = var.yres - 1 - ly; break;
	case 3: x = ly; y = var.yres - 1 - lx; break;
	default: x = lx; y = ly; break;
	}
	return (size_t)y * (fix.line_length / 4) + x;
}

static void blit(void)
{
	for (int ly = 0; ly < H; ly++)
		for (int lx = 0; lx < W; lx++)
			fbmem[fb_off(lx, ly)] = canvas[ly * W + lx];
}

static int fb_open(void)
{
	FILE *f = fopen("/sys/class/graphics/fbcon/rotate", "r");
	int fd = open("/dev/fb0", O_RDWR);

	if (f) {
		if (fscanf(f, "%d", &rot) != 1)
			rot = 0;
		fclose(f);
	}
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix) || var.bits_per_pixel != 32) {
		fprintf(stderr, "need a 32 bpp /dev/fb0\n");
		return -1;
	}
	fbmem = mmap(NULL, fix.line_length * var.yres, PROT_READ | PROT_WRITE,
		     MAP_SHARED, fd, 0);
	if (fbmem == MAP_FAILED)
		return -1;
	W = rot & 1 ? var.yres : var.xres;
	H = rot & 1 ? var.xres : var.yres;
	canvas = calloc((size_t)W * H, 4);
	return canvas ? 0 : -1;
}

static int shot(const char *path)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		return 1;
	fprintf(f, "P6\n%d %d\n255\n", W / 2, H / 2);
	for (int ly = 0; ly < H; ly += 2)
		for (int lx = 0; lx < W; lx += 2) {
			uint32_t c = fbmem[fb_off(lx, ly)];
			uint8_t rgb[3] = { c >> 16, c >> 8, c };

			fwrite(rgb, 1, 3, f);
		}
	return fclose(f) ? 1 : 0;
}

/* ---- gamepad state ---- */
static struct {
	int code;
	const char *name;
	int down;
} keys[] = {
	{ BTN_EAST, "A" }, { BTN_SOUTH, "B" }, { BTN_NORTH, "X" }, { BTN_WEST, "Y" },
	{ BTN_TL, "L1" }, { BTN_TR, "R1" },
	{ BTN_SELECT, "SELECT" }, { BTN_START, "START" }, { BTN_MODE, "MENU" },
	{ BTN_DPAD_UP, "UP" }, { BTN_DPAD_DOWN, "DOWN" },
	{ BTN_DPAD_LEFT, "LEFT" }, { BTN_DPAD_RIGHT, "RIGHT" },
};
#define NKEYS (int)(sizeof(keys) / sizeof(keys[0]))

static int axis[ABS_CNT];
static struct input_absinfo absinfo[ABS_CNT];
static char log_lines[6][32];

static int key_idx(int code)
{
	for (int i = 0; i < NKEYS; i++)
		if (keys[i].code == code)
			return i;
	return -1;
}

static int down(int code)
{
	int i = key_idx(code);

	return i >= 0 && keys[i].down;
}

static void log_event(const char *s)
{
	memmove(log_lines[1], log_lines[0], sizeof(log_lines) - sizeof(log_lines[0]));
	snprintf(log_lines[0], sizeof(log_lines[0]), "%s", s);
}

static int pad_open(void)
{
	char path[32], name[64];

	for (int i = 0; i < 32; i++) {
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 &&
		    !strcmp(name, PAD_NAME)) {
			for (int a = 0; a < ABS_CNT; a++)
				if (!ioctl(fd, EVIOCGABS(a), &absinfo[a]))
					axis[a] = absinfo[a].value;
			return fd;
		}
		close(fd);
	}
	return -1;
}

/* ---- the view ---- */
static void button(int cx, int cy, int r, const char *label, int on)
{
	disc(cx, cy, r, on ? C_ON : C_IDLE);
	ring(cx, cy, r, 3, on ? C_ON : C_EDGE);
	text_c(cx, cy - 10, label, 3, on ? C_BG : C_TEXT);
}

static void pill(int cx, int cy, int w, const char *label, int on)
{
	rrect(cx - w / 2, cy - 24, w, 48, 24, on ? C_ON : C_IDLE);
	text_c(cx, cy - 7, label, 2, on ? C_BG : C_TEXT);
}

static void dpad(int cx, int cy)
{
	int s = 62;

	rect(cx - s / 2, cy - s / 2, s, s, C_IDLE);
	rrect(cx - s / 2, cy - s / 2 - s, s, s, 10, down(BTN_DPAD_UP) ? C_ON : C_IDLE);
	rrect(cx - s / 2, cy + s / 2, s, s, 10, down(BTN_DPAD_DOWN) ? C_ON : C_IDLE);
	rrect(cx - s / 2 - s, cy - s / 2, s, s, 10, down(BTN_DPAD_LEFT) ? C_ON : C_IDLE);
	rrect(cx + s / 2, cy - s / 2, s, s, 10, down(BTN_DPAD_RIGHT) ? C_ON : C_IDLE);
}

static void stick(int cx, int cy, int ax, int ay, const char *label)
{
	int r = 110;
	char buf[32];
	int x = axis[ax], y = axis[ay];
	int max = absinfo[ax].maximum ? absinfo[ax].maximum : 32767;

	disc(cx, cy, r, C_PANEL);
	ring(cx, cy, r, 3, C_EDGE);
	rect(cx - r, cy, 2 * r, 1, C_EDGE);
	rect(cx, cy - r, 1, 2 * r, C_EDGE);
	disc(cx + x * (r - 18) / max, cy + y * (r - 18) / max, 18, C_DOT);
	text_c(cx, cy + r + 14, label, 2, C_DIM);
	snprintf(buf, sizeof(buf), "%+6d %+6d", x, y);
	text_c(cx, cy + r + 40, buf, 2, C_TEXT);
}

static void draw(void)
{
	rect(0, 0, W, H, C_BG);
	text_c(W / 2, 22, "TRIMUI SMART PRO - GAMEPAD TEST", 3, C_TEXT);
	text_c(W / 2, 56, "HOLD SELECT + START TO QUIT", 2, C_DIM);

	pill(170, 60, 200, "L1", down(BTN_TL));
	pill(W - 170, 60, 200, "R1", down(BTN_TR));

	stick(220, 250, ABS_X, ABS_Y, "LEFT STICK");
	dpad(220, 560);

	button(W - 220, 165, 46, "X", down(BTN_NORTH));
	button(W - 315, 255, 46, "Y", down(BTN_WEST));
	button(W - 125, 255, 46, "A", down(BTN_EAST));
	button(W - 220, 345, 46, "B", down(BTN_SOUTH));
	stick(W - 220, 545, ABS_RX, ABS_RY, "RIGHT STICK");

	rrect(W / 2 - 200, 130, 400, 330, 16, C_PANEL);
	text_c(W / 2, 150, "LAST EVENTS", 2, C_DIM);
	for (int i = 0; i < 6; i++)
		text_c(W / 2, 195 + i * 42, log_lines[i], 3, i ? C_DIM : C_TEXT);

	pill(W / 2 - 150, 560, 130, "SELECT", down(BTN_SELECT));
	pill(W / 2, 560, 130, "MENU", down(BTN_MODE));
	pill(W / 2 + 150, 560, 130, "START", down(BTN_START));
}

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static void restore_tty(void)
{
	if (tty >= 0) {
		ioctl(tty, KDSETMODE, KD_TEXT);
		close(tty);
		tty = -1;
	}
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	struct input_event ev[64];
	long quit_since = 0;
	int fd, dirty = 1;

	if (fb_open())
		return 1;
	if (argc == 3 && !strcmp(argv[1], "--shot"))
		return shot(argv[2]);

	fd = pad_open();
	if (fd < 0) {
		fprintf(stderr, "no \"%s\" input device\n", PAD_NAME);
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	tty = open("/dev/tty0", O_RDWR);
	if (tty >= 0)
		ioctl(tty, KDSETMODE, KD_GRAPHICS);
	log_event("PRESS SOMETHING");

	while (!quit) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (dirty) {
			draw();
			blit();
			dirty = 0;
		}
		if (poll(&pfd, 1, 100) < 0 && errno != EINTR)
			break;

		while ((n = read(fd, ev, sizeof(ev))) > 0) {
			for (int i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
				char buf[32];
				int k;

				if (ev[i].type == EV_KEY && (k = key_idx(ev[i].code)) >= 0) {
					keys[k].down = ev[i].value != 0;
					snprintf(buf, sizeof(buf), "%s %s", keys[k].name,
						 ev[i].value ? "DOWN" : "UP");
					log_event(buf);
					dirty = 1;
				} else if (ev[i].type == EV_ABS && ev[i].code < ABS_CNT) {
					axis[ev[i].code] = ev[i].value;
					dirty = 1;
				}
			}
		}

		if (down(BTN_SELECT) && down(BTN_START)) {
			if (!quit_since)
				quit_since = now_ms();
			else if (now_ms() - quit_since > 1000)
				quit = 1;
		} else {
			quit_since = 0;
		}
	}

	restore_tty();
	return 0;
}
