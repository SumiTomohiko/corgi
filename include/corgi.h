#if !defined(CORGI_H_INCLUDED)
#define CORGI_H_INCLUDED

#if defined(HAVE_CONFIG_H)
#   include "corgi/config.h"
#endif

#if SIZEOF_INT == 4
#   define CHAR_TYPE int
#elif SIZEOF_LONG == 4
#   define CHAR_TYPE long
#else
#   error "Cannot determine CorgiChar type"
#endif
typedef unsigned CHAR_TYPE CorgiChar;

#endif
/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
