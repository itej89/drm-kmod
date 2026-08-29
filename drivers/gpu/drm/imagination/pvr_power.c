// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright (c) 2023 Imagination Technologies Ltd. */

#include "pvr_device.h"
#include "pvr_fw.h"
#include "pvr_fw_startstop.h"
#include "pvr_power.h"
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif
#ifdef __FreeBSD__
#include <dev/pwrdom/pwrdom.h>
#endif
#include "pvr_fw_trace.h"
#include "pvr_queue.h"
#include "pvr_rogue_fwif.h"

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define POWER_SYNC_TIMEOUT_US (1000000) /* 1s */

#define WATCHDOG_TIME_MS (500)

/**
 * pvr_device_lost() - Mark GPU device as lost
 * @pvr_dev: Target PowerVR device.
 *
 * This will cause the DRM device to be unplugged.
 */
void
pvr_device_lost(struct pvr_device *pvr_dev)
{
	if (!pvr_dev->lost) {
		pvr_dev->lost = true;
		drm_dev_unplug(from_pvr_device(pvr_dev));
	}
}

static int
pvr_power_send_command(struct pvr_device *pvr_dev, struct rogue_fwif_kccb_cmd *pow_cmd)
{
	struct pvr_fw_device *fw_dev = &pvr_dev->fw_dev;
	u32 slot_nr;
	u32 value;
	int err;

	WRITE_ONCE(*fw_dev->power_sync, 0);
#ifdef __FreeBSD__
	pvr_dma_cache_wbinv(fw_dev->power_sync, sizeof(*fw_dev->power_sync));
#endif

	err = pvr_kccb_send_cmd_powered(pvr_dev, pow_cmd, &slot_nr);
	if (err)
		return err;

	/* Wait for FW to acknowledge. */
	return readl_poll_timeout(pvr_dev->fw_dev.power_sync, value,
				  ({ pvr_dma_cache_inv(pvr_dev->fw_dev.power_sync,
				     sizeof(*pvr_dev->fw_dev.power_sync));
				  value != 0; }), 100,
				  POWER_SYNC_TIMEOUT_US);
}

static int
pvr_power_request_idle(struct pvr_device *pvr_dev)
{
	struct rogue_fwif_kccb_cmd pow_cmd;

	/* Send FORCED_IDLE request to FW. */
	pow_cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_POW;
	pow_cmd.cmd_data.pow_data.pow_type = ROGUE_FWIF_POW_FORCED_IDLE_REQ;
	pow_cmd.cmd_data.pow_data.power_req_data.pow_request_type = ROGUE_FWIF_POWER_FORCE_IDLE;

	return pvr_power_send_command(pvr_dev, &pow_cmd);
}

/**
 * pvr_power_notify_apm_latency() - Tell the firmware to re-read the APM latency.
 * @pvr_dev: Target PowerVR device.
 *
 * The driver fills in runtime_cfg->active_pm_latency_ms once at init and never
 * tells the firmware, so the firmware keeps whatever it booted with. Comparing
 * firmware traces against the DDK on the same silicon, "Active PM latency set
 * to %d ms" appears once per 3D TQ kick there and **never** here, while our
 * power request rate is 19.3 per kick against their 6.2.
 *
 * The command carries no payload - it makes the firmware re-read runtime_cfg.
 */
int
pvr_power_notify_apm_latency(struct pvr_device *pvr_dev)
{
	struct rogue_fwif_kccb_cmd pow_cmd = { 0 };

	pow_cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_POW;
	pow_cmd.cmd_data.pow_data.pow_type = ROGUE_FWIF_POW_APM_LATENCY_CHANGE;

	return pvr_power_send_command(pvr_dev, &pow_cmd);
}

static int
pvr_power_request_pwr_off(struct pvr_device *pvr_dev)
{
	struct rogue_fwif_kccb_cmd pow_cmd;

	/* Send POW_OFF request to firmware. */
	pow_cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_POW;
	pow_cmd.cmd_data.pow_data.pow_type = ROGUE_FWIF_POW_OFF_REQ;
	pow_cmd.cmd_data.pow_data.power_req_data.forced = true;

	return pvr_power_send_command(pvr_dev, &pow_cmd);
}

