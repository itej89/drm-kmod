// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright (c) 2023 Imagination Technologies Ltd. */

#include "pvr_device.h"
#include "pvr_fw.h"
#include "pvr_fw_meta.h"
#include "pvr_fw_startstop.h"
#include "pvr_rogue_cr_defs.h"
#include "pvr_rogue_meta.h"
#include "pvr_vm.h"

#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/types.h>

#define POLL_TIMEOUT_USEC 1000000

static void
rogue_axi_ace_list_init(struct pvr_device *pvr_dev)
{
	/* Setup AXI-ACE config. Set everything to outer cache. */
	u64 reg_val =
		(3U << ROGUE_CR_AXI_ACE_LITE_CONFIGURATION_AWDOMAIN_NON_SNOOPING_SHIFT) |
		(3U << ROGUE_CR_AXI_ACE_LITE_CONFIGURATION_ARDOMAIN_NON_SNOOPING_SHIFT) |
		(2U << ROGUE_CR_AXI_ACE_LITE_CONFIGURATION_ARDOMAIN_CACHE_MAINTENANCE_SHIFT) |
		(2U << ROGUE_CR_AXI_ACE_LITE_CONFIGURATION_AWDOMAIN_COHERENT_SHIFT) |
		(2U << ROGUE_CR_AXI_ACE_LITE_CONFIGURATION_ARDOMAIN_COHERENT_SHIFT) |
		(2U << ROGUE_CR_AXI_ACE_LITE_CONFIGURATION_AWCACHE_COHERENT_SHIFT) |
		(2U << ROGUE_CR_AXI_ACE_LITE_CONFIGURATION_ARCACHE_COHERENT_SHIFT) |
		(2U << ROGUE_CR_AXI_ACE_LITE_CONFIGURATION_ARCACHE_CACHE_MAINTENANCE_SHIFT);

	pvr_cr_write64(pvr_dev, ROGUE_CR_AXI_ACE_LITE_CONFIGURATION, reg_val);
}

static void
rogue_bif_init(struct pvr_device *pvr_dev)
{
	dma_addr_t pc_dma_addr;
	u64 pc_addr;

	/* Acquire the address of the Kernel Page Catalogue. */
	pc_dma_addr = pvr_vm_get_page_table_root_addr(pvr_dev->kernel_vm_ctx);

	/* Write the kernel catalogue base. */
	pc_addr = ((((u64)pc_dma_addr >> ROGUE_CR_BIF_CAT_BASE0_ADDR_ALIGNSHIFT)
		    << ROGUE_CR_BIF_CAT_BASE0_ADDR_SHIFT) &
		   ~ROGUE_CR_BIF_CAT_BASE0_ADDR_CLRMSK);

	pvr_cr_write64(pvr_dev, BIF_CAT_BASEX(MMU_CONTEXT_MAPPING_FWPRIV),
		       pc_addr);
}

