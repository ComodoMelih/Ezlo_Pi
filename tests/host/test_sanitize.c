/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**
** 1. Redistributions of source code must retain the above copyright notice,
**    this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. Neither the name of the copyright holder nor the names of its
**    contributors may be used to endorse or promote products derived from
**    this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
** POSSIBILITY OF SUCH DAMAGE.
** ===========================================================================
*/

/**
 * @file    test_sanitize.c
 * @brief   Regression tests for ezlopi-util-sanitize.
 *
 * Every case below corresponds to a real defect that shipped in this firmware.
 * The helpers under test replaced open-coded versions in the BLE, UART and
 * cloud paths; each of those versions mishandled input that an unauthenticated
 * client could send. The point of this file is that those specific inputs can
 * never pass again, and that the check runs on a laptop in under a second
 * rather than requiring a board to be flashed.
 *
 * Deliberately has no test-framework dependency -- plain C and a counter -- so
 * that CI needs nothing but a C compiler, and so that a contributor can run it
 * without installing anything. See tests/host/Makefile.
 */

#include <stdio.h>
#include <string.h>

#include "ezlopi_util_sanitize.h"

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static unsigned int tests_run = 0;
static unsigned int tests_failed = 0;

/*******************************************************************************
 *                          Macros
 *******************************************************************************/
#define CHECK(cond, ...)                             \
    do                                               \
    {                                                \
        tests_run++;                                 \
        if (!(cond))                                 \
        {                                            \
            tests_failed++;                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                     \
            printf("\n");                            \
        }                                            \
    } while (0)

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * @brief Assert that the scanner ends @p json at the given offset.
 *
 * @param json  text to scan
 * @param want  expected offset of the first byte after the value, or -1 for
 *              "no complete value here"
 */
static void expect_value_end(const char *json, int want)
{
    const char *end = EZPI_util_json_value_end(json);
    int got = (NULL == end) ? -1 : (int)(end - json);

    CHECK(got == want, "json_value_end(\"%s\") = %d, want %d", json, got, want);
}

/**
 * @brief Assert that in-place redaction turns @p in into @p want.
 */
static void expect_redacted(const char *in, const char *want)
{
    char buf[256];

    CHECK(strlen(in) < sizeof(buf), "test input too long: %s", in);

    snprintf(buf, sizeof(buf), "%s", in);
    EZPI_util_redact_secrets_in_place(buf);

    CHECK(0 == strcmp(buf, want), "redact(\"%s\") = \"%s\", want \"%s\"", in, buf, want);
}

static void test_json_value_end(void)
{
    /* The reason this function exists: the UART driver hands over everything
     * that accumulated in its RX buffer, so two commands sent back to back
     * arrive as one buffer. cJSON_Parse stops after the first value and still
     * reports success, so every command after the first was dropped silently.
     * The scanner has to find where the first object ends so each can be
     * dispatched in turn. */
    expect_value_end("{\"cmd\":4}{\"cmd\":1}", 9);
    expect_value_end("{\"cmd\":4}", 9);
    expect_value_end("[1,2,3]", 7);
    expect_value_end("{\"a\":{\"b\":[1,2]}}", 17);

    /* A brace inside a string must not count towards the depth, or the value
     * ends in the middle of the object. */
    expect_value_end("{\"a\":\"}\"}", 9);
    expect_value_end("{\"a\":\"{{{\"}", 11);

    /* The original scanner did not treat a backslash escape as a unit, so an
     * escaped quote read as the end of the string and every delimiter after it
     * was counted at the wrong depth. */
    expect_value_end("{\"a\":\"\\\"}\"}", 11);
    expect_value_end("{\"a\":\"\\\\\"}", 10);

    /* Incomplete input: a partial command must be rejected, not acted on. */
    expect_value_end("{\"cmd\":4", -1);
    expect_value_end("{\"a\":\"unterminated", -1);
    expect_value_end("{\"a\":\"trailing escape\\", -1);
    expect_value_end("", -1);

    /* No container was opened, so there is no value to delimit. */
    expect_value_end("}", -1);
    expect_value_end("123", -1);

    CHECK(NULL == EZPI_util_json_value_end(NULL), "NULL input must give NULL");
}

