/* Host tests for the MB1013 range-frame parser.
 *
 * The driver reads a buffer whose length is chosen by whatever is on the other
 * end of the UART line, so the two things worth asserting are that no byte
 * outside output_len is ever touched, and that a well-formed frame still
 * decodes to exactly the value the old code produced.
 *
 * Both need ASan to mean anything, and the first needs allocations that are
 * exactly as long as the input. A short frame copied into a 64-byte scratch
 * array would let an over-read land in the same array and pass; the frames
 * here are malloc'd at their true length, so the byte after the last one is
 * poisoned and a one-byte over-read is a hard failure.
 *
 * The oracle for well-formed payloads is atoi() on a NUL-terminated copy --
 * which is what the driver used to call. That is deliberate: the point of the
 * rewrite was to stop calling a string function on unterminated memory, not to
 * change what a real frame decodes to, and comparing against atoi on a
 * *terminated* string is how that stays honest.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "mb1013_parse.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what);
    }
}

/* Run the parser against an exact-size heap copy of n bytes. */
static bool parse_exact(const void *bytes, size_t n, float *out)
{
    uint8_t *heap = NULL;
    if (n > 0)
    {
        heap = malloc(n);
        memcpy(heap, bytes, n);
    }
    bool found = EZPI_mb1013_parse_range(heap, (uint32_t)n, out);
    free(heap);
    return found;
}

/* ------------------------------------------------------------------ */

static void test_well_formed_sweep(void)
{
    /* Every value the part can emit: "R" + four digits + CR, 0000..9999.
       Compared against atoi() on a terminated copy, so this asserts the
       bounded parser and the original agree on all 10000 real frames. */
    int mismatches = 0;
    int not_found = 0;

    for (int v = 0; v < 10000; v++)
    {
        char frame[6];
        frame[0] = 'R';
        frame[1] = (char)('0' + (v / 1000) % 10);
        frame[2] = (char)('0' + (v / 100) % 10);
        frame[3] = (char)('0' + (v / 10) % 10);
        frame[4] = (char)('0' + v % 10);
        frame[5] = '\r';

        /* The oracle: exactly what the driver used to compute, but on a copy
           that really is NUL-terminated. */
        char terminated[5];
        memcpy(terminated, &frame[1], 4);
        terminated[4] = '\0';
        float expected = (float)(atoi(terminated) / 10.0);

        float got = -1.0f;
        if (!parse_exact(frame, sizeof(frame), &got))
        {
            not_found++;
            continue;
        }
        if (got != expected)
        {
            mismatches++;
        }
    }

    check(not_found == 0, "every 4-digit frame is recognised");
    check(mismatches == 0, "every 4-digit frame decodes exactly as atoi did");
    printf("  swept 10000 four-digit frames: %d unrecognised, %d mismatched\n",
           not_found, mismatches);
}

static void test_scale_and_endpoints(void)
{
    float v = -1.0f;
    check(parse_exact("R0000\r", 6, &v) && v == 0.0f, "R0000 -> 0.0");
    check(parse_exact("R0001\r", 6, &v) && v == 0.1f, "R0001 -> 0.1");
    check(parse_exact("R0123\r", 6, &v) && v == 12.3f, "R0123 -> 12.3");
    check(parse_exact("R9999\r", 6, &v) && v == 999.9f, "R9999 -> 999.9");
}

static void test_whitespace_payload_no_overread(void)
{
    /* The bug. atoi() skips whitespace, including the frame's own '\r', so
       these made it walk past the terminator and off the end of the block.
       Under ASan an over-read here aborts the process, so reaching the
       assertion at all is part of the result. */
    float v = -1.0f;

    check(!parse_exact("R    \r", 6, &v), "four spaces is not a reading");
    check(!parse_exact("R\t\t\t\t\r", 6, &v), "four tabs is not a reading");
    check(!parse_exact("R\n\n\n\n\r", 6, &v), "four newlines is not a reading");
    check(!parse_exact("R\v\v\v\v\r", 6, &v), "four vertical tabs is not a reading");
    check(!parse_exact("R\f\f\f\f\r", 6, &v), "four form feeds is not a reading");
    check(!parse_exact("R\r\r\r\r\r", 6, &v), "four CRs is not a reading");

    /* Same inputs with the frame at the very end of a longer buffer, so the
       poisoned byte is immediately after the '\r' rather than after padding. */
    check(!parse_exact("xxR    \r", 8, &v), "trailing spaces frame, offset 2");
    check(!parse_exact("xxxxxR\t\t\t\t\r", 11, &v), "trailing tabs frame, offset 5");
}

static void test_space_padded_still_read(void)
{
    /* Leading spaces were decoded correctly by atoi and still are -- the fix
       must not quietly drop readings it used to produce. */
    float v = -1.0f;
    check(parse_exact("R 123\r", 6, &v) && v == 12.3f, "' 123' -> 12.3");
    check(parse_exact("R  12\r", 6, &v) && v == 1.2f, "'  12' -> 1.2");
    check(parse_exact("R   1\r", 6, &v) && v == 0.1f, "'   1' -> 0.1");
    check(parse_exact("R\t123\r", 6, &v) && v == 12.3f, "tab then digits -> 12.3");
}

static void test_digits_then_junk(void)
{
    /* atoi stopped at the first non-digit; so does this. */
    float v = -1.0f;
    check(parse_exact("R12z3\r", 6, &v) && v == 1.2f, "'12z3' -> 1.2 (stops at z)");
    check(parse_exact("R1 23\r", 6, &v) && v == 0.1f, "'1 23' -> 0.1 (space ends it)");
    check(parse_exact("R999x\r", 6, &v) && v == 99.9f, "'999x' -> 99.9");
}

