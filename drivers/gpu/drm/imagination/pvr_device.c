// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright (c) 2023 Imagination Technologies Ltd. */

#include "pvr_device.h"
#include "pvr_device_info.h"

#include "pvr_fw.h"
#include "pvr_params.h"
#include "pvr_power.h"
#include "pvr_queue.h"
#include "pvr_rogue_cr_defs.h"
#include "pvr_stream.h"
#include "pvr_vm.h"

#include <drm/drm_print.h>

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/compiler_attributes.h>
#include <linux/compiler_types.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#ifdef __FreeBSD__
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/pwrdom/pwrdom.h>
#endif
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/workqueue.h>

/* Major number for the supported version of the firmware. */
#define PVR_FW_VERSION_MAJOR 1

/**
 * pvr_device_reg_init() - Initialize kernel access to a PowerVR device's
 * control registers.
 * @pvr_dev: Target PowerVR device.
 *
 * Sets struct pvr_device->regs.
 *
 * This method of mapping the device control registers into memory ensures that
 * they are unmapped when the driver is detached (i.e. no explicit cleanup is
 * required).
 *
 * Return:
 *  * 0 on success, or
 *  * Any error returned by devm_platform_ioremap_resource().
 */
static int
pvr_device_reg_init(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct platform_device *plat_dev = to_platform_device(drm_dev->dev);
	struct resource *regs_resource;
	void __iomem *regs;

	pvr_dev->regs_resource = NULL;
	pvr_dev->regs = NULL;

	regs = devm_platform_get_and_ioremap_resource(plat_dev, 0, &regs_resource);
	if (IS_ERR(regs))
		return dev_err_probe(drm_dev->dev, PTR_ERR(regs),
				     "failed to ioremap gpu registers\n");

	pvr_dev->regs = regs;
	pvr_dev->regs_resource = regs_resource;

	return 0;
}

/**
 * pvr_device_clk_init() - Initialize clocks required by a PowerVR device
 * @pvr_dev: Target PowerVR device.
 *
 * Sets struct pvr_device->core_clk, struct pvr_device->sys_clk and
 * struct pvr_device->mem_clk.
 *
 * Three clocks are required by the PowerVR device: core, sys and mem. On
 * return, this function guarantees that the clocks are in one of the following
 * states:
 *
 *  * All successfully initialized,
 *  * Core errored, sys and mem uninitialized,
 *  * Core deinitialized, sys errored, mem uninitialized, or
 *  * Core and sys deinitialized, mem errored.
 *
 * Return:
 *  * 0 on success,
 *  * Any error returned by devm_clk_get(), or
 *  * Any error returned by devm_clk_get_optional().
 */
static int pvr_device_clk_init(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct clk *core_clk;
	struct clk *sys_clk;
	struct clk *mem_clk;

	core_clk = devm_clk_get(drm_dev->dev, "core");
	if (IS_ERR(core_clk))
		return dev_err_probe(drm_dev->dev, PTR_ERR(core_clk),
				     "failed to get core clock\n");

	sys_clk = devm_clk_get_optional(drm_dev->dev, "sys");
	if (IS_ERR(sys_clk))
		return dev_err_probe(drm_dev->dev, PTR_ERR(sys_clk),
				     "failed to get sys clock\n");

	mem_clk = devm_clk_get_optional(drm_dev->dev, "mem");
	if (IS_ERR(mem_clk))
		return dev_err_probe(drm_dev->dev, PTR_ERR(mem_clk),
				     "failed to get mem clock\n");

	pvr_dev->core_clk = core_clk;
	pvr_dev->sys_clk = sys_clk;
	pvr_dev->mem_clk = mem_clk;

	return 0;
}

/**
 * pvr_device_process_active_queues() - Process all queue related events.
 * @pvr_dev: PowerVR device to check
 *
 * This is called any time we receive a FW event. It iterates over all
 * active queues and calls pvr_queue_process() on them.
 */
static void pvr_device_process_active_queues(struct pvr_device *pvr_dev)
{
	struct pvr_queue *queue, *tmp_queue;
	LIST_HEAD(active_queues);

	mutex_lock(&pvr_dev->queues.lock);

	/* Move all active queues to a temporary list. Queues that remain
	 * active after we're done processing them are re-inserted to
	 * the queues.active list by pvr_queue_process().
	 */
	list_splice_init(&pvr_dev->queues.active, &active_queues);

	list_for_each_entry_safe(queue, tmp_queue, &active_queues, node)
		pvr_queue_process(queue);

	mutex_unlock(&pvr_dev->queues.lock);
}

