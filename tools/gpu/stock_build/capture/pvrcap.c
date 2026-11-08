// libpvrcap.so: LD_PRELOAD capture of GPU (DRM / pvrsrvkm) mmaps and ioctls. See README.md for the log format.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>

#define MAXMAPS 4096
#define PTRMAX 4096
#define PROBE 256
#define MAXL1 64
#define MAXL2 256

struct map { void *addr; size_t len; off_t off; int fd; char path[64]; };
static struct map maps[MAXMAPS];
static volatile int mlock_, llock_;
static char dir[256] = "/tmp/pvrcap";
static int logfd = -1, pfd[2] = { -1, -1 };
static uint64_t seq;
static int inited;

static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static void *(*real_mmap64)(void *, size_t, int, int, int, off_t);
static int (*real_munmap)(void *, size_t);
static int (*real_ioctl)(int, unsigned long, void *);

static void lock(volatile int *l) { while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) usleep(0); }
static void unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

static void init(void)
{
	if (inited) return;
	inited = 1;
	real_mmap = dlsym(RTLD_NEXT, "mmap");
	real_mmap64 = dlsym(RTLD_NEXT, "mmap64");
	if (!real_mmap64) real_mmap64 = real_mmap;
	real_munmap = dlsym(RTLD_NEXT, "munmap");
	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	const char *d = getenv("PVRCAP_DIR");
	if (d && *d && strlen(d) < sizeof(dir) - 64) strcpy(dir, d);
	mkdir(dir, 0777);
	if (pipe2(pfd, O_NONBLOCK) == 0) fcntl(pfd[1], 1031 /*F_SETPIPE_SZ*/, 1 << 20);
	char p[300];
	snprintf(p, sizeof p, "%s/ioctl.log", dir);
	logfd = open(p, O_WRONLY | O_CREAT | O_APPEND, 0666);
}

__attribute__((constructor)) static void ctor(void) { init(); }

// Copy n bytes from possibly-bad user pointer src; returns bytes copied (0 on fault at first byte).
static size_t safe_copy(void *dst, const void *src, size_t n)
{
	if (pfd[0] < 0) return 0;
	size_t done = 0;
	while (done < n) {
		size_t c = n - done > 65536 ? 65536 : n - done;
		ssize_t w = write(pfd[1], (const char *)src + done, c);
		if (w <= 0) break;
		size_t got = 0;
		while (got < (size_t)w) {
			ssize_t r = read(pfd[0], (char *)dst + done + got, w - got);
			if (r <= 0) break;
			got += r;
		}
		done += got;
		if ((size_t)w < c) break;
	}
	return done;
}

static int gpu_path(int fd, char *out, size_t sz)
{
	char l[40], b[sz];
	if (fd < 0) return 0;
	snprintf(l, sizeof l, "/proc/self/fd/%d", fd);
	ssize_t n = readlink(l, b, sz - 1);
	if (n <= 0) return 0;
	b[n] = 0;
	if (strncmp(b, "/dev/dri/", 9) && strcmp(b, "/dev/pvrsrvkm")) return 0;
	strcpy(out, b);
	return 1;
}

// ---- log: record = 56-byte header (7 x u64, little endian) + len bytes ----
// magic 'PVRCAPv1', kind, seq, fd, req(or ptr), aux(or off in parent), len ; see README
static void rec(uint64_t kind, uint64_t sq, int fd, uint64_t a, uint64_t b, const void *data, uint64_t len)
{
	if (logfd < 0) return;
	uint64_t h[7] = { 0x31765041435256ull /* "VRCAPv1" */, kind, sq, (uint64_t)(int64_t)fd, a, b, len };
	struct iovec v[2] = { { h, sizeof h }, { (void *)data, len } };
	ssize_t r = writev(logfd, v, len ? 2 : 1);
	(void)r;
}

static int looks_ptr(uint64_t v) { return v >= 0x10000 && v < (1ull << 48); }

