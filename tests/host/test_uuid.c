/* Host tests for ezlopi-util-uuid.
 *
 * This component needs no stubs at all -- not the UART HAL headers, not
 * EZLOPI_USER_CONFIG.h, nothing. ezlopi_util_uuid.c includes <stdlib.h>,
 * <time.h> and its own header, and the header includes nothing. It is the
 * cheapest rule in this Makefile, and it is worth stating why that matters:
 * the component was unreachable from any test for no reason other than nobody
 * having written one. Four defects had been sitting in
 * ezlopi_util_uuid_parse() the whole time, one of them a read past the end of
 * the input buffer.
 *
 * None of the four was reachable on a device, because nothing in the
 * repository calls any function in this component -- it compiles into the
 * image as dead code. That is the honest severity: these are tests for a
 * library nobody uses yet, written so that the first caller does not inherit
 * the defects. It is not a field bug fix and should not be read as one.
 *
 * The parse tests are the substance. generate_random() is tested only for the
 * RFC 4122 version and variant bits it sets, not for randomness: it is seeded
 * from time(NULL) via srand(), which is not a property a unit test should
 * pretend to check, and is noted as a weakness at the bottom of this file
 * rather than asserted about.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ezlopi_util_uuid.h"

static int g_checks;
static int g_fails;

static void ok(int cond, const char *what)
{
    g_checks++;
    if (!cond)
    {
        g_fails++;
        printf("  FAIL: %s\n", what);
    }
}

static void hexdump(const char *label, const ezlopi_uuid_t u)
{
    printf("    %-14s", label);
    for (int i = 0; i < 16; i++)
    {
        printf("%02x", u[i]);
    }
    printf("\n");
}

/* The reference value used throughout, and its canonical string. */
static const ezlopi_uuid_t REF = {
    0x55, 0x0e, 0x84, 0x00, 0xe2, 0x9b, 0x41, 0xd4,
    0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00};
static const char *REF_STR = "550e8400-e29b-41d4-a716-446655440000";

static void expect_parse(const char *in, const ezlopi_uuid_t want, const char *what)
{
    ezlopi_uuid_t got;
    memset(got, 0xcd, sizeof got); /* poison, so "untouched" cannot pass */
    ezlopi_util_uuid_parse(in, got);
    int same = (0 == memcmp(got, want, 16));
    ok(same, what);
    if (!same)
    {
        printf("    input: %s\n", in ? in : "(NULL)");
        hexdump("got:", got);
        hexdump("want:", want);
    }
}

/* ------------------------------------------------------------------ */

static void test_unparse(void)
{
    printf("unparse(): canonical form\n");

    char out[EZLOPI_UUID_STR_SIZE];
    memset(out, 0xcd, sizeof out);
    ezlopi_util_uuid_unparse(REF, out);

    ok(0 == strcmp(out, REF_STR), "unparse produces the canonical string");
    ok(36 == strlen(out), "unparse writes exactly 36 characters");
    ok('\0' == out[36], "unparse terminates at index 36");

    /* The hyphens belong after bytes 4, 6, 8 and 10, i.e. at character
       offsets 8, 13, 18 and 23. Asserted positionally so a change to the
       grouping cannot pass by accident. */
    ok('-' == out[8] && '-' == out[13] && '-' == out[18] && '-' == out[23],
       "hyphens at offsets 8, 13, 18, 23");
    int hyphens = 0;
    for (int i = 0; i < 36; i++)
    {
        if ('-' == out[i])
        {
            hyphens++;
        }
    }
    ok(4 == hyphens, "exactly four hyphens");

    /* Lowercase, as documented. 0xe2 would expose an uppercase table. */
    ok(NULL == strpbrk(out, "ABCDEF"), "unparse emits lowercase hex");
}

static void test_parse_roundtrip(void)
{
    printf("parse(): round trip through unparse\n");

    /* This is the case that used to fail. unparse() emits the hyphenated
       form; parse() advanced by two on the hyphen, eating it and the digit
       after it, so the two halves of this component did not agree. The result
       was 550e840029ff41d471ff446655440000 -- wrong from byte [4], which is
       where the first hyphen sits. */
    char text[EZLOPI_UUID_STR_SIZE];
    ezlopi_util_uuid_unparse(REF, text);
    expect_parse(text, REF, "parse(unparse(x)) == x");

    /* Every byte, not just one fixed value: a nibble-level misalignment is
       invisible for a UUID whose adjacent nibbles happen to repeat. That is
       not hypothetical -- running these tests against the old implementation
       failed 240 of the 256 patterns below and PASSED 16 of them, so roughly
       one value in sixteen would have given a false green had this been a
       single hard-coded case. */
    for (int b = 0; b < 256; b++)
    {
        ezlopi_uuid_t u;
        for (int i = 0; i < 16; i++)
        {
            u[i] = (unsigned char)((b + i * 17) & 0xff);
        }
        char s[EZLOPI_UUID_STR_SIZE];
        ezlopi_util_uuid_unparse(u, s);

        ezlopi_uuid_t back;
        memset(back, 0xcd, sizeof back);
        ezlopi_util_uuid_parse(s, back);
        ok(0 == memcmp(u, back, 16), "round trip holds for a swept byte pattern");
    }
}

