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
#include <drm/drm_probe_helper.h>

extern void jh7110_hdmi_enable(void);
extern void jh7110_hdmi_disable(void);

#include "vs_bridge.h"
#include "vs_bridge_regs.h"
#include "vs_crtc.h"
#include "vs_dc.h"

void vs_hdmi_enable(struct vs_crtc *vcrtc)
{
	struct vs_dc *dc = vcrtc->dc;
	unsigned int output = vcrtc->id;

	printf("vs_hdmi_enable: start output=%u\n", output);

	jh7110_hdmi_enable();
	printf("vs_hdmi_enable: hdmi phy done\n");

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
	printf("vs_hdmi_enable: DC8200 output configured\n");
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

static int vs_hdmi_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev,
		&(struct drm_display_mode) {
			DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER |
				 DRM_MODE_TYPE_PREFERRED,
				 74250, 1280, 1390, 1430, 1650, 0,
				 720, 725, 730, 750, 0,
				 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC)
		});
	if (!mode)
		return 0;

	drm_mode_probed_add(connector, mode);
	return 1;
}

static enum drm_connector_status
vs_hdmi_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static const struct drm_connector_funcs vs_hdmi_connector_funcs = {
	.detect			= vs_hdmi_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs vs_hdmi_conn_helper_funcs = {
	.get_modes = vs_hdmi_connector_get_modes,
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