static irqreturn_t pvr_device_irq_thread_handler(int irq, void *data)
{
	struct pvr_device *pvr_dev = data;
	irqreturn_t ret = IRQ_NONE;

	/* We are in the threaded handler, we can keep dequeuing events until we
	 * don't see any. This should allow us to reduce the number of interrupts
	 * when the GPU is receiving a massive amount of short jobs.
	 */
	while (pvr_fw_irq_pending(pvr_dev)) {
		pvr_fw_irq_clear(pvr_dev);

		if (pvr_dev->fw_dev.booted) {
			pvr_fwccb_process(pvr_dev);
			pvr_kccb_wake_up_waiters(pvr_dev);
			pvr_device_process_active_queues(pvr_dev);

			{
				extern void pvr_fw_program_heap_bases(struct pvr_device *);
				pvr_fw_program_heap_bases(pvr_dev);
			}
		}

		pm_runtime_mark_last_busy(from_pvr_device(pvr_dev)->dev);

		ret = IRQ_HANDLED;
	}

	/* Unmask FW irqs before returning, so new interrupts can be received. */
	pvr_fw_irq_enable(pvr_dev);
	return ret;
}

static irqreturn_t pvr_device_irq_handler(int irq, void *data)
{
	struct pvr_device *pvr_dev = data;

	if (!pvr_fw_irq_pending(pvr_dev))
		return IRQ_NONE; /* Spurious IRQ - ignore. */

	/* Mask the FW interrupts before waking up the thread. Will be unmasked
	 * when the thread handler is done processing events.
	 */
	pvr_fw_irq_disable(pvr_dev);
	return IRQ_WAKE_THREAD;
}

/**
 * pvr_device_irq_init() - Initialise IRQ required by a PowerVR device
 * @pvr_dev: Target PowerVR device.
 *
 * Returns:
 *  * 0 on success,
 *  * Any error returned by platform_get_irq_byname(), or
 *  * Any error returned by request_irq().
 */
static int
pvr_device_irq_init(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct platform_device *plat_dev = to_platform_device(drm_dev->dev);

	init_waitqueue_head(&pvr_dev->kccb.rtn_q);

	pvr_dev->irq = platform_get_irq(plat_dev, 0);
	if (pvr_dev->irq < 0)
		return pvr_dev->irq;

	/* Clear any pending events before requesting the IRQ line. */
	pvr_fw_irq_clear(pvr_dev);
	pvr_fw_irq_enable(pvr_dev);

	return devm_request_threaded_irq(drm_dev->dev, pvr_dev->irq,
					 pvr_device_irq_handler,
					 pvr_device_irq_thread_handler,
					 IRQF_SHARED, "gpu", pvr_dev);
}

/**
 * pvr_device_irq_fini() - Deinitialise IRQ required by a PowerVR device
 * @pvr_dev: Target PowerVR device.
 */
static void
pvr_device_irq_fini(struct pvr_device *pvr_dev)
{
	free_irq(pvr_dev->irq, pvr_dev);
}

/**
 * pvr_build_firmware_filename() - Construct a PowerVR firmware filename
 * @pvr_dev: Target PowerVR device.
 * @base: First part of the filename.
 * @major: Major version number.
 *
 * A PowerVR firmware filename consists of three parts separated by underscores
 * (``'_'``) along with a '.fw' file suffix. The first part is the exact value
 * of @base, the second part is the hardware version string derived from @pvr_fw
 * and the final part is the firmware version number constructed from @major with
 * a 'v' prefix, e.g. powervr/rogue_4.40.2.51_v1.fw.
 *
 * The returned string will have been slab allocated and must be freed with
 * kfree().
 *
 * Return:
 *  * The constructed filename on success, or
 *  * Any error returned by kasprintf().
 */
static char *
pvr_build_firmware_filename(struct pvr_device *pvr_dev, const char *base,
			    u8 major)
{
	struct pvr_gpu_id *gpu_id = &pvr_dev->gpu_id;

	return kasprintf(GFP_KERNEL, "%s_%d.%d.%d.%d_v%d.fw", base, gpu_id->b,
			 gpu_id->v, gpu_id->n, gpu_id->c, major);
}

static void
pvr_release_firmware(void *data)
{
	struct pvr_device *pvr_dev = data;

	release_firmware(pvr_dev->fw_dev.firmware);
}

