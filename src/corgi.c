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

#if 0
#   define VERBOSE
#endif
#if defined(VERBOSE)
#   define TRACE(v) printf v
#else
#   define TRACE(v)
#endif

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
                    printf("%s:%u\n", __FILE__, __LINE__);
                    continue;
                }
                if ((ctx->pattern[1] == SRE_OP_IN) && ((end <= ctx->ptr) || !sre_charset(ctx->pattern + 3, *ctx->ptr))) {
                    printf("%s:%u\n", __FILE__, __LINE__);
                    continue;
                }
                state->ptr = ctx->ptr;
                DO_JUMP(JUMP_BRANCH, jump_branch, ctx->pattern + 1);
                if (ret) {
                    printf("%s:%u ret=%u\n", __FILE__, __LINE__, ret);
                    if (ctx->u.rep) {
                        printf("%s:%u\n", __FILE__, __LINE__);
                        MARK_POP_DISCARD(ctx->lastmark);
                    }
                    printf("%s:%u\n", __FILE__, __LINE__);
                    RETURN_ON_ERROR(ret);
                    printf("%s:%u\n", __FILE__, __LINE__);
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

#if 0
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
        end = (CorgiChar*)state->end;
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
        end = (CorgiChar*)state->end;
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
    } else
        /* general case */
        while (ptr <= end) {
            TRACE(("|%p|%p|SEARCH\n", pattern, ptr));
            state->start = state->ptr = ptr++;
            status = sre_match(state, pattern);
            if (status != 0) {
                break;
            }
        }

    return status;
}
#endif

static void
state_init(State* state, CorgiRegexp* regexp, CorgiChar* begin, CorgiChar* end, CorgiChar* at)
{
    memset(state, 0, sizeof(State));
    state->lastmark = -1;
    state->lastindex = -1;
    state->beginning = begin;
    state->start = at;
    state->end = end;
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
    NODE_LITERAL,
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
            CorgiChar c;
        } literal;
    } u;
    struct Node* next;
};

typedef struct Node Node;

static CorgiStatus
create_node(Storage** storage, NodeType type, Node** node)
{
    *node = alloc_from_storage(storage, sizeof(Node));
    if (*node == NULL) {
        return CORGI_ERR_OUT_OF_MEMORY;
    }
    bzero(*node, sizeof(Node));
    (*node)->type = type;
    (*node)->next = NULL;
    return CORGI_OK;
}

static CorgiStatus
parse_single_pattern(Storage** storage, CorgiChar** pc, CorgiChar* end, Node** node)
{
    CorgiStatus status = create_node(storage, NODE_LITERAL, node);
    if (status != CORGI_OK) {
        return status;
    }
    (*node)->u.literal.c = **pc;
    (*pc)++;
    return CORGI_OK;
}

static CorgiStatus
parse_sub_pattern(Storage** storage, CorgiChar** pc, CorgiChar* end, Node** node)
{
    CorgiStatus status = parse_single_pattern(storage, pc, end, node);
    if (status != CORGI_OK) {
        return status;
    }

    Node* prev = *node;
    while ((*pc < end) && (status == CORGI_OK)) {
        Node* node = NULL;
        status = parse_single_pattern(storage, pc, end, &node);
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

static CorgiStatus
node2code(Storage** storage, Node* node, CorgiCode** code)
{
    return CORGI_OK;
}

static CorgiStatus
compile_with_storage(Storage** storage, CorgiRegexp* regexp, CorgiChar* begin, CorgiChar* end, CorgiCode** code)
{
    Node* node = NULL; /* gcc dislike uninitialized */
    CorgiChar* pc = begin;
    CorgiStatus status = parse_branch(storage, &pc, end, &node);
    if (status != CORGI_OK) {
        return status;
    }
    return node2code(storage, node, code);
}

CorgiStatus
corgi_compile(CorgiRegexp* regexp, CorgiChar* begin, CorgiChar* end)
{
    Storage* storage = alloc_storage(NULL);
    CorgiCode* code = NULL;
    CorgiStatus status = compile_with_storage(&storage, regexp, begin, end, &code);
    free_storage(storage);
    if (status != CORGI_OK) {
        return status;
    }
    regexp->code = code;
    return CORGI_OK;
}

CorgiStatus
corgi_match(CorgiMatch* match, CorgiRegexp* regexp, CorgiChar* begin, CorgiChar* end, CorgiChar* at)
{
    State state;
    state_init(&state, regexp, begin, end, at);
    CorgiInt ret = sre_match(&state, regexp->code);
    state_fini(&state);
    return ret == 0 ? CORGI_OK : 42;
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
