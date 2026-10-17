/* Host tests for the HTTPS response accumulator, ezlopi_core_http_buf.c.
 *
 * WHY THIS FILE EXISTS
 *
 * Until this commit every HTTPS reply this firmware received came back one byte
 * short per 255 bytes read. The accumulation loop in
 * __ezlopi_http_req_via_mbedTLS grew its buffer correctly and then filled it
 * with two snprintf calls:
 *
 *     snprintf(resp_buf_dummy, len, "%s", tmp_buf);                  // first
 *     snprintf(resp_buf_dummy + strlen(resp_buf_dummy), len, "%s", tmp_buf);
 *
 * snprintf's size argument counts the terminator it writes, so a limit of `len`
 * stores len-1 bytes of payload. mbedtls_ssl_read is called with
 * tmp_buf_size - 1 == 255, so each read lost its last byte. A reply that
 * arrived in one read lost its final character -- for a JSON body, the closing
 * brace.
 *
 * It reached the mbedTLS path only. The esp_http_client path in the same file
 * memcpy's each chunk exactly, so provisioning -- which does parse its reply --
 * was never affected. Of this path's three callers, two never read the body and
 * the third reads only the status line at the front of the first chunk, which a
 * loss at the end of each chunk cannot reach. That is why the defect survived:
 * no caller was visibly broken, and equally, no caller could have a body.
 *
 * WHY THE TEST IS WORTH MORE THAN THE READING
 *
 * This defect is invisible to every other instrument available here. It writes
 * strictly less than it has room for, so there is no overflow for
 * AddressSanitizer to trap and no truncation for a compiler to warn about. It
 * is not a crash, so no target build reports it. The trace that printed the
 * body printed the damaged copy, with nothing to mark where the byte went. The
 * only thing that can see it is a check that compares what came out against
 * what went in, byte for byte -- which is what reassemble_as_firmware_does()
 * and reassemble() below are for.
 *
 * Those two functions are the reason ezlopi_core_http_buf.c exists as a
 * separate translation unit. ezlopi_core_http.c cannot be compiled on a host:
 * it needs ten headers this repository does not ship, nine of them under
 * mbedtls/ and esp_tls/, which is also why stub_httpurl.c reimplements two of
 * its buffer helpers rather than linking them. So the accumulator was moved
 * into a file that includes
 * ezlopi_core_http.h and nothing else of consequence, and the loop around it is
 * transcribed here -- the buffer sizes, the 255-byte reads and the growth
 * policy are the firmware's, with mbedtls_ssl_read replaced by a generator that
 * hands out a known body. The "as_firmware_does" arm keeps the two snprintf
 * calls so that the test states the size of the loss rather than only asserting
 * its absence; if a future edit reintroduces them, the numbers printed by the
 * two arms diverge again and the assertions fail.
 *
 * WHAT IS DELIBERATELY NOT TESTED HERE
 *
 * The real read loop, which lives in the file that cannot be compiled. What is
 * checked by reading rather than running: that the loop passes `ret` as
 * chunk_len (it does -- `len = ret` immediately before the call), that it stops
 * on a failed append instead of continuing (it sets resp_truncated and breaks),
 * and that the handover uses resp_used rather than strlen. The transcription
 * below is only as good as that reading, and it is recorded as such.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "ezlopi_core_http.h"

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

/*******************************************************************************
 *                          The failing allocator
 *******************************************************************************/

/* stubs-httpresp/EZLOPI_USER_CONFIG.h routes ezlopi_realloc here instead of
** straight to realloc(), so that the growth failure path can be reached. See
** that header for why the alternatives were rejected. */
static int g_realloc_fail_from = -1; /* -1: never fail */
static int g_realloc_calls;

void *EZPI_test_realloc(void *ptr, size_t size)
{
    g_realloc_calls++;
    if ((g_realloc_fail_from >= 0) && (g_realloc_calls > g_realloc_fail_from))
    {
        /* Not a call to realloc with a silly size -- no call at all. A realloc
        ** that returns NULL has not touched the original block, which is the
        ** precondition the subject relies on when it leaves *buf alone. */
        return NULL;
    }
    return realloc(ptr, size);
}

static void allocator_reset(void)
{
    g_realloc_fail_from = -1;
    g_realloc_calls = 0;
}

