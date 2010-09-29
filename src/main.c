#include <alloca.h>
#include <getopt.h>
#include <stdio.h>

#include <corgi.h>

static void
usage()
{
    printf("test-corgi [-h|--help] <regexp> <string>\n");
}

static int
get_char_bytes(const char* s)
{
#define RETURN_IF_MATCH(x, v) do { \
    if ((s[0] & (x)) == (x)) { \
        return (v); \
    } \
} while (0)
    RETURN_IF_MATCH(0xfc, 6);
    RETURN_IF_MATCH(0xf8, 5);
    RETURN_IF_MATCH(0xf0, 4);
    RETURN_IF_MATCH(0xe0, 3);
    RETURN_IF_MATCH(0xc0, 2);
#undef RETURN_IF_MATCH
    return 1;
}

static CorgiChar
conv_char(const char* s)
{
#define GET_1_BIT(x)    ((x) & 0x01)
#define GET_2_BITS(x)   ((x) & 0x03)
#define GET_3_BITS(x)   ((x) & 0x07)
#define GET_4_BITS(x)   ((x) & 0x0f)
#define GET_5_BITS(x)   ((x) & 0x1f)
#define GET_6_BITS(x)   ((x) & 0x3f)
    switch (get_char_bytes(s)) {
    case 1:
        return s[0];
    case 2:
        return (GET_5_BITS(s[0]) << 6) + GET_6_BITS(s[1]);
    case 3:
        return (GET_4_BITS(s[0]) << 12) + (GET_6_BITS(s[1]) << 6) + GET_6_BITS(s[2]);
    case 4:
        return (GET_3_BITS(s[0]) << 18) + (GET_6_BITS(s[1]) << 12) + (GET_6_BITS(s[2]) << 12) + GET_6_BITS(s[3]);
    case 5:
        return (GET_2_BITS(s[0]) << 24) + (GET_6_BITS(s[1]) << 18) + (GET_6_BITS(s[2]) << 12) + (GET_6_BITS(s[3]) << 6) + GET_6_BITS(s[4]);
    case 6:
        return (GET_1_BIT(s[0]) << 30) + (GET_2_BITS(s[1]) << 24) + (GET_6_BITS(s[2]) << 18) + (GET_6_BITS(s[3]) << 12) + (GET_6_BITS(s[4]) << 6) + GET_6_BITS(s[5]);
    default:
        return '?';
    }
#undef GET_6_BITS
#undef GET_5_BITS
#undef GET_4_BITS
#undef GET_3_BITS
#undef GET_2_BITS
#undef GET_1_BIT
}

static void
conv_utf8_to_utf32(CorgiChar* dest, const char* src)
{
    const char* pc;
    CorgiChar* q = dest;
    for (pc = src; *pc != '\0'; pc += get_char_bytes(pc)) {
        *q = conv_char(pc);
        q++;
    }
}

static int
count_chars(const char* s)
{
    int n = 0;
    const char* pc;
    for (pc = s; *pc != '\0'; pc += get_char_bytes(pc)) {
        n++;
    }
    return n;
}

static int
do_with_regexp(CorgiRegexp* regexp, const char* s, const char* t)
{
    int pattern_size = count_chars(s);
    CorgiChar* pattern = alloca(sizeof(CorgiChar) * pattern_size);
    conv_utf8_to_utf32(pattern, s);
    if (corgi_compile(regexp, pattern, pattern + pattern_size) != CORGI_OK) {
        return 1;
    }

    int target_size = count_chars(t);
    CorgiChar* target = alloca(sizeof(CorgiChar) * target_size);
    conv_utf8_to_utf32(target, t);
    CorgiMatch match;
    corgi_init_match(&match);
    CorgiChar* end = target + target_size;
    CorgiStatus status = corgi_match(&match, regexp, target, end, target);
    corgi_fini_match(&match);

    return status == CORGI_OK ? 0 : 1;
}

static int
corgi_main(int argc, char* argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }
    CorgiRegexp regexp;
    corgi_init_regexp(&regexp);
    int ret = do_with_regexp(&regexp, argv[0], argv[1]);
    corgi_fini_regexp(&regexp);
    return ret;
}

int
main(int argc, char* argv[])
{
    struct option longopts[] = {
        { "help", no_argument, NULL, 'h' },
        { 0, 0, 0, 0 },
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage();
            return 0;
        case '?':
        default:
            usage();
            return 1;
        }
    }
    return corgi_main(argc - optind, argv + optind);
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