static void test_parse_hyphens(void)
{
    printf("parse(): hyphen handling\n");

    expect_parse(REF_STR, REF, "canonical hyphenated form");
    expect_parse("550e8400e29b41d4a716446655440000", REF, "32 bare hex digits");

    /* Hyphens are skipped wherever they fall, which is the documented
       behaviour. These are not canonical UUIDs; they pin down that the
       skipping is positional-independent rather than hard-coded to offsets
       8/13/18/23, because the implementation does not special-case them. */
    expect_parse("550e-8400-e29b-41d4-a716-4466-5544-0000", REF,
                 "hyphens at non-canonical positions");
    expect_parse("-550e8400e29b41d4a716446655440000", REF, "leading hyphen");
    expect_parse("550e8400e29b41d4a716446655440000-", REF, "trailing hyphen");
    expect_parse("550e8400--e29b41d4a716446655440000", REF, "doubled hyphen");
}

static void test_parse_case(void)
{
    printf("parse(): case insensitivity (RFC 4122 s3)\n");

    /* Both of these used to fail, and the hyphenless one is what proved the
       lowercase-only table was a defect in its own right rather than a
       knock-on of the hyphen bug. */
    expect_parse("550E8400-E29B-41D4-A716-446655440000", REF, "uppercase, hyphenated");
    expect_parse("550E8400E29B41D4A716446655440000", REF, "uppercase, bare");
    expect_parse("550E8400-e29b-41D4-a716-446655440000", REF, "mixed case");
}

static void test_parse_malformed(void)
{
    printf("parse(): short, malformed and NULL input\n");

    static const ezlopi_uuid_t NIL = {0};

    /* The over-read. The old loop was bounded only by i < 36 while reading
       in[i + 1], so it walked past the terminator; a 9-byte heap buffer gave
       ASan "heap-buffer-overflow READ of size 1 ... 0 bytes after 9-byte
       region". Under the sanitizers this suite already enables, that is a
       hard failure rather than a wrong answer, which is the point of running
       these here.

       This comment first claimed a string literal would not catch the
       over-read, on the assumption that it would land in adjacent rodata and
       be silent. Running the old implementation against these tests disproved
       that: ASan instruments globals as well as allocations, and reported
       "global-buffer-overflow ... 0 bytes after global '*.LC45' ... of size
       9" -- LC45 being the literal "550e8400" below. So the first case here
       is already enough to abort the old code, and the heap cases further
       down are belt and braces rather than the only way to see it. */
    expect_parse("550e8400", NIL, "8 digits -> nil, no over-read");
    expect_parse("", NIL, "empty string -> nil");
    expect_parse("5", NIL, "one digit, no second byte to read");
    expect_parse("-", NIL, "a lone hyphen");
    expect_parse("550e8400-e29b-41d4-a716-4466554400", NIL,
                 "two digits short of a full uuid");
    expect_parse(NULL, NIL, "NULL input is not dereferenced");

    /* Invalid characters. __hex2dec() returns -1 for these, and that -1 used
       to be shifted left -- UndefinedBehaviorSanitizer reported "left shift
       of negative value -1" -- and then stored as 0xff, so garbage input
       produced plausible-looking bytes. */
    expect_parse("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", NIL, "all invalid characters");
    expect_parse("550e8400-e29b-41d4-a716-44665544000g", NIL,
                 "one invalid character in the last byte");
    expect_parse("550e8400 e29b 41d4 a716 446655440000", NIL,
                 "spaces instead of hyphens are rejected, not skipped");

    /* Over-length input must stop at 16 bytes and not keep writing. The
       poison byte after the array would be overwritten if it did; ASan would
       catch a stack overflow here regardless. */
    ezlopi_uuid_t over;
    memset(over, 0xcd, sizeof over);
    ezlopi_util_uuid_parse("550e8400e29b41d4a716446655440000ffffffffffffffff", over);
    ok(0 == memcmp(over, REF, 16), "trailing digits beyond 16 bytes are ignored");
}

