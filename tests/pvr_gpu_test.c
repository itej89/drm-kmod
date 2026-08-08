/*
 * pvr_gpu_test.c — Verify PowerVR GPU driver on FreeBSD
 *
 * Tests:
 *   T1: Open /dev/dri/renderD128
 *   T2: DRM_IOCTL_VERSION — read driver name/version
 *   T3: PVR_DEV_QUERY GPU_INFO — read BVNC from driver
 *   T4: PVR_DEV_QUERY RUNTIME_INFO — read runtime params
 *   T5: PVR_CREATE_BO — allocate a GPU buffer object
 *   T6: PVR_CREATE_VM_CONTEXT — create a GPU VM context
 *
 * Build on the board:
 *   cc -o pvr_gpu_test pvr_gpu_test.c -I/usr/drm-kmod/include/uapi -I/usr/drm-kmod/include
 * Run:
 *   ./pvr_gpu_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>

/* DRM base */
#define DRM_IOCTL_BASE		'd'
#define DRM_IOWR(nr, type)	_IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_COMMAND_BASE	0x40

/* DRM_IOCTL_VERSION */
struct drm_version {
	int version_major;
	int version_minor;
	int version_patchlevel;
	unsigned long name_len;
	char *name;
	unsigned long date_len;
	char *date;
	unsigned long desc_len;
	char *desc;
};
#define DRM_IOCTL_VERSION	_IOWR(DRM_IOCTL_BASE, 0x00, struct drm_version)

/* PVR ioctls */
struct drm_pvr_ioctl_dev_query_args {
	__uint32_t type;
	__uint32_t size;
	__uint64_t pointer;
};
#define DRM_IOCTL_PVR_DEV_QUERY \
	_IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + 0x00, struct drm_pvr_ioctl_dev_query_args)

struct drm_pvr_dev_query_gpu_info {
	__uint64_t gpu_id;
	__uint32_t num_phantoms;
	__uint32_t _padding_c;
};

struct drm_pvr_dev_query_runtime_info {
	__uint64_t free_list_min_pages;
	__uint64_t free_list_max_pages;
	__uint32_t common_store_alloc_region_size;
	__uint32_t common_store_partition_space_size;
	__uint32_t max_coeffs;
	__uint32_t cdm_max_local_mem_size_regs;
};

struct drm_pvr_ioctl_create_bo_args {
	__uint64_t size;
	__uint32_t handle;
	__uint32_t _padding_c;
	__uint64_t flags;
};
#define DRM_IOCTL_PVR_CREATE_BO \
	_IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + 0x01, struct drm_pvr_ioctl_create_bo_args)

struct drm_pvr_ioctl_create_vm_context_args {
	__uint32_t handle;
	__uint32_t _padding_4;
};
#define DRM_IOCTL_PVR_CREATE_VM_CONTEXT \
	_IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + 0x03, struct drm_pvr_ioctl_create_vm_context_args)

struct drm_pvr_ioctl_destroy_vm_context_args {
	__uint32_t handle;
	__uint32_t _padding_4;
};
#define DRM_IOCTL_PVR_DESTROY_VM_CONTEXT \
	_IOW(DRM_IOCTL_BASE, DRM_COMMAND_BASE + 0x04, struct drm_pvr_ioctl_destroy_vm_context_args)

#define DRM_PVR_DEV_QUERY_GPU_INFO_GET		0
#define DRM_PVR_DEV_QUERY_RUNTIME_INFO_GET	1

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do { \
	if (cond) { \
		printf("  PASS: %s\n", name); \
		tests_passed++; \
	} else { \
		printf("  FAIL: %s\n", name); \
		tests_failed++; \
	} \
} while (0)