/**
 * pvr_request_firmware() - Load firmware for a PowerVR device
 * @pvr_dev: Target PowerVR device.
 *
 * See pvr_build_firmware_filename() for details on firmware file naming.
 *
 * Return:
 *  * 0 on success,
 *  * Any error returned by pvr_build_firmware_filename(), or
 *  * Any error returned by request_firmware().
 */
static int
pvr_request_firmware(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = &pvr_dev->base;
	char *filename;
	const struct firmware *fw;
	int err;

	filename = pvr_build_firmware_filename(pvr_dev, "powervr/rogue",
					       PVR_FW_VERSION_MAJOR);
	if (!filename)
		return -ENOMEM;

	/*
	 * This function takes a copy of &filename, meaning we can free our
	 * instance before returning.
	 */
	err = request_firmware(&fw, filename, pvr_dev->base.dev);
	if (err) {
		drm_err(drm_dev, "failed to load firmware %s (err=%d)\n",
			filename, err);
		goto err_free_filename;
	}

	drm_info(drm_dev, "loaded firmware %s\n", filename);
	kfree(filename);

	pvr_dev->fw_dev.firmware = fw;

	return devm_add_action_or_reset(drm_dev->dev, pvr_release_firmware, pvr_dev);

err_free_filename:
	kfree(filename);

	return err;
}

/**
 * pvr_load_gpu_id() - Load a PowerVR device's GPU ID (BVNC) from control registers.
 *
 * Sets struct pvr_dev.gpu_id.
 *
 * @pvr_dev: Target PowerVR device.
 */
static void
pvr_load_gpu_id(struct pvr_device *pvr_dev)
{
	struct pvr_gpu_id *gpu_id = &pvr_dev->gpu_id;
	u64 bvnc;

	/*
	 * Try reading the BVNC using the newer (cleaner) method first. If the
	 * B value is zero, fall back to the older method.
	 */
	bvnc = pvr_cr_read64(pvr_dev, ROGUE_CR_CORE_ID__PBVNC);

	gpu_id->b = PVR_CR_FIELD_GET(bvnc, CORE_ID__PBVNC__BRANCH_ID);
	if (gpu_id->b != 0) {
		gpu_id->v = PVR_CR_FIELD_GET(bvnc, CORE_ID__PBVNC__VERSION_ID);
		gpu_id->n = PVR_CR_FIELD_GET(bvnc, CORE_ID__PBVNC__NUMBER_OF_SCALABLE_UNITS);
		gpu_id->c = PVR_CR_FIELD_GET(bvnc, CORE_ID__PBVNC__CONFIG_ID);
	} else {
		u32 core_rev = pvr_cr_read32(pvr_dev, ROGUE_CR_CORE_REVISION);
		u32 core_id = pvr_cr_read32(pvr_dev, ROGUE_CR_CORE_ID);
		u16 core_id_config = PVR_CR_FIELD_GET(core_id, CORE_ID_CONFIG);

		gpu_id->b = PVR_CR_FIELD_GET(core_rev, CORE_REVISION_MAJOR);
		gpu_id->v = PVR_CR_FIELD_GET(core_rev, CORE_REVISION_MINOR);
		gpu_id->n = FIELD_GET(0xFF00, core_id_config);
		gpu_id->c = FIELD_GET(0x00FF, core_id_config);
	}
}

/**
 * pvr_set_dma_info() - Set PowerVR device DMA information
 * @pvr_dev: Target PowerVR device.
 *
 * Sets the DMA mask and max segment size for the PowerVR device.
 *
 * Return:
 *  * 0 on success,
 *  * Any error returned by PVR_FEATURE_VALUE(), or
 *  * Any error returned by dma_set_mask().
 */

static int
pvr_set_dma_info(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	u16 phys_bus_width;
	int err;

	err = PVR_FEATURE_VALUE(pvr_dev, phys_bus_width, &phys_bus_width);
	if (err) {
		drm_err(drm_dev, "Failed to get device physical bus width\n");
		return err;
	}

	drm_info(drm_dev, "GPU phys_bus_width=%u, DMA mask=0x%llx\n",
		 phys_bus_width, (unsigned long long)DMA_BIT_MASK(phys_bus_width));

	err = dma_set_mask(drm_dev->dev, DMA_BIT_MASK(phys_bus_width));
	if (err) {
		drm_err(drm_dev, "Failed to set DMA mask (err=%d)\n", err);
		return err;
	}

	dma_set_max_seg_size(drm_dev->dev, UINT_MAX);

	return 0;
}

