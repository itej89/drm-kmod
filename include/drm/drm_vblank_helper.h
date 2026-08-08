/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _DRM_VBLANK_HELPER_H_
#define _DRM_VBLANK_HELPER_H_

#include <drm/drm_vblank.h>

struct drm_atomic_commit;
struct drm_crtc;

static inline void drm_crtc_vblank_atomic_flush(struct drm_crtc *crtc,
						 struct drm_atomic_commit *commit)
{
}

static inline void drm_crtc_vblank_atomic_enable(struct drm_crtc *crtc,
						  struct drm_atomic_commit *commit)
{
	drm_crtc_vblank_on(crtc);
}

static inline void drm_crtc_vblank_atomic_disable(struct drm_crtc *crtc,
						   struct drm_atomic_commit *commit)
{
	drm_crtc_vblank_off(crtc);
}

#define DRM_CRTC_HELPER_VBLANK_FUNCS \
	.atomic_flush = drm_crtc_vblank_atomic_flush, \
	.atomic_enable = drm_crtc_vblank_atomic_enable, \
	.atomic_disable = drm_crtc_vblank_atomic_disable

#endif
