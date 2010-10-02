/*
 * Secret Labs' Regular Expression Engine
 *
 * regular expression matching engine
 *
 * partial history:
 * 1999-10-24 fl  created (based on existing template matcher code)
 * 2000-03-06 fl  first alpha, sort of
 * 2000-08-01 fl  fixes for 1.6b1
 * 2000-08-07 fl  use PyOS_CheckStack() if available
 * 2000-09-20 fl  added expand method
 * 2001-03-20 fl  lots of fixes for 2.1b2
 * 2001-04-15 fl  export copyright as Python attribute, not global
 * 2001-04-28 fl  added __copy__ methods (work in progress)
 * 2001-05-14 fl  fixes for 1.5.2 compatibility
 * 2001-07-01 fl  added BIGCHARSET support (from Martin von Loewis)
 * 2001-10-18 fl  fixed group reset issue (from Matthew Mueller)
 * 2001-10-20 fl  added split primitive; reenable unicode for 1.6/2.0/2.1
 * 2001-10-21 fl  added sub/subn primitive
 * 2001-10-24 fl  added finditer primitive (for 2.2 only)
 * 2001-12-07 fl  fixed memory leak in sub/subn (Guido van Rossum)
 * 2002-11-09 fl  fixed empty sub/subn return type
 * 2003-04-18 mvl fully support 4-byte codes
 * 2003-10-17 gn  implemented non recursive scheme
 *
 * Copyright (c) 1997-2001 by Secret Labs AB.  All rights reserved.
 *
 * This version of the SRE library can be redistributed under CNRI's
 * Python 1.6 license.  For any other use, please contact Secret Labs
 * AB (info@pythonware.com).
 *
 * Portions of this engine have been developed in cooperation with
 * CNRI.  Hewlett-Packard provided funding for 1.6 integration and
 * other compatibility work.
 */
#include "corgi/config.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "corgi.h"
#include "corgi/constants.h"
#include "corgi/private.h"

/* error codes */
#define SRE_ERROR_ILLEGAL           -1  /* illegal opcode */
#define SRE_ERROR_STATE             -2  /* illegal state */
#define SRE_ERROR_RECURSION_LIMIT   -3  /* runaway recursion */
#define SRE_ERROR_MEMORY            -9  /* out of memory */
#define SRE_ERROR_INTERRUPTED       -10 /* signal handler raised exception */
#define ERR_OUT_OF_MEMORY   1
#define ERR_INVALID_NODE    2
#define ERR_BAD_RANGE       3
#define ERR_BOGUS_ESCAPE    4

static CorgiChar
char2printable(CorgiChar c)
{
    return isprint(c) ? c : ' ';
}

const char*
corgi_strerror(CorgiStatus status)
{
    switch (status) {
    case ERR_OUT_OF_MEMORY:
        return "Out of memory";
    case ERR_INVALID_NODE:
        return "Invalid node";
    case ERR_BAD_RANGE:
        return "Bad character range";
    case ERR_BOGUS_ESCAPE:
        return "Bogus escape (end of line)";
    default:
        return "Unknown error";
    }
}

#define TRACE(v) do { \
    if (state->debug) { \
        printf v; \
    } \
} while (0)

#define SRE_DIGIT_MASK      1
#define SRE_SPACE_MASK      2
#define SRE_LINEBREAK_MASK  4
#define SRE_ALNUM_MASK      8
#define SRE_WORD_MASK       16
static char sre_char_info[128] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 6, 2,
2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 25, 25, 25, 25, 25, 25, 25, 25,
25, 25, 0, 0, 0, 0, 0, 0, 0, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 0, 0,
0, 0, 16, 0, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 0, 0, 0, 0, 0 };

#define SRE_IS_DIGIT(ch)\
    ((ch) < 128 ? (sre_char_info[(ch)] & SRE_DIGIT_MASK) : 0)
#define SRE_IS_SPACE(ch)\
    ((ch) < 128 ? (sre_char_info[(ch)] & SRE_SPACE_MASK) : 0)
#define SRE_IS_LINEBREAK(ch)\
    ((ch) < 128 ? (sre_char_info[(ch)] & SRE_LINEBREAK_MASK) : 0)
#define SRE_IS_ALNUM(ch)\
    ((ch) < 128 ? (sre_char_info[(ch)] & SRE_ALNUM_MASK) : 0)
#define SRE_IS_WORD(ch)\
    ((ch) < 128 ? (sre_char_info[(ch)] & SRE_WORD_MASK) : 0)

#define SRE_UNI_IS_DIGIT(c)     corgi_is_digit((c))
#define SRE_UNI_IS_SPACE(c)     corgi_is_space((c))
#define SRE_UNI_IS_LINEBREAK(c) corgi_is_linebreak((c))
#define SRE_UNI_IS_ALNUM(c)     (corgi_is_alpha((c)) || corgi_is_decimal((c)) || corgi_is_digit((c)) || corgi_is_numeric((c)))
#define SRE_UNI_IS_WORD(c)      (SRE_UNI_IS_ALNUM((c)) || ((c) == '_'))

/* locale-specific character predicates */
/* !(c & ~N) == (c < N+1) for any unsigned c, this avoids
 * warnings when c's type supports only numbers < N+1 */
#define SRE_LOC_IS_DIGIT(ch)        (!((ch) & ~255) ? isdigit((ch)) : 0)
#define SRE_LOC_IS_SPACE(ch)        (!((ch) & ~255) ? isspace((ch)) : 0)
#define SRE_LOC_IS_LINEBREAK(ch)    ((ch) == '\n')
#define SRE_LOC_IS_ALNUM(ch)        (!((ch) & ~255) ? isalnum((ch)) : 0)
#define SRE_LOC_IS_WORD(ch)         (SRE_LOC_IS_ALNUM((ch)) || ((ch) == '_'))

static int
sre_category(CorgiCode category, CorgiChar ch)
{
    switch (category) {
    case SRE_CATEGORY_DIGIT:
        return SRE_IS_DIGIT(ch);
    case SRE_CATEGORY_NOT_DIGIT:
        return !SRE_IS_DIGIT(ch);
    case SRE_CATEGORY_SPACE:
        return SRE_IS_SPACE(ch);
    case SRE_CATEGORY_NOT_SPACE:
        return !SRE_IS_SPACE(ch);
    case SRE_CATEGORY_WORD:
        return SRE_IS_WORD(ch);
    case SRE_CATEGORY_NOT_WORD:
        return !SRE_IS_WORD(ch);
    case SRE_CATEGORY_LINEBREAK:
        return SRE_IS_LINEBREAK(ch);
    case SRE_CATEGORY_NOT_LINEBREAK:
        return !SRE_IS_LINEBREAK(ch);
    case SRE_CATEGORY_LOC_WORD:
        return SRE_LOC_IS_WORD(ch);
    case SRE_CATEGORY_LOC_NOT_WORD:
        return !SRE_LOC_IS_WORD(ch);
    case SRE_CATEGORY_UNI_DIGIT:
        return SRE_UNI_IS_DIGIT(ch);
    case SRE_CATEGORY_UNI_NOT_DIGIT:
        return !SRE_UNI_IS_DIGIT(ch);
    case SRE_CATEGORY_UNI_SPACE:
        return SRE_UNI_IS_SPACE(ch);
    case SRE_CATEGORY_UNI_NOT_SPACE:
        return !SRE_UNI_IS_SPACE(ch);
    case SRE_CATEGORY_UNI_WORD:
        return SRE_UNI_IS_WORD(ch);
    case SRE_CATEGORY_UNI_NOT_WORD:
        return !SRE_UNI_IS_WORD(ch);
    case SRE_CATEGORY_UNI_LINEBREAK:
        return SRE_UNI_IS_LINEBREAK(ch);
    case SRE_CATEGORY_UNI_NOT_LINEBREAK:
        return !SRE_UNI_IS_LINEBREAK(ch);
    }
    return 0;
}

/* FIXME: <fl> shouldn't be a constant, really... */
#define SRE_MARK_SIZE 200

struct Repeat {
    CorgiInt count;
    CorgiCode* pattern; /* points to REPEAT operator arguments */
    void* last_ptr; /* helper to check for infinite loops */
    struct Repeat* prev; /* points to previous repeat context */
};

typedef struct Repeat Repeat;

struct State {
    /* string pointers */
    CorgiChar* ptr; /* current position (also end of current slice) */
    CorgiChar* beginning; /* start of original string */
    CorgiChar* start; /* start of current slice */
    CorgiChar* end; /* end of original string */
    /* registers */
    CorgiInt lastindex;
    CorgiInt lastmark;
    void* mark[SRE_MARK_SIZE];
    /* dynamically allocated stuff */
    char* data_stack;
    size_t data_stack_size;
    size_t data_stack_base;
    /* current repeat context */
    Repeat *repeat;
    Bool debug;
};

typedef struct State State;

static void
data_stack_dealloc(State* state)
{
    if (state->data_stack) {
        free(state->data_stack);
        state->data_stack = NULL;
    }
    state->data_stack_size = state->data_stack_base = 0;
}

static int
data_stack_grow(State* state, CorgiInt size)
{
    CorgiInt needed_size = state->data_stack_base + size;
    if (needed_size <= state->data_stack_size) {
        return 0;
    }
    CorgiInt new_size = needed_size + needed_size / 4 + 1024;
    TRACE(("allocate/grow stack %d\n", new_size));
    void* stack = realloc(state->data_stack, new_size);
    if (stack == NULL) {
        data_stack_dealloc(state);
        return SRE_ERROR_MEMORY;
    }
    state->data_stack = (char*)stack;
    state->data_stack_size = new_size;
    return 0;
}

static int
sre_at(State* state, CorgiChar* ptr, CorgiCode at)
{
    /* check if pointer is at given position */
    CorgiInt thisp;
    CorgiInt thatp;
    switch (at) {
    case SRE_AT_BEGINNING:
    case SRE_AT_BEGINNING_STRING:
        return ptr == state->beginning;
    case SRE_AT_BEGINNING_LINE:
        return ((ptr == state->beginning) || SRE_IS_LINEBREAK(ptr[-1]));
    case SRE_AT_END:
        return (((ptr + 1 == state->end) && SRE_IS_LINEBREAK(ptr[0])) || (ptr == state->end));
    case SRE_AT_END_LINE:
        return ((ptr == state->end) || SRE_IS_LINEBREAK(ptr[0]));
    case SRE_AT_END_STRING:
        return ptr == state->end;
    case SRE_AT_BOUNDARY:
        if (state->beginning == state->end) {
            return 0;
        }
        thatp = state->beginning < ptr ? SRE_IS_WORD(ptr[-1]) : 0;
        thisp = ptr < state->end ? SRE_IS_WORD(ptr[0]) : 0;
        return thisp != thatp;
    case SRE_AT_NON_BOUNDARY:
        if (state->beginning == state->end) {
            return 0;
        }
        thatp = state->beginning < ptr ? SRE_IS_WORD(ptr[-1]) : 0;
        thisp = ptr < state->end ? SRE_IS_WORD(ptr[0]) : 0;
        return thisp == thatp;
    case SRE_AT_LOC_BOUNDARY:
        if (state->beginning == state->end) {
            return 0;
        }
        thatp = state->beginning < ptr ? SRE_LOC_IS_WORD(ptr[-1]) : 0;
        thisp = ptr < state->end ? SRE_LOC_IS_WORD(ptr[0]) : 0;
        return thisp != thatp;
    case SRE_AT_LOC_NON_BOUNDARY:
        if (state->beginning == state->end) {
            return 0;
        }
        thatp = state->beginning < ptr ? SRE_LOC_IS_WORD(ptr[-1]) : 0;
        thisp = ptr < state->end ? SRE_LOC_IS_WORD(ptr[0]) : 0;
        return thisp == thatp;
    case SRE_AT_UNI_BOUNDARY:
        if (state->beginning == state->end) {
            return 0;
        }
        thatp = state->beginning < ptr ? SRE_UNI_IS_WORD(ptr[-1]) : 0;
        thisp = ptr < state->end ? SRE_UNI_IS_WORD(ptr[0]) : 0;
        return thisp != thatp;
    case SRE_AT_UNI_NON_BOUNDARY:
        if (state->beginning == state->end) {
            return 0;
        }
        thatp = state->beginning < ptr ? SRE_UNI_IS_WORD(ptr[-1]) : 0;
        thisp = ptr < state->end ? SRE_UNI_IS_WORD(ptr[0]) : 0;
        return thisp == thatp;
    default:
        abort();
        break;
    }

    return 0;
}

static int
sre_charset(CorgiCode* set, CorgiCode ch)
{
    /* check if character is a member of the given set */
    int ok = 1;
    for (;;) {
        switch (*set++) {
        case SRE_OP_FAILURE:
            return !ok;
        case SRE_OP_LITERAL:
            /* <LITERAL> <code> */
            if (ch == set[0]) {
                return ok;
            }
            set++;
            break;
        case SRE_OP_CATEGORY:
            /* <CATEGORY> <code> */
            if (sre_category(set[0], ch)) {
                return ok;
            }
            set += 1;
            break;
        case SRE_OP_CHARSET:
            /* <CHARSET> <bitmap> (32 bits per code word) */
            if ((ch < 256) && (set[ch >> 5] & (1 << (ch & 31)))) {
                return ok;
            }
            set += 8;
            break;
        case SRE_OP_RANGE:
            /* <RANGE> <lower> <upper> */
            if ((set[0] <= ch) && (ch <= set[1])) {
                return ok;
            }
            set += 2;
            break;
        case SRE_OP_NEGATE:
            ok = !ok;
            break;
        case SRE_OP_BIGCHARSET:
            /* <BIGCHARSET> <blockcount> <256 blockindices> <blocks> */
            {
                CorgiInt count = *(set++);
                /* !(c & ~N) == (c < N+1) for any unsigned c, this avoids
                 * warnings when c's type supports only numbers < N+1 */
                CorgiInt block = !(ch & ~65535) ? ((unsigned char*)set)[ch >> 8] : -1;
                set += 64;
                if ((0 <= block) && (set[block * 8 + ((ch & 255) >> 5)] & (1 << (ch & 31)))) {
                    return ok;
                }
                set += count * 8;
            }
            break;
        default:
            /* internal error -- there's not much we can do about it
               here, so let's just pretend it didn't match... */
            assert(FALSE);
            abort();
        }
    }
}

