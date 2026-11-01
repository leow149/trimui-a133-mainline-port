// Minimal offscreen Vulkan test: clear a W x H image to red, draw one green
// triangle, read it back, report coverage per 32x32 block and write a PPM.
// Usage: vktest [width height] [out.ppm]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "tri_vert.h"
#include "tri_frag.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n");
	exit(1);
}

int main(int argc, char **argv)
{
	uint32_t W = argc > 2 ? atoi(argv[1]) : 256, H = argc > 2 ? atoi(argv[2]) : 128;
	const char *out = argc > 3 ? argv[3] : (argc == 2 ? argv[1] : "/tmp/vktest.ppm");
	VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
	/* VKTEST_LINEAR: render into a host-visible linear image, read it without a transfer */
	int linear = getenv("VKTEST_LINEAR") != NULL;

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
				  .apiVersion = VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
				     .pApplicationInfo = &app };
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	VkPhysicalDevice pd;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	if (!n) { fprintf(stderr, "no device\n"); return 1; }

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					.queueFamilyIndex = 0, .queueCount = 1,
					.pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				   .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	/* colour attachment */
	VkImageCreateInfo imci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { W, H, 1 },
		.mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			 VK_IMAGE_USAGE_TRANSFER_DST_BIT };
	VkImage img;
	CHECK(vkCreateImage(dev, &imci, NULL, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, img, &mr);
	printf("image %ux%u: size %llu align %llu\n", W, H,
	       (unsigned long long)mr.size, (unsigned long long)mr.alignment);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type(pd, mr.memoryTypeBits, linear ?
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT :
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	VkDeviceMemory imem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
	CHECK(vkBindImageMemory(dev, img, imem, 0));
	uint8_t *lin = NULL;
	VkSubresourceLayout lay = { 0 };
	if (linear) {
		VkImageSubresource isr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
		vkGetImageSubresourceLayout(dev, img, &isr, &lay);
		CHECK(vkMapMemory(dev, imem, 0, VK_WHOLE_SIZE, 0, (void **)&lin));
		memset(lin, 0x55, mr.size);
		printf("linear: offset %llu rowPitch %llu\n", (unsigned long long)lay.offset,
		       (unsigned long long)lay.rowPitch);
	}
	VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView view;
	CHECK(vkCreateImageView(dev, &ivci, NULL, &view));

	/* optional depth attachment */
	int depth = getenv("VKTEST_DEPTH") != NULL;
	VkFormat dfmt = getenv("VKTEST_D32F") ? VK_FORMAT_D32_SFLOAT :
			getenv("VKTEST_D24") ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D16_UNORM;
	VkImageView dview = VK_NULL_HANDLE;
	if (depth) {
		VkImageCreateInfo dci = imci;
		dci.format = dfmt;
		dci.tiling = VK_IMAGE_TILING_OPTIMAL;
		dci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		VkImage dimg;
		CHECK(vkCreateImage(dev, &dci, NULL, &dimg));
		VkMemoryRequirements dmr;
		vkGetImageMemoryRequirements(dev, dimg, &dmr);
		VkMemoryAllocateInfo dmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = dmr.size,
			.memoryTypeIndex = mem_type(pd, dmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
		VkDeviceMemory dmem;
		CHECK(vkAllocateMemory(dev, &dmai, NULL, &dmem));
		CHECK(vkBindImageMemory(dev, dimg, dmem, 0));
		VkImageViewCreateInfo dvci = ivci;
		dvci.image = dimg;
		dvci.format = dfmt;
		dvci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		CHECK(vkCreateImageView(dev, &dvci, NULL, &dview));
	}

	/* readback buffer */
	VkDeviceSize bsize = (VkDeviceSize)W * H * 4;
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bsize, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = mem_type(pd, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory bmem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem));
	CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
	uint8_t *px;
	CHECK(vkMapMemory(dev, bmem, 0, bsize, 0, (void **)&px));
	memset(px, 0x55, bsize);	/* marker: untouched bytes stay 0x55 */

	/* render pass + framebuffer */
	VkAttachmentDescription att = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = linear ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentDescription atts[2] = { att, {
		.format = dfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
	VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &aref,
		.pDepthStencilAttachment = depth ? &dref : NULL };
	VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = depth ? 2 : 1, .pAttachments = atts, .subpassCount = 1, .pSubpasses = &sp };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp, .attachmentCount = depth ? 2 : 1,
		.pAttachments = (VkImageView[]){ view, dview },
		.width = W, .height = H, .layers = 1 };
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

	/* pipeline: one triangle from gl_VertexIndex */
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
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
		.lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24 };
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = st, .pVertexInputState = &vi,
		.pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
		.pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl,
		.pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
			.depthCompareOp = VK_COMPARE_OP_LESS },
		.renderPass = rp, .subpass = 0 };
	VkPipeline pipe;
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

	/* record */
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					 .queueFamilyIndex = 0 };
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
	VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK(vkBeginCommandBuffer(cmd, &cbbi));
	VkClearValue clears[2] = { { .color = { .float32 = { 1, 0, 0, 1 } } },
				   { .depthStencil = { 1.0f, 0 } } };
	VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp, .framebuffer = fb,
		.renderArea = { { 0, 0 }, { getenv("VKTEST_RA_W") ? (uint32_t)atoi(getenv("VKTEST_RA_W")) : W,
					     getenv("VKTEST_RA_H") ? (uint32_t)atoi(getenv("VKTEST_RA_H")) : H } },
		.clearValueCount = depth ? 2 : 1, .pClearValues = clears };
	if (getenv("VKTEST_CLEARIMG")) {	/* transfer-job clear instead of a render pass */
		VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
		VkClearColorValue cc = { .float32 = { 1, 0, 0, 1 } };
		VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rr);
		goto recorded;
	}
	vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	if (!getenv("VKTEST_NO_DRAW")) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		/* green near (z 0.3); with VKTEST_TWO also blue far (z 0.6), drawn
		 * last and shifted right: where they overlap green must win */
		float pc_g[6] = { 0, 1, 0, 1, 0.3f, 0 };
		vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				   0, 24, pc_g);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		if (getenv("VKTEST_TWO")) {
			float pc_b[6] = { 0, 0, 1, 1, 0.6f, 0.4f };
			vkCmdPushConstants(cmd, pl,
					   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					   0, 24, pc_b);
			vkCmdDraw(cmd, 3, 1, 0, 0);
		}
	}
	vkCmdEndRenderPass(cmd);
