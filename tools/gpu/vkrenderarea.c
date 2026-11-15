// renderArea preservation test for the PowerVR Vulkan driver.
// Pass 1 clears a 256x256 RGBA8 image to red (loadOp CLEAR, full renderArea).
// Pass 2 (loadOp LOAD, storeOp STORE) uses a smaller renderArea and clears a rect inside it
// to green with vkCmdClearAttachments. Vulkan requires everything outside the renderArea
// to keep its contents, so the result must be green inside the rect and red elsewhere.
// Cases vary the rect (tile-aligned, unaligned, corners) and whether the passes share a
// command buffer or are submitted separately (as PPSSPP does across frames).
// Depth cases: pass 1 clears color red and depth 1.0, pass 2 loads both with the smaller
// renderArea, pass 3 loads both full-screen and draws a full-screen triangle at depth 0.5
// with LESS: everything must turn green, red pixels mean depth was not preserved there.
// Usage: vkrenderarea [filter-substring]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "ra_depth_vert.h"
#include "ra_green_frag.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

#define W 256
#define H 256

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
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n");
	exit(2);
}

static VkCommandBuffer begin_cmd(void)
{
	VkCommandBuffer cb;
	VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
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

static VkImage img;
static VkImageView view;
static VkRenderPass rp_clear, rp_load;
static VkFramebuffer fb_clear, fb_load;
static VkBuffer rbuf;
static VkDeviceMemory rmem;

static VkRenderPass make_rp(VkAttachmentLoadOp load, VkImageLayout initial)
{
	VkAttachmentDescription ad = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = initial, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &ar };
	VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rci, NULL, &rp));
	return rp;
}

static VkFramebuffer make_fb(VkRenderPass rp)
{
	VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp, .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb));
	return fb;
}

static void setup(void)
{
	VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
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
	fb_clear = make_fb(rp_clear);
	fb_load = make_fb(rp_load);

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

static void pass_clear_red(VkCommandBuffer cb)
{
	VkClearValue cv = { .color = { .float32 = { 1, 0, 0, 1 } } };
	VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp_clear,
		.framebuffer = fb_clear, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &cv };
	vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(cb);
}

static void barrier_att(VkCommandBuffer cb)
{
	VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void pass_load_green(VkCommandBuffer cb, VkRect2D area, VkRect2D rect, int draw)
{
	VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp_load,
		.framebuffer = fb_load, .renderArea = area };
	vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	if (draw) {
		VkClearAttachment ca = { VK_IMAGE_ASPECT_COLOR_BIT, 0, { .color = { .float32 = { 0, 1, 0, 1 } } } };
		VkClearRect cr = { rect, 0, 1 };
		vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
	}
	vkCmdEndRenderPass(cb);
}

static void readback(VkCommandBuffer cb)
{
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
}

static int inside(VkRect2D r, int x, int y)
{
	return x >= r.offset.x && y >= r.offset.y &&
		x < r.offset.x + (int)r.extent.width && y < r.offset.y + (int)r.extent.height;
}

/* Returns the number of wrong pixels; prints a coarse 16x16-block map of where they are. */
static int run_case(const char *name, VkRect2D area, VkRect2D rect, int draw, int separate)
{
	VkCommandBuffer cb = begin_cmd();
	pass_clear_red(cb);
	if (separate) {
		submit(cb);
		cb = begin_cmd();
	} else {
		barrier_att(cb);
	}
	pass_load_green(cb, area, rect, draw);
	readback(cb);
	submit(cb);

	uint32_t *px;
	CHECK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
	int bad = 0, bad_out = 0, bad_in = 0;
	char map[H / 16][W / 16 + 1];
	memset(map, '.', sizeof(map));
	for (int by = 0; by < H / 16; by++)
		map[by][W / 16] = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			int green = draw && inside(rect, x, y);
			uint32_t want = green ? 0xff00ff00u : 0xff0000ffu; /* ABGR in memory: R8G8B8A8 */
			if (px[y * W + x] != want) {
				bad++;
				if (green)
					bad_in++;
				else
					bad_out++;
				map[y / 16][x / 16] = 'X';
			}
		}
	uint32_t corner = px[0];
	vkUnmapMemory(dev, rmem);
	printf("%-34s %s  wrong=%d (outside rect %d, inside %d) px[0]=%08x\n", name, bad ? "FAIL" : "ok  ",
		bad, bad_out, bad_in, corner);
	if (bad && getenv("VKRA_MAP"))
		for (int by = 0; by < H / 16; by++)
			printf("    %s\n", map[by]);
	return bad;
}

