m4_include([version.m4])
AC_INIT([blktap], BLKTAP_VERSION)
AM_INIT_AUTOMAKE([foreign])
# AM_SILENT_RULES([yes])

CFLAGS="$CFLAGS -std=gnu11"

AC_PROG_CC
AC_PROG_LIBTOOL
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
AC_CANONICAL_HOST
AC_SYS_LARGEFILE
AC_CHECK_HEADERS([uuid/uuid.h], [], [Need uuid-dev])
AC_CHECK_HEADERS([libaio.h], [], [Need libaio-dev])
AC_CHECK_HEADERS([limits.h], [], [AC_MSG_ERROR([cannot find limits.h])])
AC_CHECK_HEADERS([time.h], [], [AC_MSG_ERROR([cannot find time.h])])
AC_CHECK_HEADERS([xen/io/ring.h], [], [AC_MSG_ERROR([cannot find xen/io/ring.h, install xen-devel])])
AC_CHECK_HEADERS([openssl/evp.h], [], [AC_MSG_ERROR([cannot find openssl/evp.h, install xs-openssl-devel])])
AC_CHECK_HEADERS([xenevtchn.h], [], [AC_MSG_ERROR([cannot find xenevtchn.h, install xen-libs-devel])])

AC_ARG_WITH([libiconv],
	     [AS_HELP_STRING([--with-libiconv],
			     [link to libiconv])],
             [],
             [with_libiconv=check])

AC_CHECK_LIB([aio], [main],
             [:],
             AC_MSG_ERROR([Need libaio-dev]))

AC_CHECK_LIB([uuid], [main],
             [:],
	     AC_MSG_ERROR([Need uuid-dev]))

PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([GLIB], [glib-2.0])
PKG_CHECK_MODULES([ZLIB], [zlib])
PKG_CHECK_MODULES([LIBZSTD], [libzstd])

AS_IF([test x$with_libiconv != xno],
      [AC_CHECK_LIB([iconv], [main],
		    [AC_SUBST([LIBICONV], ["-liconv"])],
		    [if test x$with_libiconv == xyes; then
		       AC_MSG_FAILURE([--with-libiconv given, but test failed])
		     fi])],
      [AC_CHECK_LIB([], [iconv_open],
                    [:],
		    AC_MSG_ERROR([Missing iconv in libc]))])

AC_ARG_ENABLE([tests],
	      [AS_HELP_STRING([--enable-tests],
			      [build test programs])],
	      [],
	      [enable_tests=no])

AC_ARG_ENABLE([lttng],
	      [AS_HELP_STRING([--enable-lttng],
			      [enable LTTng-UST tracepoints])],
	      [],
	      [enable_lttng=no])

AS_IF([test x$enable_lttng = xyes],
      [PKG_CHECK_MODULES([LTTNG_UST], [lttng-ust],
			 [AC_DEFINE([HAVE_LTTNG], [1],
				    [Define if LTTng-UST is available])],
			 [AC_MSG_ERROR([--enable-lttng requires lttng-ust development files])])])

AM_CONDITIONAL([ENABLE_TESTS],
	       [test x$enable_tests = xyes])
AM_CONDITIONAL([ENABLE_LTTNG],
	       [test x$enable_lttng = xyes])
AM_CONDITIONAL([ENABLE_STATIC],
	       [test "x$enable_static" = "xyes"])

AC_CHECK_FUNCS([eventfd])



# AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AC_DEFINE(_BLKTAP, 1,
		  Indicates whether this is an internal or external compilation.)
AC_CONFIG_FILES([
Makefile
lvm/Makefile
cpumond/Makefile
cbt/Makefile
vhd/Makefile
vhd/lib/Makefile
vhd/lib/test/Makefile
qcow2/Makefile
qcow2/lib/Makefile
control/Makefile
drivers/Makefile
drivers/crypto/Makefile
include/Makefile
tapback/Makefile
mockatests/Makefile
mockatests/wrappers/Makefile
mockatests/cbt/Makefile
mockatests/drivers/Makefile
mockatests/control/Makefile
mockatests/vhd/Makefile
])
AC_OUTPUT