int main(void)
{
	int fd;
	int ret;

	printf("=== PowerVR GPU Driver Test ===\n\n");

	/* T1: Open render node */
	printf("[T1] Open /dev/dri/renderD128\n");
	fd = open("/dev/dri/renderD128", O_RDWR);
	TEST("open renderD128", fd >= 0);
	if (fd < 0) {
		printf("  errno=%d (%s)\n", errno, strerror(errno));
		printf("\n=== ABORT: cannot open device ===\n");
		return 1;
	}

	/* T2: DRM version */
	printf("\n[T2] DRM_IOCTL_VERSION\n");
	{
		struct drm_version ver;
		char name[64] = {0};
		char date[64] = {0};
		char desc[128] = {0};

		memset(&ver, 0, sizeof(ver));
		ver.name_len = sizeof(name);
		ver.name = name;
		ver.date_len = sizeof(date);
		ver.date = date;
		ver.desc_len = sizeof(desc);
		ver.desc = desc;

		ret = ioctl(fd, DRM_IOCTL_VERSION, &ver);
		TEST("ioctl VERSION", ret == 0);
		if (ret == 0) {
			printf("  driver: %s %d.%d.%d\n", name,
			    ver.version_major, ver.version_minor, ver.version_patchlevel);
			printf("  date: %s\n", date);
			printf("  desc: %s\n", desc);
			TEST("driver name is 'powervr'", strcmp(name, "powervr") == 0);
		} else {
			printf("  errno=%d (%s)\n", errno, strerror(errno));
		}
	}

	/* T3: GPU info query */
	printf("\n[T3] PVR_DEV_QUERY GPU_INFO\n");
	{
		struct drm_pvr_ioctl_dev_query_args query;
		struct drm_pvr_dev_query_gpu_info gpu_info;

		memset(&query, 0, sizeof(query));
		memset(&gpu_info, 0, sizeof(gpu_info));
		query.type = DRM_PVR_DEV_QUERY_GPU_INFO_GET;
		query.size = sizeof(gpu_info);
		query.pointer = (__uint64_t)(uintptr_t)&gpu_info;

		ret = ioctl(fd, DRM_IOCTL_PVR_DEV_QUERY, &query);
		TEST("ioctl DEV_QUERY GPU_INFO", ret == 0);
		if (ret == 0) {
			unsigned b = (gpu_info.gpu_id >> 48) & 0xffff;
			unsigned v = (gpu_info.gpu_id >> 32) & 0xffff;
			unsigned n = (gpu_info.gpu_id >> 16) & 0xffff;
			unsigned c = gpu_info.gpu_id & 0xffff;
			printf("  BVNC: %u.%u.%u.%u\n", b, v, n, c);
			printf("  num_phantoms: %u\n", gpu_info.num_phantoms);
			TEST("BVNC is 36.50.54.182", b == 36 && v == 50 && n == 54 && c == 182);
		} else {
			printf("  errno=%d (%s)\n", errno, strerror(errno));
		}
	}

	/* T4: Runtime info query */
	printf("\n[T4] PVR_DEV_QUERY RUNTIME_INFO\n");
	{
		struct drm_pvr_ioctl_dev_query_args query;
		struct drm_pvr_dev_query_runtime_info rt_info;

		memset(&query, 0, sizeof(query));
		memset(&rt_info, 0, sizeof(rt_info));
		query.type = DRM_PVR_DEV_QUERY_RUNTIME_INFO_GET;
		query.size = sizeof(rt_info);
		query.pointer = (__uint64_t)(uintptr_t)&rt_info;

		ret = ioctl(fd, DRM_IOCTL_PVR_DEV_QUERY, &query);
		TEST("ioctl DEV_QUERY RUNTIME_INFO", ret == 0);
		if (ret == 0) {
			printf("  free_list_min_pages: %llu\n", (unsigned long long)rt_info.free_list_min_pages);
			printf("  free_list_max_pages: %llu\n", (unsigned long long)rt_info.free_list_max_pages);
			printf("  max_coeffs: %u\n", rt_info.max_coeffs);
			printf("  cdm_max_local_mem: %u\n", rt_info.cdm_max_local_mem_size_regs);
		} else {
			printf("  errno=%d (%s)\n", errno, strerror(errno));
		}
	}

	/* T5: Create buffer object */
	printf("\n[T5] PVR_CREATE_BO (4096 bytes)\n");
	{
		struct drm_pvr_ioctl_create_bo_args bo;

		memset(&bo, 0, sizeof(bo));
		bo.size = 4096;
		bo.flags = 0;

		ret = ioctl(fd, DRM_IOCTL_PVR_CREATE_BO, &bo);
		TEST("ioctl CREATE_BO", ret == 0);
		if (ret == 0) {
			printf("  handle: %u\n", bo.handle);
			printf("  size: %llu\n", (unsigned long long)bo.size);
		} else {
			printf("  errno=%d (%s)\n", errno, strerror(errno));
		}
	}

	/* T6: Create/destroy VM context */
	printf("\n[T6] PVR_CREATE_VM_CONTEXT\n");
	{
		struct drm_pvr_ioctl_create_vm_context_args vm_ctx;
		struct drm_pvr_ioctl_destroy_vm_context_args vm_destroy;

		memset(&vm_ctx, 0, sizeof(vm_ctx));
		ret = ioctl(fd, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &vm_ctx);
		TEST("ioctl CREATE_VM_CONTEXT", ret == 0);
		if (ret == 0) {
			printf("  vm_context handle: %u\n", vm_ctx.handle);

			memset(&vm_destroy, 0, sizeof(vm_destroy));
			vm_destroy.handle = vm_ctx.handle;
			ret = ioctl(fd, DRM_IOCTL_PVR_DESTROY_VM_CONTEXT, &vm_destroy);
			TEST("ioctl DESTROY_VM_CONTEXT", ret == 0);
			if (ret != 0)
				printf("  destroy errno=%d (%s)\n", errno, strerror(errno));
		} else {
			printf("  errno=%d (%s)\n", errno, strerror(errno));
		}
	}

	close(fd);

	printf("\n=== Results: %d passed, %d failed ===\n",
	    tests_passed, tests_failed);
	return tests_failed > 0 ? 1 : 0;
}
