// Sample one pixel column (x) of the framebuffer shown on a plane, many times,
// and count background/non-background transitions along it (vkcube background
// is 0x333333). colgrab <card> <plane> <x> <samples>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int fd = open(argv[1], O_RDWR);
	unsigned plane = atoi(argv[2]), x = atoi(argv[3]), n = atoi(argv[4]);
	unsigned hist[16] = { 0 };
	struct { uint32_t fb; uint8_t *map; uint32_t pitch, h; } cache[8];
	int nc = 0;
	for (unsigned s = 0; s < n; s++) {
		struct drm_mode_get_plane gp = { .plane_id = plane };
		ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &gp);
		int c;
		for (c = 0; c < nc && cache[c].fb != gp.fb_id; c++)
			;
		if (c == nc) {
			struct drm_mode_fb_cmd2 fb = { .fb_id = gp.fb_id };
			if (ioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb)) { perror("GETFB2"); return 1; }
			struct drm_mode_map_dumb md = { .handle = fb.handles[0] };
			ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
			cache[c].map = mmap(NULL, (size_t)fb.pitches[0] * fb.height, PROT_READ,
					    MAP_SHARED, fd, md.offset);
			cache[c].fb = gp.fb_id; cache[c].pitch = fb.pitches[0]; cache[c].h = fb.height;
			nc++;
		}
		int prev = 0, trans = 0;
		for (uint32_t y = 0; y < cache[c].h; y++) {
			uint32_t px = *(uint32_t *)(cache[c].map + y * cache[c].pitch + x * 4) & 0xffffff;
			int fg = px != 0x333333;
			if (y > 0 && fg != prev)
				trans++;
			prev = fg;
		}
		hist[trans < 15 ? trans : 15]++;
		usleep(3000);
	}
	printf("transitions per column sample (count): ");
	for (int i = 0; i < 16; i++)
		if (hist[i])
			printf("%d:%u ", i, hist[i]);
	printf("\n");
	return 0;
}
