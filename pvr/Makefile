# $FreeBSD$

.include "../kconfig.mk"

SRCDIR=	${.CURDIR:H}/drivers/gpu/drm/imagination

.PATH:	${SRCDIR}

KMOD=	pvr

SRCS=	pvr_ccb.c \
	pvr_cccb.c \
	pvr_context.c \
	pvr_device.c \
	pvr_device_info.c \
	pvr_drv.c \
	pvr_free_list.c \
	pvr_fw.c \
	pvr_fw_meta.c \
	pvr_fw_mips.c \
	pvr_fw_startstop.c \
	pvr_fw_trace.c \
	pvr_gem.c \
	pvr_hwrt.c \
	pvr_job.c \
	pvr_mmu.c \
	pvr_params.c \
	pvr_power.c \
	pvr_queue.c \
	pvr_stream.c \
	pvr_stream_defs.c \
	pvr_sync.c \
	pvr_vm.c \
	pvr_vm_mips.c

SRCS+=	pvr_debugfs.c

SRCS+=	\
	opt_acpi.h \
	opt_drm.h \
	opt_platform.h \
	opt_fdt.h \
	device_if.h \
	bus_if.h \
	pci_if.h \
	ofw_bus_if.h \
	ofw_if.h \
	vnode_if.h

CFLAGS+= -I${.CURDIR:H}/linuxkpi/gplv2/include
CFLAGS+= -I${.CURDIR:H}/linuxkpi/bsd/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/common/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/dummy/include
CFLAGS+= -I${.CURDIR:H}/drivers/gpu/drm
CFLAGS+= -I${SRCDIR}
CFLAGS+= -I${.CURDIR:H}/include
CFLAGS+= -I${.CURDIR:H}/include/drm
CFLAGS+= -I${.CURDIR:H}/include/uapi
CFLAGS+= -I${.CURDIR:H}/drivers/gpu

CFLAGS+= -Daligned_u64=__aligned_u64
CFLAGS+= -include ${SRCDIR}/pvr_freebsd_compat.h
CFLAGS+= '-DKBUILD_MODNAME="pvr"'
CFLAGS+= '-DLINUXKPI_PARAM_PREFIX=pvr_'
CFLAGS+= -DCONFIG_DRM_POWERVR
CFLAGS+= -DCONFIG_DEBUG_FS
CWARNFLAGS+= -Wno-integer-overflow
CWARNFLAGS.pvr_vm.c+= -Wno-constant-conversion

.include "../linuxkpi_version.mk"
.include "../compiler_flags.mk"

.include <bsd.kmod.mk>
