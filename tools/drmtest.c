/* Minimal, self-contained DRM legacy-modeset test tool. No libdrm — raw
 * ioctls only, so it links fully static against glibc with zero runtime
 * dependency on whatever's (or isn't) on the target rootfs.
 *
 * Purpose: our automatic fbcon-triggered atomic commit never seems to reach
 * the CRTC/encoder .enable() callbacks at all (confirmed via drm.debug
 * tracing — neither "Enabling the CRTC" nor "Enabling DSI output" ever
 * printed). This tool does a legacy DRM_IOCTL_MODE_SETCRTC instead of an
 * atomic commit — a different, synchronous, blocking code path — to check
 * whether the underlying hardware-enable path works at all when invoked
 * directly, decoupled from whatever's stuck in the async fbcon commit.
 *
 * Fills the framebuffer with solid red so success/failure is visually
 * obvious even without knowing exact pixel format assumptions.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)

struct drm_mode_card_res {
	uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
	uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
	uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_modeinfo {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint32_t vrefresh, flags, type;
	char name[32];
};

struct drm_mode_get_connector {
	uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
	uint32_t count_modes, count_props, count_encoders;
	uint32_t encoder_id, connector_id, connector_type, connector_type_id;
	uint32_t connection, mm_width, mm_height, subpixel;
	uint32_t pad;
};

struct drm_mode_get_encoder {
	uint32_t encoder_id, encoder_type, crtc_id;
	uint32_t possible_crtcs, possible_clones;
};

struct drm_mode_create_dumb {
	uint32_t height, width, bpp, flags;
	uint32_t handle, pitch;
	uint64_t size;
};

struct drm_mode_map_dumb {
	uint32_t handle, pad;
	uint64_t offset;
};

struct drm_mode_fb_cmd {
	uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};

struct drm_mode_crtc {
	uint64_t set_connectors_ptr;
	uint32_t count_connectors;
	uint32_t crtc_id, fb_id;
	uint32_t x, y, gamma_size, mode_valid;
	struct drm_mode_modeinfo mode;
};

#define DRM_IOCTL_MODE_GETRESOURCES   DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCONNECTOR   DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETENCODER     DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_CREATE_DUMB    DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB       DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_ADDFB          DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_SETCRTC        DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_SET_MASTER          _IO(DRM_IOCTL_BASE, 0x1e)

#define DRM_MODE_CONNECTED 1

static void die(const char *what) { perror(what); _exit(1); }

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	int fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) die("open card0");

	if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0)
		perror("SET_MASTER (continuing anyway)");

	struct drm_mode_card_res res = {0};
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) die("GETRESOURCES(count)");
	printf("fbs=%u crtcs=%u connectors=%u encoders=%u\n",
	       res.count_fbs, res.count_crtcs, res.count_connectors, res.count_encoders);

	/* Bug fixed here: count_fbs/count_encoders were left nonzero from the
	 * first call with fb_id_ptr/encoder_id_ptr still NULL, so the kernel
	 * faulted trying to copy_to_user() into address 0. Zero the counts
	 * for arrays we don't actually need back. */
	res.count_fbs = 0;
	res.count_encoders = 0;
	res.fb_id_ptr = 0;
	res.encoder_id_ptr = 0;

	uint32_t crtc_ids[8] = {0}, conn_ids[8] = {0};
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids;
	res.connector_id_ptr = (uint64_t)(uintptr_t)conn_ids;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) die("GETRESOURCES(ids)");

	uint32_t conn_id = 0, crtc_id = 0, enc_id = 0;
	struct drm_mode_modeinfo mode = {0};

	for (uint32_t i = 0; i < res.count_connectors; i++) {
		struct drm_mode_modeinfo modes[16] = {0};
		struct drm_mode_get_connector gc = {0};
		gc.connector_id = conn_ids[i];
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0) continue;
		printf("connector %u: type=%u conn=%u modes=%u enc=%u\n",
		       conn_ids[i], gc.connector_type, gc.connection,
		       gc.count_modes, gc.encoder_id);
		if (gc.connection != DRM_MODE_CONNECTED || gc.count_modes == 0)
			continue;
		if (gc.count_modes > 16) gc.count_modes = 16;
		gc.modes_ptr = (uint64_t)(uintptr_t)modes;
		gc.count_props = 0;
		gc.count_encoders = 0;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0) continue;
		conn_id = conn_ids[i];
		enc_id = gc.encoder_id;
		mode = modes[0];
		printf("picked connector %u, mode %ux%u@%uHz clock=%u\n",
		       conn_id, mode.hdisplay, mode.vdisplay, mode.vrefresh, mode.clock);
		break;
	}
	if (!conn_id) { fprintf(stderr, "no connected connector with modes found\n"); _exit(2); }

	struct drm_mode_get_encoder ge = {0};
	ge.encoder_id = enc_id;
	if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge) < 0) die("GETENCODER");
	crtc_id = ge.crtc_id ? ge.crtc_id : crtc_ids[0];
	printf("using crtc %u\n", crtc_id);

	struct drm_mode_create_dumb cd = {0};
	cd.width = mode.hdisplay;
	cd.height = mode.vdisplay;
	cd.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) die("CREATE_DUMB");
	printf("dumb buffer: handle=%u pitch=%u size=%llu\n",
	       cd.handle, cd.pitch, (unsigned long long)cd.size);

	struct drm_mode_fb_cmd fb = {0};
	fb.width = cd.width;
	fb.height = cd.height;
	fb.pitch = cd.pitch;
	fb.bpp = 32;
	fb.depth = 24;
	fb.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) die("ADDFB");
	printf("fb id=%u\n", fb.fb_id);

	struct drm_mode_map_dumb md = {0};
	md.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) die("MAP_DUMB");
	void *map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (map == MAP_FAILED) die("mmap");

	/* Solid bright red, XRGB8888 */
	for (uint32_t y = 0; y < cd.height; y++) {
		uint32_t *row = (uint32_t *)((char *)map + y * cd.pitch);
		for (uint32_t x = 0; x < cd.width; x++)
			row[x] = 0x00FF0000;
	}
	printf("framebuffer filled solid red\n");
	printf("readback right after fill: [0]=0x%08x [100]=0x%08x [last]=0x%08x\n",
	       ((uint32_t *)map)[0], ((uint32_t *)map)[100],
	       ((uint32_t *)map)[(cd.height - 1) * (cd.pitch / 4) + cd.width - 1]);

	/*
	 * Force an explicit disable before enabling. Theory: crtc_state->active
	 * got stuck true from the very first (panel-less, failed) commit
	 * attempt right after driver bind, so every subsequent commit -
	 * including this tool's own - sees no real off->on transition and
	 * DRM core silently skips calling .atomic_enable on the CRTC/encoder
	 * entirely, even though drm_atomic_commit() itself succeeds and
	 * mixer plane data gets committed. An explicit disable-then-enable
	 * pair is a known way to force a real transition through this exact
	 * class of stuck-state bug.
	 */
	struct drm_mode_crtc disable = {0};
	disable.crtc_id = crtc_id;
	printf("forcing explicit disable first...\n");
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &disable) < 0)
		perror("SETCRTC(disable) (continuing anyway)");
	usleep(200000);

	struct drm_mode_crtc sc = {0};
	sc.crtc_id = crtc_id;
	sc.fb_id = fb.fb_id;
	sc.x = 0;
	sc.y = 0;
	sc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
	sc.count_connectors = 1;
	sc.mode_valid = 1;
	sc.mode = mode;

	printf("calling SETCRTC...\n");
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc) < 0) {
		die("SETCRTC");
	}
	printf("SETCRTC returned success. Screen should be solid red now.\n");
	printf("sleeping 30s so you can look...\n");
	sleep(30);
	printf("readback after 30s: [0]=0x%08x [100]=0x%08x [last]=0x%08x\n",
	       ((uint32_t *)map)[0], ((uint32_t *)map)[100],
	       ((uint32_t *)map)[(cd.height - 1) * (cd.pitch / 4) + cd.width - 1]);
	return 0;
}