static CorgiInt sre_match(State*, CorgiCode*);

static CorgiInt
sre_count(State* state, CorgiCode* pattern, CorgiInt maxcount)
{
    CorgiChar* ptr = state->ptr;
    CorgiChar* end = state->end;

    /* adjust end */
    if ((maxcount < end - ptr) && (maxcount != 65535)) {
        end = ptr + maxcount;
    }

    CorgiCode chr;
    CorgiInt i;
    switch (pattern[0]) {
    case SRE_OP_IN:
        /* repeated set */
        TRACE(("|%p|%p|COUNT IN\n", pattern, ptr));
        while ((ptr < end) && sre_charset(pattern + 2, *ptr)) {
            ptr++;
        }
        break;
    case SRE_OP_ANY:
        /* repeated dot wildcard. */
        TRACE(("|%p|%p|COUNT ANY\n", pattern, ptr));
        while ((ptr < end) && !SRE_IS_LINEBREAK(*ptr)) {
            ptr++;
        }
        break;
    case SRE_OP_ANY_ALL:
        /* repeated dot wildcard.  skip to the end of the target
           string, and backtrack from there */
        TRACE(("|%p|%p|COUNT ANY_ALL\n", pattern, ptr));
        ptr = end;
        break;
    case SRE_OP_LITERAL:
        /* repeated literal */
        chr = pattern[1];
        TRACE(("|%p|%p|COUNT LITERAL %d (%c)\n", pattern, ptr, chr, isprint(chr) ? chr : ' '));
        while ((ptr < end) && (*ptr == chr)) {
            ptr++;
        }
        break;
    case SRE_OP_LITERAL_IGNORE:
        /* repeated literal */
        chr = pattern[1];
        TRACE(("|%p|%p|COUNT LITERAL_IGNORE %d\n", pattern, ptr, chr));
        while ((ptr < end) && (corgi_tolower(*ptr) == chr)) {
            ptr++;
        }
        break;
    case SRE_OP_NOT_LITERAL:
        /* repeated non-literal */
        chr = pattern[1];
        TRACE(("|%p|%p|COUNT NOT_LITERAL %d\n", pattern, ptr, chr));
        while ((ptr < end) && (*ptr != chr)) {
            ptr++;
        }
        break;
    case SRE_OP_NOT_LITERAL_IGNORE:
        /* repeated non-literal */
        chr = pattern[1];
        TRACE(("|%p|%p|COUNT NOT_LITERAL_IGNORE %d\n", pattern, ptr, chr));
        while ((ptr < end) && (corgi_tolower(*ptr) != chr)) {
            ptr++;
        }
        break;
    default:
        /* repeated single character pattern */
        TRACE(("|%p|%p|COUNT SUBPATTERN\n", pattern, ptr));
        while (state->ptr < end) {
            i = sre_match(state, pattern);
            if (i < 0) {
                return i;
            }
            if (!i) {
                break;
            }
        }
        TRACE(("|%p|%p|COUNT %d\n", pattern, ptr, state->ptr - ptr));
        return state->ptr - ptr;
    }

    TRACE(("|%p|%p|COUNT %d\n", pattern, ptr, ptr - state->ptr));
    return ptr - state->ptr;
}

/* The macros below should be used to protect recursive sre_match()
 * calls that *failed* and do *not* return immediately (IOW, those
 * that will backtrack). Explaining:
 *
 * - Recursive sre_match() returned true: that's usually a success
 *   (besides atypical cases like ASSERT_NOT), therefore there's no
 *   reason to restore lastmark;
 *
 * - Recursive sre_match() returned false but the current sre_match()
 *   is returning to the caller: If the current sre_match() is the
 *   top function of the recursion, returning false will be a matching
 *   failure, and it doesn't matter where lastmark is pointing to.
 *   If it's *not* the top function, it will be a recursive sre_match()
 *   failure by itself, and the calling sre_match() will have to deal
 *   with the failure by the same rules explained here (it will restore
 *   lastmark by itself if necessary);
 *
 * - Recursive sre_match() returned false, and will continue the
 *   outside 'for' loop: must be protected when breaking, since the next
 *   OP could potentially depend on lastmark;
 *
 * - Recursive sre_match() returned false, and will be called again
 *   inside a local for/while loop: must be protected between each
 *   loop iteration, since the recursive sre_match() could do anything,
 *   and could potentially depend on lastmark.
 *
 * For more information, check the discussion at SF patch #712900.
 */
#define LASTMARK_SAVE() do { \
    ctx->lastmark = state->lastmark; \
    ctx->lastindex = state->lastindex; \
} while (0)
#define LASTMARK_RESTORE() do { \
    state->lastmark = ctx->lastmark; \
    state->lastindex = ctx->lastindex; \
} while (0)

#define RETURN_ERROR(i) return (i)
#define __RETURN__(status) do { \
    ret = (status); \
    goto exit; \
} while (0)
#define RETURN_FAILURE __RETURN__(0)
#define RETURN_SUCCESS __RETURN__(1)

#define RETURN_ON_ERROR(i) do { \
    if (i < 0) { \
        RETURN_ERROR(i); \
    } \
} while (0)
#define RETURN_ON_SUCCESS(i) do { \
    RETURN_ON_ERROR(i); \
    if (0 < i) { \
        RETURN_SUCCESS; \
    } \
} while (0)
#define RETURN_ON_FAILURE(i) do { \
    RETURN_ON_ERROR(i); \
    if (i == 0) { \
        RETURN_FAILURE; \
    } \
} while (0)

#define DATA_STACK_ALLOC(state, type, ptr) do { \
    alloc_pos = state->data_stack_base; \
    TRACE(("allocating %s in %d (%d)\n", #type, alloc_pos, sizeof(type))); \
    if (state->data_stack_size < alloc_pos + sizeof(type)) { \
        int j = data_stack_grow(state, sizeof(type)); \
        if (j < 0) { \
            return j; \
        } \
        if (ctx_pos != -1) { \
            DATA_STACK_LOOKUP_AT(state, sre_match_context, ctx, ctx_pos); \
        } \
    } \
    ptr = (type*)(state->data_stack + alloc_pos); \
    state->data_stack_base += sizeof(type); \
} while (0)

#define DATA_STACK_LOOKUP_AT(state, type, ptr, pos) do { \
    TRACE(("looking up %s at %d\n", #type, pos)); \
    ptr = (type*)(state->data_stack + pos); \
} while (0)

#define DATA_STACK_PUSH(state, data, size) do { \
    TRACE(("copy data in %p to %d (%d)\n", data, state->data_stack_base, size)); \
    if (state->data_stack_size < state->data_stack_base + size) { \
        int j = data_stack_grow(state, size); \
        if (j < 0) { \
            return j; \
        } \
        if (ctx_pos != -1) { \
            DATA_STACK_LOOKUP_AT(state, sre_match_context, ctx, ctx_pos); \
        } \
    } \
    memcpy(state->data_stack + state->data_stack_base, data, size); \
    state->data_stack_base += size; \
} while (0)

#define DATA_STACK_POP(state, data, size, discard) do { \
    TRACE(("copy data to %p from %d (%d)\n", data, state->data_stack_base-size, size)); \
    memcpy(data, state->data_stack + state->data_stack_base - size, size); \
    if (discard) { \
        state->data_stack_base -= size; \
    } \
} while (0)

#define DATA_STACK_POP_DISCARD(state, size) do { \
    TRACE(("discard data from %d (%d)\n", state->data_stack_base-size, size)); \
    state->data_stack_base -= size; \
} while(0)

#define DATA_PUSH(x)                DATA_STACK_PUSH(state, (x), sizeof(*(x)))
#define DATA_POP(x)                 DATA_STACK_POP(state, (x), sizeof(*(x)), 1)
#define DATA_POP_DISCARD(x)         DATA_STACK_POP_DISCARD(state, sizeof(*(x)))
#define DATA_ALLOC(t, p)            DATA_STACK_ALLOC(state, t, p)
#define DATA_LOOKUP_AT(t, p, pos)   DATA_STACK_LOOKUP_AT(state, t, p, pos)

#define MARK_PUSH(lastmark) do { \
    if (0 < lastmark) { \
        i = lastmark; /* ctx->lastmark may change if reallocated */ \
        DATA_STACK_PUSH(state, state->mark, (i + 1) * sizeof(void*)); \
    } \
} while (0)
#define MARK_POP(lastmark) do { \
    if (0 < lastmark) { \
        DATA_STACK_POP(state, state->mark, (lastmark + 1) * sizeof(void*), 1); \
    } \
} while (0)
#define MARK_POP_KEEP(lastmark) do { \
    if (0 < lastmark) { \
        DATA_STACK_POP(state, state->mark, (lastmark + 1) * sizeof(void*), 0); \
    } \
} while (0)
#define MARK_POP_DISCARD(lastmark) do { \
    if (0 < lastmark) { \
        DATA_STACK_POP_DISCARD(state, (lastmark + 1) * sizeof(void*)); \
    } \
} while (0)

#define JUMP_NONE           0
#define JUMP_MAX_UNTIL_1    1
#define JUMP_MAX_UNTIL_2    2
#define JUMP_MAX_UNTIL_3    3
#define JUMP_MIN_UNTIL_1    4
#define JUMP_MIN_UNTIL_2    5
#define JUMP_MIN_UNTIL_3    6
#define JUMP_REPEAT         7
#define JUMP_REPEAT_ONE_1   8
#define JUMP_REPEAT_ONE_2   9
#define JUMP_MIN_REPEAT_ONE 10
#define JUMP_BRANCH         11
#define JUMP_ASSERT         12
#define JUMP_ASSERT_NOT     13

#define DO_JUMP(jumpvalue, jumplabel, nextpattern) \
    DATA_ALLOC(sre_match_context, nextctx); \
    nextctx->last_ctx_pos = ctx_pos; \
    nextctx->jump = jumpvalue; \
    nextctx->pattern = nextpattern; \
    ctx_pos = alloc_pos; \
    ctx = nextctx; \
    goto entrance; \
    jumplabel: \
    while (0) /* gcc doesn't like labels at end of scopes */ \

typedef struct {
    CorgiInt last_ctx_pos;
    CorgiInt jump;
    CorgiChar* ptr;
    CorgiCode* pattern;
    CorgiInt count;
    CorgiInt lastmark;
    CorgiInt lastindex;
    union {
        CorgiCode chr;
        Repeat* rep;
    } u;
} sre_match_context;

/* check if string matches the given pattern.  returns <0 for
   error, 0 for failure, and 1 for success */
