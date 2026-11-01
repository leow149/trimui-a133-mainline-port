// Save the framebuffer currently shown on a DRM plane: fbgrab <card> <plane_id> <out> [count] [interval_ms]
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int fd = open(argv[1], O_RDWR);
	unsigned plane_id = atoi(argv[2]);
	int count = argc > 4 ? atoi(argv[4]) : 1, ms = argc > 5 ? atoi(argv[5]) : 0;
	if (fd < 0) { perror("open"); return 1; }
	for (int n = 0; n < count; n++) {
		struct drm_mode_get_plane gp = { .plane_id = plane_id };
		if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp)) { perror("GETPLANE"); return 1; }
		struct drm_mode_fb_cmd2 fb = { .fb_id = gp.fb_id };
		if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb)) { perror("GETFB2"); return 1; }
		struct drm_mode_map_dumb md = { .handle = fb.handles[0] };
		if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("MAP_DUMB"); return 1; }
		size_t sz = (size_t)fb.pitches[0] * fb.height;
		void *p = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, md.offset);
		if (p == MAP_FAILED) { perror("mmap"); return 1; }
		char name[256];
		snprintf(name, sizeof(name), "%s.%d.raw", argv[3], n);
		FILE *f = fopen(name, "wb");
		fwrite(p, 1, sz, f);
		fclose(f);
		munmap(p, sz);
		struct drm_gem_close gc = { .handle = fb.handles[0] };
		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
		printf("%s fb=%u %ux%u pitch %u\n", name, gp.fb_id, fb.width, fb.height, fb.pitches[0]);
		usleep(ms * 1000);
	}
	return 0;
}
