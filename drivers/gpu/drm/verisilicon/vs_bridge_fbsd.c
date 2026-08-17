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

static enum drm_connector_status
vs_hdmi_connector_detect(struct drm_connector *connector, bool force)
{
	/*
	 * Now that the HPD pin is muxed (it was not before the hdmi-0 pin
	 * group was added), the core's hot-plug bit is meaningful. The helper
	 * also reports connected if a valid EDID has ever been read, so a sink
	 * that answers DDC but drives HPD weakly is not declared absent.
	 */
	if (jh7110_hdmi_is_connected())
		return (connector_status_connected);

	return (connector_status_disconnected);
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
static enum drm_mode_status
vs_hdmi_connector_mode_valid(struct drm_connector *connector,
			     struct drm_display_mode *mode)
{
	if (!jh7110_hdmi_pixclock_supported((uint32_t)mode->clock * 1000))
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
	drm_connector_attach_encoder(conn, enc);
	bridge->conn = conn;

	return bridge;
}

#endif /* __FreeBSD__ */