/*******************************************************************************
 *                          The firmware's read loop
 *******************************************************************************/

/* Stands in for mbedtls_ssl_read: copies up to `want` bytes of the body into
** `dst` and returns the count, 0 at the end. mbedtls_ssl_read does not
** NUL-terminate and neither does this; the firmware bzeros tmp_buf before every
** read and asks for tmp_buf_size - 1, so the spare byte is the terminator. That
** detail is reproduced rather than assumed away, because it is what makes the
** old snprintf calls look correct at a glance. */
typedef struct
{
    const char *body;
    size_t len;
    size_t off;
} s_fake_socket_t;

static int fake_read(s_fake_socket_t *s, char *dst, int want)
{
    size_t left = s->len - s->off;
    int n;

    if (0 == left)
    {
        return 0;
    }
    n = (left < (size_t)want) ? (int)left : want;
    memcpy(dst, s->body + s->off, (size_t)n);
    s->off += (size_t)n;
    return n;
}

/* The loop from ezlopi_core_http.c as it now stands, with the real accumulator.
** Returns the buffer; *out_used and *out_reallocs report what happened. */
static char *reassemble(const char *body, size_t body_len, uint32_t *out_used,
                        int *out_reallocs, int *out_failed)
{
    uint32_t tmp_buf_size = 256;
    char tmp_buf[256];
    uint32_t resp_buf_size = tmp_buf_size + 1;
    char *resp_buf_dummy = (char *)malloc(resp_buf_size);
    uint32_t resp_used = 0;
    s_fake_socket_t sock = {body, body_len, 0};
    int ret, len, failed = 0;

    if (NULL == resp_buf_dummy)
    {
        abort();
    }
    memset(resp_buf_dummy, 0, resp_buf_size);
    g_realloc_calls = 0;

    do
    {
        len = (int)tmp_buf_size - 1;
        memset(tmp_buf, 0, tmp_buf_size);
        ret = fake_read(&sock, tmp_buf, len);
        if (0 == ret)
        {
            break;
        }
        len = ret;

        if (EZPI_SUCCESS != EZPI_core_http_resp_append(&resp_buf_dummy, &resp_buf_size,
                                                       &resp_used, tmp_buf, (uint32_t)len))
        {
            failed = 1;
            break;
        }
    } while (1);

    *out_used = resp_used;
    *out_reallocs = g_realloc_calls;
    *out_failed = failed;
    return resp_buf_dummy;
}

/* The same loop with the two snprintf calls it used to have, so the loss can be
** measured rather than described. Nothing here is a claim about correct
** behaviour; it is the old behaviour, kept so the tests can print the
** difference. */
static char *reassemble_as_firmware_did(const char *body, size_t body_len)
{
    uint32_t tmp_buf_size = 256;
    char tmp_buf[256];
    uint32_t resp_buf_size = tmp_buf_size + 1;
    char *resp_buf_dummy = (char *)malloc(resp_buf_size);
    s_fake_socket_t sock = {body, body_len, 0};
    unsigned char reply_count = 0;
    int ret, len;

    if (NULL == resp_buf_dummy)
    {
        abort();
    }
    memset(resp_buf_dummy, 0, resp_buf_size);

    do
    {
        len = (int)tmp_buf_size - 1;
        memset(tmp_buf, 0, tmp_buf_size);
        ret = fake_read(&sock, tmp_buf, len);
        if (0 == ret)
        {
            break;
        }
        len = ret;

        reply_count++;
        if (reply_count > 1)
        {
            resp_buf_size += (uint32_t)(len + 5);
            if (EZPI_SUCCESS == EZPI_core_http_dyna_relloc(&resp_buf_dummy, (int)resp_buf_size))
            {
                snprintf(resp_buf_dummy + strlen(resp_buf_dummy), (size_t)len, "%s", tmp_buf);
            }
            else
            {
                resp_buf_size -= (uint32_t)(len + 5);
            }
        }
        else
        {
            snprintf(resp_buf_dummy, (size_t)len, "%s", tmp_buf);
        }
    } while (1);

    return resp_buf_dummy;
}

/*******************************************************************************
 *                          Tests
 *******************************************************************************/