static CorgiInt
sre_match(State* state, CorgiCode* pattern)
{
    TRACE(("|%p|%p|ENTER\n", pattern, state->ptr));

    sre_match_context* ctx;
    CorgiInt ctx_pos = -1;
    CorgiInt alloc_pos;
    DATA_ALLOC(sre_match_context, ctx);
    ctx->last_ctx_pos = -1;
    ctx->jump = JUMP_NONE;
    ctx->pattern = pattern;
    ctx_pos = alloc_pos;

    CorgiInt ret = 0;
entrance:
    ctx->ptr = state->ptr;
    CorgiChar* end = state->end;
    if (ctx->pattern[0] == SRE_OP_INFO) {
        /* optimization info block */
        /* <INFO> <1=skip> <2=flags> <3=min> ... */
        if (ctx->pattern[3] && (end - ctx->ptr < ctx->pattern[3])) {
            TRACE(("reject (got %d chars, need %d)\n", end - ctx->ptr, ctx->pattern[3]));
            RETURN_FAILURE;
        }
        ctx->pattern += ctx->pattern[1] + 1;
    }

    sre_match_context* nextctx;
    CorgiInt i;
    for (;;) {
        switch (*ctx->pattern++) {
        case SRE_OP_MARK:
            /* set mark */
            /* <MARK> <gid> */
            TRACE(("|%p|%p|MARK %d\n", ctx->pattern, ctx->ptr, ctx->pattern[0]));
            i = ctx->pattern[0];
            if (i & 1) {
                state->lastindex = i / 2 + 1;
            }
            if (state->lastmark < i) {
                /* state->lastmark is the highest valid index in the
                   state->mark array.  If it is increased by more than 1,
                   the intervening marks must be set to NULL to signal
                   that these marks have not been encountered. */
                CorgiInt j = state->lastmark + 1;
                while (j < i) {
                    state->mark[j++] = NULL;
                }
                state->lastmark = i;
            }
            state->mark[i] = ctx->ptr;
            ctx->pattern++;
            break;
        case SRE_OP_LITERAL:
            /* match literal string */
            /* <LITERAL> <code> */
            TRACE(("|%p|%p|LITERAL %d (%c)\n", ctx->pattern, ctx->ptr, *ctx->pattern, isprint(*ctx->pattern) ? *ctx->pattern : ' '));
            if ((end <= ctx->ptr) || (ctx->ptr[0] != ctx->pattern[0])) {
                RETURN_FAILURE;
            }
            ctx->pattern++;
            ctx->ptr++;
            break;
        case SRE_OP_NOT_LITERAL:
            /* match anything that is not literal character */
            /* <NOT_LITERAL> <code> */
            TRACE(("|%p|%p|NOT_LITERAL %d\n", ctx->pattern, ctx->ptr, *ctx->pattern));
            if ((end <= ctx->ptr) || (ctx->ptr[0] == ctx->pattern[0])) {
                RETURN_FAILURE;
            }
            ctx->pattern++;
            ctx->ptr++;
            break;
        case SRE_OP_SUCCESS:
            /* end of pattern */
            TRACE(("|%p|%p|SUCCESS\n", ctx->pattern, ctx->ptr));
            state->ptr = ctx->ptr;
            RETURN_SUCCESS;
        case SRE_OP_AT:
            /* match at given position */
            /* <AT> <code> */
            TRACE(("|%p|%p|AT %d\n", ctx->pattern, ctx->ptr, *ctx->pattern));
            if (!sre_at(state, ctx->ptr, *ctx->pattern)) {
                RETURN_FAILURE;
            }
            ctx->pattern++;
            break;
        case SRE_OP_CATEGORY:
            /* match at given category */
            /* <CATEGORY> <code> */
            TRACE(("|%p|%p|CATEGORY %d\n", ctx->pattern, ctx->ptr, *ctx->pattern));
            if ((end <= ctx->ptr) || !sre_category(ctx->pattern[0], ctx->ptr[0])) {
                RETURN_FAILURE;
            }
            ctx->pattern++;
            ctx->ptr++;
            break;
        case SRE_OP_ANY:
            /* match anything (except a newline) */
            /* <ANY> */
            TRACE(("|%p|%p|ANY\n", ctx->pattern, ctx->ptr));
            if ((end <= ctx->ptr) || SRE_IS_LINEBREAK(ctx->ptr[0])) {
                RETURN_FAILURE;
            }
            ctx->ptr++;
            break;
        case SRE_OP_ANY_ALL:
            /* match anything */
            /* <ANY_ALL> */
            TRACE(("|%p|%p|ANY_ALL\n", ctx->pattern, ctx->ptr));
            if (end <= ctx->ptr) {
                RETURN_FAILURE;
            }
            ctx->ptr++;
            break;
        case SRE_OP_IN:
            /* match set member (or non_member) */
            /* <IN> <skip> <set> */
            TRACE(("|%p|%p|IN\n", ctx->pattern, ctx->ptr));
            if ((end <= ctx->ptr) || !sre_charset(ctx->pattern + 1, *ctx->ptr)) {
                RETURN_FAILURE;
            }
            ctx->pattern += ctx->pattern[0];
            ctx->ptr++;
            break;
        case SRE_OP_LITERAL_IGNORE:
            TRACE(("|%p|%p|LITERAL_IGNORE %d\n", ctx->pattern, ctx->ptr, ctx->pattern[0]));
            if ((end <= ctx->ptr) || (corgi_tolower(*ctx->ptr) != corgi_tolower(*ctx->pattern))) {
                RETURN_FAILURE;
            }
            ctx->pattern++;
            ctx->ptr++;
            break;
        case SRE_OP_NOT_LITERAL_IGNORE:
            TRACE(("|%p|%p|NOT_LITERAL_IGNORE %d\n", ctx->pattern, ctx->ptr, *ctx->pattern));
            if ((end <= ctx->ptr) || (corgi_tolower(*ctx->ptr) == corgi_tolower(*ctx->pattern))) {
                RETURN_FAILURE;
            }
            ctx->pattern++;
            ctx->ptr++;
            break;
        case SRE_OP_IN_IGNORE:
            TRACE(("|%p|%p|IN_IGNORE\n", ctx->pattern, ctx->ptr));
            if ((end <= ctx->ptr) || !sre_charset(ctx->pattern + 1, corgi_tolower(*ctx->ptr))) {
                RETURN_FAILURE;
            }
            ctx->pattern += ctx->pattern[0];
            ctx->ptr++;
            break;
        case SRE_OP_JUMP:
        case SRE_OP_INFO:
            /* jump forward */
            /* <JUMP> <offset> */
            TRACE(("|%p|%p|JUMP %d\n", ctx->pattern, ctx->ptr, ctx->pattern[0]));
            ctx->pattern += ctx->pattern[0];
            break;
        case SRE_OP_BRANCH:
            /* alternation */
            /* <BRANCH> <0=skip> code <JUMP> ... <NULL> */
            TRACE(("|%p|%p|BRANCH\n", ctx->pattern, ctx->ptr));
            LASTMARK_SAVE();
            ctx->u.rep = state->repeat;
            if (ctx->u.rep) {
                MARK_PUSH(ctx->lastmark);
            }
            for (; ctx->pattern[0]; ctx->pattern += ctx->pattern[0]) {
                if ((ctx->pattern[1] == SRE_OP_LITERAL) && ((end <= ctx->ptr) || (*ctx->ptr != ctx->pattern[2]))) {
                    continue;
                }
                if ((ctx->pattern[1] == SRE_OP_IN) && ((end <= ctx->ptr) || !sre_charset(ctx->pattern + 3, *ctx->ptr))) {
                    continue;
                }
                state->ptr = ctx->ptr;
                DO_JUMP(JUMP_BRANCH, jump_branch, ctx->pattern + 1);
                if (ret) {
                    if (ctx->u.rep) {
                        MARK_POP_DISCARD(ctx->lastmark);
                    }
                    RETURN_ON_ERROR(ret);
                    RETURN_SUCCESS;
                }
                if (ctx->u.rep) {
                    MARK_POP_KEEP(ctx->lastmark);
                }
                LASTMARK_RESTORE();
            }
            if (ctx->u.rep) {
                MARK_POP_DISCARD(ctx->lastmark);
            }
            RETURN_FAILURE;
        case SRE_OP_REPEAT_ONE:
            /* match repeated sequence (maximizing regexp) */
            /* this operator only works if the repeated item is
               exactly one character wide, and we're not already
               collecting backtracking points.  for other cases,
               use the MAX_REPEAT operator */
            /* <REPEAT_ONE> <skip> <1=min> <2=max> item <SUCCESS> tail */
            TRACE(("|%p|%p|REPEAT_ONE %d %d\n", ctx->pattern, ctx->ptr, ctx->pattern[1], ctx->pattern[2]));
            if (end < ctx->ptr + ctx->pattern[1]) {
                RETURN_FAILURE; /* cannot match */
            }

            state->ptr = ctx->ptr;

            ret = sre_count(state, ctx->pattern + 3, ctx->pattern[2]);
            RETURN_ON_ERROR(ret);
            DATA_LOOKUP_AT(sre_match_context, ctx, ctx_pos);
            ctx->count = ret;
            ctx->ptr += ctx->count;

            /* when we arrive here, count contains the number of
               matches, and ctx->ptr points to the tail of the target
               string.  check if the rest of the pattern matches,
               and backtrack if not. */

            if (ctx->count < (CorgiInt)ctx->pattern[1]) {
                RETURN_FAILURE;
            }

            if (ctx->pattern[ctx->pattern[0]] == SRE_OP_SUCCESS) {
                /* tail is empty.  we're finished */
                state->ptr = ctx->ptr;
                RETURN_SUCCESS;
            }

            LASTMARK_SAVE();

            if (ctx->pattern[ctx->pattern[0]] == SRE_OP_LITERAL) {
                /* tail starts with a literal. skip positions where
                   the rest of the pattern cannot possibly match */
                ctx->u.chr = ctx->pattern[ctx->pattern[0] + 1];
                for (;;) {
                    while (((CorgiInt)ctx->pattern[1] <= ctx->count) && ((end <= ctx->ptr) || (*ctx->ptr != ctx->u.chr))) {
                        ctx->ptr--;
                        ctx->count--;
                    }
                    if (ctx->count < (CorgiInt)ctx->pattern[1]) {
                        break;
                    }
                    state->ptr = ctx->ptr;
                    DO_JUMP(JUMP_REPEAT_ONE_1, jump_repeat_one_1, ctx->pattern + ctx->pattern[0]);
                    if (ret) {
                        RETURN_ON_ERROR(ret);
                        RETURN_SUCCESS;
                    }

                    LASTMARK_RESTORE();

                    ctx->ptr--;
                    ctx->count--;
                }
            } else {
                /* general case */
                while ((CorgiInt)ctx->pattern[1] <= ctx->count) {
                    state->ptr = ctx->ptr;
                    DO_JUMP(JUMP_REPEAT_ONE_2, jump_repeat_one_2, ctx->pattern + ctx->pattern[0]);
                    if (ret) {
                        RETURN_ON_ERROR(ret);
                        RETURN_SUCCESS;
                    }
                    ctx->ptr--;
                    ctx->count--;
                    LASTMARK_RESTORE();
                }
            }
            RETURN_FAILURE;
        case SRE_OP_MIN_REPEAT_ONE:
            /* match repeated sequence (minimizing regexp) */
            /* this operator only works if the repeated item is
               exactly one character wide, and we're not already
               collecting backtracking points.  for other cases,
               use the MIN_REPEAT operator */
            /* <MIN_REPEAT_ONE> <skip> <1=min> <2=max> item <SUCCESS> tail */
            TRACE(("|%p|%p|MIN_REPEAT_ONE %d %d\n", ctx->pattern, ctx->ptr, ctx->pattern[1], ctx->pattern[2]));
            if (end < ctx->ptr + ctx->pattern[1]) {
                RETURN_FAILURE; /* cannot match */
            }

            state->ptr = ctx->ptr;

            if (ctx->pattern[1] == 0) {
                ctx->count = 0;
            }
            else {
                /* count using pattern min as the maximum */
                ret = sre_count(state, ctx->pattern + 3, ctx->pattern[1]);
                RETURN_ON_ERROR(ret);
                DATA_LOOKUP_AT(sre_match_context, ctx, ctx_pos);
                if (ret < (CorgiInt)ctx->pattern[1]) {
                    /* didn't match minimum number of times */
                    RETURN_FAILURE;
                }
                /* advance past minimum matches of repeat */
                ctx->count = ret;
                ctx->ptr += ctx->count;
            }

            if (ctx->pattern[ctx->pattern[0]] == SRE_OP_SUCCESS) {
                /* tail is empty.  we're finished */
                state->ptr = ctx->ptr;
                RETURN_SUCCESS;
            } else {
                /* general case */
                LASTMARK_SAVE();
                while (((CorgiInt)ctx->pattern[2] == 65535) || (ctx->count <= (CorgiInt)ctx->pattern[2])) {
                    state->ptr = ctx->ptr;
                    DO_JUMP(JUMP_MIN_REPEAT_ONE, jump_min_repeat_one, ctx->pattern + ctx->pattern[0]);
                    if (ret) {
                        RETURN_ON_ERROR(ret);
                        RETURN_SUCCESS;
                    }
                    state->ptr = ctx->ptr;
                    ret = sre_count(state, ctx->pattern + 3, 1);
                    RETURN_ON_ERROR(ret);
                    DATA_LOOKUP_AT(sre_match_context, ctx, ctx_pos);
                    if (ret == 0) {
                        break;
                    }
                    assert(ret == 1);
                    ctx->ptr++;
                    ctx->count++;
                    LASTMARK_RESTORE();
                }
            }
            RETURN_FAILURE;
        case SRE_OP_REPEAT:
            /* create repeat context.  all the hard work is done
               by the UNTIL operator (MAX_UNTIL, MIN_UNTIL) */
            /* <REPEAT> <skip> <1=min> <2=max> item <UNTIL> tail */
            TRACE(("|%p|%p|REPEAT %d %d\n", ctx->pattern, ctx->ptr, ctx->pattern[1], ctx->pattern[2]));

            /* install new repeat context */
            ctx->u.rep = (Repeat*)malloc(sizeof(*ctx->u.rep));
            if (ctx->u.rep == NULL) {
                RETURN_FAILURE;
            }
            ctx->u.rep->count = -1;
            ctx->u.rep->pattern = ctx->pattern;
            ctx->u.rep->prev = state->repeat;
            ctx->u.rep->last_ptr = NULL;
            state->repeat = ctx->u.rep;

            state->ptr = ctx->ptr;
            DO_JUMP(JUMP_REPEAT, jump_repeat, ctx->pattern + ctx->pattern[0]);
            state->repeat = ctx->u.rep->prev;
            free(ctx->u.rep);

            if (ret) {
                RETURN_ON_ERROR(ret);
                RETURN_SUCCESS;
            }
            RETURN_FAILURE;
        case SRE_OP_MAX_UNTIL:
            /* maximizing repeat */
            /* <REPEAT> <skip> <1=min> <2=max> item <MAX_UNTIL> tail */
            /* FIXME: we probably need to deal with zero-width
               matches in here... */
            ctx->u.rep = state->repeat;
            if (!ctx->u.rep) {
                RETURN_ERROR(SRE_ERROR_STATE);
            }

            state->ptr = ctx->ptr;

            ctx->count = ctx->u.rep->count + 1;

            TRACE(("|%p|%p|MAX_UNTIL %d\n", ctx->pattern, ctx->ptr, ctx->count));

            if (ctx->count < ctx->u.rep->pattern[1]) {
                /* not enough matches */
                ctx->u.rep->count = ctx->count;
                DO_JUMP(JUMP_MAX_UNTIL_1, jump_max_until_1, ctx->u.rep->pattern + 3);
                if (ret) {
                    RETURN_ON_ERROR(ret);
                    RETURN_SUCCESS;
                }
                ctx->u.rep->count = ctx->count - 1;
                state->ptr = ctx->ptr;
                RETURN_FAILURE;
            }

            if (((ctx->count < ctx->u.rep->pattern[2]) || (ctx->u.rep->pattern[2] == 65535)) && (state->ptr != ctx->u.rep->last_ptr)) {
                /* we may have enough matches, but if we can
                   match another item, do so */
                ctx->u.rep->count = ctx->count;
                LASTMARK_SAVE();
                MARK_PUSH(ctx->lastmark);
                /* zero-width match protection */
                DATA_PUSH(&ctx->u.rep->last_ptr);
                ctx->u.rep->last_ptr = state->ptr;
                DO_JUMP(JUMP_MAX_UNTIL_2, jump_max_until_2, ctx->u.rep->pattern + 3);
                DATA_POP(&ctx->u.rep->last_ptr);
                if (ret) {
                    MARK_POP_DISCARD(ctx->lastmark);
                    RETURN_ON_ERROR(ret);
                    RETURN_SUCCESS;
                }
                MARK_POP(ctx->lastmark);
                LASTMARK_RESTORE();
                ctx->u.rep->count = ctx->count-1;
                state->ptr = ctx->ptr;
            }

            /* cannot match more repeated items here.  make sure the
               tail matches */
            state->repeat = ctx->u.rep->prev;
            DO_JUMP(JUMP_MAX_UNTIL_3, jump_max_until_3, ctx->pattern);
            RETURN_ON_SUCCESS(ret);
            state->repeat = ctx->u.rep;
            state->ptr = ctx->ptr;
            RETURN_FAILURE;
        case SRE_OP_MIN_UNTIL:
            /* minimizing repeat */
            /* <REPEAT> <skip> <1=min> <2=max> item <MIN_UNTIL> tail */
            ctx->u.rep = state->repeat;
            if (!ctx->u.rep) {
                RETURN_ERROR(SRE_ERROR_STATE);
            }

            state->ptr = ctx->ptr;

            ctx->count = ctx->u.rep->count + 1;

            TRACE(("|%p|%p|MIN_UNTIL %d %p\n", ctx->pattern, ctx->ptr, ctx->count, ctx->u.rep->pattern));

            if (ctx->count < ctx->u.rep->pattern[1]) {
                /* not enough matches */
                ctx->u.rep->count = ctx->count;
                DO_JUMP(JUMP_MIN_UNTIL_1, jump_min_until_1, ctx->u.rep->pattern + 3);
                if (ret) {
                    RETURN_ON_ERROR(ret);
                    RETURN_SUCCESS;
                }
                ctx->u.rep->count = ctx->count - 1;
                state->ptr = ctx->ptr;
                RETURN_FAILURE;
            }

            LASTMARK_SAVE();

            /* see if the tail matches */
            state->repeat = ctx->u.rep->prev;
            DO_JUMP(JUMP_MIN_UNTIL_2, jump_min_until_2, ctx->pattern);
            if (ret) {
                RETURN_ON_ERROR(ret);
                RETURN_SUCCESS;
            }

            state->repeat = ctx->u.rep;
            state->ptr = ctx->ptr;

            LASTMARK_RESTORE();

            if ((ctx->u.rep->pattern[2] <= ctx->count) && (ctx->u.rep->pattern[2] != 65535)) {
                RETURN_FAILURE;
            }

            ctx->u.rep->count = ctx->count;
            DO_JUMP(JUMP_MIN_UNTIL_3, jump_min_until_3, ctx->u.rep->pattern + 3);
            if (ret) {
                RETURN_ON_ERROR(ret);
                RETURN_SUCCESS;
            }
            ctx->u.rep->count = ctx->count - 1;
            state->ptr = ctx->ptr;
            RETURN_FAILURE;
        case SRE_OP_GROUPREF:
            /* match backreference */
            TRACE(("|%p|%p|GROUPREF %d\n", ctx->pattern, ctx->ptr, ctx->pattern[0]));
            i = ctx->pattern[0];
            {
                CorgiInt groupref = i + i;
                if (state->lastmark <= groupref) {
                    RETURN_FAILURE;
                }
                CorgiChar* p = (CorgiChar*)state->mark[groupref];
                CorgiChar* e = (CorgiChar*)state->mark[groupref + 1];
                if (!p || !e || (e < p)) {
                    RETURN_FAILURE;
                }
                while (p < e) {
                    if ((end <= ctx->ptr) || (*ctx->ptr != *p)) {
                        RETURN_FAILURE;
                    }
                    p++;
                    ctx->ptr++;
                }
            }
            ctx->pattern++;
            break;
        case SRE_OP_GROUPREF_IGNORE:
            /* match backreference */
            TRACE(("|%p|%p|GROUPREF_IGNORE %d\n", ctx->pattern, ctx->ptr, ctx->pattern[0]));
            i = ctx->pattern[0];
            {
                CorgiInt groupref = i+i;
                if (groupref >= state->lastmark) {
                    RETURN_FAILURE;
                }
                CorgiChar* p = (CorgiChar*)state->mark[groupref];
                CorgiChar* e = (CorgiChar*)state->mark[groupref + 1];
                if (!p || !e || (e < p)) {
                    RETURN_FAILURE;
                }
                while (p < e) {
                    if ((end <= ctx->ptr) || (corgi_tolower(*ctx->ptr) != corgi_tolower(*p))) {
                        RETURN_FAILURE;
                    }
                    p++;
                    ctx->ptr++;
                }
            }
            ctx->pattern++;
            break;

        case SRE_OP_GROUPREF_EXISTS:
            TRACE(("|%p|%p|GROUPREF_EXISTS %d\n", ctx->pattern, ctx->ptr, ctx->pattern[0]));
            /* <GROUPREF_EXISTS> <group> <skip> codeyes <JUMP> codeno ... */
            i = ctx->pattern[0];
            {
                CorgiInt groupref = i + i;
                if (state->lastmark <= groupref) {
                    ctx->pattern += ctx->pattern[1];
                    break;
                }
                else {
                    CorgiChar* p = (CorgiChar*)state->mark[groupref];
                    CorgiChar* e = (CorgiChar*)state->mark[groupref + 1];
                    if (!p || !e || (e < p)) {
                        ctx->pattern += ctx->pattern[1];
                        break;
                    }
                }
            }
            ctx->pattern += 2;
            break;
        case SRE_OP_ASSERT:
            /* assert subpattern */
            /* <ASSERT> <skip> <back> <pattern> */
            TRACE(("|%p|%p|ASSERT %d\n", ctx->pattern, ctx->ptr, ctx->pattern[1]));
            state->ptr = ctx->ptr - ctx->pattern[1];
            if (state->ptr < state->beginning) {
                RETURN_FAILURE;
            }
            DO_JUMP(JUMP_ASSERT, jump_assert, ctx->pattern + 2);
            RETURN_ON_FAILURE(ret);
            ctx->pattern += ctx->pattern[0];
            break;
        case SRE_OP_ASSERT_NOT:
            /* assert not subpattern */
            /* <ASSERT_NOT> <skip> <back> <pattern> */
            TRACE(("|%p|%p|ASSERT_NOT %d\n", ctx->pattern, ctx->ptr, ctx->pattern[1]));
            state->ptr = ctx->ptr - ctx->pattern[1];
            if (state->beginning <= state->ptr) {
                DO_JUMP(JUMP_ASSERT_NOT, jump_assert_not, ctx->pattern + 2);
                if (ret) {
                    RETURN_ON_ERROR(ret);
                    RETURN_FAILURE;
                }
            }
            ctx->pattern += ctx->pattern[0];
            break;
        case SRE_OP_FAILURE:
            /* immediate failure */
            TRACE(("|%p|%p|FAILURE\n", ctx->pattern, ctx->ptr));
            RETURN_FAILURE;
        default:
            TRACE(("|%p|%p|UNKNOWN %d\n", ctx->pattern, ctx->ptr, ctx->pattern[-1]));
            RETURN_ERROR(SRE_ERROR_ILLEGAL);
        }
    }

exit:
    ctx_pos = ctx->last_ctx_pos;
    CorgiInt jump = ctx->jump;
    DATA_POP_DISCARD(ctx);
    if (ctx_pos == -1) {
        return ret;
    }
    DATA_LOOKUP_AT(sre_match_context, ctx, ctx_pos);

    switch (jump) {
    case JUMP_MAX_UNTIL_2:
        TRACE(("|%p|%p|JUMP_MAX_UNTIL_2\n", ctx->pattern, ctx->ptr));
        goto jump_max_until_2;
    case JUMP_MAX_UNTIL_3:
        TRACE(("|%p|%p|JUMP_MAX_UNTIL_3\n", ctx->pattern, ctx->ptr));
        goto jump_max_until_3;
    case JUMP_MIN_UNTIL_2:
        TRACE(("|%p|%p|JUMP_MIN_UNTIL_2\n", ctx->pattern, ctx->ptr));
        goto jump_min_until_2;
    case JUMP_MIN_UNTIL_3:
        TRACE(("|%p|%p|JUMP_MIN_UNTIL_3\n", ctx->pattern, ctx->ptr));
        goto jump_min_until_3;
    case JUMP_BRANCH:
        TRACE(("|%p|%p|JUMP_BRANCH\n", ctx->pattern, ctx->ptr));
        goto jump_branch;
    case JUMP_MAX_UNTIL_1:
        TRACE(("|%p|%p|JUMP_MAX_UNTIL_1\n", ctx->pattern, ctx->ptr));
        goto jump_max_until_1;
    case JUMP_MIN_UNTIL_1:
        TRACE(("|%p|%p|JUMP_MIN_UNTIL_1\n", ctx->pattern, ctx->ptr));
        goto jump_min_until_1;
    case JUMP_REPEAT:
        TRACE(("|%p|%p|JUMP_REPEAT\n", ctx->pattern, ctx->ptr));
        goto jump_repeat;
    case JUMP_REPEAT_ONE_1:
        TRACE(("|%p|%p|JUMP_REPEAT_ONE_1\n", ctx->pattern, ctx->ptr));
        goto jump_repeat_one_1;
    case JUMP_REPEAT_ONE_2:
        TRACE(("|%p|%p|JUMP_REPEAT_ONE_2\n", ctx->pattern, ctx->ptr));
        goto jump_repeat_one_2;
    case JUMP_MIN_REPEAT_ONE:
        TRACE(("|%p|%p|JUMP_MIN_REPEAT_ONE\n", ctx->pattern, ctx->ptr));
        goto jump_min_repeat_one;
    case JUMP_ASSERT:
        TRACE(("|%p|%p|JUMP_ASSERT\n", ctx->pattern, ctx->ptr));
        goto jump_assert;
    case JUMP_ASSERT_NOT:
        TRACE(("|%p|%p|JUMP_ASSERT_NOT\n", ctx->pattern, ctx->ptr));
        goto jump_assert_not;
    case JUMP_NONE:
        TRACE(("|%p|%p|RETURN %d\n", ctx->pattern, ctx->ptr, ret));
        break;
    }

    return ret; /* should never get here */
}

