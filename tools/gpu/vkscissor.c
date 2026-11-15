// Dynamic scissor/viewport test for the PowerVR Vulkan driver (region clip handling).
// Each scenario is a list of full-viewport triangle draws with their own viewport, scissor and
// color, optionally split across render passes (loadOp LOAD) or separate submits. The expected
// image is computed on the CPU (each draw paints viewport ∩ scissor); any difference is reported
// with a coarse 16x16-block map (X = wrong).
// Usage: vkscissor [filter-substring]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "ra_depth_vert.h"
#include "ra_pc_frag.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

#define W 256
#define H 256

enum { DRAW, NEWPASS, NEWSUBMIT, END };
struct step { int op; VkRect2D vp, sc; uint32_t color; };

#define FULL { { 0, 0 }, { W, H } }
#define SMALL { { 96, 112 }, { 48, 32 } }
#define TOPLEFT { { 0, 0 }, { 64, 64 } }
#define RED   0xff0000ffu
#define GREEN 0xff00ff00u
#define BLUE  0xffff0000u

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static VkCommandPool pool;
static VkImage img;
static VkImageView view;
static VkRenderPass rp_clear, rp_load;
static VkFramebuffer fb_clear, fb_load;
static VkPipelineLayout layout;
static VkPipeline pipe;
static VkBuffer rbuf;
static VkDeviceMemory rmem;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	exit(2);
}

static VkRenderPass make_rp(VkAttachmentLoadOp load, VkImageLayout initial)
{
	VkAttachmentDescription ad = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = initial, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &ar };
	VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 };
	VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd,
		.dependencyCount = 1, .pDependencies = &dep };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rci, NULL, &rp));
	return rp;
}

static void setup(void)
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
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));

	VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
	CHECK(vkCreateImage(dev, &ii, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory im;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &im));
	CHECK(vkBindImageMemory(dev, img, im, 0));
	VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	CHECK(vkCreateImageView(dev, &vi, NULL, &view));
	rp_clear = make_rp(VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED);
	rp_load = make_rp(VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
	fci.renderPass = rp_clear;
	CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb_clear));
	fci.renderPass = rp_load;
	CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb_load));

	VkShaderModule vs, fs;
	VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(ra_depth_vert), .pCode = ra_depth_vert };
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs));
	smi.codeSize = sizeof(ra_pc_frag);
	smi.pCode = ra_pc_frag;
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs));
	VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &layout));
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
	VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .scissorCount = 1 };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2, .pDynamicStates = dyn };
	VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = st, .pVertexInputState = &vis, .pInputAssemblyState = &ia,
		.pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
		.pColorBlendState = &cbs, .pDynamicState = &dsi, .layout = layout, .renderPass = rp_load };
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe));

	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	CHECK(vkCreateBuffer(dev, &bci, NULL, &rbuf));
	vkGetBufferMemoryRequirements(dev, rbuf, &mr);
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = mem_type(mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CHECK(vkAllocateMemory(dev, &mai, NULL, &rmem));
	CHECK(vkBindBufferMemory(dev, rbuf, rmem, 0));
}

static VkCommandBuffer begin_cmd(void)
{
	VkCommandBuffer cb;
	VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK(vkBeginCommandBuffer(cb, &bi));
	return cb;
}

static void submit(VkCommandBuffer cb)
{
	CHECK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(q));
	vkFreeCommandBuffers(dev, pool, 1, &cb);
}

static void begin_load_pass(VkCommandBuffer cb)
{
	VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp_load,
		.framebuffer = fb_load, .renderArea = FULL };
	vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
}

static int inside(VkRect2D r, int x, int y)
{
	return x >= r.offset.x && y >= r.offset.y &&
		x < r.offset.x + (int)r.extent.width && y < r.offset.y + (int)r.extent.height;
}