static void test_parse_heap_bounded(void)
{
    printf("parse(): exact-sized heap buffer, for ASan redzones\n");

    /* The one case that needs the heap. ASan puts redzones around a malloc
       but not around a string literal, so this is the arrangement that turned
       the old over-read from a silently wrong answer into a hard error. Sizes
       chosen to stop mid-byte and just after a hyphen, the two places the old
       arithmetic could step past the end. */
    static const char *frag[] = {
        "5", "55", "550", "550e", "550e8", "550e8400",
        "550e8400-", "550e8400-e", "550e8400-e29b-41d4-a716-44665544000",
    };

    for (size_t k = 0; k < sizeof frag / sizeof frag[0]; k++)
    {
        size_t n = strlen(frag[k]) + 1;
        char *buf = malloc(n);
        ok(NULL != buf, "allocation for the bounded-buffer case");
        if (NULL == buf)
        {
            continue;
        }
        memcpy(buf, frag[k], n);

        ezlopi_uuid_t u;
        memset(u, 0xcd, sizeof u);
        ezlopi_util_uuid_parse(buf, u);

        /* The assertion that matters is that we reach this line at all: ASan
           aborts the process on the old code. The value is checked too --
           every one of these is incomplete, so all must give the nil UUID. */
        static const ezlopi_uuid_t NIL = {0};
        int nil = (0 == memcmp(u, NIL, 16));
        ok(nil, "incomplete input on an exact-sized buffer gives nil");
        if (!nil)
        {
            printf("    fragment: \"%s\"\n", frag[k]);
            hexdump("got:", u);
        }
        free(buf);
    }
}

static void test_generate_and_copy(void)
{
    printf("generate_random() bit fields, and copy()\n");

    /* Not a randomness test. srand(time(NULL)) is seeded once per process at
       one-second resolution, so two devices powering up together can produce
       the same sequence -- noted at the bottom of this file, not asserted
       here. What is checkable is that the version and variant nibbles RFC
       4122 s4.4 requires are actually set, every time. */
    for (int t = 0; t < 64; t++)
    {
        ezlopi_uuid_t u;
        memset(u, 0, sizeof u);
        ezlopi_util_uuid_generate_random(u);
        ok(0x40 == (u[6] & 0xf0), "version nibble is 4");
        ok(0x80 == (u[8] & 0xc0), "variant bits are 10x (RFC 4122)");
    }

    /* The generated form must survive this component's own round trip, which
       ties the three functions together rather than testing each alone. */
    ezlopi_uuid_t gen;
    ezlopi_util_uuid_generate_random(gen);
    char s[EZLOPI_UUID_STR_SIZE];
    ezlopi_util_uuid_unparse(gen, s);
    ezlopi_uuid_t back;
    ezlopi_util_uuid_parse(s, back);
    ok(0 == memcmp(gen, back, 16), "a generated uuid round-trips");

    /* copy() is trivial, but it had a signed/unsigned comparison that -Werror
       rejects, so it is exercised to justify having touched it. */
    ezlopi_uuid_t dst;
    memset(dst, 0xcd, sizeof dst);
    ezlopi_util_uuid_copy(dst, REF);
    ok(0 == memcmp(dst, REF, 16), "copy duplicates all 16 bytes");

    /* The uuid_generate() convenience macro in the header must still reach
       the same function; it is otherwise never compiled anywhere. */
    ezlopi_uuid_t viamacro;
    memset(viamacro, 0, sizeof viamacro);
    uuid_generate(viamacro);
    ok(0x40 == (viamacro[6] & 0xf0), "the uuid_generate() macro still compiles and works");
}

int main(void)
{
    printf("=== ezlopi-util-uuid ===\n");

    test_unparse();
    test_parse_roundtrip();
    test_parse_hyphens();
    test_parse_case();
    test_parse_malformed();
    test_parse_heap_bounded();
    test_generate_and_copy();

    /* Same wording as the other eight suites, so that one grep over the whole
       run totals them. */
    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);

    /* Deliberately not tested, so that nobody reads a green run as meaning
       more than it does:

         - Randomness. generate_random() calls srand(time(NULL)) once and then
           rand(). On ESP-IDF that is a weak PRNG seeded at one-second
           resolution, so two devices booting in the same second produce
           identical UUIDs. esp_random() / esp_fill_random() is the hardware
           RNG and is what this should use. Not changed here because doing so
           adds an ESP-IDF dependency to the one component in this suite that
           currently has none, which would cost it its host tests -- the fix
           is a seam, not a one-liner, and it belongs in its own commit.

         - Whether any of this is reachable on a device. It is not. Nothing in
           the repository calls these functions. */
    if (g_fails)
    {
        printf("UUID TESTS FAILED\n");
        return 1;
    }
    printf("UUID TESTS PASSED\n");
    return 0;
}