static CorgiInt
sre_search(State* state, CorgiCode* pattern)
{
    CorgiChar* ptr = state->start;
    CorgiChar* end = state->end;
    CorgiInt status = 0;
    CorgiInt prefix_len = 0;
    CorgiInt prefix_skip = 0;
    CorgiCode* prefix = NULL;
    CorgiCode* charset = NULL;
    CorgiCode* overlap = NULL;
    int flags = 0;

    if (pattern[0] == SRE_OP_INFO) {
        /* optimization info block */
        /* <INFO> <1=skip> <2=flags> <3=min> <4=max> <5=prefix info>  */
        flags = pattern[2];
        if (1 < pattern[3]) {
            /* adjust end point (but make sure we leave at least one
               character in there, so literal search will work) */
            end -= pattern[3] - 1;
            if (end <= ptr) {
                end = ptr + 1;
            }
        }

        if (flags & SRE_INFO_PREFIX) {
            /* pattern starts with a known prefix */
            /* <length> <skip> <prefix data> <overlap data> */
            prefix_len = pattern[5];
            prefix_skip = pattern[6];
            prefix = pattern + 7;
            overlap = prefix + prefix_len - 1;
        } else if (flags & SRE_INFO_CHARSET) {
            /* pattern starts with a character from a known set */
            /* <charset> */
            charset = pattern + 5;
        }

        pattern += 1 + pattern[1];
    }

    TRACE(("prefix = %p %d %d\n", prefix, prefix_len, prefix_skip));
    TRACE(("charset = %p\n", charset));

    if (1 < prefix_len) {
        /* pattern starts with a known prefix.  use the overlap
           table to skip forward as fast as we possibly can */
        CorgiInt i = 0;
        end = state->end;
        while (ptr < end) {
            for (;;) {
                if (ptr[0] != prefix[i]) {
                    if (!i) {
                        break;
                    }
                    i = overlap[i];
                } else {
                    if (++i == prefix_len) {
                        /* found a potential match */
                        TRACE(("|%p|%p|SEARCH SCAN\n", pattern, ptr));
                        state->start = ptr + 1 - prefix_len;
                        state->ptr = ptr + 1 - prefix_len + prefix_skip;
                        if (flags & SRE_INFO_LITERAL) {
                            return 1; /* we got all of it */
                        }
                        status = sre_match(state, pattern + 2 * prefix_skip);
                        if (status != 0) {
                            return status;
                        }
                        /* close but no cigar -- try again */
                        i = overlap[i];
                    }
                    break;
                }
            }
            ptr++;
        }
        return 0;
    }

    if (pattern[0] == SRE_OP_LITERAL) {
        /* pattern starts with a literal character.  this is used
           for short prefixes, and if fast search is disabled */
        CorgiCode chr = pattern[1];
        end = state->end;
        for (;;) {
            while ((ptr < end) && (ptr[0] != chr)) {
                ptr++;
            }
            if (end <= ptr) {
                return 0;
            }
            TRACE(("|%p|%p|SEARCH LITERAL\n", pattern, ptr));
            state->start = ptr;
            state->ptr = ++ptr;
            if (flags & SRE_INFO_LITERAL) {
                return 1; /* we got all of it */
            }
            status = sre_match(state, pattern + 2);
            if (status != 0) {
                break;
            }
        }
    } else if (charset) {
        /* pattern starts with a character from a known set */
        end = (CorgiChar*)state->end;
        for (;;) {
            while ((ptr < end) && !sre_charset(charset, ptr[0])) {
                ptr++;
            }
            if (end <= ptr) {
                return 0;
            }
            TRACE(("|%p|%p|SEARCH CHARSET\n", pattern, ptr));
            state->start = ptr;
            state->ptr = ptr;
            status = sre_match(state, pattern);
            if (status != 0) {
                break;
            }
            ptr++;
        }
    }
    else {
        /* general case */
        while (ptr <= end) {
            TRACE(("|%p|%p|SEARCH\n", pattern, ptr));
            state->start = state->ptr = ptr++;
            status = sre_match(state, pattern);
            if (status != 0) {
                break;
            }
        }
    }

    return status;
}

