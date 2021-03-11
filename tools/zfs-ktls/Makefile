SRCTOP=/usr/home/somers/src/freebsd.org/src

ZFSTOP= ${SRCTOP}/sys/contrib/openzfs

MAN=
PROG=	zfs-sslrecv

#WARNS?= 2

CFLAGS=	-O0
CFLAGS+= \
        -DIN_BASE \
        -I${ZFSTOP}/include \
        -I${ZFSTOP}/include/os/freebsd \
        -I${ZFSTOP}/lib/libspl/include \
        -I${ZFSTOP}/lib/libspl/include/os/freebsd \
        -I${SRCTOP}/sys \
        -I${SRCTOP}/cddl/compat/opensolaris/include \
        -I${ZFSTOP}/module/icp/include \
        -include ${ZFSTOP}/include/os/freebsd/spl/sys/ccompile.h \
        -DHAVE_ISSETUGID \
        -include ${SRCTOP}/sys/modules/zfs/zfs_config.h \
        -I${SRCTOP}/sys/modules/zfs

LDFLAGS= -l ssl	-l crypto -l zfs
LIBADD=	ssl zfs
#LIBADD= jail avl nvpair geom  uutil zfs_core spl tpool zutil zfs m crypto
#LDADD+= -pthread
.include <bsd.prog.mk>
