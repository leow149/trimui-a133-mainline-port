// Time a full-screen textured pass into a LINEAR vs OPTIMAL render target on the PowerVR driver.
// Mimics what a compositor does per output frame: sample a client texture, write the scan-out buffer.
// Usage: vkpass [w=720] [H=1280] [rt=linear|optimal] [src=linear|optimal|none] [reps=60]
//               [draws=1] [shader=tex|flat] [rot=0|1] [blend=0|1] [under=0|1] [load=clear|load|dontcare] [batch=1] [fmt=rgba|bgrx|565]
// Prints the per-submit wall time (submit .. fence signalled): min / median / max in ms.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "lin_vert.h"
#include "lin_frag.h"
#include "flat_frag.h"
#include "lin_rot_vert.h"
#include "const_frag.h"
#include "const_ef_frag.h"
#include "flat_ef_frag.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static VkPhysicalDevice pd;
static VkDevice dev;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n");
	exit(1);
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static const char *arg(int argc, char **argv, const char *key, const char *def)
{
	size_t n = strlen(key);
	for (int i = 1; i < argc; i++)
		if (!strncmp(argv[i], key, n) && argv[i][n] == '=')
			return argv[i] + n + 1;
	return def;
}

static VkImage mkimage(uint32_t W, uint32_t H, VkFormat f, VkImageTiling t, VkImageUsageFlags u,
		       VkDeviceMemory *mem)
{
	VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = { W, H, 1 }, .mipLevels = 1,
		.arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = t, .usage = u,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	VkImage img;
	CHECK(vkCreateImage(dev, &ci, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) };
	CHECK(vkAllocateMemory(dev, &mai, NULL, mem));
	CHECK(vkBindImageMemory(dev, img, *mem, 0));
	return img;
}