static void
state_init(State* state, CorgiRegexp* regexp, CorgiChar* begin, CorgiChar* end, CorgiChar* at, Bool debug)
{
    memset(state, 0, sizeof(State));
    state->lastmark = state->lastindex = -1;
    state->beginning = begin;
    state->ptr = state->start = at;
    state->end = end;
    state->debug = debug;
}

static void
state_fini(State* state)
{
    data_stack_dealloc(state);
}

#if 0
/* -------------------------------------------------------------------- */
/* Code validation */

/* To learn more about this code, have a look at the _compile() function in
   Lib/sre_compile.py.  The validation functions below checks the code array
   for conformance with the code patterns generated there.

   The nice thing about the generated code is that it is position-independent:
   all jumps are relative jumps forward.  Also, jumps don't cross each other:
   the target of a later jump is always earlier than the target of an earlier
   jump.  IOW, this is okay:

   J---------J-------T--------T
    \         \_____/        /
     \______________________/

   but this is not:

   J---------J-------T--------T
    \_________\_____/        /
               \____________/

   It also helps that Code is always an unsigned type, either 2 bytes or 4
   bytes wide (the latter if Python is compiled for "wide" unicode support).
*/

/* Defining this one enables tracing of the validator */
#undef VVERBOSE

/* Trace macro for the validator */
#if defined(VVERBOSE)
#   define VTRACE(v) printf v
#else
#   define VTRACE(v)
#endif

/* Report failure */
#define FAIL do { \
    VTRACE(("FAIL: %d\n", __LINE__)); \
    return 0; \
} while (0)

/* Extract opcode, argument, or skip count from code array */
#define GET_OP do { \
    VTRACE(("%p: ", code)); \
    if (end <= code) { \
        FAIL; \
    } \
    op = *code++; \
    VTRACE(("%lu (op)\n", (unsigned long)op)); \
} while (0)
#define GET_ARG do { \
    VTRACE(("%p= ", code)); \
    if (end <= code) { \
        FAIL; \
    } \
    arg = *code++; \
    VTRACE(("%lu (arg)\n", (unsigned long)arg)); \
} while (0)
#define GET_SKIP_ADJ(adj) do { \
    VTRACE(("%p= ", code)); \
    if (end <= code) { \
        FAIL; \
    } \
    skip = *code; \
    VTRACE(("%lu (skip to %p)\n", (unsigned long)skip, code + skip)); \
    if ((code + skip - adj < code) || (end < code + skip - adj)) { \
        FAIL; \
    } \
    code++; \
} while (0)
#define GET_SKIP GET_SKIP_ADJ(0)

static int
_validate_charset(CorgiCode *code, CorgiCode *end)
{
    /* Some variables are manipulated by the macros above */
    CorgiCode op;
    CorgiCode arg;
    CorgiCode offset;
    int i;

    while (code < end) {
        GET_OP;
        switch (op) {
        case SRE_OP_NEGATE:
            break;
        case SRE_OP_LITERAL:
            GET_ARG;
            break;
        case SRE_OP_RANGE:
            GET_ARG;
            GET_ARG;
            break;
        case SRE_OP_CHARSET:
            offset = 32 / sizeof(CorgiCode); /* 32-byte bitmap */
            if ((code + offset < code) || (end < code + offset)) {
                FAIL;
            }
            code += offset;
            break;
        case SRE_OP_BIGCHARSET:
            GET_ARG; /* Number of blocks */
            offset = 256 / sizeof(CorgiCode); /* 256-byte table */
            if ((code + offset < code) || (end < code + offset)) {
                FAIL;
            }
            /* Make sure that each byte points to a valid block */
            for (i = 0; i < 256; i++) {
                if (arg <= ((unsigned char *)code)[i]) {
                    FAIL;
                }
            }
            code += offset;
            offset = arg * 32 / sizeof(CorgiCode); /* 32-byte bitmap times arg */
            if ((code + offset < code) || (end < code + offset)) {
                FAIL;
            }
            code += offset;
            break;
        case SRE_OP_CATEGORY:
            GET_ARG;
            switch (arg) {
            case SRE_CATEGORY_DIGIT:
            case SRE_CATEGORY_NOT_DIGIT:
            case SRE_CATEGORY_SPACE:
            case SRE_CATEGORY_NOT_SPACE:
            case SRE_CATEGORY_WORD:
            case SRE_CATEGORY_NOT_WORD:
            case SRE_CATEGORY_LINEBREAK:
            case SRE_CATEGORY_NOT_LINEBREAK:
            case SRE_CATEGORY_LOC_WORD:
            case SRE_CATEGORY_LOC_NOT_WORD:
            case SRE_CATEGORY_UNI_DIGIT:
            case SRE_CATEGORY_UNI_NOT_DIGIT:
            case SRE_CATEGORY_UNI_SPACE:
            case SRE_CATEGORY_UNI_NOT_SPACE:
            case SRE_CATEGORY_UNI_WORD:
            case SRE_CATEGORY_UNI_NOT_WORD:
            case SRE_CATEGORY_UNI_LINEBREAK:
            case SRE_CATEGORY_UNI_NOT_LINEBREAK:
                break;
            default:
                FAIL;
            }
            break;
        default:
            FAIL;

        }
    }

    return 1;
}

static int
_validate_inner(CorgiCode *code, CorgiCode *end, CorgiInt groups)
{
    /* Some variables are manipulated by the macros above */
    CorgiCode op;
    CorgiCode arg;
    CorgiCode skip;
    VTRACE(("code=%p, end=%p\n", code, end));
    if (end < code) {
        FAIL;
    }
    while (code < end) {
        GET_OP;
        switch (op) {
        case SRE_OP_MARK:
            /* We don't check whether marks are properly nested; the
               sre_match() code is robust even if they don't, and the worst
               you can get is nonsensical match results. */
            GET_ARG;
            if (2 * groups + 1 < arg) {
                VTRACE(("arg=%d, groups=%d\n", (int)arg, (int)groups));
                FAIL;
            }
            break;
        case SRE_OP_LITERAL:
        case SRE_OP_NOT_LITERAL:
        case SRE_OP_LITERAL_IGNORE:
        case SRE_OP_NOT_LITERAL_IGNORE:
            GET_ARG;
            /* The arg is just a character, nothing to check */
            break;
        case SRE_OP_SUCCESS:
        case SRE_OP_FAILURE:
            /* Nothing to check; these normally end the matching process */
            break;
        case SRE_OP_AT:
            GET_ARG;
            switch (arg) {
            case SRE_AT_BEGINNING:
            case SRE_AT_BEGINNING_STRING:
            case SRE_AT_BEGINNING_LINE:
            case SRE_AT_END:
            case SRE_AT_END_LINE:
            case SRE_AT_END_STRING:
            case SRE_AT_BOUNDARY:
            case SRE_AT_NON_BOUNDARY:
            case SRE_AT_LOC_BOUNDARY:
            case SRE_AT_LOC_NON_BOUNDARY:
            case SRE_AT_UNI_BOUNDARY:
            case SRE_AT_UNI_NON_BOUNDARY:
                break;
            default:
                FAIL;
            }
            break;
        case SRE_OP_ANY:
        case SRE_OP_ANY_ALL:
            /* These have no operands */
            break;
        case SRE_OP_IN:
        case SRE_OP_IN_IGNORE:
            GET_SKIP;
            /* Stop 1 before the end; we check the FAILURE below */
            if (!_validate_charset(code, code + skip - 2)) {
                FAIL;
            }
            if (code[skip - 2] != SRE_OP_FAILURE) {
                FAIL;
            }
            code += skip - 1;
            break;
        case SRE_OP_INFO:
            {
                /* A minimal info field is
                   <INFO> <1=skip> <2=flags> <3=min> <4=max>;
                   If SRE_INFO_PREFIX or SRE_INFO_CHARSET is in the flags,
                   more follows. */
                CorgiCode flags, min, max, i;
                CorgiCode *newcode;
                GET_SKIP;
                newcode = code + skip - 1;
                GET_ARG; flags = arg;
                GET_ARG; min = arg;
                GET_ARG; max = arg;
                /* Check that only valid flags are present */
                if ((flags & ~(SRE_INFO_PREFIX | SRE_INFO_LITERAL | SRE_INFO_CHARSET)) != 0) {
                    FAIL;
                }
                /* PREFIX and CHARSET are mutually exclusive */
                if ((flags & SRE_INFO_PREFIX) && (flags & SRE_INFO_CHARSET)) {
                    FAIL;
                }
                /* LITERAL implies PREFIX */
                if ((flags & SRE_INFO_LITERAL) && !(flags & SRE_INFO_PREFIX)) {
                    FAIL;
                }
                /* Validate the prefix */
                if (flags & SRE_INFO_PREFIX) {
                    GET_ARG;
                    CorgiCode prefix_len = arg;
                    GET_ARG;
                    CorgiCode prefix_skip = arg;
                    /* Here comes the prefix string */
                    if ((code + prefix_len < code) || (newcode < code + prefix_len)) {
                        FAIL;
                    }
                    code += prefix_len;
                    /* And here comes the overlap table */
                    if ((code + prefix_len < code) || (newcode < code + prefix_len)) {
                        FAIL;
                    }
                    /* Each overlap value should be < prefix_len */
                    for (i = 0; i < prefix_len; i++) {
                        if (prefix_len <= code[i]) {
                            FAIL;
                        }
                    }
                    code += prefix_len;
                }
                /* Validate the charset */
                if (flags & SRE_INFO_CHARSET) {
                    if (!_validate_charset(code, newcode - 1)) {
                        FAIL;
                    }
                    if (newcode[-1] != SRE_OP_FAILURE) {
                        FAIL;
                    }
                    code = newcode;
                }
                else if (code != newcode) {
                    VTRACE(("code=%p, newcode=%p\n", code, newcode));
                    FAIL;
                }
            }
            break;
        case SRE_OP_BRANCH:
            {
                CorgiCode *target = NULL;
                for (;;) {
                    GET_SKIP;
                    if (skip == 0) {
                        break;
                    }
                    /* Stop 2 before the end; we check the JUMP below */
                    if (!_validate_inner(code, code + skip - 3, groups)) {
                        FAIL;
                    }
                    code += skip - 3;
                    /* Check that it ends with a JUMP, and that each JUMP
                       has the same target */
                    GET_OP;
                    if (op != SRE_OP_JUMP) {
                        FAIL;
                    }
                    GET_SKIP;
                    if (target == NULL) {
                        target = code + skip - 1;
                    }
                    else if (code + skip - 1 != target) {
                        FAIL;
                    }
                }
            }
            break;
        case SRE_OP_REPEAT_ONE:
        case SRE_OP_MIN_REPEAT_ONE:
            {
                GET_SKIP;
                GET_ARG;
                CorgiCode min = arg;
                GET_ARG;
                CorgiCode max = arg;
                if (max < min) {
                    FAIL;
                }
                if (65535 < max) {
                    FAIL;
                }
                if (!_validate_inner(code, code + skip - 4, groups)) {
                    FAIL;
                }
                code += skip - 4;
                GET_OP;
                if (op != SRE_OP_SUCCESS) {
                    FAIL;
                }
            }
            break;
        case SRE_OP_REPEAT:
            {
                GET_SKIP;
                GET_ARG;
                CorgiCode min = arg;
                GET_ARG;
                CorgiCode max = arg;
                if (max < min) {
                    FAIL;
                }
                if (65535 < max) {
                    FAIL;
                }
                if (!_validate_inner(code, code + skip - 3, groups)) {
                    FAIL;
                }
                code += skip - 3;
                GET_OP;
                if ((op != SRE_OP_MAX_UNTIL) && (op != SRE_OP_MIN_UNTIL)) {
                    FAIL;
                }
            }
            break;
        case SRE_OP_GROUPREF:
        case SRE_OP_GROUPREF_IGNORE:
            GET_ARG;
            if (groups <= arg) {
                FAIL;
            }
            break;
        case SRE_OP_GROUPREF_EXISTS:
            /* The regex syntax for this is: '(?(group)then|else)', where
               'group' is either an integer group number or a group name,
               'then' and 'else' are sub-regexes, and 'else' is optional. */
            GET_ARG;
            if (groups <= arg) {
                FAIL;
            }
            GET_SKIP_ADJ(1);
            code--; /* The skip is relative to the first arg! */
            /* There are two possibilities here: if there is both a 'then'
               part and an 'else' part, the generated code looks like:

               GROUPREF_EXISTS
               <group>
               <skipyes>
               ...then part...
               JUMP
               <skipno>
               (<skipyes> jumps here)
               ...else part...
               (<skipno> jumps here)

               If there is only a 'then' part, it looks like:

               GROUPREF_EXISTS
               <group>
               <skip>
               ...then part...
               (<skip> jumps here)

               There is no direct way to decide which it is, and we don't want
               to allow arbitrary jumps anywhere in the code; so we just look
               for a JUMP opcode preceding our skip target.
            */
            if ((3 <= skip) && (code <= code + skip - 3) && (code[skip - 3] == SRE_OP_JUMP)) {
                VTRACE(("both then and else parts present\n"));
                if (!_validate_inner(code + 1, code + skip - 3, groups)) {
                    FAIL;
                }
                code += skip - 2; /* Position after JUMP, at <skipno> */
                GET_SKIP;
                if (!_validate_inner(code, code + skip - 1, groups)) {
                    FAIL;
                }
                code += skip - 1;
            }
            else {
                VTRACE(("only a then part present\n"));
                if (!_validate_inner(code + 1, code + skip - 1, groups)) {
                    FAIL;
                }
                code += skip - 1;
            }
            break;
        case SRE_OP_ASSERT:
        case SRE_OP_ASSERT_NOT:
            GET_SKIP;
            GET_ARG; /* 0 for lookahead, width for lookbehind */
            code--; /* Back up over arg to simplify math below */
            if (arg & 0x80000000) {
                FAIL; /* Width too large */
            }
            /* Stop 1 before the end; we check the SUCCESS below */
            if (!_validate_inner(code + 1, code + skip - 2, groups)) {
                FAIL;
            }
            code += skip - 2;
            GET_OP;
            if (op != SRE_OP_SUCCESS) {
                FAIL;
            }
            break;
        default:
            FAIL;
        }
    }

    VTRACE(("okay\n"));
    return 1;
}