static int
pvr_power_fw_disable(struct pvr_device *pvr_dev, bool hard_reset)
{
	if (!hard_reset) {
		int err;

		cancel_delayed_work_sync(&pvr_dev->watchdog.work);

		err = pvr_power_request_idle(pvr_dev);
		if (err)
			return err;

		err = pvr_power_request_pwr_off(pvr_dev);
		if (err)
			return err;
	}

	return pvr_fw_stop(pvr_dev);
}

static int
pvr_power_fw_enable(struct pvr_device *pvr_dev)
{
	int err;

	err = pvr_fw_start(pvr_dev);
	if (err)
		return err;

	err = pvr_wait_for_fw_boot(pvr_dev);
	if (err) {
		drm_err(from_pvr_device(pvr_dev), "Firmware failed to boot\n");
		pvr_fw_stop(pvr_dev);
		return err;
	}

	queue_delayed_work(pvr_dev->sched_wq, &pvr_dev->watchdog.work,
			   msecs_to_jiffies(WATCHDOG_TIME_MS));

	return 0;
}

bool
pvr_power_is_idle(struct pvr_device *pvr_dev)
{
	/*
	 * FW power state can be out of date if a KCCB command has been submitted but the FW hasn't
	 * started processing it yet. So also check the KCCB status.
	 */
#ifdef __FreeBSD__
	pvr_dma_cache_inv(pvr_dev->fw_dev.fwif_sysdata,
	    sizeof(*pvr_dev->fw_dev.fwif_sysdata));
#endif
	enum rogue_fwif_pow_state pow_state = READ_ONCE(pvr_dev->fw_dev.fwif_sysdata->pow_state);
	bool kccb_idle = pvr_kccb_is_idle(pvr_dev);

	return (pow_state == ROGUE_FWIF_POW_IDLE) && kccb_idle;
}

static bool
pvr_watchdog_kccb_stalled(struct pvr_device *pvr_dev)
{
	/* Check KCCB commands are progressing. */
#ifdef __FreeBSD__
	pvr_dma_cache_inv(pvr_dev->fw_dev.fwif_osdata,
	    sizeof(*pvr_dev->fw_dev.fwif_osdata));
#endif
	u32 kccb_cmds_executed = pvr_dev->fw_dev.fwif_osdata->kccb_cmds_executed;
	bool kccb_is_idle = pvr_kccb_is_idle(pvr_dev);

	if (pvr_dev->watchdog.old_kccb_cmds_executed == kccb_cmds_executed && !kccb_is_idle) {
		pvr_dev->watchdog.kccb_stall_count++;

		/*
		 * If we have commands pending with no progress for 2 consecutive polls then
		 * consider KCCB command processing stalled.
		 */
		if (pvr_dev->watchdog.kccb_stall_count == 2) {
			pvr_dev->watchdog.kccb_stall_count = 0;
			return true;
		}
	} else if (pvr_dev->watchdog.old_kccb_cmds_executed == kccb_cmds_executed) {
		bool has_active_contexts;

		mutex_lock(&pvr_dev->queues.lock);
		has_active_contexts = list_empty(&pvr_dev->queues.active);
		mutex_unlock(&pvr_dev->queues.lock);

		if (has_active_contexts) {
			/* Send a HEALTH_CHECK command so we can verify FW is still alive. */
			struct rogue_fwif_kccb_cmd health_check_cmd;

			health_check_cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_HEALTH_CHECK;

			pvr_kccb_send_cmd_powered(pvr_dev, &health_check_cmd, NULL);
		}
	} else {
		pvr_dev->watchdog.old_kccb_cmds_executed = kccb_cmds_executed;
		pvr_dev->watchdog.kccb_stall_count = 0;
	}

	return false;
}

