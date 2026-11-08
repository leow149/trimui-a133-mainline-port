// Transfer-path test suite for the PowerVR Vulkan driver: buffer<->image round trips, image copies,
// scaled blits (nearest), clears and depth/stencil aspect copies, each checked against a CPU reference.
// Every case runs in a forked child so a driver assertion/crash only fails that case.
// Usage: vkxfer [filter-substring]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static VkCommandPool pool;

static void init(void)
{
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &q);
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	exit(3);
}

struct buf { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize sz; };
static struct buf mkbuf(VkDeviceSize sz)
{
	struct buf r = { .sz = sz };
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	CHECK(vkCreateBuffer(dev, &bci, NULL, &r.b));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, r.b, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
					    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	CHECK(vkAllocateMemory(dev, &mai, NULL, &r.m));
	CHECK(vkBindBufferMemory(dev, r.b, r.m, 0));
	CHECK(vkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.p));
	return r;
}

static VkImage mkimg(VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags extra)
{
	VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = f, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra };
	VkImage img;
	CHECK(vkCreateImage(dev, &ci, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) };
	VkDeviceMemory m;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &m));
	CHECK(vkBindImageMemory(dev, img, m, 0));
	return img;
}

static VkCommandBuffer begin(void)
{
	VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK(vkBeginCommandBuffer(cb, &bi));
	return cb;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags asp, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
		VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = from, .newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img, .subresourceRange = { asp, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void submit(VkCommandBuffer cb)
{
	CHECK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	VkFence f;
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	CHECK(vkCreateFence(dev, &fci, NULL, &f));
	CHECK(vkQueueSubmit(q, 1, &si, f));
	CHECK(vkWaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull));
}

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
	for (size_t i = 0; i < n; i++) {
		seed = seed * 1103515245u + 12345u;
		p[i] = seed >> 16;
	}
}

static int cmp(const uint8_t *a, const uint8_t *b, size_t n, const char *what)
{
	for (size_t i = 0; i < n; i++)
		if (a[i] != b[i]) {
			printf("  %s: first mismatch at byte %zu: got %02x want %02x\n", what, i, a[i], b[i]);
			return 1;
		}
	return 0;
}