static int
_validate_outer(CorgiCode *code, CorgiCode *end, CorgiInt groups)
{
    if ((groups < 0) || (100 < groups) || (end <= code) || (end[-1] != SRE_OP_SUCCESS)) {
        FAIL;
    }
    if (groups == 0) { /* fix for simplejson */
        groups = 100; /* 100 groups should always be safe */
    }
    return _validate_inner(code, end - 1, groups);
}

static int
_validate(PatternObject *self)
{
    if (!_validate_outer(self->code, self->code+self->codesize, self->groups))
    {
        PyErr_SetString(PyExc_RuntimeError, "invalid SRE code");
        return 0;
    }
    else
        VTRACE(("Success!\n"));
    return 1;
}
#endif

CorgiStatus
corgi_init_regexp(CorgiRegexp* regexp)
{
    bzero(regexp, sizeof(*regexp));
    return CORGI_OK;
}

CorgiStatus
corgi_fini_regexp(CorgiRegexp* regexp)
{
    free(regexp->code);
    return CORGI_OK;
}

CorgiStatus
corgi_init_match(CorgiMatch* match)
{
    bzero(match, sizeof(*match));
    return CORGI_OK;
}

CorgiStatus
corgi_fini_match(CorgiMatch* match)
{
    return CORGI_OK;
}

struct Storage {
    struct Storage* next;
    char* free;
    char items[0];
};

typedef struct Storage Storage;

#define STORAGE_SIZE (1024 * 1024)

static Storage*
alloc_storage(Storage* next)
{
    Storage* storage = (Storage*)malloc(STORAGE_SIZE);
    if (storage == NULL) {
        return NULL;
    }
    storage->next = next;
    storage->free = storage->items;
    return storage;
}

static void* alloc_from_storage(Storage**, CorgiUInt);

static void*
alloc_from_new_storage(Storage** storage, CorgiUInt size)
{
    Storage* new_storage = alloc_storage(*storage);
    if (new_storage == NULL) {
        return NULL;
    }
    *storage = new_storage;
    return alloc_from_storage(storage, size);
}

static void*
alloc_from_storage(Storage** storage, CorgiUInt size)
{
    char* pend = (char*)(*storage) + STORAGE_SIZE;
    if (pend <= (*storage)->free + size) {
        return alloc_from_new_storage(storage, size);
    }
    void* p = (*storage)->free;
    (*storage)->free += size;
    return p;
}

static void
free_storage(Storage* storage)
{
    Storage* p = storage;
    while (p != NULL) {
        Storage* next = p->next;
        free(p);
        p = next;
    }
}

enum NodeType {
    NODE_BRANCH,
    NODE_CATEGORY,
    NODE_IN,
    NODE_LITERAL,
    NODE_MAX_REPEAT,
    NODE_NEGATE,
    NODE_RANGE,
};

typedef enum NodeType NodeType;

struct Node {
    enum NodeType type;
    union {
        struct {
            struct Node* left;
            struct Node* right;
        } branch;
        struct {
            CorgiCode type;
        } category;
        struct {
            struct Node* set;
        } in;
        struct {
            CorgiChar c;
        } literal;
        struct {
            CorgiUInt min;
            CorgiUInt max;
            struct Node* body;
        } max_repeat;
        struct {
            CorgiChar low;
            CorgiChar high;
        } range;
    } u;
    struct Node* next;
};

typedef struct Node Node;

static CorgiStatus
create_node(Storage** storage, NodeType type, Node** node)
{
    *node = alloc_from_storage(storage, sizeof(**node));
    if (*node == NULL) {
        return ERR_OUT_OF_MEMORY;
    }
    bzero(*node, sizeof(**node));
    (*node)->type = type;
    (*node)->next = NULL;
    return CORGI_OK;
}

static CorgiStatus
create_literal_node(Storage** storage, CorgiChar c, Node** node)
{
    CorgiStatus status = create_node(storage, NODE_LITERAL, node);
    if (status != CORGI_OK) {
        return status;
    }
    (*node)->u.literal.c = c;
    return CORGI_OK;
}

static CorgiStatus
create_two_literal_nodes(Storage** storage, CorgiChar c1, CorgiChar c2, Node** node)
{
    CorgiStatus status = create_literal_node(storage, c1, node);
    if (status != CORGI_OK) {
        return status;
    }
    Node* next = NULL;
    status = create_literal_node(storage, c2, &next);
    if (status != CORGI_OK) {
        return status;
    }
    (*node)->next = next;
    return CORGI_OK;
}

static CorgiStatus
parse_in_internal(Storage** storage, CorgiChar** pc, CorgiChar* end, Node** node)
{
    CorgiChar c = **pc;
    (*pc)++;
    if (c == '^') {
        return create_node(storage, NODE_NEGATE, node);
    }
    if ((end <= *pc) || (**pc != '-')) {
        return create_literal_node(storage, c, node);
    }
    assert(**pc == '-');
    (*pc)++;
    if ((end <= *pc) || (**pc == ']')) {
        return create_two_literal_nodes(storage, c, '-', node);
    }
    if (**pc < c) {
        return ERR_BAD_RANGE;
    }
    CorgiStatus status = create_node(storage, NODE_RANGE, node);
    if (status != CORGI_OK) {
        return status;
    }
    (*node)->u.range.low = c;
    (*node)->u.range.high = **pc;
    (*pc)++;
    return CORGI_OK;
}

static CorgiStatus
create_in_node(Storage** storage, Node** node)
{
    return create_node(storage, NODE_IN, node);
}

static CorgiStatus
parse_in(Storage** storage, CorgiChar** pc, CorgiChar* end, Node** node)
{
    CorgiStatus status = create_in_node(storage, node);
    if (status != CORGI_OK) {
        return status;
    }
    Node** last = &(*node)->u.in.set;
    while ((status == CORGI_OK) && (*pc < end) && (**pc != ']')) {
        Node* node = NULL;
        status = parse_in_internal(storage, pc, end, &node);
        *last = node;
        last = &node->next;
    }
    if ((status != CORGI_OK) || (end <= *pc)) {
        return status;
    }
    assert(**pc == ']');
    (*pc)++;
    return CORGI_OK;
}

static CorgiStatus
create_category_node(Storage** storage, CorgiCode type, Node** node)
{
    CorgiStatus status = create_node(storage, NODE_CATEGORY, node);
    if (status != CORGI_OK) {
        return status;
    }
    (*node)->u.category.type = type;
    return CORGI_OK;
}

static CorgiStatus
create_in_with_category_node(Storage** storage, CorgiCode type, Node** node)
{
    CorgiStatus status = create_in_node(storage, node);
    if (status != CORGI_OK) {
        return status;
    }
    Node* n = NULL;
    status = create_category_node(storage, type, &n);
    if (status != CORGI_OK) {
        return status;
    }
    (*node)->u.in.set = n;
    return CORGI_OK;
}

static CorgiStatus
parse_escape(Storage** storage, CorgiChar** pc, CorgiChar* end, Node** node)
{
    if (end <= *pc) {
        return ERR_BOGUS_ESCAPE;
    }
    CorgiCode type;
    CorgiChar c = **pc;
    (*pc)++;
    switch (c) {
    case 'S':
        type = SRE_CATEGORY_UNI_NOT_SPACE;
        break;
    case 'W':
        type = SRE_CATEGORY_UNI_NOT_WORD;
        break;
    case 's':
        type = SRE_CATEGORY_UNI_SPACE;
        break;
    case 'w':
        type = SRE_CATEGORY_UNI_WORD;
        break;
    default:
        return create_literal_node(storage, c, node);
    }
    return create_in_with_category_node(storage, type, node);
}

static CorgiStatus
parse_single_pattern(Storage** storage, CorgiChar** pc, CorgiChar* end, Node** node)
{
    if (**pc == '[') {
        (*pc)++;
        return parse_in(storage, pc, end, node);
    }
    if (**pc == '\\') {
        (*pc)++;
        return parse_escape(storage, pc, end, node);
    }

    CorgiStatus status = create_literal_node(storage, **pc, node);
    if (status != CORGI_OK) {
        return status;
    }
    (*pc)++;
    return CORGI_OK;
}

static CorgiStatus
make_repeat(Storage** storage, CorgiChar** pc, CorgiChar* end, CorgiUInt min, CorgiUInt max, Node* body, Node** node)
{
    CorgiStatus status = create_node(storage, NODE_MAX_REPEAT, node);
    if (status != CORGI_OK) {
        return status;
    }
    (*node)->u.max_repeat.min = min;
    (*node)->u.max_repeat.max = max;
    (*node)->u.max_repeat.body = body;
    return CORGI_OK;
}

static CorgiStatus
parse_repeat(Storage** storage, CorgiChar** pc, CorgiChar* end, Node** node)
{
    Node* n = NULL;
    CorgiStatus status = parse_single_pattern(storage, pc, end, &n);
    if (status != CORGI_OK) {
        return status;
    }
    if (end <= *pc) {
        *node = n;
        return CORGI_OK;
    }
    if (**pc == '*') {
        (*pc)++;
        return make_repeat(storage, pc, end, 0, 65535, n, node);
    }
    *node = n;
    return CORGI_OK;
}

static CorgiStatus
parse_sub_pattern(Storage** storage, CorgiChar** pc, CorgiChar* end, Node** node)
{
    CorgiStatus status = parse_repeat(storage, pc, end, node);
    if (status != CORGI_OK) {
        return status;
    }

    Node* prev = *node;
    while ((*pc < end) && (**pc != '|') && (**pc != ')') && (status == CORGI_OK)) {
        Node* node = NULL;
        status = parse_repeat(storage, pc, end, &node);
        prev->next = node;
        prev = node;
    }

    return status;
}

static CorgiStatus
parse_branch(Storage** storage, CorgiChar** pc, CorgiChar* end, Node** node)
{
    CorgiStatus status = create_node(storage, NODE_BRANCH, node);
    if (status != CORGI_OK) {
        return status;
    }
    (*node)->u.branch.left = (*node)->u.branch.right = NULL;

    Node* left = NULL;
    status = parse_sub_pattern(storage, pc, end, &left);
    if (status != CORGI_OK) {
        return status;
    }
    (*node)->u.branch.left = left;

    if ((end <= (*pc)) || ((**pc) != '|')) {
        *node = left;
        return CORGI_OK;
    }
    (*pc)++;

    Node* right = NULL;
    status = parse_sub_pattern(storage, pc, end, &right);
    if (status != CORGI_OK) {
        return status;
    }
    (*node)->u.branch.right = right;

    return CORGI_OK;
}

enum InstructionType {
    INST_BRANCH,
    INST_CATEGORY,
    INST_FAILURE,
    INST_IN,
    INST_JUMP,
    INST_LABEL,
    INST_LITERAL,
    INST_MAX_UNTIL,
    INST_NEGATE,
    INST_OFFSET,
    INST_RANGE,
    INST_REPEAT,
    INST_SUCCESS,
};