/**
 * pvr_device_gpu_init() - GPU-specific initialization for a PowerVR device
 * @pvr_dev: Target PowerVR device.
 *
 * The following steps are taken to ensure the device is ready:
 *
 *  1. Read the hardware version information from control registers,
 *  2. Initialise the hardware feature information,
 *  3. Setup the device DMA information,
 *  4. Setup the device-scoped memory context, and
 *  5. Load firmware into the device.
 *
 * Return:
 *  * 0 on success,
 *  * -%ENODEV if the GPU is not supported,
 *  * Any error returned by pvr_set_dma_info(),
 *  * Any error returned by pvr_memory_context_init(), or
 *  * Any error returned by pvr_request_firmware().
 */
static int
pvr_device_gpu_init(struct pvr_device *pvr_dev)
{
	int err;

	pvr_load_gpu_id(pvr_dev);

	err = pvr_request_firmware(pvr_dev);
	if (err)
		return err;

	err = pvr_fw_validate_init_device_info(pvr_dev);
	if (err)
		return err;

	if (PVR_HAS_FEATURE(pvr_dev, meta))
		pvr_dev->fw_dev.processor_type = PVR_FW_PROCESSOR_TYPE_META;
	else if (PVR_HAS_FEATURE(pvr_dev, mips))
		pvr_dev->fw_dev.processor_type = PVR_FW_PROCESSOR_TYPE_MIPS;
	else if (PVR_HAS_FEATURE(pvr_dev, riscv_fw_processor))
		pvr_dev->fw_dev.processor_type = PVR_FW_PROCESSOR_TYPE_RISCV;
	else
		return -EINVAL;

	dev_info(from_pvr_device(pvr_dev)->dev,
	    "FW processor type: %s (%u) [meta=%d mips=%d riscv=%d]\n",
	    pvr_dev->fw_dev.processor_type == PVR_FW_PROCESSOR_TYPE_META ? "META" :
	    pvr_dev->fw_dev.processor_type == PVR_FW_PROCESSOR_TYPE_MIPS ? "MIPS" :
	    "RISCV", pvr_dev->fw_dev.processor_type,
	    PVR_HAS_FEATURE(pvr_dev, meta),
	    PVR_HAS_FEATURE(pvr_dev, mips),
	    PVR_HAS_FEATURE(pvr_dev, riscv_fw_processor));

	pvr_stream_create_musthave_masks(pvr_dev);

	dev_info(from_pvr_device(pvr_dev)->dev, "pvr_device_gpu_init: set_dma_info\n");
	err = pvr_set_dma_info(pvr_dev);
	if (err) {
		dev_err(from_pvr_device(pvr_dev)->dev, "pvr_set_dma_info failed: %d\n", err);
		return err;
	}

	if (pvr_dev->fw_dev.processor_type != PVR_FW_PROCESSOR_TYPE_MIPS) {
		dev_info(from_pvr_device(pvr_dev)->dev, "pvr_device_gpu_init: creating VM context\n");
		pvr_dev->kernel_vm_ctx = pvr_vm_create_context(pvr_dev, false);
		if (IS_ERR(pvr_dev->kernel_vm_ctx)) {
			dev_err(from_pvr_device(pvr_dev)->dev, "pvr_vm_create_context failed: %ld\n",
			    PTR_ERR(pvr_dev->kernel_vm_ctx));
			return PTR_ERR(pvr_dev->kernel_vm_ctx);
		}
	}

#ifdef __FreeBSD__
	/*
	 * Replay the firmware's power-request sequence from the host, with the
	 * firmware not yet running.
	 *
	 * Disassembling our firmware (microMIPS, text at 0xc0000000) shows the
	 * power controller handshake at 0xc000a454:
	 *
	 *     sw   1,      0x890(CR)   ; XPU_BROADCAST = core 0
	 *     sw   mask,   0x038(CR)   ; unit mask, bit0 = on/off
	 *     sw   mask|2, 0x038(CR)   ; bit1 = request
	 *     poll 0x130(CR) for 0x400 (COMPLETE) / 0x800 (ABORT), 2000 times
	 *
	 * 0x038 is not defined in pvr_rogue_cr_defs.h - the host driver never
	 * touches it. The firmware gets ABORT for every request it makes here
	 * while the reference platform completes every one. Doing the identical
	 * sequence from the host separates the two possibilities: if the
	 * hardware aborts for us too, it is an integration property and not
	 * something our firmware build is doing wrong.
	 *
	 * hw.pvr.powtest=1 to run. Uses the exact masks seen in the traces.
	 */
	{
		char *ev = kern_getenv("hw.pvr.powtest");
		int run = 0;

		if (ev != NULL) {
			run = (int)strtoul(ev, NULL, 0);
			freeenv(ev);
		}

		if (run) {
			/*
			 * Sanity: can the host read these registers at all?
			 * If known-nonzero IDs come back as 0, every
			 * register-based conclusion here is worthless.
			 */
			static const struct { const char *name; u32 off; }
			probe[] = {
				{ "CORE_ID(0x18)",       0x0018 },
				{ "CORE_REVISION(0x20)", 0x0020 },
				{ "CLK_STATUS(0x08)",    0x0008 },
				{ "DESIGNER_REV2(0x30)", 0x0030 },
				{ "CHANGESET(0x40)",     0x0040 },
				{ "EVENT_STATUS(0x130)", 0x0130 },
				{ "XPU_BCAST(0x890)",    0x0890 },
				{ "PWRREQ?(0x38)",       0x0038 },
			};
			u32 pi;

			for (pi = 0; pi < ARRAY_SIZE(probe); pi++)
				dev_info(from_pvr_device(pvr_dev)->dev,
					 "REGSAN %-22s = 0x%08x\n",
					 probe[pi].name,
					 pvr_cr_read32(pvr_dev, probe[pi].off));
		}

		if (run) {
			static const struct { const char *what; u32 mask; }
			reqs[] = {
				{ "OFF", 0x00000701u },
				{ "ON",  0x01000703u },
			};
			u32 i, n, st;

			for (i = 0; i < ARRAY_SIZE(reqs); i++) {
				/* clear any stale COMPLETE/ABORT */
				pvr_cr_write32(pvr_dev, 0x0138, 0xc00);
				pvr_cr_write32(pvr_dev, ROGUE_CR_XPU_BROADCAST, 1);
				(void)pvr_cr_read32(pvr_dev, ROGUE_CR_XPU_BROADCAST);
				pvr_cr_write32(pvr_dev, 0x0038, reqs[i].mask);
				pvr_cr_write32(pvr_dev, 0x0038, reqs[i].mask | 2);

				st = 0;
				for (n = 0; n < 2000; n++) {
					st = pvr_cr_read32(pvr_dev, ROGUE_CR_EVENT_STATUS);
					if (st & 0xc00)
						break;
					udelay(1);
				}
				dev_info(from_pvr_device(pvr_dev)->dev,
					 "POWTEST %s mask=0x%08x -> EVENT_STATUS=0x%08x %s after %u polls (0x038 reads 0x%08x)\n",
					 reqs[i].what, reqs[i].mask, st,
					 (st & 0x400) ? "COMPLETE" :
					 (st & 0x800) ? "ABORT" : "TIMEOUT",
					 n, pvr_cr_read32(pvr_dev, 0x0038));
				pvr_cr_write32(pvr_dev, 0x0138, 0xc00);
			}
		}
	}
#endif

	dev_info(from_pvr_device(pvr_dev)->dev, "pvr_device_gpu_init: calling pvr_fw_init\n");
	err = pvr_fw_init(pvr_dev);
	if (err)
		goto err_vm_ctx_put;

	return 0;

err_vm_ctx_put:
	if (pvr_dev->fw_dev.processor_type != PVR_FW_PROCESSOR_TYPE_MIPS) {
		pvr_vm_context_put(pvr_dev->kernel_vm_ctx);
		pvr_dev->kernel_vm_ctx = NULL;
	}

	return err;
}

