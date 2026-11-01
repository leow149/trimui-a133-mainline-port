// Pipelined rendering test: N frames are submitted back to back without
// waiting, each into its own linear image (shared depth buffer, like vkcube).
// Frame i clears to red and draws a green triangle shifted by i * 16 pixels.
// Afterwards every image must contain exactly its own triangle.
// Usage: vkmulti [frames] [width height]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "tri_vert.h"
#include "tri_frag.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)
#define MAXF 16

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

static VkImage mkimage(VkFormat fmt, uint32_t w, uint32_t h, VkImageTiling tiling,
		       VkImageUsageFlags usage, VkMemoryPropertyFlags memf, VkDeviceMemory *mem_out)
{
	VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, 1 },
		.mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = tiling, .usage = usage };
	VkImage img;
	CHECK(vkCreateImage(dev, &ci, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, memf) };
	CHECK(vkAllocateMemory(dev, &ai, NULL, mem_out));
	CHECK(vkBindImageMemory(dev, img, *mem_out, 0));
	return img;
}

static VkImageView mkview(VkImage img, VkFormat fmt, VkImageAspectFlags aspect)
{
	VkImageViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
		.subresourceRange = { aspect, 0, 1, 0, 1 } };
	VkImageView v;
	CHECK(vkCreateImageView(dev, &ci, NULL, &v));
	return v;
}

int main(int argc, char **argv)
{
	uint32_t N = argc > 1 ? atoi(argv[1]) : 8;
	uint32_t W = argc > 3 ? atoi(argv[2]) : 256, H = argc > 3 ? atoi(argv[3]) : 128;
	const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM, dfmt = VK_FORMAT_D16_UNORM;
	if (N > MAXF)
		N = MAXF;

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
				  .apiVersion = VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
				     .pApplicationInfo = &app };
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	VkAttachmentDescription atts[2] = {
		{ .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
		  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
		{ .format = dfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
		  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		  .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
	VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &aref, .pDepthStencilAttachment = &dref };
	/* like vkcube: the shared depth buffer is protected by a subpass dependency */
	VkSubpassDependency dep = { VK_SUBPASS_EXTERNAL, 0,
		VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0 };
	VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2, .pAttachments = atts, .subpassCount = 1, .pSubpasses = &sp,
		.dependencyCount = 1, .pDependencies = &dep };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));

	VkDeviceMemory dmem;
	VkImage dimg = mkimage(dfmt, W, H, VK_IMAGE_TILING_OPTIMAL,
			       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dmem);
	VkImageView dview = mkview(dimg, dfmt, VK_IMAGE_ASPECT_DEPTH_BIT);

	VkImage img[MAXF];
	VkDeviceMemory mem[MAXF];
	VkFramebuffer fb[MAXF];
	uint8_t *map[MAXF];
	VkSubresourceLayout lay;
	for (uint32_t i = 0; i < N; i++) {
		img[i] = mkimage(fmt, W, H, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
				 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				 &mem[i]);
		VkImageSubresource isr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
		vkGetImageSubresourceLayout(dev, img[i], &isr, &lay);
		CHECK(vkMapMemory(dev, mem[i], 0, VK_WHOLE_SIZE, 0, (void **)&map[i]));
		memset(map[i], 0x55, lay.size);
		VkImageView views[2] = { mkview(img[i], fmt, VK_IMAGE_ASPECT_COLOR_BIT), dview };
		VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = rp, .attachmentCount = 2, .pAttachments = views,
			.width = W, .height = H, .layers = 1 };
		CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb[i]));
	}

	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(tri_vert), .pCode = tri_vert };
	VkShaderModule vs, fs;
	CHECK(vkCreateShaderModule(dev, &smci, NULL, &vs));
	smci.codeSize = sizeof(tri_frag);
	smci.pCode = tri_frag;
	CHECK(vkCreateShaderModule(dev, &smci, NULL, &fs));
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
	VkPipelineVertexInputStateCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, (float)W, (float)H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkPipelineDepthStencilStateCreateInfo ds = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS };
	VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24 };
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
		.pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
		.pDepthStencilState = &ds, .pColorBlendState = &cb, .layout = pl,
		.renderPass = rp, .subpass = 0 };
	VkPipeline pipe;
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBuffer cmd[MAXF];
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = N };
	CHECK(vkAllocateCommandBuffers(dev, &cbai, cmd));
	VkClearValue clears[2] = { { .color = { .float32 = { 1, 0, 0, 1 } } },
				   { .depthStencil = { 1.0f, 0 } } };
	for (uint32_t i = 0; i < N; i++) {
		VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		CHECK(vkBeginCommandBuffer(cmd[i], &bi));
		VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = rp, .framebuffer = fb[i], .renderArea = { { 0, 0 }, { W, H } },
			.clearValueCount = 2, .pClearValues = clears };
		vkCmdBeginRenderPass(cmd[i], &rpbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cmd[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		float pc[6] = { 0, 1, 0, 1, 0.5f, i * 32.0f / W };	/* shift by 16 px */
		vkCmdPushConstants(cmd[i], pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				   0, 24, pc);
		vkCmdDraw(cmd[i], 3, 1, 0, 0);
		vkCmdEndRenderPass(cmd[i]);
		CHECK(vkEndCommandBuffer(cmd[i]));
	}

	/* one submission per frame, no waiting in between */
	for (uint32_t i = 0; i < N; i++) {
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				    .commandBufferCount = 1, .pCommandBuffers = &cmd[i] };
		CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	}
	CHECK(vkQueueWaitIdle(q));

	/* left edge of the triangle in the middle row: expected at 5% of W + i * 16 */
	int bad = 0;
	for (uint32_t i = 0; i < N; i++) {
		uint32_t y = H / 4, first = 0, green = 0, other = 0;
		for (uint32_t yy = 0; yy < H; yy++)
			for (uint32_t x = 0; x < W; x++) {
				uint8_t *p = map[i] + lay.offset + yy * lay.rowPitch + x * 4;
				if (p[1] > 200 && p[0] < 50)
					green++;
				else if (!(p[0] > 200 && p[1] < 50))
					other++;
			}
		for (uint32_t x = 0; x < W; x++) {
			uint8_t *p = map[i] + lay.offset + y * lay.rowPitch + x * 4;
			if (p[1] > 200 && p[0] < 50) {
				first = x;
				break;
			}
		}
		uint32_t expect = (uint32_t)(0.05f * W + 0.5f) + i * 16;
		int ok = (first >= expect - 1 && first <= expect + 1) && other == 0;
		bad += !ok;
		printf("frame %2u: left edge %3u (expect %3u), green %6u, other %u %s\n", i, first,
		       expect, green, other, ok ? "ok" : "BAD");
	}
	printf("%s\n", bad ? "MISMATCH" : "all frames correct");
	return bad != 0;
}