static int
rogue_slc_init(struct pvr_device *pvr_dev)
{
	u16 slc_cache_line_size_bits;
	u32 reg_val;
	int err;

	/*
	 * SLC Misc control.
	 *
	 * Note: This is a 64bit register and we set only the lower 32bits
	 *       leaving the top 32bits (ROGUE_CR_SLC_CTRL_MISC_SCRAMBLE_BITS)
	 *       unchanged from the HW default.
	 */
	reg_val = (pvr_cr_read32(pvr_dev, ROGUE_CR_SLC_CTRL_MISC) &
		      ROGUE_CR_SLC_CTRL_MISC_ENABLE_PSG_HAZARD_CHECK_EN) |
		     ROGUE_CR_SLC_CTRL_MISC_ADDR_DECODE_MODE_PVR_HASH1;

	err = PVR_FEATURE_VALUE(pvr_dev, slc_cache_line_size_bits, &slc_cache_line_size_bits);
	if (err)
		return err;

	/* Bypass burst combiner if SLC line size is smaller than 1024 bits. */
	if (slc_cache_line_size_bits < 1024)
		reg_val |= ROGUE_CR_SLC_CTRL_MISC_BYPASS_BURST_COMBINER_EN;

	if (PVR_HAS_QUIRK(pvr_dev, 71242) && !PVR_HAS_FEATURE(pvr_dev, gpu_multicore_support))
		reg_val |= ROGUE_CR_SLC_CTRL_MISC_LAZYWB_OVERRIDE_EN;

#ifdef __FreeBSD__
	/*
	 * hw.pvr.force_lazywb=1 applies the BRN 71242 SLC writeback override
	 * regardless of the multicore feature.
	 *
	 * The firmware declares BRN 71242 *and* gpu_multicore_support, so the
	 * condition above skips the workaround. But that feature means the
	 * design supports multicore, not that cores are present - this part is
	 * BXE-4-32 MC1 and the DDK reports "1 core in the system". If the
	 * erratum applies to single-core instances, we are skipping a cache
	 * workaround we need.
	 */
	{
		char *ev = kern_getenv("hw.pvr.force_lazywb");

		if (ev != NULL) {
			if (strtoul(ev, NULL, 0) != 0)
				reg_val |= ROGUE_CR_SLC_CTRL_MISC_LAZYWB_OVERRIDE_EN;
			freeenv(ev);
		}
	}
#endif

#ifdef __FreeBSD__
	/*
	 * The BRN 71242 workaround sets LAZYWB_OVERRIDE, but only when the
	 * part lacks multicore support - so whether a cache-writeback
	 * workaround is applied depends on what the firmware declares. Print
	 * the inputs and the resulting SLC control value.
	 */
	printf("PVRSLC brn71242=%d multicore=%d slc_line_bits=%u reg_val=0x%08x lazywb=%d\n",
	       PVR_HAS_QUIRK(pvr_dev, 71242) ? 1 : 0,
	       PVR_HAS_FEATURE(pvr_dev, gpu_multicore_support) ? 1 : 0,
	       slc_cache_line_size_bits, reg_val,
	       (reg_val & ROGUE_CR_SLC_CTRL_MISC_LAZYWB_OVERRIDE_EN) ? 1 : 0);
#endif
	pvr_cr_write32(pvr_dev, ROGUE_CR_SLC_CTRL_MISC, reg_val);

	return 0;
}

/**
 * pvr_fw_start() - Start FW processor and boot firmware
 * @pvr_dev: Target PowerVR device.
 *
 * Returns:
 *  * 0 on success, or
 *  * Any error returned by rogue_slc_init().
 */
int
pvr_fw_start(struct pvr_device *pvr_dev)
{
	bool has_reset2 = PVR_HAS_FEATURE(pvr_dev, xe_tpu2);
	u64 soft_reset_mask;
	int err;

	if (PVR_HAS_FEATURE(pvr_dev, pbe2_in_xe))
		soft_reset_mask = ROGUE_CR_SOFT_RESET__PBE2_XE__MASKFULL;
	else
		soft_reset_mask = ROGUE_CR_SOFT_RESET_MASKFULL;

	if (PVR_HAS_FEATURE(pvr_dev, sys_bus_secure_reset)) {
		/*
		 * Disable the default sys_bus_secure protection to perform
		 * minimal setup.
		 */
		pvr_cr_write32(pvr_dev, ROGUE_CR_SYS_BUS_SECURE, 0);
		(void)pvr_cr_read32(pvr_dev, ROGUE_CR_SYS_BUS_SECURE); /* Fence write */
	}

	/* Set Rogue in soft-reset. */
	pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET, soft_reset_mask);
	if (has_reset2)
		pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET2, ROGUE_CR_SOFT_RESET2_MASKFULL);

	/* Read soft-reset to fence previous write in order to clear the SOCIF pipeline. */
	(void)pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET);
	if (has_reset2)
		(void)pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET2);

	/* Take Rascal and Dust out of reset. */
	pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET,
		       soft_reset_mask ^ ROGUE_CR_SOFT_RESET_RASCALDUSTS_EN);
	if (has_reset2)
		pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET2, 0);

	(void)pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET);
	if (has_reset2)
		(void)pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET2);

	/* Take everything out of reset but the FW processor. */
	pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET, ROGUE_CR_SOFT_RESET_GARTEN_EN);
	if (has_reset2)
		pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET2, 0);

	(void)pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET);
	if (has_reset2)
		(void)pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET2);

	err = rogue_slc_init(pvr_dev);
	if (err)
		goto err_reset;

	/* Initialise Firmware wrapper. */
	pvr_dev->fw_dev.defs->wrapper_init(pvr_dev);

	/* We must init the AXI-ACE interface before first BIF transaction. */
	rogue_axi_ace_list_init(pvr_dev);

	if (pvr_dev->fw_dev.processor_type != PVR_FW_PROCESSOR_TYPE_MIPS) {
		/* Initialise BIF. */
		rogue_bif_init(pvr_dev);
	}

	/* Need to wait for at least 16 cycles before taking the FW processor out of reset ... */
	udelay(3);

	pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET, 0x0);
	(void)pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET);