// kinds: 1 ioctl-in struct, 2 ioctl-out struct, 3 L1 ptr buf (in), 4 L2 ptr buf (in), 5 L1 (out), 6 L2 (out)
static void log_ioctl(int fd, unsigned long req, void *arg, int out, uint64_t sq, long ret, int err)
{
	size_t sz = _IOC_SIZE(req);
	if (sz > 4096) sz = 4096;
	uint8_t s[4096], c[4096];
	size_t got = sz && arg ? safe_copy(s, arg, sz) : 0;
	lock(&llock_);
	rec(out ? 2 : 1, sq, fd, req, out ? (uint64_t)(int64_t)(ret < 0 ? -err : ret) : 0, s, got);
	int b1 = MAXL1, b2 = MAXL2;
	uint8_t probe[8];
	for (size_t off = 0; off + 8 <= got && b1 > 0; off += 8) {
		uint64_t v;
		memcpy(&v, s + off, 8);
		if (!looks_ptr(v) || safe_copy(probe, (void *)(uintptr_t)v, 8) != 8) continue;
		size_t g = safe_copy(c, (void *)(uintptr_t)v, 4096);
		if (!g) continue;
		b1--;
		rec(out ? 5 : 3, sq, fd, v, off, c, g);
		for (size_t o2 = 0; o2 + 8 <= g && b2 > 0; o2 += 8) {
			uint64_t w;
			memcpy(&w, c + o2, 8);
			if (!looks_ptr(w) || safe_copy(probe, (void *)(uintptr_t)w, 8) != 8) continue;
			uint8_t d[4096];
			size_t g2 = safe_copy(d, (void *)(uintptr_t)w, 4096);
			if (!g2) continue;
			b2--;
			rec(out ? 6 : 4, sq, fd, w, ((uint64_t)off << 32) | o2, d, g2);
		}
	}
	unlock(&llock_);
}

int ioctl(int fd, unsigned long req, ...)
{
	init();
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	char path[64];
	int gpu = gpu_path(fd, path, sizeof path);
	uint64_t sq = 0;
	if (gpu) {
		sq = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
		log_ioctl(fd, req, arg, 0, sq, 0, 0);
	}
	long r = real_ioctl(fd, req, arg);
	int e = errno;
	if (gpu) {
		log_ioctl(fd, req, arg, 1, sq, r, e);
		errno = e;
	}
	return r;
}

static void add_map(void *a, size_t len, off_t off, int fd, const char *path)
{
	lock(&mlock_);
	for (int i = 0; i < MAXMAPS; i++)
		if (!maps[i].addr) {
			maps[i].addr = a; maps[i].len = len; maps[i].off = off; maps[i].fd = fd;
			strcpy(maps[i].path, path);
			break;
		}
	unlock(&mlock_);
}

void *mmap(void *a, size_t len, int prot, int flags, int fd, off_t off)
{
	init();
	void *r = real_mmap(a, len, prot, flags, fd, off);
	char path[64];
	if (r != MAP_FAILED && !(flags & MAP_ANONYMOUS)) {
		int e = errno;
		if (gpu_path(fd, path, sizeof path)) add_map(r, len, off, fd, path);
		errno = e;
	}
	return r;
}

void *mmap64(void *a, size_t len, int prot, int flags, int fd, off_t off)
{
	init();
	void *r = real_mmap64(a, len, prot, flags, fd, off);
	char path[64];
	if (r != MAP_FAILED && !(flags & MAP_ANONYMOUS)) {
		int e = errno;
		if (gpu_path(fd, path, sizeof path)) add_map(r, len, off, fd, path);
		errno = e;
	}
	return r;
}

