// Reproduces PPSSPP's per-frame pattern on the PowerVR Vulkan driver (seen with PVR_RP_LOG in NFS
// Most Wanted): in ONE command buffer,
//   A: render pass on the 480x272 framebuffer (LOAD/STORE), full-screen draw (green)
//   B: render pass on a 256x256 target, optionally sampling A's color image
//   C: render pass on the 480x272 framebuffer again (LOAD/STORE), draw only a small rect (blue)
// A must survive outside the rect: the result has to be green with a blue rect. Variants: with or
// without a D24S8 depth/stencil attachment, with or without layout transitions of A's image
// (attachment -> shader read -> attachment) and with or without B sampling A.
// Usage: vkrtt [filter-substring]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "ra_depth_vert.h"
#include "ra_pc_frag.h"
#include "ra_tex_frag.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

#define AW 480
#define AH 272
#define BW 256
#define BH 256
#define RED   0xff0000ffu
#define GREEN 0xff00ff00u
#define BLUE  0xffff0000u
#define DSFMT VK_FORMAT_D24_UNORM_S8_UINT

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static VkCommandPool pool;

struct target {
	int w, h, depth;
	VkImage col, ds;
	VkImageView colv, dsv;
	VkRenderPass rp_clear, rp_load;
	VkFramebuffer fb_clear, fb_load;
};

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	exit(2);
}

static void make_image(VkFormat f, int w, int h, VkImageUsageFlags usage, VkImageAspectFlags asp,
		       VkImage *img, VkImageView *view)
{
	VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = f, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
	CHECK(vkCreateImage(dev, &ii, NULL, img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, *img, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory m;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &m));
	CHECK(vkBindImageMemory(dev, *img, m, 0));
	VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f, .subresourceRange = { asp, 0, 1, 0, 1 } };
	CHECK(vkCreateImageView(dev, &vi, NULL, view));
}

static VkRenderPass make_rp(int depth, VkAttachmentLoadOp load)
{
	VkImageLayout ci = load == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageLayout di = load == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	VkAttachmentDescription ad[2] = {
		{ .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = load,
		  .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = ci,
		  .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
		{ .format = DSFMT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		  .stencilLoadOp = load, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE, .initialLayout = di,
		  .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
	VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference dr = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &cr, .pDepthStencilAttachment = depth ? &dr : NULL };
	VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = depth ? 2 : 1, .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sd };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rci, NULL, &rp));
	return rp;
}

static void make_target(struct target *t, int w, int h, int depth)
{
	t->w = w; t->h = h; t->depth = depth;
	make_image(VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &t->col, &t->colv);
	if (depth)
		make_image(DSFMT, w, h, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, &t->ds, &t->dsv);
	t->rp_clear = make_rp(depth, VK_ATTACHMENT_LOAD_OP_CLEAR);
	t->rp_load = make_rp(depth, VK_ATTACHMENT_LOAD_OP_LOAD);
	VkImageView atts[2] = { t->colv, t->dsv };
	VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.attachmentCount = depth ? 2 : 1, .pAttachments = atts, .width = w, .height = h, .layers = 1 };
	fci.renderPass = t->rp_clear;
	CHECK(vkCreateFramebuffer(dev, &fci, NULL, &t->fb_clear));
	fci.renderPass = t->rp_load;
	CHECK(vkCreateFramebuffer(dev, &fci, NULL, &t->fb_load));
}

static VkPipelineLayout lay_pc, lay_tex;
static VkDescriptorSetLayout dsl;
static VkSampler sampler;

static VkPipeline make_pipe(VkRenderPass rp, int tex)
{
	VkShaderModule vs, fs;
	VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(ra_depth_vert), .pCode = ra_depth_vert };
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs));
	smi.codeSize = tex ? sizeof(ra_tex_frag) : sizeof(ra_pc_frag);
	smi.pCode = tex ? ra_tex_frag : ra_pc_frag;
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs));
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
	VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_ALWAYS };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2, .pDynamicStates = dyn };
	VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = st, .pVertexInputState = &vis, .pInputAssemblyState = &ia,
		.pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
		.pColorBlendState = &cbs, .pDynamicState = &dsi, .layout = tex ? lay_tex : lay_pc, .renderPass = rp };
	VkPipeline p;
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &p));
	return p;
}

static void setup_common(void)
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

	VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	CHECK(vkCreatePipelineLayout(dev, &lci, NULL, &lay_pc));
	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &b };
	CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
	VkPipelineLayoutCreateInfo lti = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl };
	CHECK(vkCreatePipelineLayout(dev, &lti, NULL, &lay_tex));
	VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
	CHECK(vkCreateSampler(dev, &si, NULL, &sampler));
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

static void color_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
			  VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags dst)
{
	VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa,
		.dstAccessMask = da, .oldLayout = from, .newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cb, ss, dst, 0, 0, NULL, 0, NULL, 1, &ib);
}

static void begin_pass(VkCommandBuffer cb, struct target *t, int clear)
{
	VkClearValue cv[2] = { { .color = { .float32 = { 1, 0, 0, 1 } } }, { .depthStencil = { 1.0f, 0 } } };
	VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = clear ? t->rp_clear : t->rp_load, .framebuffer = clear ? t->fb_clear : t->fb_load,
		.renderArea = { { 0, 0 }, { t->w, t->h } }, .clearValueCount = clear ? 2 : 0, .pClearValues = cv };
	vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

