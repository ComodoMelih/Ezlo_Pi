/* Host tests for ezlopi-util-uuid.
 *
 * This component needed no stubs at all when these tests were written -- not
 * the UART HAL headers, not EZLOPI_USER_CONFIG.h, nothing. That was the
 * cheapest rule in this Makefile, and it was worth stating why it mattered:
 * the component was unreachable from any test for no reason other than nobody
 * having written one. Four defects had been sitting in
 * ezlopi_util_uuid_parse() the whole time, one of them a read past the end of
 * the input buffer.
 *
 * It now takes exactly one stub, stubs-uuid/esp_random.h, because
 * generate_random() was changed to draw from the hardware RNG instead of
 * srand(time(NULL)) plus rand(). The stub is a faithful copy of the real
 * header's two declarations, so the translation unit under test is the same
 * one the firmware compiles; only the entropy source behind it differs. The
 * alternative -- an #ifdef ESP_PLATFORM in the component -- would have meant
 * testing code the firmware does not run.
 *
 * None of the four parse defects was reachable on a device, because nothing in
 * the repository calls any function in this component. Earlier versions of
 * this comment added "it compiles into the image as dead code", which is
 * wrong, and the RNG change is what measured it: all four targets produced
 * images byte-for-byte identical in size to the build before it, and `nm` on
 * the esp32c3 ELF finds no ezlopi_util_uuid_* symbol at all. The component is
 * compiled and then discarded at link. So it costs nothing in flash, and a
 * defect in it cannot be reached even in principle.
 *
 * That is the honest severity: these are tests for a library nobody uses yet,
 * written so that the first caller does not inherit the defects. It is not a
 * field bug fix and should not be read as one. The same applies to the RNG
 * change -- a weak UUID generator that is not in the image has no victim.
 *
 * The parse tests are the substance. For generate_random() a unit test still
 * cannot check randomness, but since the entropy now arrives through a seam
 * the test controls, it can check the thing that actually went wrong: that all
 * sixteen bytes come from that source and only the two RFC 4122 nibbles are
 * overwritten afterwards.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ezlopi_util_uuid.h"
#include "stub_random.h"

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

    /* Not a randomness test; see test_generate_uses_the_rng_for_every_byte()
       below for what is actually checkable. What this one covers is that the
       version and variant nibbles RFC 4122 s4.4 requires are set every time,
       across enough calls that a value-dependent path would show up. */
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

/* The defect this guards against is not "the UUID looks non-random" -- no
   assertion can see that -- it is that only some of the bytes came from the
   entropy source. The old loop ran `i < 16 / RAND_LENGTH` and filled
   `out[i * RAND_LENGTH + j]`, so the count of bytes written depended on a
   macro; had RAND_LENGTH been 3 or 5, four or one bytes of the UUID would have
   been whatever the caller's stack already held, and every existing assertion
   above would still have passed. With the entropy arriving through a stub the
   test controls, "all sixteen, from the RNG, in order" becomes checkable.

   Asserting the exact buffer rather than a property also pins down the
   reverse: that generate_random() does not post-process the bytes beyond the
   two nibbles RFC 4122 requires. */
static void test_generate_uses_the_rng_for_every_byte(void)
{
    printf("generate_random() takes all 16 bytes from the hardware RNG\n");

    ezlopi_uuid_t u;
    unsigned char want[16];

    /* Poison, so a byte the generator never writes cannot coincidentally
       match the value the stub would have supplied. */
    memset(u, 0xee, sizeof u);

    stub_random_reset();
    ezlopi_util_uuid_generate_random(u);

    ok(1 == g_stub_random_calls, "the RNG is consulted exactly once");
    ok(16 == g_stub_random_bytes, "exactly 16 bytes are drawn from the RNG");

    for (size_t i = 0; i < sizeof want; i++)
    {
        want[i] = stub_random_byte_at(i);
    }
    /* The only two bytes the function is allowed to alter afterwards. */
    want[6] = (unsigned char)((want[6] & 0x0f) | 0x40);
    want[8] = (unsigned char)((want[8] & 0x0f) | 0xa0);

    int same = (0 == memcmp(u, want, sizeof want));
    ok(same, "every byte is the RNG's, except the version and variant nibbles");
    if (!same)
    {
        hexdump("got:", u);
        hexdump("want:", want);
    }

    /* The low nibbles of bytes 6 and 8 must survive, which is what
       distinguishes `(x & 0x0f) | 0x40` from an outright assignment -- an
       assignment would also pass both bit-field checks above. */
    ok((stub_random_byte_at(6) & 0x0f) == (u[6] & 0x0f),
       "byte 6 keeps its random low nibble");
    ok((stub_random_byte_at(8) & 0x0f) == (u[8] & 0x0f),
       "byte 8 keeps its random low nibble");

    /* Successive calls must not repeat. Trivially true of a counter, but it
       would also catch a generator that filled the buffer once and cached. */
    ezlopi_uuid_t a, b;
    ezlopi_util_uuid_generate_random(a);
    ezlopi_util_uuid_generate_random(b);
    ok(0 != memcmp(a, b, sizeof a), "two calls do not return the same bytes");
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
    test_generate_uses_the_rng_for_every_byte();

    /* Same wording as the other eight suites, so that one grep over the whole
       run totals them. */
    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);

    /* Deliberately not tested, so that nobody reads a green run as meaning
       more than it does:

         - Randomness itself. The generator now calls esp_fill_random(), and
           the tests above check that it takes all sixteen bytes from there and
           alters only the two RFC 4122 nibbles. They say nothing about the
           quality of those bytes, because on the host they come from a counter
           in stubs-uuid/stub_random.c. The real guarantee is ESP-IDF's, and it
           is conditional: esp_random() is a true RNG when Wi-Fi or Bluetooth
           is up, and otherwise has prerequisites in the "Random Number
           Generation" section of the programming guide. Nothing here, and
           nothing in the component, checks that the radio is on.

         - Whether any of this is reachable on a device. It is not. Nothing in
           the repository calls these functions, so the weak generator these
           tests replaced had no victim and the strong one has no beneficiary
           yet. */
    if (g_fails)
    {
        printf("UUID TESTS FAILED\n");
        return 1;
    }
    printf("UUID TESTS PASSED\n");
    return 0;
}