#ifdef __FreeBSD__
	/*
	 * ROGUE_CR_CLK_CTRL: per-unit OFF / ON / AUTO clock gating.
	 *
	 * We never write this register - upstream does not, and neither did we,
	 * so it keeps whatever the reset default is. The DDK's start sequence
	 * does program it. This matters because the firmware's power controller
	 * cannot gate a unit whose clock the host has forced ON: EXP-129 shows
	 * every single power-off request refused (Aborted 269/269, unit mask
	 * 0x01000702) where Debian's byte-identical request on the same silicon
	 * completes 37/37.
	 *
	 * Log the live value unconditionally; hw.pvr.clk_ctrl_auto=1 programs
	 * every unit to AUTO (0xAAAAAA002A2AAAAA, within MASKFULL
	 * 0xFFFFFF003F3FFFFF) so the two can be A/B'd.
	 */
	{
		u64 cc = pvr_cr_read64(pvr_dev, ROGUE_CR_CLK_CTRL);
		char *ev = kern_getenv("hw.pvr.clk_ctrl_auto");
		int want_auto = 0;

		if (ev != NULL) {
			want_auto = (int)strtoul(ev, NULL, 0);
			freeenv(ev);
		}

		printf("PVRCLKCTRL boot=0x%016llx maskfull=0x%016llx\n",
		       (unsigned long long)cc,
		       (unsigned long long)ROGUE_CR_CLK_CTRL_MASKFULL);

		if (want_auto) {
			pvr_cr_write64(pvr_dev, ROGUE_CR_CLK_CTRL,
				       0xAAAAAA002A2AAAAAULL);
			cc = pvr_cr_read64(pvr_dev, ROGUE_CR_CLK_CTRL);
			printf("PVRCLKCTRL forced all-AUTO, readback=0x%016llx\n",
			       (unsigned long long)cc);
		}
	}
#endif

	/* ... and afterwards. */
	udelay(3);

	return 0;

err_reset:
	/* Put everything back into soft-reset. */
	pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET, soft_reset_mask);

	return err;
}

/**
 * pvr_fw_stop() - Stop FW processor
 * @pvr_dev: Target PowerVR device.
 *
 * Returns:
 *  * 0 on success, or
 *  * Any error returned by pvr_cr_poll_reg32().
 */