recorded:
	VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
				  .imageExtent = { W, H, 1 } };
	VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	if (!linear) {
		vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
				     0, 1, &mb, 0, NULL, 0, NULL);
	} else {
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				     VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	}
	CHECK(vkEndCommandBuffer(cmd));

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			    .commandBufferCount = 1, .pCommandBuffers = &cmd };
	CHECK(vkQueueSubmit(q, 1, &si, fence));
	VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
	printf("fence: %d\n", wr);
	if (linear)	/* repack rows into the readback buffer layout */
		for (uint32_t y = 0; y < H; y++)
			memcpy(px + y * W * 4, lin + lay.offset + y * lay.rowPitch, W * 4);
	{	/* raw values of a few blocks outside the top-left quarter */
		uint32_t pts[][2] = { { W - 8, 8 }, { 8, H - 8 }, { W - 8, H - 8 } };
		for (int i = 0; i < 3; i++) {
			uint8_t *p = px + (pts[i][1] * W + pts[i][0]) * 4;
			printf("px(%u,%u) = %02x %02x %02x %02x\n", pts[i][0], pts[i][1],
			       p[0], p[1], p[2], p[3]);
		}
	}

	/* report: one character per 16x16 block: R red, G green, ? untouched, x other */
	for (uint32_t by = 0; by < H; by += 16) {
		for (uint32_t bx = 0; bx < W; bx += 16) {
			uint8_t *p = px + ((by + 8) * W + bx + 8) * 4;
			char c = 'x';
			if (p[0] == 0x55 && p[1] == 0x55 && p[2] == 0x55)
				c = '?';
			else if (p[0] > 200 && p[1] < 50 && p[2] < 50)
				c = 'R';
			else if (p[0] < 50 && p[1] > 200 && p[2] < 50)
				c = 'G';
			else if (p[0] < 50 && p[1] < 50 && p[2] > 200)
				c = 'B';
			putchar(c);
		}
		putchar('\n');
	}
	FILE *f = fopen(out, "wb");
	if (f) {
		fprintf(f, "P6\n%u %u\n255\n", W, H);
		for (uint32_t i = 0; i < W * H; i++)
			fwrite(px + i * 4, 1, 3, f);
		fclose(f);
		printf("wrote %s\n", out);
	}
	return 0;
}