/**
 * pvr_device_gpu_fini() - GPU-specific deinitialization for a PowerVR device
 * @pvr_dev: Target PowerVR device.
 */
static void
pvr_device_gpu_fini(struct pvr_device *pvr_dev)
{
	pvr_fw_fini(pvr_dev);

	if (pvr_dev->fw_dev.processor_type != PVR_FW_PROCESSOR_TYPE_MIPS) {
		WARN_ON(!pvr_vm_context_put(pvr_dev->kernel_vm_ctx));
		pvr_dev->kernel_vm_ctx = NULL;
	}
}

/**
 * pvr_device_init() - Initialize a PowerVR device
 * @pvr_dev: Target PowerVR device.
 *
 * If this function returns successfully, the device will have been fully
 * initialized. Otherwise, any parts of the device initialized before an error
 * occurs will be de-initialized before returning.
 *
 * NOTE: The initialization steps currently taken are the bare minimum required
 *       to read from the control registers. The device is unlikely to function
 *       until further initialization steps are added. [This note should be
 *       removed when that happens.]
 *
 * Return:
 *  * 0 on success,
 *  * Any error returned by pvr_device_reg_init(),
 *  * Any error returned by pvr_device_clk_init(), or
 *  * Any error returned by pvr_device_gpu_init().
 */