static int run(const char *name, const struct step *s)
{
	static uint32_t want[W * H];
	for (int i = 0; i < W * H; i++)
		want[i] = RED;

	/* clear to red in its own pass, then the scenario in load passes */
	VkCommandBuffer cb = begin_cmd();
	VkClearValue cv = { .color = { .float32 = { 1, 0, 0, 1 } } };
	VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp_clear,
		.framebuffer = fb_clear, .renderArea = FULL, .clearValueCount = 1, .pClearValues = &cv };
	vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(cb);
	begin_load_pass(cb);
	for (; s->op != END; s++) {
		if (s->op == NEWPASS || s->op == NEWSUBMIT) {
			vkCmdEndRenderPass(cb);
			if (s->op == NEWSUBMIT) {
				submit(cb);
				cb = begin_cmd();
			}
			begin_load_pass(cb);
			continue;
		}
		VkViewport vp = { s->vp.offset.x, s->vp.offset.y, s->vp.extent.width, s->vp.extent.height, 0, 1 };
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &s->sc);
		float c[4] = { (s->color & 0xff) / 255.0f, ((s->color >> 8) & 0xff) / 255.0f,
			((s->color >> 16) & 0xff) / 255.0f, 1 };
		vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c);
		vkCmdDraw(cb, 3, 1, 0, 0);
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++)
				if (inside(s->vp, x, y) && inside(s->sc, x, y))
					want[y * W + x] = s->color;
	}
	vkCmdEndRenderPass(cb);

	VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 1, &ib);
	VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &c);
	VkBufferMemoryBarrier bb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = rbuf, .size = VK_WHOLE_SIZE };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
	submit(cb);

	uint32_t *px;
	CHECK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
	int bad = 0;
	char map[H / 16][W / 16 + 1];
	memset(map, '.', sizeof(map));
	for (int by = 0; by < H / 16; by++)
		map[by][W / 16] = 0;
	for (int i = 0; i < W * H; i++)
		if (px[i] != want[i]) {
			bad++;
			map[(i / W) / 16][(i % W) / 16] = px[i] == RED ? 'R' : px[i] == GREEN ? 'G' : px[i] == BLUE ? 'B' : 'X';
		}
	vkUnmapMemory(dev, rmem);
	printf("%-40s %s  wrong=%d\n", name, bad ? "FAIL" : "ok  ", bad);
	if (bad)
		for (int by = 0; by < H / 16; by++)
			printf("    %s\n", map[by]);
	return bad;
}

#define D(v, s, c) { DRAW, v, s, c }
#define P { NEWPASS }
#define S { NEWSUBMIT }
#define E { END }

int main(int argc, char **argv)
{
	const char *filter = argc > 1 ? argv[1] : NULL;
	setup();
	struct { const char *name; struct step steps[8]; } sc[] = {
		{ "s1_full_then_small",        { D(FULL, FULL, GREEN), D(FULL, SMALL, BLUE), E } },
		{ "s2_small_then_full",        { D(FULL, SMALL, BLUE), D(FULL, FULL, GREEN), E } },
		{ "s3_pass_small|small,full",  { D(FULL, SMALL, BLUE), P, D(FULL, SMALL, BLUE), D(FULL, FULL, GREEN), E } },
		{ "s3b_pass_small|full",       { D(FULL, SMALL, BLUE), P, D(FULL, FULL, GREEN), E } },
		{ "s3c_submit_small|small,full", { D(FULL, SMALL, BLUE), S, D(FULL, SMALL, BLUE), D(FULL, FULL, GREEN), E } },
		{ "s3d_submit_small|full",     { D(FULL, SMALL, BLUE), S, D(FULL, FULL, GREEN), E } },
		{ "s4_smallvp_then_full",      { D(SMALL, FULL, BLUE), D(FULL, FULL, GREEN), E } },
		{ "s5_pass_smallvp|smallvp,full", { D(SMALL, FULL, BLUE), P, D(SMALL, FULL, BLUE), D(FULL, FULL, GREEN), E } },
		{ "s6_corner_then_full",       { D(TOPLEFT, TOPLEFT, BLUE), D(FULL, FULL, GREEN), E } },
		{ "s7_full_small_full",        { D(FULL, FULL, GREEN), D(FULL, SMALL, BLUE), D(FULL, FULL, GREEN), E } },
	};
	int fails = 0;
	for (unsigned i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) {
		if (filter && !strstr(sc[i].name, filter))
			continue;
		fails += run(sc[i].name, sc[i].steps) != 0;
	}
	printf("%d failing case(s)\n", fails);
	return fails ? 1 : 0;
}
