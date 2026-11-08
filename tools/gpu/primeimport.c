// Can the display device (sun4i-drm, card1) import a dmabuf allocated by the GPU (powervr)?
// If not, wlroots can never direct-scan-out a client buffer allocated by the GPU's Vulkan/GL stack
// (the sun4i GEM helper needs DMA-contiguous memory; powervr BOs are scattered shmem pages).
// Usage: primeimport [card=/dev/dri/card1] [size_kb=3600]   (run in the edge chroot)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <vulkan/vulkan.h>

struct prime_fd_to_handle { unsigned int handle, flags; int fd; };
#define DRM_IOCTL_PRIME_FD_TO_HANDLE _IOWR('d', 0x2e, struct prime_fd_to_handle)

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

int main(int argc, char **argv)
{
	const char *card = argc > 1 ? argv[1] : "/dev/dri/card1";
	VkDeviceSize size = (argc > 2 ? atoi(argv[2]) : 3600) * 1024ull;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	VkPhysicalDevice pd;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	const char *ext[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_KHR_external_memory" };
	float prio = 1;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci, .enabledExtensionCount = 3, .ppEnabledExtensionNames = ext };
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkExportMemoryAllocateInfo exp = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exp,
		.allocationSize = size, .memoryTypeIndex = 0 };
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
	PFN_vkGetMemoryFdKHR gfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
	VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
	int fd = -1;
	CHECK(gfd(dev, &gi, &fd));
	int c = open(card, O_RDWR);
	if (c < 0) { perror(card); return 1; }
	struct prime_fd_to_handle a = { .fd = fd };
	int r = ioctl(c, DRM_IOCTL_PRIME_FD_TO_HANDLE, &a);
	printf("powervr dmabuf (%llu KiB) -> %s PRIME_FD_TO_HANDLE: %s (handle %u)\n",
	       (unsigned long long)(size / 1024), card, r ? strerror(errno) : "OK", a.handle);
	return r ? 2 : 0;
}