static void
pvr_watchdog_worker(struct work_struct *work)
{
	struct pvr_device *pvr_dev = container_of(work, struct pvr_device,
						  watchdog.work.work);
	bool stalled;

	if (pvr_dev->lost)
		return;

#ifdef __FreeBSD__
	/*
	 * hw.pvr.trace_watchdog dumps the firmware trace from the watchdog, so
	 * it can be read during normal operation. The fault-path dump only
	 * fires on a context reset, which is useless for comparing the power
	 * state machine against the working Debian reference on runs that do
	 * not fault.
	 */
	{
		static int wd_dumps;
		int wd_max = 0;
		char *ev = kern_getenv("hw.pvr.trace_watchdog");

		if (ev != NULL) {
			wd_max = (int)strtoul(ev, NULL, 0);
			freeenv(ev);
		}

		if (wd_max > 0 && wd_dumps < wd_max) {
			u32 n = 3000;
			char *dv = kern_getenv("hw.pvr.fw_trace_dwords");

			if (dv != NULL) {
				n = (u32)strtoul(dv, NULL, 0);
				freeenv(dv);
			}
			wd_dumps++;
			pvr_fw_trace_dump(pvr_dev, n);
		}
	}
#endif

#ifdef __FreeBSD__
	/*
	 * hw.pvr.force_reset=N performs N hardware resets from the watchdog.
	 *
	 * Rendering is correct until the first GPU reset of a boot and
	 * produces nothing for every context afterwards, so the question is
	 * whether the firmware can be brought back without rebooting. Set the
	 * tunable, wait a moment, then re-run tests/vk_reuse_render.
	 */
	{
		static int forced_resets;
		int want = 0;
		char *ev = kern_getenv("hw.pvr.force_reset");

		if (ev != NULL) {
			want = (int)strtoul(ev, NULL, 0);
			freeenv(ev);
		}

		if (want > forced_resets) {
			forced_resets++;
			drm_info(from_pvr_device(pvr_dev),
				 "force_reset: performing hard reset %d\n",
				 forced_resets);
			pvr_power_reset(pvr_dev, true);
			goto out_requeue;
		}
	}
#endif

	if (pm_runtime_get_if_in_use(from_pvr_device(pvr_dev)->dev) <= 0)
		goto out_requeue;

	if (!pvr_dev->fw_dev.booted)
		goto out_pm_runtime_put;

	stalled = pvr_watchdog_kccb_stalled(pvr_dev);

	if (stalled) {
		drm_err(from_pvr_device(pvr_dev), "FW stalled, trying hard reset");

		pvr_power_reset(pvr_dev, true);
		/* Device may be lost at this point. */
	}

out_pm_runtime_put:
	pm_runtime_put(from_pvr_device(pvr_dev)->dev);

out_requeue:
	if (!pvr_dev->lost) {
		queue_delayed_work(pvr_dev->sched_wq, &pvr_dev->watchdog.work,
				   msecs_to_jiffies(WATCHDOG_TIME_MS));
	}
}

/**
 * pvr_watchdog_init() - Initialise watchdog for device
 * @pvr_dev: Target PowerVR device.
 *
 * Returns:
 *  * 0 on success, or
 *  * -%ENOMEM on out of memory.
 */
int
pvr_watchdog_init(struct pvr_device *pvr_dev)
{
	INIT_DELAYED_WORK(&pvr_dev->watchdog.work, pvr_watchdog_worker);

	return 0;
}

int
pvr_power_device_suspend(struct device *dev)
{
	struct platform_device *plat_dev = to_platform_device(dev);
	struct drm_device *drm_dev = platform_get_drvdata(plat_dev);
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	int err = 0;
	int idx;

	if (!drm_dev_enter(drm_dev, &idx))
		return -EIO;

	if (pvr_dev->fw_dev.booted) {
		err = pvr_power_fw_disable(pvr_dev, false);
		if (err)
			goto err_drm_dev_exit;
	}

	clk_disable_unprepare(pvr_dev->mem_clk);
	clk_disable_unprepare(pvr_dev->sys_clk);
	clk_disable_unprepare(pvr_dev->core_clk);

#ifdef __FreeBSD__
	/*
	 * Drop the SoC power island (JH7110_PD_GPUA) last, once the firmware is
	 * stopped and the clocks are gated. Debian's genpd does exactly this
	 * and its CURR_POWER_MODE shows the GPUA bit clear while the GPU is
	 * idle; ours has never powered the island down at all.
	 */
	if (pvr_dev->fbsd_pwrdom != NULL) {
		int perr = pwrdom_disable((pwrdom_t)pvr_dev->fbsd_pwrdom);

		drm_info(from_pvr_device(pvr_dev),
			 "power domain disable err=%d\n", perr);
	}
#endif

err_drm_dev_exit:
	drm_dev_exit(idx);

	return err;
}