int
pvr_device_init(struct pvr_device *pvr_dev)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	struct device *dev = drm_dev->dev;
	int err;

	/*
	 * Setup device parameters. We do this first in case other steps
	 * depend on them.
	 */
	err = pvr_device_params_init(&pvr_dev->params);
	if (err)
		return err;

	/* Enable and initialize clocks required for the device to operate. */
	err = pvr_device_clk_init(pvr_dev);
	if (err)
		return err;

	/* Explicitly power the GPU so we can access control registers before the FW is booted. */
	err = pm_runtime_resume_and_get(dev);
	if (err)
		return err;

#ifdef __FreeBSD__
	/*
	 * pm_runtime is no-ops on FreeBSD. Manually enable all GPU clocks
	 * and deassert resets following the StarFive JH7110 power sequence.
	 */
	{
		struct clk *clk_apb, *clk_rtc, *clk_axi, *clk_div, *clk_pll;
		struct clk *clk_root;
		struct reset_control *rst_apb, *rst_doma;

		clk_apb = devm_clk_get_optional(dev, "apb");
		clk_rtc = devm_clk_get_optional(dev, "rtc");
		clk_axi = devm_clk_get_optional(dev, "axi");
		clk_div = devm_clk_get_optional(dev, "div");
		clk_pll = devm_clk_get_optional(dev, "pll");
		clk_root = devm_clk_get_optional(dev, "root");

		if (!IS_ERR_OR_NULL(clk_apb))
			clk_prepare_enable(clk_apb);
		if (!IS_ERR_OR_NULL(clk_rtc))
			clk_prepare_enable(clk_rtc);
		if (!IS_ERR_OR_NULL(clk_div)) {
			/*
			 * GPU core clock rate, overridable at boot with the
			 * loader tunable hw.pvr.core_clk_hz.
			 *
			 * The reference platform runs gpu_core at 396 MHz off a
			 * 1188 MHz gpu_root. We ask for 594 MHz here, and with
			 * our gpu_root at 1000 MHz that lands on 500 MHz - about
			 * 26% above the reference. Renders lose random tiles at a
			 * rate that grows with the amount of work, which is what
			 * an over-clocked core looks like, so make the rate easy
			 * to sweep instead of rebuilding for each value.
			 */
			unsigned long rate = 396000000UL;
			char *ev = kern_getenv("hw.pvr.core_clk_hz");

			/*
			 * Program the parent PLL first. gpu_root muxes over
			 * {pll0_out, pll2_out} and gpu_core is an integer
			 * divider off it, so the core rate is only reachable if
			 * the PLL is right. The reference platform runs
			 * pll2_out at 1188 MHz and divides by 3.
			 */
			/*
			 * gpu_root is a mux over {pll0_out, pll2_out}. We come
			 * up selecting pll0_out, which on this board runs at
			 * 1000 MHz, so the divider can only reach 500 or
			 * 333 MHz. The reference platform selects pll2_out at
			 * 1188 MHz and divides by 3 for exactly 396 MHz.
			 *
			 * pll2_out is already at the right rate here - the mux
			 * selection is the whole problem - so reparent it and
			 * then ask for 396.
			 */
			if (!IS_ERR_OR_NULL(clk_root) &&
			    !IS_ERR_OR_NULL(clk_pll)) {
				int perr = clk_set_parent(clk_root, clk_pll);

				dev_info(dev,
					 "gpu_root reparent to pll2: err=%d, root now %lu Hz\n",
					 perr,
					 (unsigned long)clk_get_rate(clk_root));
			} else {
				dev_info(dev,
					 "gpu_root reparent skipped: root=%d pll=%d\n",
					 clk_root ? 0 : 1, clk_pll ? 0 : 1);
			}

			if (ev != NULL) {
				rate = strtoul(ev, NULL, 0);
				freeenv(ev);
				if (rate < 50000000UL)
					rate = 594000000UL;
			}

			clk_set_rate(clk_div, rate);
			dev_info(dev, "gpu core clk requested %lu Hz, got %lu Hz\n",
				 rate, (unsigned long)clk_get_rate(clk_div));
		}
		err = clk_prepare_enable(pvr_dev->core_clk);
		if (err) {
			dev_err(dev, "failed to enable core clock: %d\n", err);
			return err;
		}
		if (pvr_dev->sys_clk)
			clk_prepare_enable(pvr_dev->sys_clk);
		if (!IS_ERR_OR_NULL(clk_axi))
			clk_prepare_enable(clk_axi);

		udelay(1);

		rst_apb = devm_reset_control_get_optional_exclusive(dev, "apb");
		rst_doma = devm_reset_control_get_optional_exclusive(dev, "doma");
		/*
		 * Report whether the reset lines actually resolved and
		 * deasserted. `_optional_` returns NULL when the provider is
		 * missing, which makes the deassert a silent no-op - and the
		 * GPU-internal power controller aborts every power request on
		 * this board while the DDK's completes them all.
		 */
		{
			int ra = -1, rd = -1;

			if (!IS_ERR_OR_NULL(rst_apb))
				ra = reset_control_deassert(rst_apb);
			if (!IS_ERR_OR_NULL(rst_doma))
				rd = reset_control_deassert(rst_doma);

			dev_info(dev,
				 "GPU resets: apb=%s deassert=%d, doma=%s deassert=%d\n",
				 IS_ERR(rst_apb) ? "ERR" : (rst_apb ? "ok" : "NULL"), ra,
				 IS_ERR(rst_doma) ? "ERR" : (rst_doma ? "ok" : "NULL"), rd);
		}

		udelay(10);
		dev_info(dev, "GPU clocks enabled, resets deasserted\n");

		/*
		 * GPU power domain (JH7110_PD_GPUA).
		 *
		 * Debian makes the GPU device the domain's genpd consumer and
		 * lets it suspend: CURR_POWER_MODE cycles with the GPUA bit
		 * clear (0x13 -> 0x33 observed) and genpd shows
		 * "GPUA off-0 / 18000000.gpu suspended".
		 *
		 * Ours has no consumer at all - jh7110_pmu forces GPUA on at
		 * attach (0x03 -> 0x17) and nothing can ever release it. Take
		 * ownership here so the domain has a real consumer, which is
		 * the prerequisite for ever powering it down.
		 *
		 * This step only acquires and enables: the island stays on, so
		 * behaviour is unchanged. hw.pvr.pwrdom=0 skips it.
		 */
		{
			device_t bdev = dev->bsddev;
			phandle_t node;
			pwrdom_t pd = NULL;
			int perr = ENXIO;
			char *ev = kern_getenv("hw.pvr.pwrdom");
			int want = 1;

			if (ev != NULL) {
				want = (int)strtoul(ev, NULL, 0);
				freeenv(ev);
			}

			if (want && bdev != NULL) {
				node = ofw_bus_get_node(bdev);
				perr = pwrdom_get_by_ofw_idx(bdev, node, 0, &pd);
				if (perr == 0 && pd != NULL) {
					perr = pwrdom_enable(pd);
					if (perr == 0)
						pvr_dev->fbsd_pwrdom = pd;
				}
			}

			dev_info(dev, "GPU power domain: %s err=%d\n",
				 pvr_dev->fbsd_pwrdom ? "acquired+enabled" :
				 (want ? "NOT acquired" : "disabled by tunable"),
				 perr);

			/* Hand the device to the hw.pvr_suspend sysctl. */
			pvr_power_fbsd_register(pvr_dev);
		}
	}
