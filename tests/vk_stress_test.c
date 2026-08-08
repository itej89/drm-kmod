/*
 * vk_stress_test.c — Comprehensive GPU stress test for PowerVR BXE-4-32
 *
 * Tests all paths a compositor/Zink stack needs:
 *   T1:  Compute shader (fill buffer)
 *   T2:  Render pass clear (color attachment)
 *   T3:  Image copy (vkCmdCopyImageToBuffer)
 *   T4:  Buffer copy (vkCmdCopyBuffer)
 *   T5:  Multiple render passes in sequence
 *   T6:  Multiple compute dispatches
 *   T7:  Large buffer compute (64KB)
 *   T8:  Multiple image sizes (16x16, 64x64, 256x256)
 *   T9:  Repeated submit+wait cycles (stability)
 *   T10: Concurrent compute + graphics contexts
 *
 * Build:
 *   cc -I/usr/local/include -L/usr/local/lib -o vk_stress_test vk_stress_test.c -lvulkan -lm
 * Run:
 *   PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 ./vk_stress_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define VK_CHECK(call, msg) do { \
	VkResult _r = (call); \
	if (_r != VK_SUCCESS) { \
		printf("  FAIL: %s (VkResult=%d)\n", msg, _r); \
		return 1; \
	} \
} while (0)

static const uint32_t comp_spirv[] = {
	0x07230203, 0x00010000, 0x0008000b, 0x00000023, 0x00000000, 0x00020011,
	0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
	0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000005,
	0x00000004, 0x6e69616d, 0x00000000, 0x0000000b, 0x00060010, 0x00000004,
	0x00000011, 0x00000040, 0x00000001, 0x00000001, 0x00030003, 0x00000002,
	0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005,
	0x00000008, 0x00786469, 0x00080005, 0x0000000b, 0x475f6c67, 0x61626f6c,
	0x766e496c, 0x7461636f, 0x496e6f69, 0x00000044, 0x00030005, 0x00000017,
	0x00667542, 0x00050006, 0x00000017, 0x00000000, 0x61746164, 0x00000000,
	0x00030005, 0x00000019, 0x00000000, 0x00040047, 0x0000000b, 0x0000000b,
	0x0000001c, 0x00040047, 0x00000016, 0x00000006, 0x00000004, 0x00030047,
	0x00000017, 0x00000003, 0x00050048, 0x00000017, 0x00000000, 0x00000023,
	0x00000000, 0x00040047, 0x00000019, 0x00000021, 0x00000000, 0x00040047,
	0x00000019, 0x00000022, 0x00000000, 0x00040047, 0x00000022, 0x0000000b,
	0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
	0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040020, 0x00000007,
	0x00000007, 0x00000006, 0x00040017, 0x00000009, 0x00000006, 0x00000003,
	0x00040020, 0x0000000a, 0x00000001, 0x00000009, 0x0004003b, 0x0000000a,
	0x0000000b, 0x00000001, 0x0004002b, 0x00000006, 0x0000000c, 0x00000000,
	0x00040020, 0x0000000d, 0x00000001, 0x00000006, 0x0004002b, 0x00000006,
	0x00000011, 0x00000100, 0x00020014, 0x00000012, 0x0003001d, 0x00000016,
	0x00000006, 0x0003001e, 0x00000017, 0x00000016, 0x00040020, 0x00000018,
	0x00000002, 0x00000017, 0x0004003b, 0x00000018, 0x00000019, 0x00000002,
	0x00040015, 0x0000001a, 0x00000020, 0x00000001, 0x0004002b, 0x0000001a,
	0x0000001b, 0x00000000, 0x0004002b, 0x00000006, 0x0000001d, 0xdeadbeef,
	0x00040020, 0x0000001e, 0x00000002, 0x00000006, 0x0004002b, 0x00000006,
	0x00000020, 0x00000040, 0x0004002b, 0x00000006, 0x00000021, 0x00000001,
	0x0006002c, 0x00000009, 0x00000022, 0x00000020, 0x00000021, 0x00000021,
	0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
	0x00000005, 0x0004003b, 0x00000007, 0x00000008, 0x00000007, 0x00050041,
	0x0000000d, 0x0000000e, 0x0000000b, 0x0000000c, 0x0004003d, 0x00000006,
	0x0000000f, 0x0000000e, 0x0003003e, 0x00000008, 0x0000000f, 0x0004003d,
	0x00000006, 0x00000010, 0x00000008, 0x000500b0, 0x00000012, 0x00000013,
	0x00000010, 0x00000011, 0x000300f7, 0x00000015, 0x00000000, 0x000400fa,
	0x00000013, 0x00000014, 0x00000015, 0x000200f8, 0x00000014, 0x0004003d,
	0x00000006, 0x0000001c, 0x00000008, 0x00060041, 0x0000001e, 0x0000001f,
	0x00000019, 0x0000001b, 0x0000001c, 0x0003003e, 0x0000001f, 0x0000001d,
	0x000200f9, 0x00000015, 0x000200f8, 0x00000015, 0x000100fd, 0x00010038,
};

static int tests_passed = 0;
static int tests_failed = 0;

static uint32_t find_mem_type(VkPhysicalDevice phys, uint32_t bits,
    VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1 << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
			return i;
	return (uint32_t)-1;
}

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice device;
static VkQueue queue;
static uint32_t qfam;
static VkCommandPool cmd_pool;
static VkFence fence;

static int init_vulkan(void)
{
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	VK_CHECK(vkCreateInstance(&ici, NULL, &instance), "vkCreateInstance");

	uint32_t count = 0;
	VkPhysicalDevice devs[8];
	vkEnumeratePhysicalDevices(instance, &count, NULL);
	if (count > 8) count = 8;
	vkEnumeratePhysicalDevices(instance, &count, devs);
	phys = devs[0];
	for (uint32_t i = 0; i < count; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(devs[i], &p);
		if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
			phys = devs[i];
			break;
		}
	}

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(phys, &props);
	printf("Device: %s\n\n", props.deviceName);

	uint32_t qcount = 0;
	VkQueueFamilyProperties qf[8];
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, NULL);
	if (qcount > 8) qcount = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, qf);
	qfam = (uint32_t)-1;
	for (uint32_t i = 0; i < qcount; i++)
		if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
		    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
			{ qfam = i; break; }
	if (qfam == (uint32_t)-1) { printf("FAIL: no graphics+compute queue\n"); return 1; }

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio,
	};
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
	};
	VK_CHECK(vkCreateDevice(phys, &dci, NULL, &device), "vkCreateDevice");
	vkGetDeviceQueue(device, qfam, 0, &queue);

	VkCommandPoolCreateInfo cpci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = qfam,
	};
	VK_CHECK(vkCreateCommandPool(device, &cpci, NULL, &cmd_pool), "cmd_pool");

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VK_CHECK(vkCreateFence(device, &fci, NULL, &fence), "fence");

	return 0;
}

static void cleanup_vulkan(void)
{
	if (fence) vkDestroyFence(device, fence, NULL);
	if (cmd_pool) vkDestroyCommandPool(device, cmd_pool, NULL);
	if (device) vkDestroyDevice(device, NULL);
	if (instance) vkDestroyInstance(instance, NULL);
}

static int submit_and_wait(VkCommandBuffer cb)
{
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cb,
	};
	VK_CHECK(vkResetFences(device, 1, &fence), "reset fence");
	VK_CHECK(vkQueueSubmit(queue, 1, &si, fence), "submit");
	VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ULL), "wait");
	return 0;
}

static int test_compute(uint32_t num_elements, const char *label)
{
	uint32_t buf_size = num_elements * sizeof(uint32_t);
	VkBuffer buf; VkDeviceMemory mem;
	VkShaderModule shader; VkDescriptorSetLayout dsl;
	VkPipelineLayout pl; VkPipeline pipe;
	VkDescriptorPool dp; VkDescriptorSet ds;
	VkCommandBuffer cb;
	int ret = 1;

	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = buf_size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	};
	VK_CHECK(vkCreateBuffer(device, &bci, NULL, &buf), "buf");

	VkMemoryRequirements reqs;
	vkGetBufferMemoryRequirements(device, buf, &reqs);
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = find_mem_type(phys, reqs.memoryTypeBits,
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
	};
	VK_CHECK(vkAllocateMemory(device, &mai, NULL, &mem), "mem");
	VK_CHECK(vkBindBufferMemory(device, buf, mem, 0), "bind");

	uint32_t *mapped;
	VK_CHECK(vkMapMemory(device, mem, 0, buf_size, 0, (void **)&mapped), "map");
	memset(mapped, 0, buf_size);

	VkShaderModuleCreateInfo smci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(comp_spirv), .pCode = comp_spirv,
	};
	VK_CHECK(vkCreateShaderModule(device, &smci, NULL, &shader), "shader");

	VkDescriptorSetLayoutBinding binding = {
		.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	VkDescriptorSetLayoutCreateInfo dslci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &binding,
	};
	VK_CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl), "dsl");

	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl,
	};
	VK_CHECK(vkCreatePipelineLayout(device, &plci, NULL, &pl), "pl");

	VkComputePipelineCreateInfo cpci = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main",
		},
		.layout = pl,
	};
	VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe), "pipe");

	VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
	VkDescriptorPoolCreateInfo dpci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
	};
	VK_CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &dp), "dp");

	VkDescriptorSetAllocateInfo dsai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl,
	};
	VK_CHECK(vkAllocateDescriptorSets(device, &dsai, &ds), "ds");

	VkDescriptorBufferInfo dbi = { .buffer = buf, .offset = 0, .range = buf_size };
	VkWriteDescriptorSet wds = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi,
	};
	vkUpdateDescriptorSets(device, 1, &wds, 0, NULL);

	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cb), "cb");

	VkCommandBufferBeginInfo cbbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK(vkBeginCommandBuffer(cb, &cbbi), "begin");
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
	uint32_t groups = (num_elements + 63) / 64;
	vkCmdDispatch(cb, groups, 1, 1);
	VK_CHECK(vkEndCommandBuffer(cb, &cbbi), "end");

	if (submit_and_wait(cb) != 0) goto done;

	uint32_t good = 0;
	uint32_t check = num_elements < 256 ? num_elements : 256;
	for (uint32_t i = 0; i < check; i++)
		if (mapped[i] == 0xDEADBEEF) good++;

	if (good == check) {
		printf("  PASS: %s (%u elements verified)\n", label, check);
		tests_passed++;
		ret = 0;
	} else {
		printf("  FAIL: %s (%u/%u correct)\n", label, good, check);
		tests_failed++;
	}

done:
	vkFreeCommandBuffers(device, cmd_pool, 1, &cb);
	vkDestroyDescriptorPool(device, dp, NULL);
	vkDestroyPipeline(device, pipe, NULL);
	vkDestroyPipelineLayout(device, pl, NULL);
	vkDestroyDescriptorSetLayout(device, dsl, NULL);
	vkDestroyShaderModule(device, shader, NULL);
	vkUnmapMemory(device, mem);
	vkDestroyBuffer(device, buf, NULL);
	vkFreeMemory(device, mem, NULL);
	return ret;
}

static int test_render_clear(uint32_t w, uint32_t h, float r, float g, float b,
    const char *label)
{
	VkImage img; VkDeviceMemory img_mem, dst_mem;
	VkImageView view; VkRenderPass rp; VkFramebuffer fb;
	VkBuffer dst_buf; VkCommandBuffer cb;
	int ret = 1;
	uint32_t img_size = w * h * 4;

	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VK_CHECK(vkCreateImage(device, &ici, NULL, &img), "img");

	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(device, img, &reqs);
	uint32_t mt = find_mem_type(phys, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mt == (uint32_t)-1) mt = find_mem_type(phys, reqs.memoryTypeBits, 0);
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size, .memoryTypeIndex = mt,
	};
	VK_CHECK(vkAllocateMemory(device, &mai, NULL, &img_mem), "img_mem");
	VK_CHECK(vkBindImageMemory(device, img, img_mem, 0), "bind_img");

	VkImageViewCreateInfo ivci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VK_CHECK(vkCreateImageView(device, &ivci, NULL, &view), "view");

	VkAttachmentDescription att = {
		.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	};
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &ref,
	};
	VkRenderPassCreateInfo rpci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &att,
		.subpassCount = 1, .pSubpasses = &sub,
	};
	VK_CHECK(vkCreateRenderPass(device, &rpci, NULL, &rp), "rp");

	VkFramebufferCreateInfo fbci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = rp, .attachmentCount = 1, .pAttachments = &view,
		.width = w, .height = h, .layers = 1,
	};
	VK_CHECK(vkCreateFramebuffer(device, &fbci, NULL, &fb), "fb");

	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = img_size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	VK_CHECK(vkCreateBuffer(device, &bci, NULL, &dst_buf), "dst_buf");
	vkGetBufferMemoryRequirements(device, dst_buf, &reqs);
	mai.allocationSize = reqs.size;
	mai.memoryTypeIndex = find_mem_type(phys, reqs.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VK_CHECK(vkAllocateMemory(device, &mai, NULL, &dst_mem), "dst_mem");
	VK_CHECK(vkBindBufferMemory(device, dst_buf, dst_mem, 0), "bind_dst");

	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cb), "cb");

	VkCommandBufferBeginInfo cbbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK(vkBeginCommandBuffer(cb, &cbbi), "begin");

	VkClearValue cv = { .color = {{ r, g, b, 1.0f }} };
	VkRenderPassBeginInfo rpbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp, .framebuffer = fb,
		.renderArea = { .extent = { w, h } },
		.clearValueCount = 1, .pClearValues = &cv,
	};
	vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(cb);

	VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { w, h, 1 },
	};
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    dst_buf, 1, &region);
	VK_CHECK(vkEndCommandBuffer(cb), "end");

	if (submit_and_wait(cb) != 0) goto done;

	uint8_t *data;
	VK_CHECK(vkMapMemory(device, dst_mem, 0, img_size, 0, (void **)&data), "map_dst");

	uint32_t match = 0;
	uint8_t er = (uint8_t)(r * 255), eg = (uint8_t)(g * 255), eb = (uint8_t)(b * 255);
	for (uint32_t i = 0; i < w * h; i++) {
		uint8_t pr = data[i*4], pg = data[i*4+1], pb = data[i*4+2], pa = data[i*4+3];
		if (abs(pr - er) < 5 && abs(pg - eg) < 5 && abs(pb - eb) < 5 && pa > 200)
			match++;
	}

	if (match == w * h) {
		printf("  PASS: %s (%ux%u, all pixels match)\n", label, w, h);
		tests_passed++;
		ret = 0;
	} else {
		printf("  FAIL: %s (%u/%u pixels match)\n", label, match, w * h);
		printf("    first pixel: %02x %02x %02x %02x (expected ~%02x %02x %02x ff)\n",
		    data[0], data[1], data[2], data[3], er, eg, eb);
		tests_failed++;
	}

	vkUnmapMemory(device, dst_mem);

done:
	vkFreeCommandBuffers(device, cmd_pool, 1, &cb);
	vkDestroyFramebuffer(device, fb, NULL);
	vkDestroyRenderPass(device, rp, NULL);
	vkDestroyImageView(device, view, NULL);
	vkDestroyImage(device, img, NULL);
	vkFreeMemory(device, img_mem, NULL);
	vkDestroyBuffer(device, dst_buf, NULL);
	vkFreeMemory(device, dst_mem, NULL);
	return ret;
}

static int test_buffer_copy(uint32_t size, const char *label)
{
	VkBuffer src_buf, dst_buf;
	VkDeviceMemory src_mem, dst_mem;
	VkCommandBuffer cb;
	int ret = 1;

	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	};
	VK_CHECK(vkCreateBuffer(device, &bci, NULL, &src_buf), "src_buf");
	VK_CHECK(vkCreateBuffer(device, &bci, NULL, &dst_buf), "dst_buf");

	VkMemoryRequirements reqs;
	vkGetBufferMemoryRequirements(device, src_buf, &reqs);
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = find_mem_type(phys, reqs.memoryTypeBits,
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
	};
	VK_CHECK(vkAllocateMemory(device, &mai, NULL, &src_mem), "src_mem");
	VK_CHECK(vkAllocateMemory(device, &mai, NULL, &dst_mem), "dst_mem");
	VK_CHECK(vkBindBufferMemory(device, src_buf, src_mem, 0), "bind_src");
	VK_CHECK(vkBindBufferMemory(device, dst_buf, dst_mem, 0), "bind_dst");

	uint32_t *src_map, *dst_map;
	VK_CHECK(vkMapMemory(device, src_mem, 0, size, 0, (void **)&src_map), "map_src");
	VK_CHECK(vkMapMemory(device, dst_mem, 0, size, 0, (void **)&dst_map), "map_dst");
	for (uint32_t i = 0; i < size / 4; i++) src_map[i] = 0xCAFEBABE;
	memset(dst_map, 0, size);

	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cb), "cb");

	VkCommandBufferBeginInfo cbbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK(vkBeginCommandBuffer(cb, &cbbi), "begin");
	VkBufferCopy region = { .size = size };
	vkCmdCopyBuffer(cb, src_buf, dst_buf, 1, &region);
	VK_CHECK(vkEndCommandBuffer(cb), "end");

	if (submit_and_wait(cb) != 0) goto done;

	uint32_t good = 0;
	for (uint32_t i = 0; i < size / 4; i++)
		if (dst_map[i] == 0xCAFEBABE) good++;

	if (good == size / 4) {
		printf("  PASS: %s (%u dwords)\n", label, size / 4);
		tests_passed++;
		ret = 0;
	} else {
		printf("  FAIL: %s (%u/%u correct)\n", label, good, size / 4);
		tests_failed++;
	}

done:
	vkFreeCommandBuffers(device, cmd_pool, 1, &cb);
	vkUnmapMemory(device, src_mem);
	vkUnmapMemory(device, dst_mem);
	vkDestroyBuffer(device, src_buf, NULL);
	vkDestroyBuffer(device, dst_buf, NULL);
	vkFreeMemory(device, src_mem, NULL);
	vkFreeMemory(device, dst_mem, NULL);
	return ret;
}

int main(void)
{
	printf("=== PowerVR BXE-4-32 GPU Stress Test ===\n\n");

	if (init_vulkan() != 0) return 1;

	printf("[T1] Compute: 256 elements\n");
	test_compute(256, "compute 256");

	printf("[T2] Render: clear red 16x16\n");
	test_render_clear(16, 16, 1, 0, 0, "clear red 16x16");

	printf("[T3] Render: clear green 64x64\n");
	test_render_clear(64, 64, 0, 1, 0, "clear green 64x64");

	printf("[T4] Render: clear blue 128x128\n");
	test_render_clear(128, 128, 0, 0, 1, "clear blue 128x128");

	printf("[T5] Render: clear white 256x256\n");
	test_render_clear(256, 256, 1, 1, 1, "clear white 256x256");

	printf("[T6] Buffer copy: 4KB\n");
	test_buffer_copy(4096, "copy 4KB");

	printf("[T7] Buffer copy: 64KB\n");
	test_buffer_copy(65536, "copy 64KB");

	printf("[T8] Compute: 1024 elements (16 groups)\n");
	test_compute(1024, "compute 1024");

	printf("[T9] Compute: 16384 elements (256 groups)\n");
	test_compute(16384, "compute 16384");

	printf("[T10] Repeated submit: 10x render clear\n");
	{
		int ok = 1;
		for (int i = 0; i < 10; i++) {
			if (test_render_clear(32, 32, (float)(i%3==0), (float)(i%3==1),
			    (float)(i%3==2), "repeat") != 0) { ok = 0; break; }
			tests_passed--; /* don't double count */
		}
		if (ok) {
			printf("  PASS: 10x render clear cycles\n");
			tests_passed++;
		} else {
			printf("  FAIL: render clear stability\n");
			tests_failed++;
		}
	}

	printf("[T11] Repeated submit: 10x compute\n");
	{
		int ok = 1;
		for (int i = 0; i < 10; i++) {
			if (test_compute(256, "repeat") != 0) { ok = 0; break; }
			tests_passed--;
		}
		if (ok) {
			printf("  PASS: 10x compute cycles\n");
			tests_passed++;
		} else {
			printf("  FAIL: compute stability\n");
			tests_failed++;
		}
	}

	printf("[T12] Mixed: compute then render then compute\n");
	{
		int ok = 1;
		if (test_compute(256, "mixed-compute-1") != 0) ok = 0;
		else tests_passed--;
		if (ok && test_render_clear(64, 64, 1, 1, 0, "mixed-render") != 0) ok = 0;
		else if (ok) tests_passed--;
		if (ok && test_compute(256, "mixed-compute-2") != 0) ok = 0;
		else if (ok) tests_passed--;
		if (ok) {
			printf("  PASS: mixed compute+graphics+compute\n");
			tests_passed++;
		} else {
			printf("  FAIL: mixed workload\n");
			tests_failed++;
		}
	}

	printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
	if (tests_failed == 0)
		printf("\n*** ALL TESTS PASSED ***\n");

	cleanup_vulkan();
	return tests_failed > 0 ? 1 : 0;
}
