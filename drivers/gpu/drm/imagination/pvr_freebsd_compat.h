/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Tej Kiran
 *
 * FreeBSD compatibility shims for the PowerVR drm/imagination driver.
 */

#ifndef _PVR_FREEBSD_COMPAT_H_
#define	_PVR_FREEBSD_COMPAT_H_

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/reset.h>

struct drm_device;

/* _BITULL is BIT_ULL in FreeBSD LinuxKPI */
#ifndef _BITULL
#define	_BITULL(x)	BIT_ULL(x)
#endif

/* iowrite64 */
#ifndef iowrite64
static inline void iowrite64(u64 value, volatile void __iomem *addr)
{
	writeq(value, addr);
}
#endif

/* ioread64 */
#ifndef ioread64
static inline u64 ioread64(const volatile void __iomem *addr)
{
	return (readq(addr));
}
#endif

/* readl_poll_timeout */
#ifndef readl_poll_timeout
#define	readl_poll_timeout(addr, val, cond, delay_us, timeout_us) ({	\
	unsigned long __timeout = (timeout_us);				\
	unsigned long __elapsed = 0;					\
	int __ret = 0;							\
	for (;;) {							\
		(val) = readl(addr);					\
		if (cond) break;					\
		if (__timeout && __elapsed >= __timeout) {		\
			(val) = readl(addr);				\
			__ret = (cond) ? 0 : -ETIMEDOUT;		\
			break;						\
		}							\
		if ((delay_us)) udelay(delay_us);			\
		__elapsed += (delay_us) ? (delay_us) : 1;		\
	}								\
	__ret;								\
})
#endif

/* readq_poll_timeout */
#ifndef readq_poll_timeout
#define	readq_poll_timeout(addr, val, cond, delay_us, timeout_us) ({	\
	unsigned long __timeout = (timeout_us);				\
	unsigned long __elapsed = 0;					\
	int __ret = 0;							\
	for (;;) {							\
		(val) = readq(addr);					\
		if (cond) break;					\
		if (__timeout && __elapsed >= __timeout) {		\
			(val) = readq(addr);				\
			__ret = (cond) ? 0 : -ETIMEDOUT;		\
			break;						\
		}							\
		if ((delay_us)) udelay(delay_us);			\
		__elapsed += (delay_us) ? (delay_us) : 1;		\
	}								\
	__ret;								\
})
#endif

/* pm_runtime stubs */
#ifndef pm_runtime_resume_and_get
static inline int pm_runtime_resume_and_get(struct device *dev)
{
	return (0);
}
#endif

#ifndef devm_pm_runtime_enable
static inline int devm_pm_runtime_enable(struct device *dev)
{
	return (0);
}
#endif

#ifndef pm_runtime_status_suspended
static inline bool pm_runtime_status_suspended(struct device *dev)
{
	return (false);
}
#endif

#ifndef pm_runtime_set_active
static inline int pm_runtime_set_active(struct device *dev)
{
	return (0);
}
#endif

#ifndef pm_runtime_set_suspended
static inline int pm_runtime_set_suspended(struct device *dev)
{
	return (0);
}
#endif

/* drmm_mutex_init */
#ifndef drmm_mutex_init
#include <linux/mutex.h>
static inline int drmm_mutex_init(struct drm_device *dev, struct mutex *lock)
{
	mutex_init(lock);
	return (0);
}
#endif

/* list_entry_is_head */
#ifndef list_entry_is_head
#define	list_entry_is_head(pos, head, member) \
	(&pos->member == (head))
#endif

/* IRQF_ONESHOT */
#ifndef IRQF_ONESHOT
#define	IRQF_ONESHOT	0x00002000
#endif

/* devm_platform_get_and_ioremap_resource — also fills in resource info.
 * The driver uses struct resource * but we use lkpi_platform_resource.
 * They have the same layout (start, end, flags) so a cast is safe. */