/* ---- depth preservation ---- */
static VkPipeline make_pipeline(VkRenderPass rp)
{
	VkShaderModule vs, fs;
	VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(ra_depth_vert), .pCode = ra_depth_vert };
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs));
	smi.codeSize = sizeof(ra_green_frag);
	smi.pCode = ra_green_frag;
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs));
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
	VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, W, H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE, .depthWriteEnable = VK_FALSE, .depthCompareOp = VK_COMPARE_OP_LESS };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &pl));
	VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
		.pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
		.pDepthStencilState = &ds, .pColorBlendState = &cb, .layout = pl, .renderPass = rp };
	VkPipeline pipe;
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe));
	return pipe;
}

static VkRenderPass make_rp_depth(VkFormat df, VkAttachmentLoadOp load, VkImageLayout ci, VkImageLayout di)
{
	VkAttachmentDescription ad[2] = {
		{ .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = load,
		  .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = ci,
		  .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
		{ .format = df, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		  .stencilLoadOp = load, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE, .initialLayout = di,
		  .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
	VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference dr = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &cr, .pDepthStencilAttachment = &dr };
	VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2, .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sd };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rci, NULL, &rp));
	return rp;
}

static int run_depth_case(const char *name, VkFormat df, VkRect2D area, int separate)
{
	VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = df, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage dimg;
	CHECK(vkCreateImage(dev, &ii, NULL, &dimg));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, dimg, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory dm;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &dm));
	CHECK(vkBindImageMemory(dev, dimg, dm, 0));
	VkImageAspectFlags asp = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (df == VK_FORMAT_D24_UNORM_S8_UINT || df == VK_FORMAT_D32_SFLOAT_S8_UINT)
		asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
	VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = dimg,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = df, .subresourceRange = { asp, 0, 1, 0, 1 } };
	VkImageView dview;
	CHECK(vkCreateImageView(dev, &vci, NULL, &dview));

	VkRenderPass rpc = make_rp_depth(df, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED);
	VkRenderPass rpl = make_rp_depth(df, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	VkImageView atts[2] = { view, dview };
	VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .attachmentCount = 2,
		.pAttachments = atts, .width = W, .height = H, .layers = 1 };
	VkFramebuffer fbc, fbl;
	fci.renderPass = rpc;
	CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fbc));
	fci.renderPass = rpl;
	CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fbl));
	VkPipeline pipe = make_pipeline(rpl);

	VkClearValue cv[2] = { { .color = { .float32 = { 1, 0, 0, 1 } } }, { .depthStencil = { 1.0f, 0 } } };
	VkCommandBuffer cb = begin_cmd();
	vkCmdBeginRenderPass(cb, &(VkRenderPassBeginInfo){ .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rpc, .framebuffer = fbc, .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 2,
		.pClearValues = cv }, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(cb);
	if (separate) { submit(cb); cb = begin_cmd(); }
	/* pass 2: partial renderArea, loads and stores both attachments, draws nothing */
	vkCmdBeginRenderPass(cb, &(VkRenderPassBeginInfo){ .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rpl, .framebuffer = fbl, .renderArea = area }, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(cb);
	if (separate) { submit(cb); cb = begin_cmd(); }
	/* pass 3: full renderArea, full-screen triangle at z=0.5, depth test LESS against 1.0 */
	vkCmdBeginRenderPass(cb, &(VkRenderPassBeginInfo){ .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rpl, .framebuffer = fbl, .renderArea = { { 0, 0 }, { W, H } } }, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	readback(cb);
	submit(cb);

	uint32_t *px;
	CHECK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
	int bad = 0, bad_in = 0;
	char map[H / 16][W / 16 + 1];
	memset(map, '.', sizeof(map));
	for (int by = 0; by < H / 16; by++)
		map[by][W / 16] = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			if (px[y * W + x] != 0xff00ff00u) {
				bad++;
				if (inside(area, x, y))
					bad_in++;
				map[y / 16][x / 16] = 'X';
			}
	vkUnmapMemory(dev, rmem);
	printf("%-34s %s  wrong=%d (inside area %d, outside %d)\n", name, bad ? "FAIL" : "ok  ", bad, bad_in, bad - bad_in);
	if (bad && getenv("VKRA_MAP"))
		for (int by = 0; by < H / 16; by++)
			printf("    %s\n", map[by]);
	vkDeviceWaitIdle(dev);
	vkDestroyPipeline(dev, pipe, NULL);
	vkDestroyFramebuffer(dev, fbc, NULL);
	vkDestroyFramebuffer(dev, fbl, NULL);
	vkDestroyRenderPass(dev, rpc, NULL);
	vkDestroyRenderPass(dev, rpl, NULL);
	vkDestroyImageView(dev, dview, NULL);
	vkDestroyImage(dev, dimg, NULL);
	vkFreeMemory(dev, dm, NULL);
	return bad;
}

int main(int argc, char **argv)
{
	const char *filter = argc > 1 ? argv[1] : NULL;
	init();
	setup();
	struct { const char *name; VkRect2D area; } cases[] = {
		{ "full",              { { 0, 0 }, { W, H } } },
		{ "aligned_64",        { { 64, 64 }, { 64, 64 } } },
		{ "aligned_32",        { { 96, 96 }, { 32, 32 } } },
		{ "unaligned",         { { 70, 90 }, { 50, 40 } } },
		{ "corner_tl_16",      { { 0, 0 }, { 16, 16 } } },
		{ "corner_br",         { { 200, 180 }, { 56, 76 } } },
		{ "wide_strip",        { { 0, 100 }, { W, 30 } } },
	};
	int fails = 0;
	char name[64];
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		for (int draw = 0; draw < 2; draw++)
			for (int sep = 0; sep < 2; sep++) {
				snprintf(name, sizeof(name), "%s%s%s", cases[i].name, draw ? "+draw" : "+empty",
					sep ? "+sep" : "+same");
				if (filter && !strstr(name, filter))
					continue;
				fails += run_case(name, cases[i].area, cases[i].area, draw, sep) != 0;
			}
	struct { const char *name; VkFormat f; } dfs[] = {
		{ "d16", VK_FORMAT_D16_UNORM }, { "d24s8", VK_FORMAT_D24_UNORM_S8_UINT },
		{ "d32s8", VK_FORMAT_D32_SFLOAT_S8_UINT }, { "d32", VK_FORMAT_D32_SFLOAT } };
	for (unsigned f = 0; f < sizeof(dfs) / sizeof(dfs[0]); f++) {
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(pd, dfs[f].f, &fp);
		if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
			printf("depth %s: not supported, skipped\n", dfs[f].name);
			continue;
		}
		for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
			for (int sep = 0; sep < 2; sep++) {
				snprintf(name, sizeof(name), "depth_%s_%s%s", dfs[f].name, cases[i].name, sep ? "+sep" : "+same");
				if (filter && !strstr(name, filter))
					continue;
				fails += run_depth_case(name, dfs[f].f, cases[i].area, sep) != 0;
			}
	}
	printf("%d failing case(s)\n", fails);
	return fails ? 1 : 0;
}