int
pvr_power_device_resume(struct device *dev)
{
	struct platform_device *plat_dev = to_platform_device(dev);
	struct drm_device *drm_dev = platform_get_drvdata(plat_dev);
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);
	int idx;
	int err;

	if (!drm_dev_enter(drm_dev, &idx))
		return -EIO;

#ifdef __FreeBSD__
	/* Island first: the clocks and registers below live inside it. */
	if (pvr_dev->fbsd_pwrdom != NULL) {
		err = pwrdom_enable((pwrdom_t)pvr_dev->fbsd_pwrdom);
		if (err) {
			drm_err(from_pvr_device(pvr_dev),
				"power domain enable failed (%d)\n", err);
			goto err_drm_dev_exit;
		}
	}
#endif

	err = clk_prepare_enable(pvr_dev->core_clk);
	if (err)
		goto err_drm_dev_exit;

	err = clk_prepare_enable(pvr_dev->sys_clk);
	if (err)
		goto err_core_clk_disable;

	err = clk_prepare_enable(pvr_dev->mem_clk);
	if (err)
		goto err_sys_clk_disable;

	if (pvr_dev->fw_dev.booted) {
		err = pvr_power_fw_enable(pvr_dev);
		if (err)
			goto err_mem_clk_disable;
	}

	drm_dev_exit(idx);

	return 0;

err_mem_clk_disable:
	clk_disable_unprepare(pvr_dev->mem_clk);

err_sys_clk_disable:
	clk_disable_unprepare(pvr_dev->sys_clk);

err_core_clk_disable:
	clk_disable_unprepare(pvr_dev->core_clk);

err_drm_dev_exit:
	drm_dev_exit(idx);

	return err;
}

int
pvr_power_device_idle(struct device *dev)
{
	struct platform_device *plat_dev = to_platform_device(dev);
	struct drm_device *drm_dev = platform_get_drvdata(plat_dev);
	struct pvr_device *pvr_dev = to_pvr_device(drm_dev);

	return pvr_power_is_idle(pvr_dev) ? 0 : -EBUSY;
}

/**
 * pvr_power_reset() - Reset the GPU
 * @pvr_dev: Device pointer
 * @hard_reset: %true for hard reset, %false for soft reset
 *
 * If @hard_reset is %false and the FW processor fails to respond during the reset process, this
 * function will attempt a hard reset.
 *
 * If a hard reset fails then the GPU device is reported as lost.
 *
 * Returns:
 *  * 0 on success, or
 *  * Any error code returned by pvr_power_get, pvr_power_fw_disable or pvr_power_fw_enable().
 */