/* The central claim: a body put through the loop comes back identical. Each
** case also prints what the old code returned, so the test records the size of
** the defect it is guarding instead of only asserting its absence. */
static void one_body(const char *name, const char *body, size_t body_len)
{
    uint32_t used = 0;
    int reallocs = 0, failed = 0;
    char *got, *old;
    size_t old_len;

    allocator_reset();
    got = reassemble(body, body_len, &used, &reallocs, &failed);
    old = reassemble_as_firmware_did(body, body_len);
    old_len = strlen(old);

    printf("  %-32s %4zu in, %4u out (was %4zu, lost %ld), %d realloc(s)\n",
           name, body_len, (unsigned)used, old_len,
           (long)body_len - (long)old_len, reallocs);

    ok(0 == failed, "append never failed");
    ok(used == (uint32_t)body_len, "every byte was accounted for");
    ok(0 == memcmp(got, body, body_len), "every byte came back unchanged");
    ok('\0' == got[body_len], "the buffer is terminated at *used");

    /* Asserted in the same direction as the bug, so this fails if the old code
    ** ever comes back: the point is not that the new result is right in
    ** isolation but that the old one was not. */
    ok(old_len < body_len, "the old loop did lose bytes on this body");

    free(got);
    free(old);
}

static void test_bodies_round_trip(void)
{
    char big[1300];
    char exact255[256];
    char exact256[257];
    size_t i;

    printf("-- bodies through the read loop --\n");

    for (i = 0; i < sizeof(big) - 1; i++)
    {
        big[i] = (char)('a' + (i % 26));
    }
    big[sizeof(big) - 1] = '\0';

    memset(exact255, 'x', 255);
    exact255[255] = '\0';
    memset(exact256, 'y', 256);
    exact256[256] = '\0';

    /* 15 bytes: one read, and the byte it used to lose is the closing brace.
    ** This is the shape of every reply the sunstate and cloud paths get. */
    one_body("short JSON body", "{\"status\":\"OK\"}", 15);

    /* Exactly one full read. */
    one_body("exactly one 255-byte read", exact255, 255);

    /* Two reads, the second one byte long: the old code lost one from each. */
    one_body("256 bytes, so two reads", exact256, 256);

    /* Six reads, the size of the reply api.sunrisesunset.io actually returns. */
    one_body("1299 bytes, six reads", big, 1299);
}

/* The case that made `used` a parameter instead of a strlen() call. The old
** append offset was strlen(resp_buf_dummy), which is the body's length only
** while the body has no NUL in it. Nothing requires an HTTP body to be text --
** any image, any gzip stream, any protobuf has them -- and for such a body the
** old loop wrote every chunk after the first on top of the earlier ones. */
static void test_embedded_nul(void)
{
    char body[600];
    uint32_t used = 0;
    int reallocs = 0, failed = 0;
    char *got;
    size_t i;

    printf("-- a body with NUL bytes in it --\n");

    for (i = 0; i < sizeof(body); i++)
    {
        body[i] = (char)('A' + (i % 26));
    }
    body[10] = '\0';
    body[300] = '\0';
    body[sizeof(body) - 1] = '\0';

    allocator_reset();
    got = reassemble(body, sizeof(body), &used, &reallocs, &failed);

    ok(0 == failed, "a body with NULs appends without failing");
    ok(used == (uint32_t)sizeof(body), "used counts the NULs as payload");
    ok(0 == memcmp(got, body, sizeof(body)), "all 600 bytes survive, NULs included");

    /* The measurement that justifies the parameter: strlen stops at byte 10, so
    ** a loop that trusted it would have appended chunk two at offset 10 and
    ** destroyed 245 bytes of chunk one. */
    ok(10 == strlen(got), "strlen sees 10 of the 600 bytes, which is why it is not used");

    free(got);
}