typedef enum InstructionType InstructionType;

struct Instruction {
    enum InstructionType type;
    CorgiUInt pos;
    union {
        struct {
            CorgiCode type;
        } category;
        struct {
            struct Instruction* dest;
        } in;
        struct {
            struct Instruction* dest;
        } jump;
        struct {
            CorgiChar c;
        } literal;
        struct {
            struct Instruction* dest;
        } offset;
        struct {
            CorgiChar low;
            CorgiChar high;
        } range;
        struct {
            struct Instruction* dest;
            CorgiUInt min;
            CorgiUInt max;
        } repeat;
    } u;
    struct Instruction* next;
};

typedef struct Instruction Instruction;

static CorgiStatus
create_instruction(Storage** storage, InstructionType type, Instruction** inst)
{
    *inst = alloc_from_storage(storage, sizeof(**inst));
    if (*inst == NULL) {
        return ERR_OUT_OF_MEMORY;
    }
    bzero(*inst, sizeof(**inst));
    (*inst)->type = type;
    return CORGI_OK;
}

static CorgiStatus
create_label(Storage** storage, Instruction** inst)
{
    return create_instruction(storage, INST_LABEL, inst);
}

static Instruction*
get_last_instruction(Instruction* inst)
{
    if (inst == NULL) {
        return NULL;
    }

    Instruction* i;
    for (i = inst; i->next != NULL; i = i->next) {
    }
    return i;
}

static CorgiStatus node2instruction(Storage**, Node*, Instruction**);

static CorgiStatus
branch_child2instruction(Storage** storage, Node* node, Instruction* branch_last, Instruction** inst)
{
    CorgiStatus status = create_instruction(storage, INST_OFFSET, inst);
    if (status != CORGI_OK) {
        return status;
    }
    Instruction* last = NULL;
    status = create_label(storage, &last);
    if (status != CORGI_OK) {
        return status;
    }
    (*inst)->u.offset.dest = last;

    Instruction* i = NULL;
    status = node2instruction(storage, node, &i);
    if (status != CORGI_OK) {
        return status;
    }
    (*inst)->next = i;
    Instruction* rear = get_last_instruction(i);

    Instruction* jump = NULL;
    status = create_instruction(storage, INST_JUMP, &jump);
    if (status != CORGI_OK) {
        return status;
    }
    rear->next = jump;
    jump->u.jump.dest = branch_last;
    jump->next = last;

    return CORGI_OK;
}

static CorgiStatus
branch_children2instruction(Storage** storage, Node* node, Instruction* branch_last, Instruction** inst)
{
    assert(node->type == NODE_BRANCH);
    CorgiStatus status = branch_child2instruction(storage, node->u.branch.left, branch_last, inst);
    if (status != CORGI_OK) {
        return status;
    }
    Instruction* i = NULL;
    if (node->u.branch.right->type != NODE_BRANCH) {
        status = branch_child2instruction(storage, node->u.branch.right, branch_last, &i);
    }
    else {
        status = branch_children2instruction(storage, node->u.branch.right, branch_last, &i);
    }
    if (status != CORGI_OK) {
        return status;
    }
    get_last_instruction(*inst)->next = i;
    return CORGI_OK;
}

static CorgiStatus single_node2instruction(Storage**, Node*, Instruction**);

static CorgiStatus
in2instruction(Storage** storage, Node* node, Instruction** inst)
{
    CorgiStatus status = create_instruction(storage, INST_IN, inst);
    if (status != CORGI_OK) {
        return status;
    }
    Instruction* dest = NULL;
    status = create_label(storage, &dest);
    if (status != CORGI_OK) {
        return status;
    }
    (*inst)->u.in.dest = dest;
    Instruction* last = *inst;
    Node* n;
    for (n = node->u.in.set; (n != NULL) && (status == CORGI_OK); n = n->next) {
        Instruction* i = NULL;
        status = single_node2instruction(storage, n, &i);
        last->next = i;
        last = get_last_instruction(i);
    }
    Instruction* failure = NULL;
    status = create_instruction(storage, INST_FAILURE, &failure);
    if (status != CORGI_OK) {
        return status;
    }
    last->next = failure;
    failure->next = dest;
    return status;
}

static CorgiStatus
branch2instruction(Storage** storage, Node* node, Instruction** inst)
{
    assert(node->type == NODE_BRANCH);
    CorgiStatus status = create_instruction(storage, INST_BRANCH, inst);
    if (status != CORGI_OK) {
        return status;
    }
    Instruction* label = NULL;
    status = create_label(storage, &label);
    if (status != CORGI_OK) {
        return status;
    }
    Instruction* internal = NULL;
    status = branch_children2instruction(storage, node, label, &internal);
    if (status != CORGI_OK) {
        return status;
    }
    (*inst)->next = internal;
    Instruction* failure = NULL;
    status = create_instruction(storage, INST_FAILURE, &failure);
    if (status != CORGI_OK) {
        return status;
    }
    Instruction* rear = get_last_instruction(internal);
    rear->next = failure;
    failure->next = label;
    return CORGI_OK;
}

static CorgiStatus
literal2instruction(Storage** storage, Node* node, Instruction** inst)
{
    CorgiStatus status = create_instruction(storage, INST_LITERAL, inst);
    if (status != CORGI_OK) {
        return status;
    }
    (*inst)->u.literal.c = node->u.literal.c;
    return CORGI_OK;
}

static CorgiStatus
range2instruction(Storage** storage, Node* node, Instruction** inst)
{
    CorgiStatus status = create_instruction(storage, INST_RANGE, inst);
    if (status != CORGI_OK) {
        return status;
    }
    (*inst)->u.range.low = node->u.range.low;
    (*inst)->u.range.high = node->u.range.high;
    return CORGI_OK;
}

static CorgiStatus
max_repeat2instruction(Storage** storage, Node* node, Instruction** inst)
{
    CorgiStatus status = create_instruction(storage, INST_REPEAT, inst);
    if (status != CORGI_OK) {
        return status;
    }
    (*inst)->u.repeat.min = node->u.max_repeat.min;
    (*inst)->u.repeat.max = node->u.max_repeat.max;
    Instruction* dest = NULL;
    status = create_label(storage, &dest);
    if (status != CORGI_OK) {
        return status;
    }
    (*inst)->u.repeat.dest = dest;
    Instruction* i = NULL;
    status = single_node2instruction(storage, node->u.max_repeat.body, &i);
    if (status != CORGI_OK) {
        return status;
    }
    (*inst)->next = i;
    get_last_instruction(i)->next = dest;
    Instruction* max_until = NULL;
    status = create_instruction(storage, INST_MAX_UNTIL, &max_until);
    if (status != CORGI_OK) {
        return status;
    }
    dest->next = max_until;
    return CORGI_OK;
}

static CorgiStatus
category2instruction(Storage** storage, Node* node, Instruction** inst)
{
    CorgiStatus status = create_instruction(storage, INST_CATEGORY, inst);
    if (status != CORGI_OK) {
        return status;
    }
    (*inst)->u.category.type = node->u.category.type;
    return CORGI_OK;
}

static CorgiStatus
single_node2instruction(Storage** storage, Node* node, Instruction** inst)
{
    CorgiStatus (*f)(Storage**, Node*, Instruction**);
    switch (node->type) {
    case NODE_BRANCH:
        f = branch2instruction;
        break;
    case NODE_CATEGORY:
        f = category2instruction;
        break;
    case NODE_IN:
        f = in2instruction;
        break;
    case NODE_LITERAL:
        f = literal2instruction;
        break;
    case NODE_MAX_REPEAT:
        f = max_repeat2instruction;
        break;
    case NODE_NEGATE:
        return create_instruction(storage, INST_NEGATE, inst);
    case NODE_RANGE:
        f = range2instruction;
        break;
    default:
        return ERR_INVALID_NODE;
    }
    return f(storage, node, inst);
}

static CorgiStatus
node2instruction(Storage** storage, Node* node, Instruction** inst)
{
    CorgiStatus status = single_node2instruction(storage, node, inst);
    if (status != CORGI_OK) {
        return status;
    }

    Instruction* rear = get_last_instruction(*inst);
    Node* n = node->next;
    while ((n != NULL) && (status == CORGI_OK)) {
        Instruction* i = NULL;
        status = single_node2instruction(storage, n, &i);
        rear->next = i;

        rear = get_last_instruction(i);
        n = n->next;
    }
    return status;
}

static CorgiUInt
get_operands_number(Instruction* inst)
{
    switch (inst->type) {
    case INST_BRANCH:
        return 0;
    case INST_CATEGORY:
        return 1;
    case INST_FAILURE:
        return 0;
    case INST_IN:
        return 1;
    case INST_JUMP:
        return 1;
    case INST_LITERAL:
        return 1;
    case INST_MAX_UNTIL:
        return 0;
    case INST_NEGATE:
        return 0;
    case INST_OFFSET:
        return 0;
    case INST_RANGE:
        return 2;
    case INST_REPEAT:
        return 3;
    case INST_SUCCESS:
        return 0;
    case INST_LABEL:
    default:
        assert(FALSE);
    }

    return 0; /* gcc dislike this function without this statement */
}

static CorgiUInt
get_instruction_size(Instruction* inst)
{
    if (inst->type == INST_LABEL) {
        return 0;
    }
    return 1 + get_operands_number(inst);
}

static CorgiUInt
compute_instruction_position(Instruction* inst)
{
    CorgiUInt pos = 0;
    Instruction* i;
    for (i = inst; i != NULL; i = i->next) {
        i->pos = pos;
        pos += get_instruction_size(i);
    }
    return pos;
}

static void
write_code(CorgiCode** code, Instruction* inst)
{
    switch (inst->type) {
    case INST_BRANCH:
        **code = SRE_OP_BRANCH;
        (*code)++;
        break;
    case INST_CATEGORY:
        **code = SRE_OP_CATEGORY;
        (*code)++;
        **code = inst->u.category.type;
        (*code)++;
        break;
    case INST_FAILURE:
        **code = SRE_OP_FAILURE;
        (*code)++;
        break;
    case INST_IN:
        **code = SRE_OP_IN;
        (*code)++;
        **code = inst->u.in.dest->pos - inst->pos - 1;
        (*code)++;
        break;
    case INST_JUMP:
        **code = SRE_OP_JUMP;
        (*code)++;
        **code = inst->u.jump.dest->pos - inst->pos - 1;
        (*code)++;
        break;
    case INST_LABEL:
        break;
    case INST_LITERAL:
        **code = SRE_OP_LITERAL;
        (*code)++;
        **code = inst->u.literal.c;
        (*code)++;
        break;
    case INST_MAX_UNTIL:
        **code = SRE_OP_MAX_UNTIL;
        (*code)++;
        break;
    case INST_NEGATE:
        **code = SRE_OP_NEGATE;
        (*code)++;
        break;
    case INST_OFFSET:
        **code = inst->u.offset.dest->pos - inst->pos;
        (*code)++;
        break;
    case INST_RANGE:
        **code = SRE_OP_RANGE;
        (*code)++;
        **code = inst->u.range.low;
        (*code)++;
        **code = inst->u.range.high;
        (*code)++;
        break;
    case INST_REPEAT:
        **code = SRE_OP_REPEAT;
        (*code)++;
        **code = inst->u.repeat.dest->pos - inst->pos - 1;
        (*code)++;
        **code = inst->u.repeat.min;
        (*code)++;
        **code = inst->u.repeat.max;
        (*code)++;
        break;
    case INST_SUCCESS:
        **code = SRE_OP_SUCCESS;
        (*code)++;
        break;
    default:
        assert(FALSE);
    }
}

static void
instruction2binary(CorgiCode* code, Instruction* inst)
{
    CorgiCode** p = &code;
    Instruction* i;
    for (i = inst; i != NULL; i = i->next) {
        write_code(p, i);
    }
}

static CorgiStatus
instruction2code(Instruction* inst, CorgiCode** code, CorgiUInt* code_size)
{
    *code_size = compute_instruction_position(inst);
    *code = (CorgiCode*)malloc(sizeof(CorgiCode) * *code_size);
    if (*code == NULL) {
        return ERR_OUT_OF_MEMORY;
    }
    instruction2binary(*code, inst);

    return CORGI_OK;
}

static CorgiStatus
parse_to_instruction(Storage** storage, CorgiChar* begin, CorgiChar* end, Instruction** inst)
{
    Node* node = NULL; /* gcc dislike uninitialized */
    CorgiChar* pc = begin;
    CorgiStatus status = parse_branch(storage, &pc, end, &node);
    if (status != CORGI_OK) {
        return status;
    }
    status = node2instruction(storage, node, inst);
    if (status != CORGI_OK) {
        return status;
    }
    Instruction* success = NULL;
    status = create_instruction(storage, INST_SUCCESS, &success);
    if (status != CORGI_OK) {
        return status;
    }
    get_last_instruction(*inst)->next = success;
    return CORGI_OK;
}

static CorgiStatus
compile_with_storage(Storage** storage, CorgiChar* begin, CorgiChar* end, CorgiCode** code, CorgiUInt* code_size)
{
    Instruction* inst = NULL;
    CorgiStatus status = parse_to_instruction(storage, begin, end, &inst);
    if (status != CORGI_OK) {
        return status;
    }
    return instruction2code(inst, code, code_size);
}