#endif

	/* Map the control registers into memory. */
	err = pvr_device_reg_init(pvr_dev);
	if (err)
		goto err_pm_runtime_put;

	/* Perform GPU-specific initialization steps. */
	err = pvr_device_gpu_init(pvr_dev);
	if (err)
		goto err_pm_runtime_put;

	err = pvr_device_irq_init(pvr_dev);
	if (err)
		goto err_device_gpu_fini;

	pm_runtime_put(dev);

	return 0;

err_device_gpu_fini:
	pvr_device_gpu_fini(pvr_dev);

err_pm_runtime_put:
	pm_runtime_put_sync_suspend(dev);

	return err;
}

/**
 * pvr_device_fini() - Deinitialize a PowerVR device
 * @pvr_dev: Target PowerVR device.
 */
void
pvr_device_fini(struct pvr_device *pvr_dev)
{
	/*
	 * Deinitialization stages are performed in reverse order compared to
	 * the initialization stages in pvr_device_init().
	 */
	pvr_device_irq_fini(pvr_dev);
	pvr_device_gpu_fini(pvr_dev);
}

bool
pvr_device_has_uapi_quirk(struct pvr_device *pvr_dev, u32 quirk)
{
	switch (quirk) {
	case 47217:
		return PVR_HAS_QUIRK(pvr_dev, 47217);
	case 48545:
		return PVR_HAS_QUIRK(pvr_dev, 48545);
	case 49927:
		return PVR_HAS_QUIRK(pvr_dev, 49927);
	case 51764:
		return PVR_HAS_QUIRK(pvr_dev, 51764);
	case 62269:
		return PVR_HAS_QUIRK(pvr_dev, 62269);
	default:
		return false;
	};
}