int munmap(void *a, size_t len)
{
	init();
	int r = real_munmap(a, len);
	if (r == 0) {
		uintptr_t s = (uintptr_t)a, e = s + len;
		lock(&mlock_);
		for (int i = 0; i < MAXMAPS; i++) {
			if (!maps[i].addr) continue;
			uintptr_t ms = (uintptr_t)maps[i].addr, me = ms + maps[i].len;
			if (me <= s || ms >= e) continue;
			if (ms >= s && me <= e) maps[i].addr = NULL;
			else if (ms >= s) { maps[i].off += e - ms; maps[i].len = me - e; maps[i].addr = (void *)e; }
			else if (me <= e) maps[i].len = s - ms;
			// middle punch: keep the head only
			else maps[i].len = s - ms;
		}
		unlock(&mlock_);
	}
	return r;
}

void pvrcap_dump_now(const char *tag)
{
	init();
	char p[400];
	if (!tag || !*tag) tag = "dump";
	snprintf(p, sizeof p, "%s/%s_maps.txt", dir, tag);
	FILE *idx = fopen(p, "w");
	static struct map snap[MAXMAPS];
	lock(&mlock_);
	memcpy(snap, maps, sizeof maps);
	unlock(&mlock_);
	/* The vendor library maps GPU memory without going through our mmap hook:
	 * take every /dev/dri mapping from /proc/self/maps instead. */
	{
		FILE *mf = fopen("/proc/self/maps", "r");
		char line[512];
		int n = 0;
		memset(snap, 0, sizeof snap);
		while (mf && fgets(line, sizeof line, mf) && n < MAXMAPS) {
			/* "a0-a1 perm off dev ino path" (no sscanf: it needs glibc >= 2.38 here) */
			unsigned long long a0 = 0, a1 = 0, off = 0;
			const char *c = line, *perm, *path;
			for (; (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'); c++)
				a0 = a0 * 16 + (*c <= '9' ? *c - '0' : *c - 'a' + 10);
			if (*c++ != '-') continue;
			for (; (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'); c++)
				a1 = a1 * 16 + (*c <= '9' ? *c - '0' : *c - 'a' + 10);
			perm = ++c;
			while (*c && *c != ' ') c++;
			c++;
			for (; (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'); c++)
				off = off * 16 + (*c <= '9' ? *c - '0' : *c - 'a' + 10);
			path = strchr(line, '/');
			if (!path || strncmp(path, "/dev/dri/", 9) || perm[0] != 'r')
				continue;
			char *nl = strchr(path, '\n');
			if (nl) *nl = 0;
			snap[n].addr = (void *)(uintptr_t)a0;
			snap[n].len = a1 - a0;
			snap[n].off = off;
			snprintf(snap[n].path, sizeof snap[n].path, "%s", path);
			n++;
		}
		if (mf) fclose(mf);
	}
	if (idx) fprintf(idx, "# index cpu_addr length mmap_offset device bytes_readable\n");
	static uint8_t buf[65536];
	for (int i = 0; i < MAXMAPS; i++) {
		if (!snap[i].addr) continue;
		snprintf(p, sizeof p, "%s/%s_map_%d.bin", dir, tag, i);
		int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0) continue;
		size_t ok = 0, pos = 0;
		while (pos < snap[i].len) {
			size_t c = snap[i].len - pos > sizeof buf ? sizeof buf : snap[i].len - pos;
			size_t g = safe_copy(buf, (char *)snap[i].addr + pos, c);
			if (g) { if (write(fd, buf, g) < 0) break; ok += g; pos += g; }
			if (g < c) { // fault: zero-fill up to next page and continue
				size_t pg = 4096 - (((uintptr_t)snap[i].addr + pos) & 4095);
				if (pg > snap[i].len - pos) pg = snap[i].len - pos;
				memset(buf, 0, pg);
				if (write(fd, buf, pg) < 0) break;
				pos += pg;
			}
		}
		close(fd);
		if (idx) fprintf(idx, "%d 0x%llx %zu 0x%llx %s %zu\n", i, (unsigned long long)(uintptr_t)snap[i].addr,
				 snap[i].len, (unsigned long long)snap[i].off, snap[i].path, ok);
	}
	if (idx) fclose(idx);
}
