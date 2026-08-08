/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DRM_CLIENT_SETUP_H
#define DRM_CLIENT_SETUP_H

struct drm_device;
struct drm_format_info;

static inline void drm_client_setup(struct drm_device *dev,
				     const struct drm_format_info *format)
{
}

static inline void drm_client_setup_with_fourcc(struct drm_device *dev,
						u32 fourcc)
{
}

#endif
