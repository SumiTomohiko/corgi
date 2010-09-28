#if !defined(CORGI_H_INCLUDED)
#define CORGI_H_INCLUDED

#include "corgi/config.h"

#if SIZEOF_INT == 4
#   define CHAR_TYPE int
#elif SIZEOF_LONG == 4
#   define CHAR_TYPE long
#else
#   error "Cannot determine CorgiChar type"
#endif
typedef unsigned CHAR_TYPE CorgiChar;
#undef CHAR_TYPE

#if SIZEOF_INT == SIZEOF_VOIDP
#   define NUMBER_TYPE int
#elif SIZEOF_LONG == SIZEOF_VOIDP
#   define NUMBER_TYPE long
#elif SIZEOF_LONG_LONG == SIZEOF_VOIDP
#   define NUMBER_TYPE long long
#else
#   error "Cannot determine CorgiStatus type"
#endif
typedef NUMBER_TYPE CorgiInt;
typedef unsigned NUMBER_TYPE CorgiUInt;
#undef NUMBER_TYPE
typedef CorgiInt CorgiStatus;

#define CORGI_OK                0
#define CORGI_ERR_OUT_OF_MEMORY 1

typedef CorgiChar CorgiCode;

struct CorgiRegexp {
    CorgiCode* code;
};

typedef struct CorgiRegexp CorgiRegexp;

struct CorgiMatch {
};

typedef struct CorgiMatch CorgiMatch;

CorgiStatus corgi_compile(CorgiRegexp*, CorgiChar*, CorgiChar*);
CorgiStatus corgi_fini_match(CorgiMatch*);
CorgiStatus corgi_fini_regexp(CorgiRegexp*);
CorgiStatus corgi_init_match(CorgiMatch*);
CorgiStatus corgi_init_regexp(CorgiRegexp*);
CorgiStatus corgi_match(CorgiMatch*, CorgiRegexp*, CorgiChar*, CorgiChar*, CorgiChar*);

#endif
/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
