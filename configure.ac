#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.63])
AC_INIT([corgi], [0.0.1], [SumiTomohiko@neko-daisuki.ddo.jp])
AM_INIT_AUTOMAKE([dist-bzip2])
AC_CONFIG_SRCDIR([src/corgi.c])
AC_CONFIG_HEADERS([include/corgi/config.h])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AC_PROG_CC
AC_PROG_LIBTOOL

# Checks for libraries.

# Checks for header files.

# Checks for typedefs, structures, and compiler characteristics.
AC_CHECK_SIZEOF(int)
AC_CHECK_SIZEOF(long)

# Checks for library functions.

AC_CONFIG_FILES([Makefile
                 src/Makefile])
AC_OUTPUT

# vim: tabstop=2 shiftwidth=2 expandtab softtabstop=2