int main(int argc, char **argv)
{
	uint32_t W = atoi(arg(argc, argv, "w", "720")), H = atoi(arg(argc, argv, "h", "1280"));
	int rt_lin = !strcmp(arg(argc, argv, "rt", "linear"), "linear");
	const char *srcm = arg(argc, argv, "src", "linear");
	int src_none = !strcmp(srcm, "none"), src_lin = !strcmp(srcm, "linear");
	int reps = atoi(arg(argc, argv, "reps", "60")), draws = atoi(arg(argc, argv, "draws", "1"));
	int batch = atoi(arg(argc, argv, "batch", "1"));
	uint32_t SW = atoi(arg(argc, argv, "sw", "0")), SH = atoi(arg(argc, argv, "sh", "0"));
	if (!SW) SW = W;
	if (!SH) SH = H;
	const char *shn = arg(argc, argv, "shader", "tex");
	int flat = !strcmp(shn, "flat"), cnst = !strcmp(shn, "const");
	int cef = !strcmp(shn, "const_ef"), fef = !strcmp(shn, "flat_ef");
	if (cnst || cef || fef) flat = 1;
	/* depth=0|16|32: add a depth attachment; dcmp=always|lequal|less|never; dwrite=0|1 */
	int depth = atoi(arg(argc, argv, "depth", "0")), dwrite = atoi(arg(argc, argv, "dwrite", "1"));
	const char *dc = arg(argc, argv, "dcmp", "lequal");
	VkCompareOp dcmp = !strcmp(dc, "always") ? VK_COMPARE_OP_ALWAYS : !strcmp(dc, "less") ? VK_COMPARE_OP_LESS :
		!strcmp(dc, "never") ? VK_COMPARE_OP_NEVER : VK_COMPARE_OP_LESS_OR_EQUAL;
	VkFormat dfmt = depth == 16 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT;
	int rot = atoi(arg(argc, argv, "rot", "0")), blend = atoi(arg(argc, argv, "blend", "0"));
	int under = atoi(arg(argc, argv, "under", "0"));	/* flat full-screen draw first, like a background rect */
	const char *ld = arg(argc, argv, "load", "clear");
	VkAttachmentLoadOp lop = !strcmp(ld, "load") ? VK_ATTACHMENT_LOAD_OP_LOAD :
		!strcmp(ld, "dontcare") ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
	const char *fs_ = arg(argc, argv, "fmt", "bgrx");
	VkFormat fmt = !strcmp(fs_, "rgba") ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	VkDeviceMemory rmem, smem;
	VkImage rt = mkimage(W, H, fmt, rt_lin ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
			     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &rmem);
	VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = rt,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { 1, 0, 1, 0, 1 } };
	VkImageView rtv, srv = VK_NULL_HANDLE;
	CHECK(vkCreateImageView(dev, &ivci, NULL, &rtv));

	VkImage src = VK_NULL_HANDLE;
	if (!src_none) {
		src = mkimage(SW, SH, fmt, src_lin ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
			      VK_IMAGE_USAGE_SAMPLED_BIT | (src_lin ? 0 : VK_IMAGE_USAGE_TRANSFER_DST_BIT), &smem);
		ivci.image = src;
		CHECK(vkCreateImageView(dev, &ivci, NULL, &srv));
	}

	VkAttachmentDescription att = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = lop,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = lop == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_GENERAL };
	VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_GENERAL };
	VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkAttachmentDescription atts[2] = { att, { .format = dfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
	VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &aref, .pDepthStencilAttachment = depth ? &dref : NULL };
	VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = depth ? 2 : 1, .pAttachments = atts, .subpassCount = 1, .pSubpasses = &sp };
	VkImageView fbv[2] = { rtv, VK_NULL_HANDLE };
	if (depth) {
		VkDeviceMemory dmem;
		VkImage dimg = mkimage(W, H, dfmt, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &dmem);
		VkImageViewCreateInfo d = ivci;
		d.image = dimg; d.format = dfmt; d.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		CHECK(vkCreateImageView(dev, &d, NULL, &fbv[1]));
	}
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
		.attachmentCount = depth ? 2 : 1, .pAttachments = fbv, .width = W, .height = H, .layers = 1 };
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

	VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
	VkSampler smp;
	CHECK(vkCreateSampler(dev, &sci, NULL, &smp));
	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &b };
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dp;
	CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet ds;
	CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	if (!src_none) {
		VkDescriptorImageInfo dii = { smp, srv, VK_IMAGE_LAYOUT_GENERAL };
		VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds,
			.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &dii };
		vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
	}
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = rot ? sizeof(lin_rot_vert) : sizeof(lin_vert), .pCode = rot ? lin_rot_vert : lin_vert };
	VkShaderModule vs, fs;
	CHECK(vkCreateShaderModule(dev, &smci, NULL, &vs));
	smci.codeSize = cef ? sizeof(const_ef_frag) : fef ? sizeof(flat_ef_frag) : cnst ? sizeof(const_frag) : flat ? sizeof(flat_frag) : sizeof(lin_frag);
	smci.pCode = cef ? const_ef_frag : fef ? flat_ef_frag : cnst ? const_frag : flat ? flat_frag : lin_frag;
	CHECK(vkCreateShaderModule(dev, &smci, NULL, &fs));
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
		  .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		  .module = fs, .pName = "main" } };
	VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, (float)W, (float)H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	if (blend) {
		cba.blendEnable = VK_TRUE;
		cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkPipelineDepthStencilStateCreateInfo dss = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE, .depthWriteEnable = dwrite, .depthCompareOp = dcmp };
	VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
		.pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
		.pColorBlendState = &cb, .pDepthStencilState = depth ? &dss : NULL, .layout = pl, .renderPass = rp, .subpass = 0 };
	VkPipeline pipe, pipe_under = VK_NULL_HANDLE;
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
	if (under) {
		VkShaderModule fs2;
		VkShaderModuleCreateInfo m2 = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = sizeof(flat_frag), .pCode = flat_frag };
		CHECK(vkCreateShaderModule(dev, &m2, NULL, &fs2));
		VkPipelineShaderStageCreateInfo st2[2] = { st[0], st[1] };
		st2[1].module = fs2;
		VkPipelineColorBlendAttachmentState c2 = { .colorWriteMask = 0xf };
		VkPipelineColorBlendStateCreateInfo cb2 = cb;
		cb2.pAttachments = &c2;
		VkGraphicsPipelineCreateInfo g2 = gpci;
		g2.pStages = st2;
		g2.pColorBlendState = &cb2;
		CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &g2, NULL, &pipe_under));
	}

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2 };
	VkCommandBuffer cmds[2];
	CHECK(vkAllocateCommandBuffers(dev, &cbai, cmds));

	/* setup cmd: fill the source texture and move everything to GENERAL */
	VkBuffer sbuf = VK_NULL_HANDLE;
	VkDeviceMemory sbmem;
	CHECK(vkBeginCommandBuffer(cmds[0], &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
	if (!src_none) {
		VkDeviceSize sz = (VkDeviceSize)SW * SH * 4;
		if (src_lin) {
			void *p;
			VkImageSubresource isr = { 1, 0, 0 };
			VkSubresourceLayout lay;
			vkGetImageSubresourceLayout(dev, src, &isr, &lay);
			CHECK(vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, &p));
			for (uint32_t y = 0; y < SH; y++)
				memset((char *)p + lay.offset + y * lay.rowPitch, 0x40 + (y & 63), SW * 4);
		} else {
			VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
				.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
			CHECK(vkCreateBuffer(dev, &bci, NULL, &sbuf));
			VkMemoryRequirements mr;
			vkGetBufferMemoryRequirements(dev, sbuf, &mr);
			VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) };
			CHECK(vkAllocateMemory(dev, &mai, NULL, &sbmem));
			CHECK(vkBindBufferMemory(dev, sbuf, sbmem, 0));
			void *p;
			CHECK(vkMapMemory(dev, sbmem, 0, sz, 0, &p));
			memset(p, 0x55, sz);
			VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = src, .subresourceRange = { 1, 0, 1, 0, 1 } };
			vkCmdPipelineBarrier(cmds[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
					     0, 0, NULL, 0, NULL, 1, &ib);
			VkBufferImageCopy reg = { .imageSubresource = { 1, 0, 0, 1 }, .imageExtent = { SW, SH, 1 } };
			vkCmdCopyBufferToImage(cmds[0], sbuf, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
		}
		VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = src_lin ? VK_IMAGE_LAYOUT_PREINITIALIZED : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = src, .subresourceRange = { 1, 0, 1, 0, 1 } };
		vkCmdPipelineBarrier(cmds[0], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				     0, 0, NULL, 0, NULL, 1, &ib);
	}
	CHECK(vkEndCommandBuffer(cmds[0]));

	/* the timed command buffer: `batch` render passes of `draws` full-screen draws each */
	CHECK(vkBeginCommandBuffer(cmds[1], &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
	VkClearValue cvs[2] = { { .color = { .float32 = { 0, 0, 0, 1 } } }, { .depthStencil = { 1.0f, 0 } } };
	for (int k = 0; k < batch; k++) {
		vkCmdBeginRenderPass(cmds[1], &(VkRenderPassBeginInfo){ .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp, .framebuffer = fb, .renderArea = { { 0, 0 }, { W, H } },
			.clearValueCount = depth ? 2 : 1, .pClearValues = cvs }, VK_SUBPASS_CONTENTS_INLINE);
		if (under) {
			vkCmdBindPipeline(cmds[1], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_under);
			vkCmdDraw(cmds[1], 3, 1, 0, 0);
		}
		vkCmdBindPipeline(cmds[1], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		if (!src_none)
			vkCmdBindDescriptorSets(cmds[1], VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
		for (int d = 0; d < draws; d++)
			vkCmdDraw(cmds[1], 3, 1, 0, 0);
		vkCmdEndRenderPass(cmds[1]);
	}
	CHECK(vkEndCommandBuffer(cmds[1]));

	VkFence fence;
	CHECK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmds[0] };
	CHECK(vkQueueSubmit(q, 1, &si, fence));
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
	CHECK(vkResetFences(dev, 1, &fence));

	si.pCommandBuffers = &cmds[1];
	double *t = malloc(sizeof(double) * reps);
	for (int i = -3; i < reps; i++) {		/* 3 warm-up submits */
		double t0 = now_ms();
		CHECK(vkQueueSubmit(q, 1, &si, fence));
		CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
		double t1 = now_ms();
		CHECK(vkResetFences(dev, 1, &fence));
		if (i >= 0) t[i] = (t1 - t0) / batch;
	}
	qsort(t, reps, sizeof(double), cmpd);
	printf("depth=%d dcmp=%s dwrite=%d shader=%s | ", depth, dc, dwrite, shn);
	printf("%ux%u %s rt=%s src=%s(%ux%u) load=%s draws=%d rot=%d blend=%d under=%d batch=%d: per pass min %.2f med %.2f max %.2f ms\n", W, H, flat ? "flat" : "tex",
	       rt_lin ? "linear" : "optimal", srcm, SW, SH, ld, draws, rot, blend, under, batch, t[0], t[reps / 2], t[reps - 1]);
	int hold = atoi(arg(argc, argv, "hold", "0"));   /* keep the device (and parameter memory) alive */
	if (hold) { fflush(stdout); sleep(hold); }
	return 0;
}
