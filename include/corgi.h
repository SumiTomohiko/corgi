#if !defined(CORGI_H_INCLUDED)
#define CORGI_H_INCLUDED

#include <stdint.h>
#include "corgi/config.h"

typedef uint32_t CorgiChar;
typedef long CorgiInt;
typedef unsigned long CorgiUInt;
typedef CorgiInt CorgiStatus;

#define CORGI_OK        0
#define CORGI_MISMATCH  1

typedef CorgiChar CorgiCode;

typedef struct CorgiGroup CorgiGroup;

struct CorgiRegexp {
    CorgiCode* code;
    CorgiUInt code_size;
    CorgiUInt groups_num;
    struct CorgiGroup** groups;
};

typedef struct CorgiRegexp CorgiRegexp;

typedef struct CorgiRange CorgiRange;

struct CorgiMatch {
    struct CorgiRegexp* regexp;
    CorgiInt begin;
    CorgiInt end;
    struct CorgiRange* groups;
};

typedef struct CorgiMatch CorgiMatch;

typedef CorgiUInt CorgiOptions;
#define CORGI_OPT_DEBUG         (1 << 0)
#define CORGI_OPT_IGNORE_CASE   (1 << 1)

CorgiStatus corgi_compile(CorgiRegexp*, CorgiChar*, CorgiChar*, CorgiOptions);
CorgiStatus corgi_disassemble(CorgiRegexp*);
CorgiStatus corgi_dump(CorgiChar*, CorgiChar*, CorgiOptions);
CorgiStatus corgi_fini_match(CorgiMatch*);
CorgiStatus corgi_fini_regexp(CorgiRegexp*);
CorgiStatus corgi_get_group_range(CorgiMatch*, CorgiUInt, CorgiInt*, CorgiInt*);
CorgiStatus corgi_group_name2id(CorgiRegexp*, CorgiChar*, CorgiChar*, CorgiUInt*);
CorgiStatus corgi_init_match(CorgiMatch*);
CorgiStatus corgi_init_regexp(CorgiRegexp*);
CorgiStatus corgi_match(CorgiMatch*, CorgiRegexp*, CorgiChar*, CorgiChar*, CorgiChar*, CorgiOptions);
CorgiStatus corgi_search(CorgiMatch*, CorgiRegexp*, CorgiChar*, CorgiChar*, CorgiChar*, CorgiOptions);
const char* corgi_strerror(CorgiStatus);

#endif
/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
