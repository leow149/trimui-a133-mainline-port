// Mimic RetroArch's VULKAN_TEXTURE_STREAMED path (linear, host-visible, persistently
// mapped image, CPU-written each frame, sampled directly) and compare with the result.
// Usage: vklinear [key=value ...]
//   fmt=bgra|rgba|565   texture format (default bgra)
//   w=256 h=224         texture size
//   mode=linear|staging|compute  (compute = what RA really does for RGB565: SSBO -> compute shader -> rgba8 storage image)
//   [old:] mode=linear|staging|optimal-buf   linear = RetroArch streamed path; staging = buffer->optimal copy (control)
//   layout=preinit|undef      initialLayout of the linear image (default preinit, as RetroArch)
//   barrier=full|none|noimg   host->shader barrier style (full = layout transition PREINIT->GENERAL like RA)
//   flush=0|1           call vkFlushMappedMemoryRanges after CPU write
//   frames=3            number of frames (pattern changes per frame)
//   filter=nearest|linear
//   mip=0|1             nothing
//   rt=optimal|linear   render target tiling; linear = CPU reads it directly (no copy job)
//   delay_ms=N dummy=N repeat=N   extra experiments, see code
//   texfmt=rgba8|r32  storage image format in compute mode (r32 only for cs=pt32 diagnostics)
//   cs=ra|const|ssbo|ids|pt|pt32|fix|dyn|atom|load   compute shader for mode=compute (ra = RetroArch's rgb565_to_rgba8888.comp; const/ssbo = diagnostics)
//   dump=file.ppm       write last frame
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "lin_vert.h"
#include "lin_frag.h"
#include "rgb565_comp.h"
#include "cs_const.h"
#include "cs_ssbo.h"
#include "cs_ids.h"
#include "cs_pt.h"
#include "cs_pt32.h"
#include "cs_fix.h"
#include "cs_atom.h"
#include "cs_load.h"
#include "cs_dyn.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static VkPhysicalDevice pd;
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n"); exit(1);
}
static const char *opt(int argc, char **argv, const char *k, const char *def)
{
	size_t n = strlen(k);
	for (int i = 1; i < argc; i++)
		if (!strncmp(argv[i], k, n) && argv[i][n] == '=') return argv[i] + n + 1;
	return def;
}

