// KMS tearing test: a white bar moves 8 px per frame across a black screen,
// double-buffered with vsync'd page flips. fliptest <card> <conn> <crtc> <seconds>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct buf { uint32_t handle, pitch, fb; uint64_t size; uint8_t *map; };

static void mkbuf(int fd, uint32_t w, uint32_t h, struct buf *b)
{
	struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 32 };
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); exit(1); }
	b->handle = cd.handle; b->pitch = cd.pitch; b->size = cd.size;
	struct drm_mode_fb_cmd2 f = { .width = w, .height = h,
		.pixel_format = 0x34325258 /* XR24 */, .handles = { b->handle },
		.pitches = { b->pitch } };
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f)) { perror("addfb2"); exit(1); }
	b->fb = f.fb_id;
	struct drm_mode_map_dumb md = { .handle = b->handle };
	ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
	b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
}

static void draw(struct buf *b, uint32_t w, uint32_t h, uint32_t pos)
{
	for (uint32_t y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(b->map + y * b->pitch);
		memset(row, 0, w * 4);
		for (uint32_t x = pos; x < pos + 24 && x < w; x++)
			row[x] = 0x00ffffff;
	}
}

int main(int argc, char **argv)
{
	int fd = open(argv[1], O_RDWR);
	uint32_t conn = atoi(argv[2]), crtc = atoi(argv[3]), secs = atoi(argv[4]);
	struct drm_mode_get_connector gc = { .connector_id = conn };
	struct drm_mode_modeinfo modes[8];
	ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc);
	gc.count_modes = gc.count_modes > 8 ? 8 : gc.count_modes;
	gc.modes_ptr = (uintptr_t)modes; gc.count_props = 0; gc.count_encoders = 0;
	ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc);
	uint32_t w = modes[0].hdisplay, h = modes[0].vdisplay;
	struct buf b[2];
	mkbuf(fd, w, h, &b[0]); mkbuf(fd, w, h, &b[1]);
	draw(&b[0], w, h, 0);
	struct drm_mode_crtc sc = { .crtc_id = crtc, .fb_id = b[0].fb, .set_connectors_ptr = (uintptr_t)&conn,
		.count_connectors = 1, .mode = modes[0], .mode_valid = 1 };
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)) { perror("setcrtc"); return 1; }
	time_t end = time(NULL) + secs;
	unsigned frames = 0, pos = 0;
	while (time(NULL) < end) {
		struct buf *back = &b[(frames + 1) & 1];
		pos = (pos + 8) % w;
		draw(back, w, h, pos);
		struct drm_mode_crtc_page_flip pf = { .crtc_id = crtc, .fb_id = back->fb,
			.flags = DRM_MODE_PAGE_FLIP_EVENT };
		if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf)) { perror("page_flip"); return 1; }
		struct pollfd p = { .fd = fd, .events = POLLIN };
		poll(&p, 1, 1000);
		char ev[256];
		read(fd, ev, sizeof(ev));
		frames++;
	}
	printf("%u flips in %u s = %.1f fps\n", frames, secs, (double)frames / secs);
	return 0;
}
