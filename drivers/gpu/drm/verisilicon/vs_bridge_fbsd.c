// SPDX-License-Identifier: GPL-2.0-only
/*
 * FreeBSD DRM connector/encoder for Verisilicon DC8200 HDMI output.
 * Creates a DRM connector (HDMIA) and encoder (TMDS), programs DC8200
 * output registers, and calls the kernel-side jh7110_inno_hdmi driver
 * for PHY init.
 */

#ifdef __FreeBSD__

#include <linux/regmap.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>

struct jh7110_hdmi_mode {
	uint32_t	pixclock;
	uint16_t	hdisplay;
	uint16_t	hsync_start;
	uint16_t	hsync_end;
	uint16_t	htotal;
	uint16_t	vdisplay;
	uint16_t	vsync_start;
	uint16_t	vsync_end;
	uint16_t	vtotal;
	bool		hsync_positive;
	bool		vsync_positive;
	bool		interlace;
};

extern void jh7110_hdmi_enable(const struct jh7110_hdmi_mode *mode);
extern void jh7110_hdmi_disable(void);
extern bool jh7110_hdmi_is_available(void);
extern int jh7110_hdmi_read_edid(uint8_t *buf, size_t len);
extern bool jh7110_hdmi_is_connected(void);
/* 1 connected, 0 disconnected, -1 unknown (no HPD GPIO). */
extern int jh7110_hdmi_hpd_state(void);
/* True if a sink answers DDC with a valid EDID header. */
extern bool jh7110_hdmi_sink_present(void);
/* Called on every HPD edge; cb may sleep. */
extern void jh7110_hdmi_set_hotplug_cb(void (*cb)(void *), void *arg);
extern bool jh7110_hdmi_pixclock_supported(uint32_t pixclock);

#include "vs_bridge.h"
#include "vs_bridge_regs.h"
#include "vs_crtc.h"
#include "vs_dc.h"

void vs_hdmi_enable(struct vs_crtc *vcrtc)
{
	struct vs_dc *dc = vcrtc->dc;
	unsigned int output = vcrtc->id;

	if (vcrtc->base.state == NULL) {
		/*
		 * Nothing to program the PHY from. Dereferencing the state
		 * unconditionally here panics the kernel, so bail out rather
		 * than guess a mode: the CRTC is enabled again with a valid
		 * state on the next atomic commit.
		 */
		printf("vs_hdmi_enable: no CRTC state, skipping PHY setup\n");
	} else {
		const struct drm_display_mode *m =
		    &vcrtc->base.state->adjusted_mode;
		struct jh7110_hdmi_mode hm;

		/*
		 * The PHY generates the pixel clock, so it has to be
		 * programmed for the mode actually being set -- see
		 * jh7110_hdmi_config_pll(). drm keeps mode->clock in kHz.
		 */
		hm.pixclock = (uint32_t)m->clock * 1000;
		hm.hdisplay = m->hdisplay;
		hm.hsync_start = m->hsync_start;
		hm.hsync_end = m->hsync_end;
		hm.htotal = m->htotal;
		hm.vdisplay = m->vdisplay;
		hm.vsync_start = m->vsync_start;
		hm.vsync_end = m->vsync_end;
		hm.vtotal = m->vtotal;
		hm.hsync_positive = (m->flags & DRM_MODE_FLAG_PHSYNC) != 0;
		hm.vsync_positive = (m->flags & DRM_MODE_FLAG_PVSYNC) != 0;
		hm.interlace = (m->flags & DRM_MODE_FLAG_INTERLACE) != 0;

		jh7110_hdmi_enable(&hm);
	}

	/*
	 * DC8200 output init — exact sequence from jh7110_display.c.
	 */

	/* Panel config: DE_EN | DAT_EN | CLK_EN */
	regmap_write(dc->regs, VSDC_DISP_PANEL_CONFIG(output), 0x111);

	/* RGB-to-RGB matrix (BT.709 to BT.2020) */
	regmap_write(dc->regs, 0x1E20, 10279 | (5395 << 16));
	regmap_write(dc->regs, 0x1E28, 709 | (1132 << 16));
	regmap_write(dc->regs, 0x1E30, 15065 | (187 << 16));
	regmap_write(dc->regs, 0x1E38, 269 | (1442 << 16));
	regmap_write(dc->regs, 0x1E40, 14674);

	/* Scale config */
	regmap_write(dc->regs, 0x1520, 0x33);

	/* Dither off */
	regmap_write(dc->regs, 0x1410, 0);

	/* Stop panel before configuring */
	regmap_clear_bits(dc->regs, VSDC_DISP_PANEL_START,
			  VSDC_DISP_PANEL_START_RUNNING(output) |
			  VSDC_DISP_PANEL_START_MULTI_DISP_SYNC);

	/* Background color black */
	regmap_write(dc->regs, 0x1528, 0x00000000);

	/* DPI config: RGB888 */
	regmap_write(dc->regs, VSDC_DISP_DPI_CONFIG(output),
		     VSDC_DISP_DPI_CONFIG_FMT_RGB888);

	/* DP config: RGB888 + DP_EN for HDMI */
	regmap_write(dc->regs, VSDC_DISP_DP_CONFIG(output),
		     VSDC_DISP_DP_CONFIG_DP_EN |
		     VSDC_DISP_DP_CONFIG_FMT_RGB888);

	/* Clear YUV in panel config */
	regmap_clear_bits(dc->regs, VSDC_DISP_PANEL_CONFIG(output),
			  VSDC_DISP_PANEL_CONFIG_YUV);

	/* Panel config: add RUNNING */
	regmap_set_bits(dc->regs, VSDC_DISP_PANEL_CONFIG(output),
			VSDC_DISP_PANEL_CONFIG_RUNNING);

	/* Start panel */
	regmap_set_bits(dc->regs, VSDC_DISP_PANEL_START,
			VSDC_DISP_PANEL_START_RUNNING(output));

	/* Commit */
	regmap_set_bits(dc->regs, VSDC_DISP_PANEL_CONFIG_EX(output),
			VSDC_DISP_PANEL_CONFIG_EX_COMMIT);
}