static inline void __iomem *
__pvr_devm_platform_get_and_ioremap_resource(struct platform_device *pdev,
    unsigned int index, void *res_out)
{
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, index);
	if (res_out && !IS_ERR(base))
		*(struct lkpi_platform_resource **)res_out = &pdev->lkpi_res[index];
	else if (res_out)
		*(void **)res_out = NULL;
	return (base);
}
#define	devm_platform_get_and_ioremap_resource \
	__pvr_devm_platform_get_and_ioremap_resource
/* Note: struct resource in the driver accesses .start which matches
 * our lkpi_platform_resource. The devm_platform_get_and_ioremap_resource
 * shim above returns lkpi_platform_resource* which has .start field. */

/* MODULE_IMPORT_NS */
#ifndef MODULE_IMPORT_NS
#define	MODULE_IMPORT_NS(x)
#endif

/* firmware struct name mapping */
#define	firmware	linuxkpi_firmware

/* task_tgid_nr */
#ifndef task_tgid_nr
static inline pid_t task_tgid_nr(struct task_struct *tsk)
{
	return (0);
}
#endif

/* vmap/vunmap — FreeBSD has these in linuxkpi */
#include <linux/vmalloc.h>

/* kmemleak stubs */
#ifndef kmemleak_alloc
#define	kmemleak_alloc(ptr, size, min, gfp)
#endif
#ifndef kmemleak_free
#define	kmemleak_free(ptr)
#endif

/* pm_runtime_force_suspend/resume */
#ifndef pm_runtime_force_suspend
static inline int pm_runtime_force_suspend(struct device *dev)
{
	return (0);
}
#endif

#ifndef pm_runtime_force_resume
static inline int pm_runtime_force_resume(struct device *dev)
{
	return (0);
}
#endif

/* __simple_attr_check_format — debugfs helper stub */
#ifndef __simple_attr_check_format
#define	__simple_attr_check_format(fmt, val)
#endif

/* pm_runtime_put_sync_suspend / pm_runtime_suspend */
#ifndef pm_runtime_put_sync_suspend
#define	pm_runtime_put_sync_suspend(dev)	do { } while (0)
#endif

#ifndef pm_runtime_suspend
#define	pm_runtime_suspend(dev)	(0)
#endif

/* RUNTIME_PM_OPS — maps to SET_RUNTIME_PM_OPS on older kernels */
#ifndef RUNTIME_PM_OPS
#define	RUNTIME_PM_OPS(suspend, resume, idle) \
	.runtime_suspend = (suspend), \
	.runtime_resume = (resume), \
	.runtime_idle = (idle),
#endif

/* copy_struct_from_user — copy from user with size checking */
#ifndef copy_struct_from_user
static inline int copy_struct_from_user(void *dst, size_t ksize,
    const void __user *src, size_t usize)
{
	size_t size = min(ksize, usize);

	if (copy_from_user(dst, src, size))
		return (-EFAULT);
	if (ksize > usize)
		memset((char *)dst + usize, 0, ksize - usize);
	return (0);
}
#endif

/* Size defines */
#ifndef SZ_1T
#define	SZ_1T	(1ULL << 40)
#endif
#ifndef SZ_128G
#define	SZ_128G	(128ULL << 30)
#endif

/* ELF types for pvr_fw_mips.c — Linux uses struct elf32_hdr/phdr,
 * FreeBSD uses Elf32_Ehdr/Phdr typedef with identical field names.
 * We can't #define elf32_hdr Elf32_Ehdr because "struct Elf32_Ehdr"
 * doesn't work with typedefs. Instead define the struct names. */