int main(int argc, char **argv)
{
	const char *sfmt = opt(argc, argv, "fmt", "bgra");
	uint32_t W = atoi(opt(argc, argv, "w", "256")), H = atoi(opt(argc, argv, "h", "224"));
	const char *mode = opt(argc, argv, "mode", "linear");
	const char *slayout = opt(argc, argv, "layout", "preinit");
	const char *sbar = opt(argc, argv, "barrier", "full");
	int flush = atoi(opt(argc, argv, "flush", "0"));
	int frames = atoi(opt(argc, argv, "frames", "3"));
	int delay = atoi(opt(argc, argv, "delay_ms", "0")), dummy = atoi(opt(argc, argv, "dummy", "0"));  /* sleep before submit; N extra clear-only submits */
	int cdbg = atoi(opt(argc, argv, "cdbg", "0"));   /* compute mode: also copy the storage image to a host buffer after the dispatch and report it */
	int repeat = atoi(opt(argc, argv, "repeat", "1"));   /* resubmit the same frame N times */
	int linfilter = !strcmp(opt(argc, argv, "filter", "nearest"), "linear");
	int rtlin = !strcmp(opt(argc, argv, "rt", "optimal"), "linear");
	const char *dump = opt(argc, argv, "dump", NULL);
	int compute = !strcmp(mode, "compute");
	int staging = !strcmp(mode, "staging") || compute;
	VkFormat fmt = !strcmp(sfmt, "565") ? VK_FORMAT_R5G6B5_UNORM_PACK16 :
		       !strcmp(sfmt, "rgba") ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
	unsigned bpp = fmt == VK_FORMAT_R5G6B5_UNORM_PACK16 ? 2 : 4;
	VkFormat texfmt = compute ? (!strcmp(opt(argc, argv, "texfmt", "rgba8"), "r32") ? VK_FORMAT_R32_UINT : VK_FORMAT_R8G8B8A8_UNORM) : fmt;	/* format of the sampled image */
	if (compute && bpp != 2) { fprintf(stderr, "mode=compute needs fmt=565\n"); return 1; }

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst; CHECK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VkDevice dev; CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

	VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, fmt, &fp);
	printf("fmt %s linearFeatures 0x%x optimalFeatures 0x%x\n", sfmt, fp.linearTilingFeatures, fp.optimalTilingFeatures);

	/* ---- source texture ---- */
	VkImageCreateInfo ti = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = fmt, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkImage timg; VkDeviceMemory tmem; uint8_t *tmap = NULL;
	VkSubresourceLayout lay = { 0 };
	VkBuffer sbuf = VK_NULL_HANDLE; VkDeviceMemory smem = VK_NULL_HANDLE; uint8_t *smap = NULL;
	VkMemoryRequirements mr;
	if (!staging) {
		ti.tiling = VK_IMAGE_TILING_LINEAR;
		ti.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		ti.initialLayout = !strcmp(slayout, "undef") ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_PREINITIALIZED;
		CHECK(vkCreateImage(dev, &ti, NULL, &timg));
		vkGetImageMemoryRequirements(dev, timg, &mr);
		VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
			.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) };
		CHECK(vkAllocateMemory(dev, &mai, NULL, &tmem));
		CHECK(vkBindImageMemory(dev, timg, tmem, 0));
		VkImageSubresource isr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
		vkGetImageSubresourceLayout(dev, timg, &isr, &lay);
		printf("linear image: memsize %llu align %llu  layout off %llu size %llu rowPitch %llu (tight %u)\n",
		       (unsigned long long)mr.size, (unsigned long long)mr.alignment, (unsigned long long)lay.offset,
		       (unsigned long long)lay.size, (unsigned long long)lay.rowPitch, W * bpp);
		CHECK(vkMapMemory(dev, tmem, lay.offset, lay.size, 0, (void **)&tmap));
	} else {
		ti.tiling = VK_IMAGE_TILING_OPTIMAL;
		ti.format = texfmt;
		ti.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			   (compute ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
		ti.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		CHECK(vkCreateImage(dev, &ti, NULL, &timg));
		vkGetImageMemoryRequirements(dev, timg, &mr);
		VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
			.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0) };
		CHECK(vkAllocateMemory(dev, &mai, NULL, &tmem));
		CHECK(vkBindImageMemory(dev, timg, tmem, 0));
		VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)((W * bpp + 3) & ~3u) * H,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
		CHECK(vkCreateBuffer(dev, &bci, NULL, &sbuf));
		vkGetBufferMemoryRequirements(dev, sbuf, &mr);
		mai.allocationSize = mr.size; mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		CHECK(vkAllocateMemory(dev, &mai, NULL, &smem));
		CHECK(vkBindBufferMemory(dev, sbuf, smem, 0));
		CHECK(vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, (void **)&smap));
		lay.rowPitch = (W * bpp + 3) & ~3u;
	}
	VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = timg,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = texfmt,
		.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A },
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView tview; CHECK(vkCreateImageView(dev, &ivci, NULL, &tview));
	VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = linfilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .minFilter = linfilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 1.0f };
	VkSampler samp; CHECK(vkCreateSampler(dev, &sci, NULL, &samp));

	/* ---- render target + readback ---- */
	VkFormat rfmt = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = rfmt,
		.extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = rtlin ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
	VkImage rimg; CHECK(vkCreateImage(dev, &ri, NULL, &rimg));
	vkGetImageMemoryRequirements(dev, rimg, &mr);
	VkMemoryAllocateInfo rmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits, rtlin ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0) };
	VkDeviceMemory rmem; CHECK(vkAllocateMemory(dev, &rmai, NULL, &rmem));
	CHECK(vkBindImageMemory(dev, rimg, rmem, 0));
	uint8_t *rlin = NULL; VkSubresourceLayout rlay = { .rowPitch = W * 4 };
	if (rtlin) {
		VkImageSubresource isr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
		vkGetImageSubresourceLayout(dev, rimg, &isr, &rlay);
		CHECK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, (void **)&rlin));
		printf("linear RT rowPitch %llu\n", (unsigned long long)rlay.rowPitch);
	}
	ivci.image = rimg; ivci.format = rfmt;
	VkImageView rview; CHECK(vkCreateImageView(dev, &ivci, NULL, &rview));
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)W * H * 4,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer rbuf; CHECK(vkCreateBuffer(dev, &bci, NULL, &rbuf));
	vkGetBufferMemoryRequirements(dev, rbuf, &mr);
	rmai.allocationSize = mr.size; rmai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	VkDeviceMemory rbmem; CHECK(vkAllocateMemory(dev, &rmai, NULL, &rbmem));
	CHECK(vkBindBufferMemory(dev, rbuf, rbmem, 0));
	uint8_t *rpx; CHECK(vkMapMemory(dev, rbmem, 0, VK_WHOLE_SIZE, 0, (void **)&rpx));

	VkAttachmentDescription att = { .format = rfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = rtlin ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &aref };
	VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sp };
	VkRenderPass rp; CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &rview, .width = W, .height = H, .layers = 1 };
	VkFramebuffer fb; CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

	/* descriptors */
	VkDescriptorSetLayoutBinding dlb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &dlb };
	VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
	VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
	VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
	VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet ds; CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	VkDescriptorImageInfo dii = { samp, tview, staging ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
	vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(lin_vert), .pCode = lin_vert };
	VkShaderModule vs, fs; CHECK(vkCreateShaderModule(dev, &smci, NULL, &vs));
	smci.codeSize = sizeof(lin_frag); smci.pCode = lin_frag; CHECK(vkCreateShaderModule(dev, &smci, NULL, &fs));
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
	VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, (float)W, (float)H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
		.pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
		.pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp };
	VkPipeline pipe; CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cmd, dcmd; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd)); CHECK(vkAllocateCommandBuffers(dev, &cbai, &dcmd));
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence; CHECK(vkCreateFence(dev, &fci, NULL, &fence));

	/* compute path objects (RA: vulkan_copy_staging_to_dynamic, compute_upload branch) */
	VkDescriptorSet cds = VK_NULL_HANDLE; VkPipeline cpipe = VK_NULL_HANDLE; VkPipelineLayout cpl = VK_NULL_HANDLE; VkBuffer ubo = VK_NULL_HANDLE; uint32_t *uboptr = NULL;
	if (compute) {
		VkDescriptorSetLayoutBinding cb2[3] = {
			{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
			{ 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
			{ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
		VkDescriptorSetLayoutCreateInfo cdl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = cb2 };
		VkDescriptorSetLayout cdsl; CHECK(vkCreateDescriptorSetLayout(dev, &cdl, NULL, &cdsl));
		VkDescriptorPoolSize cps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
		VkDescriptorPoolCreateInfo cpc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = cps };
		VkDescriptorPool cdp; CHECK(vkCreateDescriptorPool(dev, &cpc, NULL, &cdp));
		VkDescriptorSetAllocateInfo cda = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = cdp, .descriptorSetCount = 1, .pSetLayouts = &cdsl };
		CHECK(vkAllocateDescriptorSets(dev, &cda, &cds));
		VkPipelineLayoutCreateInfo cpli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &cdsl };
		CHECK(vkCreatePipelineLayout(dev, &cpli, NULL, &cpl));
		const char *scs = opt(argc, argv, "cs", "ra");
		VkShaderModuleCreateInfo csm = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(rgb565_comp), .pCode = rgb565_comp };
		if (!strcmp(scs, "const")) { csm.codeSize = sizeof(cs_const); csm.pCode = cs_const; }
		if (!strcmp(scs, "atom")) { csm.codeSize = sizeof(cs_atom); csm.pCode = cs_atom; }
		if (!strcmp(scs, "load")) { csm.codeSize = sizeof(cs_load); csm.pCode = cs_load; }
		if (!strcmp(scs, "fix")) { csm.codeSize = sizeof(cs_fix); csm.pCode = cs_fix; }
		if (!strcmp(scs, "dyn")) { csm.codeSize = sizeof(cs_dyn); csm.pCode = cs_dyn; }
		if (!strcmp(scs, "pt32")) { csm.codeSize = sizeof(cs_pt32); csm.pCode = cs_pt32; }
		if (!strcmp(scs, "pt")) { csm.codeSize = sizeof(cs_pt); csm.pCode = cs_pt; }
		if (!strcmp(scs, "ids")) { csm.codeSize = sizeof(cs_ids); csm.pCode = cs_ids; }
		if (!strcmp(scs, "ssbo")) { csm.codeSize = sizeof(cs_ssbo); csm.pCode = cs_ssbo; }
		VkShaderModule cs; CHECK(vkCreateShaderModule(dev, &csm, NULL, &cs));
		VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = cpl,
			.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main" } };
		CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &cpipe));
		VkBufferCreateInfo ubci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT };
		CHECK(vkCreateBuffer(dev, &ubci, NULL, &ubo));
		VkMemoryRequirements umr; vkGetBufferMemoryRequirements(dev, ubo, &umr);
		VkMemoryAllocateInfo uma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = umr.size, .memoryTypeIndex = mem_type(umr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) };
		VkDeviceMemory umem; CHECK(vkAllocateMemory(dev, &uma, NULL, &umem));
		CHECK(vkBindBufferMemory(dev, ubo, umem, 0));
		CHECK(vkMapMemory(dev, umem, 0, VK_WHOLE_SIZE, 0, (void **)&uboptr));
		uboptr[0] = W; uboptr[1] = H; uboptr[2] = lay.rowPitch / 4;
		/* storage image view must be identity-swizzled RGBA8 */
		VkDescriptorImageInfo sii = { VK_NULL_HANDLE, tview, VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorBufferInfo ubi = { ubo, 0, 12 }, sbi = { sbuf, 0, VK_WHOLE_SIZE };
		VkWriteDescriptorSet w3[3] = {
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = cds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ubi },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = cds, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &sii },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = cds, .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &sbi } };
		vkUpdateDescriptorSets(dev, 3, w3, 0, NULL);
	}
	VkBuffer cbuf = VK_NULL_HANDLE; uint8_t *cpx = NULL;
	if (cdbg) {
		VkBufferCreateInfo cbi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
		CHECK(vkCreateBuffer(dev, &cbi, NULL, &cbuf));
		VkMemoryRequirements cmr; vkGetBufferMemoryRequirements(dev, cbuf, &cmr);
		VkMemoryAllocateInfo cma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = cmr.size, .memoryTypeIndex = mem_type(cmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) };
		VkDeviceMemory cmem; CHECK(vkAllocateMemory(dev, &cma, NULL, &cmem));
		CHECK(vkBindBufferMemory(dev, cbuf, cmem, 0));
		CHECK(vkMapMemory(dev, cmem, 0, VK_WHOLE_SIZE, 0, (void **)&cpx));
	}
	VkImageLayout curlayout = staging ? VK_IMAGE_LAYOUT_UNDEFINED : ti.initialLayout;
	int total_bad = 0;
	uint32_t *exp = malloc(W * H * 4);
	for (int f = 0; f < frames; f++) {
		/* CPU write: pattern depends on the frame */
		uint8_t *dst = staging ? smap : tmap;
		uint32_t pitch = lay.rowPitch;
		for (uint32_t y = 0; y < H; y++)
			for (uint32_t x = 0; x < W; x++) {
				uint8_t r = (x * 255 / (W - 1) + f * 40), g = (y * 255 / (H - 1)), b = ((x ^ y) * 4 + f * 17);
				uint8_t *p = dst + y * pitch + x * bpp;
				if (bpp == 2) {
					uint16_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
					memcpy(p, &v, 2);
					exp[y * W + x] = (((v >> 11) & 31) * 255 + 15) / 31 | ((((v >> 5) & 63) * 255 + 31) / 63) << 8 | ((((v) & 31) * 255 + 15) / 31) << 16 | 0xffu << 24;
				} else if (fmt == VK_FORMAT_B8G8R8A8_UNORM) {
					p[0] = b; p[1] = g; p[2] = r; p[3] = 0xff;
					exp[y * W + x] = r | g << 8 | b << 16 | 0xffu << 24;
				} else {
					p[0] = r; p[1] = g; p[2] = b; p[3] = 0xff;
					exp[y * W + x] = r | g << 8 | b << 16 | 0xffu << 24;
				}
			}
		if (flush) {
			VkMappedMemoryRange mrg = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = staging ? smem : tmem, .offset = 0, .size = VK_WHOLE_SIZE };
			CHECK(vkFlushMappedMemoryRanges(dev, 1, &mrg));
		}

		for (int rep = 0; rep < repeat; rep++) {
		CHECK(vkResetCommandBuffer(cmd, 0));
		VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		CHECK(vkBeginCommandBuffer(cmd, &cbbi));
		VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		if (compute) {
			VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = timg,
				.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpl, 0, 1, &cds, 0, NULL);
			vkCmdDispatch(cmd, (W + 15) / 16, (H + 7) / 8, 1);
			if (cdbg) {	/* GENERAL -> copy storage image straight to a host buffer */
				VkMemoryBarrier cm = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &cm, 0, NULL, 0, NULL);
				VkBufferImageCopy cr = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
				vkCmdCopyImageToBuffer(cmd, timg, VK_IMAGE_LAYOUT_GENERAL, cbuf, 1, &cr);
				VkMemoryBarrier cm2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT };
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cm2, 0, NULL, 0, NULL);
			}
			VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b2.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
		} else if (staging) {
			VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = timg, .subresourceRange = rng };
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
			VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
			vkCmdCopyBufferToImage(cmd, sbuf, timg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
			VkImageMemoryBarrier b2 = b1; b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
		} else if (!strcmp(sbar, "full")) {
			/* exactly vulkan_transition_texture(): PREINIT/GENERAL -> GENERAL, HOST_WRITE -> SHADER_READ */
			VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				.oldLayout = curlayout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = timg,
				.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
			curlayout = VK_IMAGE_LAYOUT_GENERAL;
		} else if (!strcmp(sbar, "mem")) {
			VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
		}
		VkClearValue cv = { .color = { .float32 = { 1, 0, 1, 1 } } };
		VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
			.renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &cv };
		vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRenderPass(cmd);
		VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
		if (!rtlin) vkCmdCopyImageToBuffer(cmd, rimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &reg);
		VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = rtlin ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
		vkCmdPipelineBarrier(cmd, rtlin ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
		CHECK(vkEndCommandBuffer(cmd));
		memset(rpx, 0x55, W * H * 4); if (cpx) memset(cpx, 0x55, W * H * 4);
		if (rtlin) for (uint32_t y = 0; y < H; y++) memset(rlin + rlay.offset + y * rlay.rowPitch, 0x55, W * 4);
		if (delay) usleep(delay * 1000);
		for (int d = 0; d < dummy; d++) {	/* clear-only render pass, no texture sampling */
			CHECK(vkResetCommandBuffer(dcmd, 0));
			CHECK(vkBeginCommandBuffer(dcmd, &cbbi));
			vkCmdBeginRenderPass(dcmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdEndRenderPass(dcmd);
			CHECK(vkEndCommandBuffer(dcmd));
			VkSubmitInfo dsi = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &dcmd };
			CHECK(vkResetFences(dev, 1, &fence));
			CHECK(vkQueueSubmit(q, 1, &dsi, fence));
			CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
		}
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
		CHECK(vkResetFences(dev, 1, &fence));
		CHECK(vkQueueSubmit(q, 1, &si, fence));
		VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);

		if (rtlin) for (uint32_t y = 0; y < H; y++) memcpy(rpx + y * W * 4, rlin + rlay.offset + y * rlay.rowPitch, W * 4);
		if (cdbg && staging) {	/* raw view of the optimal image's backing memory: where did the stores land? */
			static uint32_t *raw; static size_t rawsz;
			if (!raw) { VkMemoryRequirements imr; vkGetImageMemoryRequirements(dev, timg, &imr); rawsz = imr.size; CHECK(vkMapMemory(dev, tmem, 0, VK_WHOLE_SIZE, 0, (void **)&raw)); }
			unsigned nzr = 0;
			for (size_t i = 0; i < rawsz / 4; i++) if (raw[i]) { if (nzr < 8) printf("  raw image mem word %zu (byte %zu) = %08x\n", i, i * 4, raw[i]); nzr++; }
			printf("  raw image memory: %u non-zero words of %zu\n", nzr, rawsz / 4);
		}
		if (cdbg && !strcmp(opt(argc, argv, "cs", "ra"), "ids")) {
			uint32_t *w = (uint32_t *)smap; unsigned nzw = 0, okw = 0;
			for (uint32_t i = 0; i < W * H && i * 4 < (uint32_t)((W * bpp + 3) / 4 * 4) * H; i++) { if (w[i]) nzw++; if (w[i] == i + 1) okw++; }
			printf("  SSBO writes: %u non-zero words, %u == id+1 (of %u)\n", nzw, okw, W * H);
			for (int i = 0; i < 6; i++) { uint32_t v; memcpy(&v, cpx + i * 4, 4); printf("  img px %d = %08x\n", i, v); }
		}
		if (cdbg) {
			unsigned nz = 0, cbad = 0;
			for (uint32_t i = 0; i < W * H; i++) { uint32_t v; memcpy(&v, cpx + i * 4, 4); if (v) { if (nz < 6) printf("  nonzero storage px %u (%u,%u) = %08x\n", i, i % W, i / W, v); nz++; } if ((v ^ exp[i]) & 0xffffff) cbad++; }
			printf("  storage image after dispatch (copy to host): %u/%u non-zero, %u/%u differ from expected\n", nz, W * H, cbad, W * H);
		}
		unsigned bad = 0, black = 0, magenta = 0; unsigned fx = 0, fy = 0;
		for (uint32_t y = 0; y < H; y++)
			for (uint32_t x = 0; x < W; x++) {
				uint8_t *p = rpx + (y * W + x) * 4; uint32_t e = exp[y * W + x];
				int d = 0;
				for (int c = 0; c < 3; c++) { int dv = abs((int)p[c] - (int)((e >> (8 * c)) & 255)); if (dv > (linfilter ? 24 : (bpp == 2 ? 2 : 0))) d = 1; }
				if (!p[0] && !p[1] && !p[2]) black++;
				if (p[0] == 255 && !p[1] && p[2] == 255) magenta++;
				if (d) { if (!bad) { fx = x; fy = y; } bad++; }
			}
		uint8_t *p0 = rpx + (fy * W + fx) * 4;
		printf("frame %d.%d fence %d: %u/%u pixels differ (black %u, clear-colour %u)", f, rep, wr, bad, W * H, black, magenta);
		if (bad) printf("; first at (%u,%u): got %02x%02x%02x%02x want %08x", fx, fy, p0[0], p0[1], p0[2], p0[3], exp[fy * W + fx]);
		printf("\n");
		if (rep == repeat - 1) total_bad += bad;
		if (dump && f == frames - 1 && rep == repeat - 1) {
			FILE *fo = fopen(dump, "wb");
			fprintf(fo, "P6\n%u %u\n255\n", W, H);
			for (uint32_t i = 0; i < W * H; i++) fwrite(rpx + i * 4, 1, 3, fo);
			fclose(fo);
		}
		}
	}
	printf("RESULT %s\n", total_bad ? "FAIL" : "PASS");
	return total_bad != 0;
}