CorgiStatus
corgi_compile(CorgiRegexp* regexp, CorgiChar* begin, CorgiChar* end)
{
    Storage* storage = alloc_storage(NULL);
    CorgiCode* code = NULL;
    CorgiUInt code_size = 0;
    CorgiStatus status = compile_with_storage(&storage, begin, end, &code, &code_size);
    free_storage(storage);
    if (status != CORGI_OK) {
        return status;
    }
    regexp->code = code;
    regexp->code_size = code_size;
    return CORGI_OK;
}

CorgiStatus
corgi_match(CorgiMatch* match, CorgiRegexp* regexp, CorgiChar* begin, CorgiChar* end, CorgiChar* at, CorgiOptions opts)
{
    State state;
    state_init(&state, regexp, begin, end, at, opts & CORGI_OPT_DEBUG);
    CorgiInt ret = sre_match(&state, regexp->code);
    match->begin = state.start - state.beginning;
    match->end = state.ptr - state.beginning;
    state_fini(&state);
    return ret != 0 ? CORGI_OK : 42;
}

static const char*
category_type2name(CorgiCode type)
{
    switch (type) {
    case SRE_CATEGORY_DIGIT:
        return "SRE_CATEGORY_DIGIT";
    case SRE_CATEGORY_NOT_DIGIT:
        return "SRE_CATEGORY_NOT_DIGIT";
    case SRE_CATEGORY_SPACE:
        return "SRE_IS_SPACE";
    case SRE_CATEGORY_NOT_SPACE:
        return "SRE_IS_NOT_SPACE";
    case SRE_CATEGORY_WORD:
        return "SRE_CATEGORY_WORD";
    case SRE_CATEGORY_NOT_WORD:
        return "SRE_CATEGORY_NOT_WORD";
    case SRE_CATEGORY_LINEBREAK:
        return "SRE_CATEGORY_LINEBREAK";
    case SRE_CATEGORY_NOT_LINEBREAK:
        return "SRE_CATEGORY_NOT_LINEBREAK";
    case SRE_CATEGORY_LOC_WORD:
        return "SRE_CATEGORY_LOC_WORD";
    case SRE_CATEGORY_LOC_NOT_WORD:
        return "SRE_CATEGORY_LOC_NOT_WORD";
    case SRE_CATEGORY_UNI_DIGIT:
        return "SRE_CATEGORY_UNI_DIGIT";
    case SRE_CATEGORY_UNI_NOT_DIGIT:
        return "SRE_CATEGORY_UNI_NOT_DIGIT";
    case SRE_CATEGORY_UNI_SPACE:
        return "SRE_CATEGORY_UNI_SPACE";
    case SRE_CATEGORY_UNI_NOT_SPACE:
        return "SRE_CATEGORY_UNI_NOT_SPACE";
    case SRE_CATEGORY_UNI_WORD:
        return "SRE_CATEGORY_UNI_WORD";
    case SRE_CATEGORY_UNI_NOT_WORD:
        return "SRE_CATEGORY_UNI_NOT_WORD";
    case SRE_CATEGORY_UNI_LINEBREAK:
        return "SRE_CATEGORY_UNI_LINEBREAK";
    case SRE_CATEGORY_UNI_NOT_LINEBREAK:
        return "SRE_CATEGORY_UNI_NOT_LINEBREAK";
    default:
        return "UNKNOWN";
    }
}

static void
dump_instruction(Instruction* inst)
{
    if (inst->type == INST_LABEL) {
        return;
    }

    printf("%04u ", inst->pos);
    CorgiChar c;
    CorgiChar low;
    CorgiChar high;
    CorgiCode type;
    switch (inst->type) {
    case INST_BRANCH:
        printf("BRANCH");
        break;
    case INST_CATEGORY:
        type = inst->u.category.type;
        printf("CATEGORY %u (%s)", type, category_type2name(type));
        break;
    case INST_FAILURE:
        printf("FAILURE");
        break;
    case INST_IN:
        printf("IN %u", inst->u.in.dest->pos);
        break;
    case INST_JUMP:
        printf("JUMP %u", inst->u.jump.dest->pos);
        break;
    case INST_LABEL:
        assert(FALSE);
        break;
    case INST_LITERAL:
        c = inst->u.literal.c;
        printf("LITERAL %8u (%c)", c, char2printable(c));
        break;
    case INST_MAX_UNTIL:
        printf("MAX_UNTIL");
        break;
    case INST_NEGATE:
        printf("NEGATE");
        break;
    case INST_OFFSET:
        printf("OFFSET %04u", inst->u.offset.dest->pos);
        break;
    case INST_RANGE:
        low = inst->u.range.low;
        high = inst->u.range.high;
        printf("RANGE %8u (%c) %8u (%c)", low, char2printable(low), high, char2printable(high));
        break;
    case INST_REPEAT:
        printf("REPEAT %04u %5u %5u", inst->u.repeat.dest->pos, inst->u.repeat.min, inst->u.repeat.max);
        break;
    case INST_SUCCESS:
        printf("SUCCESS");
        break;
    default:
        printf("UNKNOWN");
        break;
    }
    printf("\n");
}

static CorgiStatus
dump_with_storage(Storage** storage, CorgiChar* begin, CorgiChar* end)
{
    Instruction* inst = NULL;
    CorgiStatus status = parse_to_instruction(storage, begin, end, &inst);
    if (status != CORGI_OK) {
        return status;
    }
    compute_instruction_position(inst);
    Instruction* i;
    for (i = inst; i != NULL; i = i->next) {
        dump_instruction(i);
    }
    return CORGI_OK;
}

CorgiStatus
corgi_dump(CorgiChar* begin, CorgiChar* end)
{
    Storage* storage = alloc_storage(NULL);
    CorgiStatus status = dump_with_storage(&storage, begin, end);
    free_storage(storage);
    return status;
}

static void
disassemble_operand(int pos, CorgiCode operand)
{
    const char* name;
    switch (operand) {
    case SRE_OP_FAILURE:
        name = "FAILURE";
        break;
    case SRE_OP_SUCCESS:
        name = "SUCCESS";
        break;
    case SRE_OP_ANY:
        name = "ANY";
        break;
    case SRE_OP_ANY_ALL:
        name = "ANY_ALL";
        break;
    case SRE_OP_ASSERT:
        name = "ASSERT";
        break;
    case SRE_OP_ASSERT_NOT:
        name = "ASSERT_NOT";
        break;
    case SRE_OP_AT:
        name = "AT";
        break;
    case SRE_OP_BRANCH:
        name = "BRANCH";
        break;
    case SRE_OP_CALL:
        name = "CALL";
        break;
    case SRE_OP_CATEGORY:
        name = "CATEGORY";
        break;
    case SRE_OP_CHARSET:
        name = "CHARSET";
        break;
    case SRE_OP_BIGCHARSET:
        name = "BIGCHARSET";
        break;
    case SRE_OP_GROUPREF:
        name = "GROUPREF";
        break;
    case SRE_OP_GROUPREF_EXISTS:
        name = "GROUPREF_EXISTS";
        break;
    case SRE_OP_GROUPREF_IGNORE:
        name = "GROUPREF_IGNORE";
        break;
    case SRE_OP_IN:
        name = "IN";
        break;
    case SRE_OP_IN_IGNORE:
        name = "IN_IGNORE";
        break;
    case SRE_OP_INFO:
        name = "INFO";
        break;
    case SRE_OP_JUMP:
        name = "JUMP";
        break;
    case SRE_OP_LITERAL:
        name = "LITERAL";
        break;
    case SRE_OP_LITERAL_IGNORE:
        name = "LITERAL_IGNORE";
        break;
    case SRE_OP_MARK:
        name = "MARK";
        break;
    case SRE_OP_MAX_UNTIL:
        name = "MAX_UNTIL";
        break;
    case SRE_OP_MIN_UNTIL:
        name = "MIN_UNTIL";
        break;
    case SRE_OP_NOT_LITERAL:
        name = "NOT_LITERAL";
        break;
    case SRE_OP_NOT_LITERAL_IGNORE:
        name = "NOT_LITERAL_IGNORE";
        break;
    case SRE_OP_NEGATE:
        name = "NEGATE";
        break;
    case SRE_OP_RANGE:
        name = "RANGE";
        break;
    case SRE_OP_REPEAT:
        name = "REPEAT";
        break;
    case SRE_OP_REPEAT_ONE:
        name = "REPEAT_ONE";
        break;
    case SRE_OP_SUBPATTERN:
        name = "SUBPATTERN";
        break;
    case SRE_OP_MIN_REPEAT_ONE:
        name = "MIN_REPEAT_ONE";
        break;
    default:
        name = "UNKNOWN";
        break;
    }
    printf("%04u %s ", pos, name);
}

static void disassemble_code(CorgiCode**, CorgiCode*);

static void
disassemble_pattern(CorgiCode** p, CorgiCode* base, CorgiCode* end)
{
    while (*p < end) {
        disassemble_code(p, base);
    }
}

static void
disassemble_branch(CorgiCode** p, CorgiCode* base)
{
    while (**p != 0) {
        CorgiCode offset = **p;
        CorgiCode* end = *p + offset;
        printf("%04u (offset) %u\n", *p - base, offset);
        (*p)++;
        disassemble_pattern(p, base, end);
    }
}

static void
disassemble_code(CorgiCode** p, CorgiCode* base)
{
    CorgiCode operand = **p;
    disassemble_operand(*p - base, operand);
    (*p)++;

    CorgiCode offset;
    CorgiCode* end;
    CorgiCode c;
    switch (operand) {
    case SRE_OP_FAILURE:
    case SRE_OP_SUCCESS:
        printf("\n");
        break;
    case SRE_OP_ANY:
    case SRE_OP_ANY_ALL:
        break;
    case SRE_OP_ASSERT:
    case SRE_OP_ASSERT_NOT:
        offset = **p;
        end = *p + offset;
        printf("%u ", offset);
        (*p)++;
        printf("%u\n", **p);
        (*p)++;
        disassemble_pattern(p, base, end);
        break;
    case SRE_OP_AT:
        printf("%u\n", **p);
        (*p)++;
        break;
    case SRE_OP_BRANCH:
        printf("\n");
        disassemble_branch(p, base);
        break;
    case SRE_OP_CALL:
        offset = **p;
        end = *p + offset;
        printf("%u\n", offset);
        (*p)++;
        disassemble_pattern(p, base, end);
        break;
    case SRE_OP_CATEGORY:
        printf("%u (%s)\n", **p, category_type2name(**p));
        (*p)++;
        break;
    case SRE_OP_CHARSET:
    case SRE_OP_BIGCHARSET:
        printf("\n");
        break;
    case SRE_OP_GROUPREF:
    case SRE_OP_GROUPREF_IGNORE:
        printf("%u\n", **p);
        (*p)++;
        break;
    case SRE_OP_GROUPREF_EXISTS:
        printf("%u ", **p);
        (*p)++;
        printf("%u ", **p);
        (*p)++;
        offset = **p;
        end = *p + offset;
        printf("%u\n", offset);
        (*p)++;
        disassemble_pattern(p, base, end);
        break;
    case SRE_OP_IN:
    case SRE_OP_IN_IGNORE:
        offset = **p;
        end = *p + offset;
        printf("%u\n", offset);
        (*p)++;
        disassemble_pattern(p, base, end);
        break;
    case SRE_OP_INFO:
        offset = **p;
        end = *p + offset;
        printf("%u\n", offset);
        printf("...(snip)...\n");
        *p = end;
        break;
    case SRE_OP_JUMP:
        offset = **p;
        printf("%u\n", offset);
        (*p)++;
        break;
    case SRE_OP_LITERAL:
    case SRE_OP_LITERAL_IGNORE:
    case SRE_OP_NOT_LITERAL:
    case SRE_OP_NOT_LITERAL_IGNORE:
        c = **p;
        printf("%8u (%c)\n", c, isprint(c) ? c : ' ');
        (*p)++;
        break;
    case SRE_OP_MARK:
        printf("%u\n", **p);
        (*p)++;
        break;
    case SRE_OP_MAX_UNTIL:
    case SRE_OP_MIN_UNTIL:
        printf("\n");
        break;
    case SRE_OP_NEGATE:
        printf("\n");
        break;
    case SRE_OP_RANGE:
        printf("%u ", **p);
        (*p)++;
        printf("%u\n", **p);
        (*p)++;
        break;
    case SRE_OP_REPEAT:
    case SRE_OP_REPEAT_ONE:
    case SRE_OP_MIN_REPEAT_ONE:
        offset = **p;
        end = *p + offset;
        printf("%u ", offset);
        (*p)++;
        printf("%u ", **p);
        (*p)++;
        printf("%u\n", **p);
        (*p)++;
        disassemble_pattern(p, base, end);
        break;
    case SRE_OP_SUBPATTERN:
    default:
        printf("\n");
        break;
    }
}

CorgiStatus
corgi_disassemble(CorgiRegexp* regexp)
{
    CorgiCode* begin = regexp->code;
    disassemble_pattern(&begin, begin, begin + regexp->code_size);
    return CORGI_OK;
}

CorgiStatus
corgi_search(CorgiMatch* match, CorgiRegexp* regexp, CorgiChar* begin, CorgiChar* end, CorgiChar* at, CorgiOptions opts)
{
    State state;
    state_init(&state, regexp, begin, end, at, opts & CORGI_OPT_DEBUG);
    CorgiInt ret = sre_search(&state, regexp->code);
    match->begin = state.start - state.beginning;
    match->end = state.ptr - state.beginning;
    state_fini(&state);
    return ret != 0 ? CORGI_OK : 42;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
