.if exists(${SYSDIR}/compat/linuxkpi/common/include/linux/compiler_types.h)
# Linux' Makefile (scripts/Makefile.lib) always include
# <linux/compiler_types.h> from the compiler command line.
#
# At least `drivers/gpu/drm/drm_atomic_uapi.c` and
# `include/drm/drm_atomic_uap.h` depends on this to compile.
CFLAGS+=	-include ${SYSDIR}/compat/linuxkpi/common/include/linux/compiler_types.h
.endif

# Other headers are always included. See the definition of `$(USERINCLUDE)` in
# the Makefile at the root of the Linux source tree.
.if exists(${SYSDIR}/compat/linuxkpi/common/include/linux/compiler-version.h)
CFLAGS+=	-include ${SYSDIR}/compat/linuxkpi/common/include/linux/compiler-version.h
.endif
CFLAGS+=	-include ${SYSDIR}/compat/linuxkpi/common/include/linux/kconfig.h

CWARNFLAGS+=	-Wno-pointer-sign

# Globally silence a new warning from Clang 21.
# See https://lkml.org/lkml/2025/5/6/1681.
.if defined(COMPILER_TYPE) && ${COMPILER_TYPE} == clang && ${COMPILER_VERSION} >= 210100
CWARNFLAGS+=	-Wno-default-const-init-var-unsafe
.endif