void vs_hdmi_disable(struct vs_crtc *vcrtc)
{
	struct vs_dc *dc = vcrtc->dc;
	unsigned int output = vcrtc->id;

	jh7110_hdmi_disable();

	regmap_clear_bits(dc->regs, VSDC_DISP_PANEL_START,
			  VSDC_DISP_PANEL_START_RUNNING(output));
	regmap_clear_bits(dc->regs, VSDC_DISP_PANEL_CONFIG(output),
			  VSDC_DISP_PANEL_CONFIG_RUNNING);
	regmap_set_bits(dc->regs, VSDC_DISP_PANEL_CONFIG_EX(output),
			VSDC_DISP_PANEL_CONFIG_EX_COMMIT);
}

/*
 * Fallback geometry, used only when DDC/EDID fails. Previously this driver
 * advertised a single hardcoded 1280x720 mode unconditionally, so every
 * panel was driven at 720p regardless of what it actually supported -- the
 * cause of the undersized, unscaled desktop.
 */
#define	VS_HDMI_FALLBACK_MAX_W	1920
#define	VS_HDMI_FALLBACK_MAX_H	1080
#define	VS_HDMI_FALLBACK_PREF_W	1280
#define	VS_HDMI_FALLBACK_PREF_H	1024

#define	VS_EDID_BLOCK_LEN	128
#define	VS_EDID_MAX_BLOCKS	2