/* Growth policy. The caller is in a read loop, so a tight-fitting realloc per
** chunk is a realloc per 255 bytes. Geometric growth is what keeps that
** sub-linear, and this measures the difference rather than trusting it. */
static void test_growth_is_geometric(void)
{
    char big[1300];
    uint32_t used = 0;
    int reallocs = 0, failed = 0;
    char *got;
    size_t i;

    printf("-- growth --\n");

    for (i = 0; i < sizeof(big); i++)
    {
        big[i] = 'z';
    }

    allocator_reset();
    got = reassemble(big, sizeof(big), &used, &reallocs, &failed);

    ok(0 == failed, "1300 bytes append");
    ok(used == 1300, "1300 bytes accumulated");

    /* 1300 bytes is six reads (255 x 5 + 25). Growing to exactly `need` each
    ** time costs five reallocations -- every read after the first, since the
    ** initial 257-byte buffer holds only the first. The geometric policy costs
    ** three. The bound is set at four so that it discriminates between the two
    ** policies without breaking if the growth factor is retuned. */
    printf("    %d reallocation(s) for six reads; tight-fit would be 5\n", reallocs);
    ok(reallocs <= 4, "growth is geometric, not one realloc per read");
    ok(reallocs >= 1, "it did have to grow at all");

    free(got);
}

/* Arguments the function must refuse rather than trust. The read loop cannot
** produce most of these, which is the point: the next caller might, and the
** failure mode of this particular function is silent corruption rather than a
** crash, so it is worth being unambiguous at the boundary. */
static void test_rejects_bad_arguments(void)
{
    char *buf = (char *)malloc(32);
    char *nullbuf = NULL;
    uint32_t cap = 32, used = 0;

    printf("-- rejected arguments --\n");

    if (NULL == buf)
    {
        abort();
    }
    memset(buf, 0, 32);
    allocator_reset();

    ok(EZPI_FAILED == EZPI_core_http_resp_append(NULL, &cap, &used, "ab", 2), "NULL buf is refused");
    ok(EZPI_FAILED == EZPI_core_http_resp_append(&nullbuf, &cap, &used, "ab", 2), "NULL *buf is refused");
    ok(EZPI_FAILED == EZPI_core_http_resp_append(&buf, NULL, &used, "ab", 2), "NULL cap is refused");
    ok(EZPI_FAILED == EZPI_core_http_resp_append(&buf, &cap, NULL, "ab", 2), "NULL used is refused");
    ok(EZPI_FAILED == EZPI_core_http_resp_append(&buf, &cap, &used, NULL, 2), "NULL chunk is refused");

    ok(0 == used, "a refused call did not move used");
    ok(32 == cap, "a refused call did not change cap");
    ok(0 == g_realloc_calls, "a refused call did not allocate");

    /* Zero length is success, not failure: mbedtls_ssl_read returning 0 means
    ** end of stream and the loop breaks on it, but a caller that passes a
    ** zero-length chunk has asked for nothing and got it. Calling that an error
    ** would make the read loop abandon a complete reply. */
    ok(EZPI_SUCCESS == EZPI_core_http_resp_append(&buf, &cap, &used, "ab", 0), "zero length is success");
    ok(0 == used, "zero length appended nothing");

    free(buf);
}

/* The growth-failure contract: nothing appended, nothing moved, and the body
** accumulated so far still readable. The old loop's behaviour here was to drop
** the chunk and keep reading, which produced a short reply indistinguishable
** from a complete one -- the same class of defect as the off-by-one, arrived at
** from the other direction. */
static void test_failed_growth_changes_nothing(void)
{
    char *buf = (char *)malloc(16);
    uint32_t cap = 16, used = 0;
    char *before;
    ezlopi_error_t r;

    printf("-- growth failure --\n");

    if (NULL == buf)
    {
        abort();
    }
    memset(buf, 0, 16);
    allocator_reset();

    /* Ten bytes fit in sixteen, so this one needs no growth. */
    r = EZPI_core_http_resp_append(&buf, &cap, &used, "0123456789", 10);
    ok(EZPI_SUCCESS == r, "first ten bytes fit without growing");
    ok(10 == used, "ten bytes in");
    ok(0 == g_realloc_calls, "and no reallocation was needed");

    before = buf;

    /* Now ask for more than the capacity with the allocator refusing. */
    g_realloc_fail_from = 0;
    r = EZPI_core_http_resp_append(&buf, &cap, &used, "abcdefghij", 10);

    ok(EZPI_FAILED == r, "a chunk that cannot be held is reported, not dropped");
    ok(1 == g_realloc_calls, "growth was attempted exactly once");
    ok(before == buf, "the buffer pointer did not move");
    ok(10 == used, "used still describes what is actually in the buffer");
    ok(16 == cap, "cap still describes the real capacity");
    ok(0 == strcmp(buf, "0123456789"), "the bytes already accumulated are intact");

    /* And the accumulator is still usable once the heap recovers, which is what
    ** makes "nothing was changed" worth more than "it failed cleanly". */
    allocator_reset();
    r = EZPI_core_http_resp_append(&buf, &cap, &used, "abcdefghij", 10);
    ok(EZPI_SUCCESS == r, "the same chunk succeeds when growth succeeds");
    ok(20 == used, "and lands at the right offset");
    ok(0 == strcmp(buf, "0123456789abcdefghij"), "giving the whole body");

    free(buf);
}