/* buffer -> image -> buffer */
static int t_roundtrip(VkFormat f, uint32_t bpp, uint32_t w, uint32_t h)
{
	size_t sz = (size_t)w * h * bpp;
	struct buf src = mkbuf(sz), dst = mkbuf(sz);
	fill(src.p, sz, w * 131 + h);
	memset(dst.p, 0xcd, sz);
	VkImage img = mkimg(f, w, h, 0);
	VkCommandBuffer cb = begin();
	barrier(cb, img, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy r = { .imageSubresource = { 1, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
	vkCmdCopyBufferToImage(cb, src.b, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	barrier(cb, img, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &r);
	submit(cb);
	return cmp(dst.p, src.p, sz, "roundtrip");
}

/* buffer -> image A -> (sub-rect copy) image B -> buffer */
static int t_imgcopy(VkFormat f, uint32_t bpp, uint32_t w, uint32_t h)
{
	size_t sz = (size_t)w * h * bpp;
	struct buf src = mkbuf(sz), dst = mkbuf(sz);
	fill(src.p, sz, w * 7 + h * 3);
	memset(dst.p, 0, sz);
	VkImage a = mkimg(f, w, h, 0), b = mkimg(f, w, h, 0);
	VkCommandBuffer cb = begin();
	barrier(cb, a, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	barrier(cb, b, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy r = { .imageSubresource = { 1, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
	vkCmdCopyBufferToImage(cb, src.b, a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	vkCmdCopyBufferToImage(cb, dst.b, b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);   /* zero B */
	barrier(cb, a, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	barrier(cb, b, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	uint32_t ox = w / 3, oy = h / 3, cw = w - ox, ch = h - oy;
	VkImageCopy ic = { .srcSubresource = { 1, 0, 0, 1 }, .dstSubresource = { 1, 0, 0, 1 },
		.srcOffset = { 0, 0, 0 }, .dstOffset = { (int32_t)ox, (int32_t)oy, 0 }, .extent = { cw, ch, 1 } };
	vkCmdCopyImage(cb, a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
	barrier(cb, b, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdCopyImageToBuffer(cb, b, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &r);
	submit(cb);
	uint8_t *ref = calloc(1, sz);
	for (uint32_t y = 0; y < ch; y++)
		memcpy(ref + ((size_t)(y + oy) * w + ox) * bpp, (uint8_t *)src.p + (size_t)y * w * bpp, (size_t)cw * bpp);
	return cmp(dst.p, ref, sz, "imgcopy");
}

/* scaled blit, nearest: RGBA8 sw x sh -> dw x dh */
static int t_blit(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
	VkFormat f = VK_FORMAT_R8G8B8A8_UNORM;
	size_t ssz = (size_t)sw * sh * 4, dsz = (size_t)dw * dh * 4;
	struct buf src = mkbuf(ssz), dst = mkbuf(dsz);
	fill(src.p, ssz, sw + dw);
	memset(dst.p, 0xcd, dsz);
	VkImage a = mkimg(f, sw, sh, 0), b = mkimg(f, dw, dh, 0);
	VkCommandBuffer cb = begin();
	barrier(cb, a, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	barrier(cb, b, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy r = { .imageSubresource = { 1, 0, 0, 1 }, .imageExtent = { sw, sh, 1 } };
	vkCmdCopyBufferToImage(cb, src.b, a, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	barrier(cb, a, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkImageBlit bl = { .srcSubresource = { 1, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { (int32_t)sw, (int32_t)sh, 1 } },
		.dstSubresource = { 1, 0, 0, 1 }, .dstOffsets = { { 0, 0, 0 }, { (int32_t)dw, (int32_t)dh, 1 } } };
	vkCmdBlitImage(cb, a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
		       VK_FILTER_NEAREST);
	barrier(cb, b, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy r2 = { .imageSubresource = { 1, 0, 0, 1 }, .imageExtent = { dw, dh, 1 } };
	vkCmdCopyImageToBuffer(cb, b, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &r2);
	submit(cb);
	/* nearest: dst pixel centre maps to src floor((x+0.5)*sw/dw); allow exact-boundary ambiguity */
	int bad = 0;
	for (uint32_t y = 0; y < dh && bad < 3; y++)
		for (uint32_t x = 0; x < dw && bad < 3; x++) {
			double fx = (x + 0.5) * sw / dw, fy = (y + 0.5) * sh / dh;
			uint32_t sx = (uint32_t)fx, sy = (uint32_t)fy;
			const uint8_t *g = (uint8_t *)dst.p + ((size_t)y * dw + x) * 4;
			const uint8_t *e = (uint8_t *)src.p + ((size_t)sy * sw + sx) * 4;
			if (memcmp(g, e, 4)) {
				int edge = (fx - sx < 1e-6 && sx) || (fy - sy < 1e-6 && sy);
				if (!edge) {
					printf("  blit: (%u,%u) got %02x%02x%02x%02x want %02x%02x%02x%02x\n", x, y,
					       g[0], g[1], g[2], g[3], e[0], e[1], e[2], e[3]);
					bad++;
				}
			}
		}
	return bad != 0;
}

/* clear colour image */
static int t_clear(VkFormat f, uint32_t w, uint32_t h)
{
	size_t sz = (size_t)w * h * 4;
	struct buf dst = mkbuf(sz);
	memset(dst.p, 0xcd, sz);
	VkImage img = mkimg(f, w, h, 0);
	VkCommandBuffer cb = begin();
	barrier(cb, img, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkClearColorValue cv = { .uint32 = { 0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00 } };
	if (f == VK_FORMAT_R8G8B8A8_UNORM) {
		cv.float32[0] = 1.0f; cv.float32[1] = 0.0f; cv.float32[2] = 0.5f; cv.float32[3] = 1.0f;
	}
	VkImageSubresourceRange rr = { 1, 0, 1, 0, 1 };
	vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rr);
	barrier(cb, img, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy r = { .imageSubresource = { 1, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.b, 1, &r);
	submit(cb);
	uint8_t want[4];
	if (f == VK_FORMAT_R8G8B8A8_UNORM) { want[0] = 255; want[1] = 0; want[2] = 128; want[3] = 255; }
	else memcpy(want, &cv.uint32[0], 4);
	for (size_t i = 0; i < (size_t)w * h; i++)
		if (memcmp((uint8_t *)dst.p + i * 4, want, 4) &&
		    !(f == VK_FORMAT_R8G8B8A8_UNORM && ((uint8_t *)dst.p)[i * 4 + 2] == 127)) {
			uint8_t *g = (uint8_t *)dst.p + i * 4;
			printf("  clear: px %zu got %02x%02x%02x%02x\n", i, g[0], g[1], g[2], g[3]);
			return 1;
		}
	return 0;
}

/* depth/stencil: D24S8 / D32S8 depth aspect and stencil aspect round trips separately */
static int t_ds(VkFormat f, uint32_t w, uint32_t h)
{
	int dbpp = 4;
	size_t dsz = (size_t)w * h * dbpp, ssz = (size_t)w * h;
	struct buf d = mkbuf(dsz), s = mkbuf(ssz), d2 = mkbuf(dsz), s2 = mkbuf(ssz);
	fill(d.p, dsz, 77); fill(s.p, ssz, 99);
	if (f == VK_FORMAT_D24_UNORM_S8_UINT)
		for (size_t i = 0; i < (size_t)w * h; i++) ((uint8_t *)d.p)[i * 4 + 3] = 0;  /* x8 */
	if (f == VK_FORMAT_D32_SFLOAT_S8_UINT)
		for (size_t i = 0; i < (size_t)w * h; i++) ((float *)d.p)[i] = (float)(i % 1000) / 1000.0f;
	VkImage img = mkimg(f, w, h, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	VkCommandBuffer cb = begin();
	barrier(cb, img, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy rd = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
	VkBufferImageCopy rs = { .imageSubresource = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
	vkCmdCopyBufferToImage(cb, d.b, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rd);
	vkCmdCopyBufferToImage(cb, s.b, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rs);
	barrier(cb, img, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d2.b, 1, &rd);
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s2.b, 1, &rs);
	submit(cb);
	int bad = 0;
	if (f == VK_FORMAT_D24_UNORM_S8_UINT) {
		for (size_t i = 0; i < (size_t)w * h && !bad; i++)
			if (memcmp((uint8_t *)d2.p + i * 4, (uint8_t *)d.p + i * 4, 3)) {
				printf("  ds depth: px %zu\n", i); bad = 1;
			}
	} else bad |= cmp(d2.p, d.p, dsz, "ds depth");
	bad |= cmp(s2.p, s.p, ssz, "ds stencil");
	return bad;
}

struct tcase { char name[64]; int kind; VkFormat f; uint32_t a, b, c, d; };

int main(int argc, char **argv)
{
	const char *filt = argc > 1 ? argv[1] : NULL;
	static struct tcase t[256];
	int n = 0;
	struct { VkFormat f; uint32_t bpp; const char *nm; } fm[] = {
		{ VK_FORMAT_R8G8B8A8_UNORM, 4, "rgba8" }, { VK_FORMAT_R32_UINT, 4, "r32ui" },
		{ VK_FORMAT_R8_UNORM, 1, "r8" }, { VK_FORMAT_R16G16_SFLOAT, 4, "rg16f" },
		{ VK_FORMAT_R32G32B32A32_SFLOAT, 16, "rgba32f" }, { VK_FORMAT_R5G6B5_UNORM_PACK16, 2, "565" } };
	uint32_t sizes[][2] = { { 1, 1 }, { 3, 1 }, { 159, 1 }, { 1, 37 }, { 64, 64 }, { 300, 7 }, { 1000, 3 }, { 17, 13 }, { 720, 1280 } };
	for (unsigned i = 0; i < sizeof(fm) / sizeof(fm[0]); i++)
		for (unsigned j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
			snprintf(t[n].name, 64, "rt_%s_%ux%u", fm[i].nm, sizes[j][0], sizes[j][1]);
			t[n].kind = 0; t[n].f = fm[i].f; t[n].a = fm[i].bpp; t[n].b = sizes[j][0]; t[n].c = sizes[j][1]; n++;
			snprintf(t[n].name, 64, "cp_%s_%ux%u", fm[i].nm, sizes[j][0], sizes[j][1]);
			t[n].kind = 1; t[n].f = fm[i].f; t[n].a = fm[i].bpp; t[n].b = sizes[j][0]; t[n].c = sizes[j][1]; n++;
		}
	uint32_t bl[][4] = { { 64, 64, 37, 29 }, { 240, 160, 1080, 720 }, { 256, 224, 940, 720 }, { 100, 1, 300, 1 }, { 720, 1280, 360, 640 } };
	for (unsigned j = 0; j < 5; j++) {
		snprintf(t[n].name, 64, "blit_%ux%u_to_%ux%u", bl[j][0], bl[j][1], bl[j][2], bl[j][3]);
		t[n].kind = 2; t[n].a = bl[j][0]; t[n].b = bl[j][1]; t[n].c = bl[j][2]; t[n].d = bl[j][3]; n++;
	}
	uint32_t cs[][2] = { { 3, 1 }, { 64, 64 }, { 300, 7 } };
	for (unsigned j = 0; j < 3; j++) {
		snprintf(t[n].name, 64, "clear_rgba8_%ux%u", cs[j][0], cs[j][1]);
		t[n].kind = 3; t[n].f = VK_FORMAT_R8G8B8A8_UNORM; t[n].b = cs[j][0]; t[n].c = cs[j][1]; n++;
		snprintf(t[n].name, 64, "clear_r32ui_%ux%u", cs[j][0], cs[j][1]);
		t[n].kind = 3; t[n].f = VK_FORMAT_R32_UINT; t[n].b = cs[j][0]; t[n].c = cs[j][1]; n++;
	}
	VkFormat dsf[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };
	const char *dsn[] = { "d24s8", "d32s8" };
	for (unsigned i = 0; i < 2; i++)
		for (unsigned j = 0; j < 3; j++) {
			snprintf(t[n].name, 64, "ds_%s_%ux%u", dsn[i], cs[j][0], cs[j][1]);
			t[n].kind = 4; t[n].f = dsf[i]; t[n].b = cs[j][0]; t[n].c = cs[j][1]; n++;
		}

	int pass = 0, fail = 0, crash = 0;
	for (int i = 0; i < n; i++) {
		if (filt && !strstr(t[i].name, filt))
			continue;
		fflush(stdout);
		pid_t p = fork();
		if (!p) {
			init();
			int r = 0;
			switch (t[i].kind) {
			case 0: r = t_roundtrip(t[i].f, t[i].a, t[i].b, t[i].c); break;
			case 1: r = t_imgcopy(t[i].f, t[i].a, t[i].b, t[i].c); break;
			case 2: r = t_blit(t[i].a, t[i].b, t[i].c, t[i].d); break;
			case 3: r = t_clear(t[i].f, t[i].b, t[i].c); break;
			case 4: r = t_ds(t[i].f, t[i].b, t[i].c); break;
			}
			fflush(stdout);
			_exit(r);
		}
		int st;
		waitpid(p, &st, 0);
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { pass++; printf("PASS  %s\n", t[i].name); }
		else if (WIFEXITED(st)) { fail++; printf("FAIL  %s (exit %d)\n", t[i].name, WEXITSTATUS(st)); }
		else { crash++; printf("CRASH %s (signal %d)\n", t[i].name, WTERMSIG(st)); }
	}
	printf("TOTAL pass=%d fail=%d crash=%d\n", pass, fail, crash);
	return fail || crash;
}
