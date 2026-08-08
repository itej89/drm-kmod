# $FreeBSD$

.include "../kconfig.mk"

SRCDIR= ${.CURDIR:H}/drivers/gpu/drm/verisilicon

.PATH: ${SRCDIR}

KMOD=   verisilicon

SRCS=   vs_drm.c \
	vs_dc.c \
	vs_crtc.c \
	vs_plane.c \
	vs_primary_plane.c \
	vs_cursor_plane.c \
	vs_hwdb.c

SRCS+=  ${LINUXKPI_GENSRCS}
SRCS+=  opt_drm.h

CFLAGS+= -I${.CURDIR:H}/linuxkpi/gplv2/include
CFLAGS+= -I${.CURDIR:H}/linuxkpi/bsd/include
CFLAGS+= ${LINUXKPI_INCLUDES}
CFLAGS+= -I${.CURDIR:H}/drivers/gpu/drm
CFLAGS+= -I${SRCDIR}
CFLAGS+= -I${.CURDIR:H}/include
CFLAGS+= -I${.CURDIR:H}/include/drm
CFLAGS+= -I${.CURDIR:H}/include/uapi
CFLAGS+= -I${.CURDIR:H}/drivers/gpu
CFLAGS+= -Daligned_u64=__aligned_u64
CFLAGS+= '-DKBUILD_MODNAME="verisilicon"'
CFLAGS+= '-DLINUXKPI_PARAM_PREFIX=vs_'

CWARNFLAGS+= -Wno-missing-prototypes
CWARNFLAGS+= -Wno-pointer-sign
CWARNFLAGS+= -Wno-int-conversion

EXPORT_SYMS= YES

.include <bsd.kmod.mk>