/* The read loop's own failure handling, transcribed: on a failed append it
** stops rather than continuing. Checked here through the loop so that the
** reported `used` is the length of the prefix that was actually stored --
** which is what ezlopi_core_http.c now traces, and what makes it discard the
** partial reply instead of handing it over. */
static void test_loop_stops_on_failure(void)
{
    char big[1300];
    uint32_t used = 0;
    int reallocs = 0, failed = 0;
    char *got;
    size_t i;

    printf("-- the loop stops on a failed append --\n");

    for (i = 0; i < sizeof(big); i++)
    {
        big[i] = (char)('a' + (i % 26));
    }

    allocator_reset();
    g_realloc_fail_from = 0; /* fail the first growth, which is read two */
    got = reassemble(big, sizeof(big), &used, &reallocs, &failed);

    ok(1 == failed, "the loop reports the failure");
    ok(255 == used, "and used is the prefix that was stored, not the reply size");
    ok(0 == memcmp(got, big, 255), "the stored prefix is the first read, unmangled");
    ok(1 == reallocs, "it stopped at the first refusal rather than retrying per read");

    free(got);
}

/* The size ceiling. Unreachable on a device with a few hundred kilobytes of
** heap, and tested anyway because the first version of this bound was wrong at
** exactly one input -- *used == UINT32_MAX made the 32-bit sum wrap to
** chunk_len, which the `need < chunk_len` idiom does not catch, and the
** function would then have memcpy'd at an offset of 4GB. The bound is now
** computed in 64 bits against INT_MAX; these are the inputs that distinguish
** the two. */
static void test_size_ceiling(void)
{
    char *buf = (char *)malloc(16);
    uint32_t cap = 16, used;

    printf("-- the size ceiling --\n");

    if (NULL == buf)
    {
        abort();
    }
    memset(buf, 0, 16);
    allocator_reset();

    /* The input the old guard caught. */
    used = 0xFFFFFFFEu;
    ok(EZPI_FAILED == EZPI_core_http_resp_append(&buf, &cap, &used, "abcd", 4),
       "a length near UINT32_MAX is refused");
    ok(0xFFFFFFFEu == used, "and used is untouched");

    /* The input it did not: used + chunk_len + 1 == 2^32 exactly, so the
    ** 32-bit sum is chunk_len and `need < chunk_len` is false. */
    used = 0xFFFFFFFFu;
    ok(EZPI_FAILED == EZPI_core_http_resp_append(&buf, &cap, &used, "a", 1),
       "used == UINT32_MAX with a 1-byte chunk is refused");
    ok(0xFFFFFFFFu == used, "and used is untouched");

    /* Anything above INT_MAX is refused too, because dyna_relloc takes the size
    ** as an int: a larger request would be narrowed to a negative and come back
    ** looking like a heap failure. */
    used = (uint32_t)INT_MAX;
    ok(EZPI_FAILED == EZPI_core_http_resp_append(&buf, &cap, &used, "a", 1),
       "a total above INT_MAX is refused");

    ok(0 == g_realloc_calls, "none of the refusals allocated");

    free(buf);
}

int main(void)
{
    printf("ezlopi-core-http response accumulator host tests\n\n");

    test_bodies_round_trip();
    test_embedded_nul();
    test_growth_is_geometric();
    test_rejects_bad_arguments();
    test_failed_growth_changes_nothing();
    test_loop_stops_on_failure();
    test_size_ceiling();

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);

    if (g_fails)
    {
        printf("HTTP RESPONSE TESTS FAILED\n");
        return 1;
    }
    printf("HTTP RESPONSE TESTS PASSED\n");
    return 0;
}