static void test_no_digit_rejected(void)
{
    /* These produced a fabricated reading of 0.0 before. Now they produce
       none, and the scan continues instead of stopping. */
    float v = 7.0f;
    check(!parse_exact("Rzzzz\r", 6, &v), "'zzzz' is not a reading");
    check(v == 7.0f, "a rejected frame leaves out_value untouched");
    check(!parse_exact("R   -\r", 6, &v), "'   -' is not a reading");
    check(!parse_exact("R-123\r", 6, &v), "a negative range is not a reading");
    check(!parse_exact("R++++\r", 6, &v), "'++++' is not a reading");
}

static void test_malformed_does_not_mask_good(void)
{
    /* Two frames: the first has no digits, the second is valid. The old code
       stopped at the first and reported 0.0. */
    float v = -1.0f;
    check(parse_exact("Rzzzz\rR0123\r", 12, &v) && v == 12.3f,
          "a junk frame no longer masks a later good one");

    /* And a valid frame first still wins. */
    check(parse_exact("R0456\rR0123\r", 12, &v) && v == 45.6f,
          "the first valid frame is the one reported");
}

static void test_short_buffers(void)
{
    /* Every length that cannot hold a frame. Exact-size allocations, so an
       over-read is caught rather than absorbed. */
    const char *six = "R0123\r";
    for (size_t n = 0; n < 6; n++)
    {
        float v = -1.0f;
        char what[64];
        snprintf(what, sizeof(what), "length %zu cannot yield a reading", n);
        check(!parse_exact(six, n, &v), what);
    }

    /* Exactly six must work -- guards against a >= / > off-by-one in the
       length test that would reject every real frame. */
    float v = -1.0f;
    check(parse_exact(six, 6, &v) && v == 12.3f, "length 6 exactly is accepted");
}

static void test_frame_at_every_offset(void)
{
    /* Slide a valid frame through a buffer, with the buffer ending right after
       it each time, so the parser has to find it at an arbitrary index without
       reading past the end. */
    int found_all = 1;
    for (size_t pad = 0; pad < 32; pad++)
    {
        size_t n = pad + 6;
        char *buf = malloc(n);
        memset(buf, 'x', pad);
        memcpy(buf + pad, "R0123\r", 6);

        float v = -1.0f;
        uint8_t *heap = malloc(n);
        memcpy(heap, buf, n);
        if (!EZPI_mb1013_parse_range(heap, (uint32_t)n, &v) || v != 12.3f)
        {
            found_all = 0;
        }
        free(heap);
        free(buf);
    }
    check(found_all, "a frame is found at every offset 0..31");
}

static void test_truncated_frame_at_end(void)
{
    /* A frame cut short by the end of the buffer must not be read. These are
       the inputs the cecf5da clamp was written for: the loop used to read
       buffer[idx + 5] when idx + 5 was outside the allocation. */
    int any_found = 0;
    for (size_t keep = 1; keep < 6; keep++)
    {
        /* "xR0123\r" truncated so the 'R' is present but the frame is not. */
        char buf[8];
        buf[0] = 'x';
        memcpy(&buf[1], "R0123\r", 6);

        float v = -1.0f;
        if (parse_exact(buf, 1 + keep, &v))
        {
            any_found = 1;
        }
    }
    check(!any_found, "a frame truncated by the buffer end is never read");
}

static void test_terminator_is_required(void)
{
    /* 'R' plus four digits is not a frame without the CR. This matters for
       more than tidiness: the '\r' requirement is what bounds the payload
       scan. The scan stops at the first non-digit, and because the caller has
       already established that byte 5 is '\r', it can never run past the four
       payload bytes. Drop the terminator check and that reasoning collapses,
       so it is asserted here rather than left implied. */
    float v = -1.0f;
    check(!parse_exact("R0123x", 6, &v), "no CR means no frame");
    check(!parse_exact("R0123\n", 6, &v), "LF is not the terminator");
    check(!parse_exact("R0123 ", 6, &v), "a trailing space is not the terminator");
    check(!parse_exact("R01234", 6, &v), "five digits and no CR is not a frame");
    check(!parse_exact("R0123\0", 6, &v), "NUL is not the terminator");
}

static void test_no_frame_at_all(void)
{
    float v = 3.0f;
    check(!parse_exact("abcdefghij", 10, &v), "no 'R' means no reading");
    check(!parse_exact("RRRRRRRRRR", 10, &v), "all 'R' and no CR means no reading");
    check(!parse_exact("\r\r\r\r\r\r\r\r", 8, &v), "all CR and no 'R' means no reading");
    check(v == 3.0f, "out_value untouched when nothing is found");
}

static void test_null_arguments(void)
{
    float v = 1.0f;
    check(!EZPI_mb1013_parse_range(NULL, 6, &v), "NULL buffer is rejected");
    check(!EZPI_mb1013_parse_range((const uint8_t *)"R0123\r", 6, NULL),
          "NULL out_value is rejected");
    check(!EZPI_mb1013_parse_range(NULL, 0, NULL), "all-NULL is rejected");
    check(v == 1.0f, "out_value untouched on NULL buffer");
}

int main(void)
{
    test_well_formed_sweep();
    test_scale_and_endpoints();
    test_whitespace_payload_no_overread();
    test_space_padded_still_read();
    test_digits_then_junk();
    test_no_digit_rejected();
    test_malformed_does_not_mask_good();
    test_short_buffers();
    test_frame_at_every_offset();
    test_truncated_frame_at_end();
    test_terminator_is_required();
    test_no_frame_at_all();
    test_null_arguments();

    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