bool
pvr_device_has_uapi_enhancement(struct pvr_device *pvr_dev, u32 enhancement)
{
	switch (enhancement) {
	case 35421:
		return PVR_HAS_ENHANCEMENT(pvr_dev, 35421);
	case 42064:
		return PVR_HAS_ENHANCEMENT(pvr_dev, 42064);
	default:
		return false;
	};
}

/**
 * pvr_device_has_feature() - Look up device feature based on feature definition
 * @pvr_dev: Device pointer.
 * @feature: Feature to look up. Should be one of %PVR_FEATURE_*.
 *
 * Returns:
 *  * %true if feature is present on device, or
 *  * %false if feature is not present on device.
 */
bool
pvr_device_has_feature(struct pvr_device *pvr_dev, u32 feature)
{
	switch (feature) {
	case PVR_FEATURE_CLUSTER_GROUPING:
		return PVR_HAS_FEATURE(pvr_dev, cluster_grouping);

	case PVR_FEATURE_COMPUTE_MORTON_CAPABLE:
		return PVR_HAS_FEATURE(pvr_dev, compute_morton_capable);

	case PVR_FEATURE_FB_CDC_V4:
		return PVR_HAS_FEATURE(pvr_dev, fb_cdc_v4);

	case PVR_FEATURE_GPU_MULTICORE_SUPPORT:
		return PVR_HAS_FEATURE(pvr_dev, gpu_multicore_support);

	case PVR_FEATURE_ISP_ZLS_D24_S8_PACKING_OGL_MODE:
		return PVR_HAS_FEATURE(pvr_dev, isp_zls_d24_s8_packing_ogl_mode);

	case PVR_FEATURE_S7_TOP_INFRASTRUCTURE:
		return PVR_HAS_FEATURE(pvr_dev, s7_top_infrastructure);

	case PVR_FEATURE_TESSELLATION:
		return PVR_HAS_FEATURE(pvr_dev, tessellation);

	case PVR_FEATURE_TPU_DM_GLOBAL_REGISTERS:
		return PVR_HAS_FEATURE(pvr_dev, tpu_dm_global_registers);

	case PVR_FEATURE_VDM_DRAWINDIRECT:
		return PVR_HAS_FEATURE(pvr_dev, vdm_drawindirect);

	case PVR_FEATURE_VDM_OBJECT_LEVEL_LLS:
		return PVR_HAS_FEATURE(pvr_dev, vdm_object_level_lls);

	case PVR_FEATURE_ZLS_SUBTILE:
		return PVR_HAS_FEATURE(pvr_dev, zls_subtile);

	/* Derived features. */
	case PVR_FEATURE_CDM_USER_MODE_QUEUE: {
		u8 cdm_control_stream_format = 0;

		PVR_FEATURE_VALUE(pvr_dev, cdm_control_stream_format, &cdm_control_stream_format);
		return (cdm_control_stream_format >= 2 && cdm_control_stream_format <= 4);
	}

	case PVR_FEATURE_REQUIRES_FB_CDC_ZLS_SETUP:
		if (PVR_HAS_FEATURE(pvr_dev, fbcdc_algorithm)) {
			u8 fbcdc_algorithm = 0;

			PVR_FEATURE_VALUE(pvr_dev, fbcdc_algorithm, &fbcdc_algorithm);
			return (fbcdc_algorithm < 3 || PVR_HAS_FEATURE(pvr_dev, fb_cdc_v4));
		}
		return false;

	default:
		WARN(true, "Looking up undefined feature %u\n", feature);
		return false;
	}
}