static void test_redact_secrets(void)
{
    /* hub.network.get reports the Wi-Fi PSK as "key", and the outgoing-payload
     * trace dumped it verbatim -- that is how the plaintext PSK reached the
     * serial console. */
    expect_redacted("{\"key\":\"hunter2\"}", "{\"key\":\"*******\"}");
    expect_redacted("{\"pass\":\"abc\"}", "{\"pass\":\"***\"}");
    expect_redacted("{\"password\":\"abc\"}", "{\"password\":\"***\"}");
    expect_redacted("{\"wifi_password\":\"abc\"}", "{\"wifi_password\":\"***\"}");

    /* Several secrets in one payload, and one that appears twice. */
    expect_redacted("{\"key\":\"aa\",\"pass\":\"bbb\"}", "{\"key\":\"**\",\"pass\":\"***\"}");
    expect_redacted("[{\"key\":\"a\"},{\"key\":\"bb\"}]", "[{\"key\":\"*\"},{\"key\":\"**\"}]");

    /* Matching includes the quotes and the colon, so a member whose name merely
     * contains "pass" or "key" keeps its value. Over-redacting is its own bug:
     * it would mask fields an operator needs in order to debug. */
    expect_redacted("{\"passkey_hint\":\"first pet\"}", "{\"passkey_hint\":\"first pet\"}");
    expect_redacted("{\"keyring\":\"abc\"}", "{\"keyring\":\"abc\"}");
    expect_redacted("{\"ssid\":\"my-network\"}", "{\"ssid\":\"my-network\"}");

    /* "wifi_password" must not be matched by the "password" entry: the byte
     * before the name is '_', not the opening quote. If it were matched at the
     * wrong offset the mask would start mid-name and corrupt the JSON. */
    expect_redacted("{\"wifi_password\":\"ab\",\"ssid\":\"x\"}",
                    "{\"wifi_password\":\"**\",\"ssid\":\"x\"}");

    /* An escaped quote inside the secret used to look like the closing quote,
     * leaving the rest of the password in the clear. */
    expect_redacted("{\"pass\":\"a\\\"b\"}", "{\"pass\":\"****\"}");
    expect_redacted("{\"pass\":\"a\\\\b\"}", "{\"pass\":\"****\"}");

    /* An empty secret, and a secret that runs to the end of the buffer with no
     * closing quote, must both terminate rather than run off the end. */
    expect_redacted("{\"key\":\"\"}", "{\"key\":\"\"}");
    expect_redacted("{\"key\":\"abc", "{\"key\":\"***");

    /* Redaction is in place and must not change the length -- the caller passes
     * a preallocated buffer it has already measured. */
    {
        char buf[64];
        size_t before;

        snprintf(buf, sizeof(buf), "{\"key\":\"hunter2\",\"ssid\":\"cia_iot\"}");
        before = strlen(buf);
        EZPI_util_redact_secrets_in_place(buf);
        CHECK(before == strlen(buf), "redaction changed the string length");
    }

    /* Must tolerate NULL: the trace path can be reached before the buffer is
     * allocated. */
    EZPI_util_redact_secrets_in_place(NULL);
}

static void test_clamp_copy_len(void)
{
    CHECK(5 == EZPI_util_clamp_copy_len(5, 10, 10), "request within both limits");
    CHECK(3 == EZPI_util_clamp_copy_len(9, 3, 10), "must clamp to the source length");
    CHECK(4 == EZPI_util_clamp_copy_len(9, 10, 4), "must clamp to the space left");
    CHECK(0 == EZPI_util_clamp_copy_len(9, 10, 0), "no space left means copy nothing");
    CHECK(0 == EZPI_util_clamp_copy_len(0, 10, 10), "nothing requested means copy nothing");

    /* The BLE 0xE501 chunked-config case this was written for: a client whose
     * chunk claims a length larger than both the string it actually sent and
     * the space left in the reassembly buffer. Unclamped, that walked off the
     * end of the heap block. */
    CHECK(7 == EZPI_util_clamp_copy_len(0xFFFFFFFFu, 7, 400), "over-claimed chunk length");
    CHECK(10 == EZPI_util_clamp_copy_len(0xFFFFFFFFu, 4096, 10), "chunk past end of buffer");
}