static int vs_hdmi_connector_get_modes(struct drm_connector *connector)
{
	uint8_t raw[VS_EDID_BLOCK_LEN * VS_EDID_MAX_BLOCKS];
	struct edid *edid = (struct edid *)raw;
	size_t want = VS_EDID_BLOCK_LEN;
	int count = 0;
	int ret;

	if (!jh7110_hdmi_is_available())
		goto fallback;

	ret = jh7110_hdmi_read_edid(raw, want);
	if (ret != 0)
		goto fallback;

	/*
	 * raw[126] is the extension block count. drm_edid_is_valid() checksums
	 * every block the base one claims exists, so the extensions have to be
	 * fetched BEFORE validating -- otherwise it walks uninitialised memory
	 * past block 0 and rejects an EDID that is actually fine.
	 */
	if (raw[126] > 0) {
		want = VS_EDID_BLOCK_LEN * (1 + raw[126]);
		if (want > sizeof(raw))
			want = sizeof(raw);

		ret = jh7110_hdmi_read_edid(raw, want);
		if (ret != 0)
			goto fallback;
	}

	if (!drm_edid_is_valid(edid))
		goto fallback;

	drm_connector_update_edid_property(connector, edid);
	count = drm_add_edid_modes(connector, edid);
	if (count > 0)
		return count;

fallback:
	drm_connector_update_edid_property(connector, NULL);
	count = drm_add_modes_noedid(connector, VS_HDMI_FALLBACK_MAX_W,
				     VS_HDMI_FALLBACK_MAX_H);
	if (count > 0)
		drm_set_preferred_mode(connector, VS_HDMI_FALLBACK_PREF_W,
				       VS_HDMI_FALLBACK_PREF_H);

	return count;
}


/*
 * HPD edge.
 *
 * drm_helper_hpd_irq_event(), not drm_kms_helper_hotplug_event(). The
 * difference is the whole design: this one re-probes every connector and
 * emits a uevent only when a status actually changed, while the other fires
 * unconditionally.
 *
 * That matters because the HPD line is not quiet. With a display attached
 * and driven it carries activity at the video refresh rate - about 30 edges
 * a second at 4K30. Firing an unconditional event on each of those made DRM
 * re-probe dozens of times a second and the compositor gave up. Re-probing
 * and comparing costs a little work per edge and produces nothing when
 * nothing changed, which is why StarFive's driver needs no debouncing.
 *
 * Runs on a taskqueue thread, so it may sleep - it re-probes and reads EDID.
 */
static void
vs_hdmi_hotplug(void *arg)
{
	struct drm_device *drm_dev = arg;

	drm_helper_hpd_irq_event(drm_dev);
}

static enum drm_connector_status
vs_hdmi_connector_detect(struct drm_connector *connector, bool force)
{
	int hpd;

	hpd = jh7110_hdmi_hpd_state();
	if (hpd >= 0)
		return (hpd > 0 ? connector_status_connected :
		    connector_status_disconnected);

	/*
	 * Only reached when there is no HPD GPIO at all.
	 *
	 * DDC deliberately is not consulted when HPD is available, even
	 * though it looks like a useful second opinion for cables that do not
	 * carry pin 19. Asking it costs every disconnect: the read returns
	 * the EDID still sitting in the controller's FIFO from the previous
	 * display, the header validates, and an unplugged connector reports
	 * connected. detect() then never changes status, so DRM never
	 * re-reads EDID and a swapped display keeps the old one's mode.
	 *
	 * A cable that does not carry HPD reads as absent instead. That is
	 * the better failure: it affects one broken cable, where trusting DDC
	 * breaks hot-plug for every cable.
	 */
	if (jh7110_hdmi_sink_present())
		return (connector_status_connected);

	/*
	 * Nothing to go on: report connected. Gating on a signal the board
	 * may not provide once left DRM never calling get_modes(), so no
	 * modes were published and the compositor exited with "no monitors
	 * available".
	 */
	return (connector_status_connected);
}

