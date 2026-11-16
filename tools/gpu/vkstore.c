/* Compile-only test: create a compute pipeline with 2D rgba8 imageStore on the
 * PowerVR driver (loaded directly as an ICD, e.g. on the noop DRM shim). */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include "st_comp.h"

#define GET(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define CHECK(x) do { VkResult r_ = (x); if (r_) { printf("%s failed: %d\n", #x, r_); return 1; } } while (0)

int main(int argc, char **argv)
{
	void *icd = dlopen(argc > 1 ? argv[1] : "libvulkan_powervr_mesa.so", RTLD_NOW);
	if (!icd) { printf("dlopen: %s\n", dlerror()); return 1; }
	PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
	GET(NULL, vkCreateInstance);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	GET(inst, vkEnumeratePhysicalDevices); GET(inst, vkCreateDevice); GET(inst, vkGetPhysicalDeviceProperties);
	uint32_t n = 1; VkPhysicalDevice pd;
	CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
	if (!n) { printf("no device\n"); return 1; }
	VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s\n", props.deviceName);
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define DGET(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
	DGET(vkCreateShaderModule); DGET(vkCreateDescriptorSetLayout); DGET(vkCreatePipelineLayout); DGET(vkCreateComputePipelines);
	VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(st_comp), .pCode = st_comp };
	VkShaderModule sm; CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));
	VkDescriptorSetLayoutBinding b[3] = {
		{ 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
	VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b };
	VkDescriptorSetLayout dsl; CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
	VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
	VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
		.layout = pl };
	VkPipeline p; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &p));
	printf("compute pipeline with 2D/2D-array rgba8 imageStore and a dynamically indexed UBO compiled\n");
	return 0;
}