static void draw(VkCommandBuffer cb, VkPipeline p, struct target *t, VkRect2D sc, uint32_t color)
{
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
	VkViewport vp = { 0, 0, t->w, t->h, 0, 1 };
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
	float c[4] = { (color & 0xff) / 255.0f, ((color >> 8) & 0xff) / 255.0f, ((color >> 16) & 0xff) / 255.0f, 1 };
	vkCmdPushConstants(cb, lay_pc, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c);
	vkCmdDraw(cb, 3, 1, 0, 0);
}

static int run(const char *name, int depth, int transitions, int sample)
{
	struct target A, B;
	make_target(&A, AW, AH, depth);
	make_target(&B, BW, BH, depth);
	VkPipeline pA = make_pipe(A.rp_load, 0), pTex = make_pipe(B.rp_load, 1);

	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
	VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dp;
	CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
	VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet set;
	CHECK(vkAllocateDescriptorSets(dev, &dai, &set));
	VkDescriptorImageInfo dii = { sampler, A.colv,
		transitions ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
		.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
	vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

	/* init: A red, B red, in a separate submit */
	VkCommandBuffer cb = begin_cmd();
	begin_pass(cb, &A, 1);
	vkCmdEndRenderPass(cb);
	begin_pass(cb, &B, 1);
	vkCmdEndRenderPass(cb);
	submit(cb);

	VkRect2D fullA = { { 0, 0 }, { AW, AH } }, fullB = { { 0, 0 }, { BW, BH } };
	VkRect2D small = { { 200, 120 }, { 64, 48 } };
	cb = begin_cmd();
	begin_pass(cb, &A, 0);                       /* A: full green */
	draw(cb, pA, &A, fullA, GREEN);
	vkCmdEndRenderPass(cb);
	if (transitions)
		color_barrier(cb, A.col, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	else
		color_barrier(cb, A.col, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	begin_pass(cb, &B, 0);                       /* B: other target */
	if (sample && transitions) {
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pTex);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, lay_tex, 0, 1, &set, 0, NULL);
		VkViewport vp = { 0, 0, BW, BH, 0, 1 };
		vkCmdSetViewport(cb, 0, 1, &vp);
		vkCmdSetScissor(cb, 0, 1, &fullB);
		vkCmdDraw(cb, 3, 1, 0, 0);
	} else {
		draw(cb, pA, &B, fullB, BLUE);
	}
	vkCmdEndRenderPass(cb);
	if (transitions)
		color_barrier(cb, A.col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	begin_pass(cb, &A, 0);                       /* C: small blue rect on A */
	draw(cb, pA, &A, small, BLUE);
	vkCmdEndRenderPass(cb);

	/* readback A */
	VkBuffer rb;
	VkDeviceMemory rm;
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = AW * AH * 4,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	CHECK(vkCreateBuffer(dev, &bci, NULL, &rb));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, rb, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	CHECK(vkAllocateMemory(dev, &mai, NULL, &rm));
	CHECK(vkBindBufferMemory(dev, rb, rm, 0));
	color_barrier(cb, A.col, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy c = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { AW, AH, 1 } };
	vkCmdCopyImageToBuffer(cb, A.col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &c);
	submit(cb);

	uint32_t *px;
	CHECK(vkMapMemory(dev, rm, 0, VK_WHOLE_SIZE, 0, (void **)&px));
	int bad = 0;
	char map[AH / 16 + 1][AW / 16 + 1];
	memset(map, '.', sizeof(map));
	for (int by = 0; by <= AH / 16; by++)
		map[by][AW / 16] = 0;
	for (int y = 0; y < AH; y++)
		for (int x = 0; x < AW; x++) {
			int in = x >= small.offset.x && y >= small.offset.y &&
				x < small.offset.x + (int)small.extent.width && y < small.offset.y + (int)small.extent.height;
			uint32_t want = in ? BLUE : GREEN, got = px[y * AW + x];
			if (got != want) {
				bad++;
				map[y / 16][x / 16] = got == RED ? 'R' : got == GREEN ? 'G' : got == BLUE ? 'B' : 'X';
			}
		}
	vkUnmapMemory(dev, rm);
	printf("%-36s %s  wrong=%d of %d\n", name, bad ? "FAIL" : "ok  ", bad, AW * AH);
	if (bad)
		for (int by = 0; by < AH / 16; by++)
			printf("    %s\n", map[by]);
	vkDeviceWaitIdle(dev);
	return bad;
}

int main(int argc, char **argv)
{
	const char *filter = argc > 1 ? argv[1] : NULL;
	setup_common();
	int fails = 0;
	char name[64];
	for (int depth = 0; depth < 2; depth++)
		for (int tr = 0; tr < 2; tr++)
			for (int smp = 0; smp < 2; smp++) {
				if (smp && !tr)
					continue;
				snprintf(name, sizeof(name), "%s%s%s", depth ? "d24s8" : "color", tr ? "+transitions" : "+barrier",
					smp ? "+B_samples_A" : "");
				if (filter && !strstr(name, filter))
					continue;
				fails += run(name, depth, tr, smp) != 0;
			}
	printf("%d failing case(s)\n", fails);
	return fails ? 1 : 0;
}