static const struct drm_connector_funcs vs_hdmi_connector_funcs = {
	.detect			= vs_hdmi_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

/*
 * Only advertise modes the PHY can actually generate. Without this a sink
 * offering, say, 1440x810 would be selected and then produce no signal,
 * because jh7110_hdmi_config_pll() has no table entry for its pixel clock.
 */
/* 3840x2160@30 needs 297 MHz; 4K60's 594 MHz does not work here. */
#define	VS_HDMI_MAX_PIXCLOCK	297000000U

static enum drm_mode_status
vs_hdmi_connector_mode_valid(struct drm_connector *connector,
			     struct drm_display_mode *mode)
{
	uint32_t pixclock = (uint32_t)mode->clock * 1000;

	/*
	 * A PLL table entry is necessary but not sufficient. The pre/post PLL
	 * tables carry entries up to 594 MHz, and the PHY does report both
	 * PLLs locked at that rate, but the sink gets no usable signal: a
	 * 3840x2160@60 modeset produces a blank display while the same panel
	 * shows a picture when driven at a lower rate. StarFive documents
	 * 4K@30 as this SoC's limit, so cap what we advertise rather than
	 * letting DRM pick a mode the hardware cannot actually drive.
	 */
	if (pixclock > VS_HDMI_MAX_PIXCLOCK)
		return (MODE_CLOCK_HIGH);

	if (!jh7110_hdmi_pixclock_supported(pixclock))
		return (MODE_CLOCK_RANGE);

	return (MODE_OK);
}

static const struct drm_connector_helper_funcs vs_hdmi_conn_helper_funcs = {
	.get_modes = vs_hdmi_connector_get_modes,
	.mode_valid = vs_hdmi_connector_mode_valid,
};

struct vs_bridge *vs_bridge_init(struct drm_device *drm_dev,
				 struct vs_crtc *crtc)
{
	struct vs_bridge *bridge;
	struct drm_encoder *enc;
	struct drm_connector *conn;
	int ret;

	if (crtc->id != 0)
		return NULL;

	bridge = drmm_kzalloc(drm_dev, sizeof(*bridge), GFP_KERNEL);
	if (!bridge)
		return ERR_PTR(-ENOMEM);

	bridge->crtc = crtc;
	bridge->intf = VSDC_OUTPUT_INTERFACE_DP;

	enc = drmm_plain_encoder_alloc(drm_dev, NULL,
				       DRM_MODE_ENCODER_TMDS, NULL);
	if (IS_ERR(enc))
		return ERR_CAST(enc);

	enc->possible_crtcs = drm_crtc_mask(&crtc->base);
	bridge->enc = enc;

	conn = drmm_kzalloc(drm_dev, sizeof(*conn), GFP_KERNEL);
	if (!conn)
		return ERR_PTR(-ENOMEM);

	ret = drm_connector_init(drm_dev, conn, &vs_hdmi_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ERR_PTR(ret);

	drm_connector_helper_add(conn, &vs_hdmi_conn_helper_funcs);
	/*
	 * Opt into output polling. vs_drm.c already calls
	 * drm_kms_helper_poll_init(), but the helper only probes connectors
	 * that ask for it - the default of 0 means detect() is called once at
	 * init and never again, so a display swapped at runtime is never
	 * noticed. With this the helper re-runs detect(), re-reads EDID on a
	 * new sink and sends a hot-plug event, which is what makes the
	 * compositor switch modes without being restarted.
	 *
	 * Polling is a stopgap: DRM's output poll period is 10 s and only
	 * reacts to a change in detect(), so a display unplugged and replaced
	 * inside one interval is invisible and the CRTC keeps the removed
	 * display's mode. Fixing that needs a real HPD interrupt, which in
	 * turn needs an interrupt controller in jh7110_gpio.
	 */
	/*
	 * DRM_CONNECTOR_POLL_HPD: this connector raises its own events, so
	 * the helper leaves it alone. Matches StarFive's driver.
	 *
	 * This is only correct because no edge is dropped. Nothing masks the
	 * pin now - the kernel throttles a storming source rather than
	 * masking it - so an unplug is always observed, which is what makes
	 * DRM re-read EDID on the replug. A missed disconnect is invisible:
	 * the connector reads connected either side of the swap, DRM sees no
	 * change and keeps the old display's mode.
	 */
	conn->polled = DRM_CONNECTOR_POLL_HPD;

	drm_connector_attach_encoder(conn, enc);

	jh7110_hdmi_set_hotplug_cb(vs_hdmi_hotplug, drm_dev);
	bridge->conn = conn;

	return bridge;
}

#endif /* __FreeBSD__ */
