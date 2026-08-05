/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Tej Kiran
 *
 * FreeBSD module declarations for the PowerVR DRM driver.
 */

#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>

MODULE_DEPEND(pvr, drmn, 2, 2, 2);
MODULE_DEPEND(pvr, linuxkpi, 1, 1, 1);
MODULE_DEPEND(pvr, dmabuf, 1, 1, 1);
MODULE_DEPEND(pvr, ttm, 1, 1, 1);