static void test_copy_str_bounded(void)
{
    /* A canary after the destination catches an off-by-one that a strcmp-based
     * check would miss. */
    struct
    {
        char dst[8];
        char canary[8];
    } g;

    memset(&g, 'Z', sizeof(g));
    CHECK(3 == EZPI_util_copy_str_bounded(g.dst, sizeof(g.dst), "abc", 3), "short copy length");
    CHECK(0 == strcmp(g.dst, "abc"), "short copy content: got \"%s\"", g.dst);
    CHECK('Z' == g.canary[0], "short copy wrote past the destination");

    /* Exactly buffer-sized input. This is the chipset_name defect: a config
     * written over BLE or UART supplied a string that filled the buffer, and
     * the open-coded copy left it unterminated for the strncmp that followed. */
    memset(&g, 'Z', sizeof(g));
    CHECK(7 == EZPI_util_copy_str_bounded(g.dst, sizeof(g.dst), "abcdefg", 7), "exact-fit length");
    CHECK(0 == strcmp(g.dst, "abcdefg"), "exact-fit content: got \"%s\"", g.dst);
    CHECK('\0' == g.dst[7], "exact-fit input must still be terminated");
    CHECK('Z' == g.canary[0], "exact-fit copy wrote past the destination");

    /* Oversized input must be truncated, not overflow the stack buffer. */
    memset(&g, 'Z', sizeof(g));
    CHECK(7 == EZPI_util_copy_str_bounded(g.dst, sizeof(g.dst), "ESP32-OVERLONG-NAME", 19), "oversize length");
    CHECK(0 == strcmp(g.dst, "ESP32-O"), "oversize content: got \"%s\"", g.dst);
    CHECK('Z' == g.canary[0], "oversize copy wrote past the destination");

    /* A length that lies about the source is still bounded by the destination:
     * the helper is given the length from the wire, not strlen. */
    memset(&g, 'Z', sizeof(g));
    CHECK(7 == EZPI_util_copy_str_bounded(g.dst, sizeof(g.dst), "0123456789ABCDEF", 0xFFFF), "lying length");
    CHECK('Z' == g.canary[0], "lying length wrote past the destination");

    /* Degenerate arguments. A missing source must still produce a usable empty
     * string, because every caller hands the result to strcmp. */
    memset(&g, 'Z', sizeof(g));
    CHECK(0 == EZPI_util_copy_str_bounded(g.dst, sizeof(g.dst), NULL, 4), "NULL source copies nothing");
    CHECK('\0' == g.dst[0], "NULL source must still terminate the destination");
    CHECK('Z' == g.canary[0], "NULL source wrote past the destination");

    CHECK(0 == EZPI_util_copy_str_bounded(NULL, 8, "abc", 3), "NULL destination");

    memset(&g, 'Z', sizeof(g));
    CHECK(0 == EZPI_util_copy_str_bounded(g.dst, 0, "abc", 3), "zero-sized destination");
    CHECK('Z' == g.dst[0], "zero-sized destination must not be written at all");

    /* A one-byte destination has room for the terminator only. */
    memset(&g, 'Z', sizeof(g));
    CHECK(0 == EZPI_util_copy_str_bounded(g.dst, 1, "abc", 3), "one-byte destination");
    CHECK('\0' == g.dst[0], "one-byte destination gets just the terminator");
    CHECK('Z' == g.dst[1], "one-byte destination wrote past its size");
}

/*******************************************************************************
 *                          Public Function Definitions
 *******************************************************************************/
int main(void)
{
    /* See the matching note in test_cjext.c. stdout is block-buffered when this
     * runs under a pipe, and a sanitizer abort does not flush, so FAIL lines
     * printed before a crash would be lost exactly when they matter most. */
    setvbuf(stdout, NULL, _IONBF, 0);

    test_json_value_end();
    test_redact_secrets();
    test_clamp_copy_len();
    test_copy_str_bounded();

    printf("%u check(s), %u failure(s)\n", tests_run, tests_failed);

    return (0 == tests_failed) ? 0 : 1;
}