int
pvr_fw_stop(struct pvr_device *pvr_dev)
{
	const u32 sidekick_idle_mask = ROGUE_CR_SIDEKICK_IDLE_MASKFULL &
				       ~(ROGUE_CR_SIDEKICK_IDLE_GARTEN_EN |
					 ROGUE_CR_SIDEKICK_IDLE_SOCIF_EN |
					 ROGUE_CR_SIDEKICK_IDLE_HOSTIF_EN);
	bool skip_garten_idle = false;
	u32 reg_value;
	int err;

	/*
	 * Wait for Sidekick/Jones to signal IDLE except for the Garten Wrapper.
	 * For cores with the LAYOUT_MARS feature, SIDEKICK would have been
	 * powered down by the FW.
	 */
	err = pvr_cr_poll_reg32(pvr_dev, ROGUE_CR_SIDEKICK_IDLE, sidekick_idle_mask,
				sidekick_idle_mask, POLL_TIMEOUT_USEC);
	if (err)
		return err;

	/* Unset MTS DM association with threads. */
	pvr_cr_write32(pvr_dev, ROGUE_CR_MTS_INTCTX_THREAD0_DM_ASSOC,
		       ROGUE_CR_MTS_INTCTX_THREAD0_DM_ASSOC_MASKFULL &
		       ROGUE_CR_MTS_INTCTX_THREAD0_DM_ASSOC_DM_ASSOC_CLRMSK);
	pvr_cr_write32(pvr_dev, ROGUE_CR_MTS_BGCTX_THREAD0_DM_ASSOC,
		       ROGUE_CR_MTS_BGCTX_THREAD0_DM_ASSOC_MASKFULL &
		       ROGUE_CR_MTS_BGCTX_THREAD0_DM_ASSOC_DM_ASSOC_CLRMSK);
	pvr_cr_write32(pvr_dev, ROGUE_CR_MTS_INTCTX_THREAD1_DM_ASSOC,
		       ROGUE_CR_MTS_INTCTX_THREAD1_DM_ASSOC_MASKFULL &
		       ROGUE_CR_MTS_INTCTX_THREAD1_DM_ASSOC_DM_ASSOC_CLRMSK);
	pvr_cr_write32(pvr_dev, ROGUE_CR_MTS_BGCTX_THREAD1_DM_ASSOC,
		       ROGUE_CR_MTS_BGCTX_THREAD1_DM_ASSOC_MASKFULL &
		       ROGUE_CR_MTS_BGCTX_THREAD1_DM_ASSOC_DM_ASSOC_CLRMSK);

	/* Extra Idle checks. */
	err = pvr_cr_poll_reg32(pvr_dev, ROGUE_CR_BIF_STATUS_MMU, 0,
				ROGUE_CR_BIF_STATUS_MMU_MASKFULL,
				POLL_TIMEOUT_USEC);
	if (err)
		return err;

	err = pvr_cr_poll_reg32(pvr_dev, ROGUE_CR_BIFPM_STATUS_MMU, 0,
				ROGUE_CR_BIFPM_STATUS_MMU_MASKFULL,
				POLL_TIMEOUT_USEC);
	if (err)
		return err;

	if (!PVR_HAS_FEATURE(pvr_dev, xt_top_infrastructure)) {
		err = pvr_cr_poll_reg32(pvr_dev, ROGUE_CR_BIF_READS_EXT_STATUS, 0,
					ROGUE_CR_BIF_READS_EXT_STATUS_MASKFULL,
					POLL_TIMEOUT_USEC);
		if (err)
			return err;
	}

	err = pvr_cr_poll_reg32(pvr_dev, ROGUE_CR_BIFPM_READS_EXT_STATUS, 0,
				ROGUE_CR_BIFPM_READS_EXT_STATUS_MASKFULL,
				POLL_TIMEOUT_USEC);
	if (err)
		return err;

	err = pvr_cr_poll_reg64(pvr_dev, ROGUE_CR_SLC_STATUS1, 0,
				ROGUE_CR_SLC_STATUS1_MASKFULL,
				POLL_TIMEOUT_USEC);
	if (err)
		return err;

	/*
	 * Wait for SLC to signal IDLE.
	 * For cores with the LAYOUT_MARS feature, SLC would have been powered
	 * down by the FW.
	 */
	err = pvr_cr_poll_reg32(pvr_dev, ROGUE_CR_SLC_IDLE,
				ROGUE_CR_SLC_IDLE_MASKFULL,
				ROGUE_CR_SLC_IDLE_MASKFULL, POLL_TIMEOUT_USEC);
	if (err)
		return err;

	/*
	 * Wait for Sidekick/Jones to signal IDLE except for the Garten Wrapper.
	 * For cores with the LAYOUT_MARS feature, SIDEKICK would have been powered
	 * down by the FW.
	 */
	err = pvr_cr_poll_reg32(pvr_dev, ROGUE_CR_SIDEKICK_IDLE, sidekick_idle_mask,
				sidekick_idle_mask, POLL_TIMEOUT_USEC);
	if (err)
		return err;

	if (pvr_dev->fw_dev.processor_type == PVR_FW_PROCESSOR_TYPE_META) {
		err = pvr_meta_cr_read32(pvr_dev, META_CR_TxVECINT_BHALT, &reg_value);
		if (err)
			return err;

		/*
		 * Wait for Sidekick/Jones to signal IDLE including the Garten
		 * Wrapper if there is no debugger attached (TxVECINT_BHALT =
		 * 0x0).
		 */
		if (reg_value)
			skip_garten_idle = true;
	}

	if (!skip_garten_idle) {
		err = pvr_cr_poll_reg32(pvr_dev, ROGUE_CR_SIDEKICK_IDLE,
					ROGUE_CR_SIDEKICK_IDLE_GARTEN_EN,
					ROGUE_CR_SIDEKICK_IDLE_GARTEN_EN,
					POLL_TIMEOUT_USEC);
		if (err)
			return err;
	}

	if (PVR_HAS_FEATURE(pvr_dev, pbe2_in_xe))
		pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET,
			       ROGUE_CR_SOFT_RESET__PBE2_XE__MASKFULL);
	else
		pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET, ROGUE_CR_SOFT_RESET_MASKFULL);

	return 0;
}