int
pvr_power_reset(struct pvr_device *pvr_dev, bool hard_reset)
{
	bool queues_disabled = false;
	int err;

	/*
	 * Take a power reference during the reset. This should prevent any interference with the
	 * power state during reset.
	 */
	WARN_ON(pvr_power_get(pvr_dev));

	down_write(&pvr_dev->reset_sem);

	if (pvr_dev->lost) {
		err = -EIO;
		goto err_up_write;
	}

	/* Disable IRQs for the duration of the reset. */
	disable_irq(pvr_dev->irq);

	do {
		if (hard_reset) {
			pvr_queue_device_pre_reset(pvr_dev);
			queues_disabled = true;
		}

		err = pvr_power_fw_disable(pvr_dev, hard_reset);
		if (!err) {
			if (hard_reset) {
				pvr_dev->fw_dev.booted = false;
				WARN_ON(pm_runtime_force_suspend(from_pvr_device(pvr_dev)->dev));

				err = pvr_fw_hard_reset(pvr_dev);
				if (err)
					goto err_device_lost;

				err = pm_runtime_force_resume(from_pvr_device(pvr_dev)->dev);
				pvr_dev->fw_dev.booted = true;
				if (err)
					goto err_device_lost;
			} else {
				/* Clear the FW faulted flags. */
#ifdef __FreeBSD__
				pvr_dma_cache_inv(pvr_dev->fw_dev.fwif_sysdata,
				    sizeof(*pvr_dev->fw_dev.fwif_sysdata));
#endif
				pvr_dev->fw_dev.fwif_sysdata->hwr_state_flags &=
					~(ROGUE_FWIF_HWR_FW_FAULT |
					  ROGUE_FWIF_HWR_RESTART_REQUESTED);
#ifdef __FreeBSD__
				pvr_dma_cache_wbinv(&pvr_dev->fw_dev.fwif_sysdata->hwr_state_flags,
				    sizeof(pvr_dev->fw_dev.fwif_sysdata->hwr_state_flags));
#endif
			}

			pvr_fw_irq_clear(pvr_dev);

			err = pvr_power_fw_enable(pvr_dev);
		}

		if (err && hard_reset)
			goto err_device_lost;

		if (err && !hard_reset) {
			drm_err(from_pvr_device(pvr_dev), "FW stalled, trying hard reset");
			hard_reset = true;
		}
	} while (err);

	if (queues_disabled)
		pvr_queue_device_post_reset(pvr_dev);

	enable_irq(pvr_dev->irq);

	up_write(&pvr_dev->reset_sem);

	pvr_power_put(pvr_dev);

	return 0;

err_device_lost:
	drm_err(from_pvr_device(pvr_dev), "GPU device lost");
	pvr_device_lost(pvr_dev);

	/* Leave IRQs disabled if the device is lost. */

	if (queues_disabled)
		pvr_queue_device_post_reset(pvr_dev);

err_up_write:
	up_write(&pvr_dev->reset_sem);

	pvr_power_put(pvr_dev);

	return err;
}

/**
 * pvr_watchdog_fini() - Shutdown watchdog for device
 * @pvr_dev: Target PowerVR device.
 */
void
pvr_watchdog_fini(struct pvr_device *pvr_dev)
{
	cancel_delayed_work_sync(&pvr_dev->watchdog.work);
}


#ifdef __FreeBSD__
/*
 * Manual suspend/resume, for proving the power-down path.
 *
 * pvr_power_device_suspend()/resume() are complete but nothing calls them:
 * LinuxKPI's pm_runtime is a pure stub, so the GPU never idles and the power
 * island stays on forever. Automatic runtime PM additionally needs a resume
 * hook on every path that submits work - miss one and the GPU is touched
 * while powered down. So drive it by hand first and confirm the island really
 * cycles:
 *
 *   sysctl hw.pvr.suspend=1   stop FW, gate clocks, drop the island
 *   sysctl hw.pvr.suspend=0   raise the island, ungate, restart FW
 */
static struct pvr_device *pvr_fbsd_dev;

void
pvr_power_fbsd_register(struct pvr_device *pvr_dev)
{
	pvr_fbsd_dev = pvr_dev;
}

static int
pvr_fbsd_suspend_sysctl(SYSCTL_HANDLER_ARGS)
{
	int val = 0, error;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (pvr_fbsd_dev == NULL)
		return (ENXIO);

	if (val)
		error = pvr_power_device_suspend(
		    from_pvr_device(pvr_fbsd_dev)->dev);
	else
		error = pvr_power_device_resume(
		    from_pvr_device(pvr_fbsd_dev)->dev);

	printf("PVRPM %s err=%d\n", val ? "suspend" : "resume", error);
	return (error > 0 ? error : -error);
}

SYSCTL_PROC(_hw, OID_AUTO, pvr_suspend,
    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_MPSAFE, NULL, 0,
    pvr_fbsd_suspend_sysctl, "I",
    "1 = suspend GPU (stop FW, gate clocks, drop power island); 0 = resume");
#endif