#include <sys/elf32.h>
#include <sys/elf_common.h>
struct elf32_hdr {
	unsigned char e_ident[EI_NIDENT];
	Elf32_Half e_type;
	Elf32_Half e_machine;
	Elf32_Word e_version;
	Elf32_Addr e_entry;
	Elf32_Off e_phoff;
	Elf32_Off e_shoff;
	Elf32_Word e_flags;
	Elf32_Half e_ehsize;
	Elf32_Half e_phentsize;
	Elf32_Half e_phnum;
	Elf32_Half e_shentsize;
	Elf32_Half e_shnum;
	Elf32_Half e_shstrndx;
};
struct elf32_phdr {
	Elf32_Word p_type;
	Elf32_Off p_offset;
	Elf32_Addr p_vaddr;
	Elf32_Addr p_paddr;
	Elf32_Word p_filesz;
	Elf32_Word p_memsz;
	Elf32_Word p_flags;
	Elf32_Word p_align;
};

/* dma_sync_sgtable_for_cpu/device */
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>

#ifndef dma_sync_sgtable_for_cpu
#define	dma_sync_sgtable_for_cpu(dev, sgt, dir) \
	dma_sync_sg_for_cpu(dev, (sgt)->sgl, (sgt)->nents, dir)
#endif

#ifndef dma_sync_sgtable_for_device
#define	dma_sync_sgtable_for_device(dev, sgt, dir) \
	dma_sync_sg_for_device(dev, (sgt)->sgl, (sgt)->nents, dir)
#endif

/* IOSYS_MAP_INIT_VADDR — initializer for iosys_map */
#include <linux/iosys-map.h>
#ifndef IOSYS_MAP_INIT_VADDR
#define	IOSYS_MAP_INIT_VADDR(addr_) \
	((struct iosys_map){ .vaddr = (addr_), .is_iomem = false })
#endif

/* pvr_params.c — override module_param to no-ops */
#undef module_param_named
#define	module_param_named(name, value, type, perm)
#undef MODULE_PARM_DESC
#define	MODULE_PARM_DESC(name, desc)

/* MODULE_DEVICE_TABLE(of, ...) — no-op on FreeBSD */
#ifndef MODULE_DEVICE_TABLE_BUS_of
#define	MODULE_DEVICE_TABLE_BUS_of(_bus, _table)
#endif

/* _ULL suffix — some firmware headers use ULL() macro */
#ifndef ULL
#define	ULL(x)	(x##ULL)
#endif

/*
 * Cache flush for non-coherent DMA (RISC-V).
 *
 * On JH7110 (SiFive U74), vmap with pgprot_writecombine produces cacheable
 * mappings because: (1) vmap() ignores pgprot, and (2) the U74 doesn't
 * support Svpbmt PTE memory attributes.  CPU writes to GPU shared memory
 * (KCCB, FW control structures) stay in L1/L2 cache, invisible to the
 * GPU's MIPS firmware processor.
 *
 * pvr_dma_cache_wbinv flushes a virtual address range through both the
 * CPU L1 data cache and the SiFive L2 cache controller (ccache) so the
 * data reaches main memory.
 *
 * pvr_dma_cache_inv invalidates a virtual address range so the CPU reads
 * fresh data written by the GPU.
 */
#if defined(__riscv)
#include <machine/md_var.h>	/* cpu_dcache_wbinv_range, cpu_dcache_inv_range */
#include <vm/vm.h>
#include <vm/pmap.h>		/* vtophys */

void sifive_ccache_flush_range(vm_paddr_t, unsigned long);

static inline void
pvr_dma_cache_wbinv(void *vaddr, size_t size)
{
	cpu_dcache_wbinv_range((vm_offset_t)vaddr, size);
	sifive_ccache_flush_range(vtophys(vaddr), size);
}

static inline void
pvr_dma_cache_inv(void *vaddr, size_t size)
{
	cpu_dcache_inv_range((vm_offset_t)vaddr, size);
	sifive_ccache_flush_range(vtophys(vaddr), size);
}
#else
static inline void pvr_dma_cache_wbinv(void *vaddr, size_t size) {}
static inline void pvr_dma_cache_inv(void *vaddr, size_t size) {}
#endif

#endif /* _PVR_FREEBSD_COMPAT_H_ */
