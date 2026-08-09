/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _DRM_VBLANK_HELPER_H_
#define _DRM_VBLANK_HELPER_H_

#include <drm/drm_vblank.h>

struct drm_atomic_state;
struct drm_crtc;

static inline void drm_crtc_vblank_atomic_flush(struct drm_crtc *crtc,
						 struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);

	if (crtc_state && crtc_state->event) {
		unsigned long flags;

		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, crtc_state->event);
		else
			drm_crtc_send_vblank_event(crtc, crtc_state->event);
		crtc_state->event = NULL;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}
}

static inline void drm_crtc_vblank_atomic_enable(struct drm_crtc *crtc,
						  struct drm_atomic_state *commit)
{
	drm_crtc_vblank_on(crtc);
}

static inline void drm_crtc_vblank_atomic_disable(struct drm_crtc *crtc,
						   struct drm_atomic_state *commit)
{
	drm_crtc_vblank_off(crtc);
}

#define DRM_CRTC_HELPER_VBLANK_FUNCS \
	.atomic_flush = drm_crtc_vblank_atomic_flush, \
	.atomic_enable = drm_crtc_vblank_atomic_enable, \
	.atomic_disable = drm_crtc_vblank_atomic_disable

#endif
