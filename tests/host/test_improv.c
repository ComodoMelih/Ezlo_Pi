/**
 * @file    test_improv.c
 * @brief   Tests for the transport-independent Improv Wi-Fi codec.
 *
 * This is the first part of the Improv work that is tested rather than merely
 * compiled. The BLE service can only be checked by linking it and reading the
 * symbol table; this codec is pure logic, so the cases below exercise the
 * things that actually go wrong in a framing parser -- attacker-chosen
 * lengths, payloads containing NUL and newline, frames split across reads, and
 * a maximal frame followed by more input.
 *
 * Two of these are regression tests for defects found while writing the code
 * rather than hypothetical ones:
 *
 *   - test_rx_max_frame_then_more_input() covers a one-past-the-end write. The
 *     state machine used to leave its length set to the finished frame, so a
 *     further byte indexed buf[265] in a 265-byte buffer. It is caught here by
 *     ASan, which is why this suite is built with sanitizers on.
 *   - test_rx_restart_on_repeated_magic() covers "IMP" + "IMPROV...", where the
 *     byte that breaks a match is itself the start of the next one. Handing
 *     that byte back for passthrough while also keeping it would have
 *     corrupted the buffer being handed over.
 *
 * The suite was itself checked by mutation: nine deliberate defects were
 * introduced into the codec one at a time to confirm the tests fail. That is
 * how the restart case above came to loop over prefix lengths -- see the
 * comment on it.
 *
 * Deliberately has no test-framework dependency -- plain C and a counter -- to
 * match the rest of tests/host.
 */

#include <stdio.h>
#include <string.h>

#include "ezlopi_util_improv.h"

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static unsigned int tests_run = 0;
static unsigned int tests_failed = 0;

/*******************************************************************************
 *                          Macros
 *******************************************************************************/
#define CHECK(cond, ...)                                \
    do                                                  \
    {                                                   \
        tests_run++;                                    \
        if (!(cond))                                    \
        {                                               \
            tests_failed++;                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * @brief Feed a whole buffer one byte at a time, collecting passthrough bytes.
 *
 * Byte-at-a-time is how the transport will really drive this, and it is also
 * the only way to catch a parser that works on a whole frame but not on a
 * frame split across two reads.
 *
 * @param rx         Receive state.
 * @param data       Bytes to feed.
 * @param len        How many.
 * @param pass_out   Collected passthrough bytes, may be NULL.
 * @param pass_cap   Capacity of @p pass_out.
 * @param pass_len   Set to the number of passthrough bytes collected.
 * @param frames     Set to the number of valid frames seen.
 * @param bad        Set to the number of checksum failures seen.
 * @return The result of the final feed.
 */
static e_ezpi_improv_rx_result_t feed_all(s_ezpi_improv_rx_t *rx,
                                          const uint8_t *data, uint16_t len,
                                          uint8_t *pass_out, uint16_t pass_cap,
                                          uint16_t *pass_len,
                                          unsigned int *frames, unsigned int *bad)
{
    e_ezpi_improv_rx_result_t last = EZPI_IMPROV_RX_NEED_MORE;

    if (NULL != pass_len)
    {
        *pass_len = 0;
    }
    if (NULL != frames)
    {
        *frames = 0;
    }
    if (NULL != bad)
    {
        *bad = 0;
    }

    for (uint16_t i = 0; i < len; i++)
    {
        const uint8_t *pt = NULL;
        uint16_t pt_len = 0;

        last = EZPI_improv_rx_feed(rx, data[i], &pt, &pt_len);

        if (EZPI_IMPROV_RX_NOT_IMPROV == last)
        {
            for (uint16_t j = 0; j < pt_len; j++)
            {
                if ((NULL != pass_out) && (NULL != pass_len) && (*pass_len < pass_cap))
                {
                    pass_out[*pass_len] = pt[j];
                    (*pass_len)++;
                }
            }
        }
        else
        {
            /* Nothing may be handed back unless the result says so. */
            CHECK(NULL == pt, "passthrough set on a non-passthrough result");
            CHECK(0 == pt_len, "passthrough length set on a non-passthrough result");

            if ((EZPI_IMPROV_RX_FRAME == last) && (NULL != frames))
            {
                (*frames)++;
            }
            if ((EZPI_IMPROV_RX_BAD_CHECKSUM == last) && (NULL != bad))
            {
                (*bad)++;
            }
        }
    }

    return last;
}

static void test_checksum(void)
{
    /* The rule taken from the reference implementation: an 8-bit wrapping sum
     * of every byte, with the wrap being deliberate rather than overflow. */
    const uint8_t a[] = {0x01, 0x02, 0x03};
    CHECK(0x06 == EZPI_improv_checksum(a, sizeof(a)), "simple sum");

    const uint8_t b[] = {0xFF, 0x02};
    CHECK(0x01 == EZPI_improv_checksum(b, sizeof(b)), "sum wraps at 8 bits");

    const uint8_t c[] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(0xFC == EZPI_improv_checksum(c, sizeof(c)), "sum wraps repeatedly");

    CHECK(0x00 == EZPI_improv_checksum(NULL, 0), "empty is zero");
    CHECK(0x00 == EZPI_improv_checksum(a, 0), "zero length ignores the data");
}

static void test_frame_build(void)
{
    uint8_t frame[EZPI_IMPROV_FRAME_BUF_MAX];
    const uint8_t payload[] = {0x02};

    const uint16_t n = EZPI_improv_frame_build(frame, sizeof(frame),
                                               EZPI_IMPROV_TYPE_CURRENT_STATE,
                                               payload, sizeof(payload));

    /* magic(6) + version + type + length + 1 data + checksum + newline */
    CHECK(12 == n, "one-byte frame is 12 bytes, got %u", (unsigned)n);
    CHECK(0 == memcmp(frame, "IMPROV", 6), "magic");
    CHECK(1 == frame[6], "version");
    CHECK(EZPI_IMPROV_TYPE_CURRENT_STATE == frame[7], "type");
    CHECK(1 == frame[8], "length");
    CHECK(0x02 == frame[9], "payload");

    /* The checksum covers the magic and header too, not just the payload --
     * this is the part the published specification does not state. */
    CHECK(EZPI_improv_checksum(frame, 10) == frame[10], "checksum covers magic and header");

    /* Computed over the payload alone it would be 0x02; proving it differs is
     * what makes the check above meaningful. */
    CHECK(0x02 != frame[10], "checksum is not the payload-only sum");

    /* Present in every frame the reference emits, and absent from the prose
     * specification, which claims there is no newline terminator. */
    CHECK('\n' == frame[11], "frame ends with a newline");

    /* The newline must not be inside the checksum. */
    CHECK(EZPI_improv_checksum(frame, 11) != frame[10], "newline excluded from checksum");
}

static void test_frame_build_limits(void)
{
    uint8_t frame[EZPI_IMPROV_FRAME_BUF_MAX];
    uint8_t big[EZPI_IMPROV_DATA_MAX + 1];
    memset(big, 'x', sizeof(big));

    CHECK(0 == EZPI_improv_frame_build(NULL, sizeof(frame), EZPI_IMPROV_TYPE_RPC_RESULT,
                                       big, 1),
          "NULL destination refused");

    CHECK(0 == EZPI_improv_frame_build(frame, sizeof(frame), EZPI_IMPROV_TYPE_RPC_RESULT,
                                       NULL, 1),
          "NULL payload with non-zero length refused");

    CHECK(0 == EZPI_improv_frame_build(frame, sizeof(frame), EZPI_IMPROV_TYPE_RPC_RESULT,
                                       big, EZPI_IMPROV_DATA_MAX + 1),
          "payload over the one-byte length field refused");

    /* One byte short of what a 1-byte payload needs. */
    CHECK(0 == EZPI_improv_frame_build(frame, 11, EZPI_IMPROV_TYPE_RPC_RESULT, big, 1),
          "undersized destination refused");

    CHECK(12 == EZPI_improv_frame_build(frame, 12, EZPI_IMPROV_TYPE_RPC_RESULT, big, 1),
          "exactly-sized destination accepted");

    /* A zero-length payload is legal: it is how an empty scan result ends. */
    const uint16_t n = EZPI_improv_frame_build(frame, sizeof(frame),
                                               EZPI_IMPROV_TYPE_RPC_RESULT, NULL, 0);
    CHECK(11 == n, "zero-length frame is 11 bytes, got %u", (unsigned)n);
    CHECK(0 == frame[8], "zero length field");
    CHECK('\n' == frame[10], "zero-length frame still ends with a newline");
}

static void test_rx_round_trip(void)
{
    uint8_t frame[EZPI_IMPROV_FRAME_BUF_MAX];
    s_ezpi_improv_rx_t rx;
    memset(&rx, 0, sizeof(rx));

    const uint8_t payload[] = {0x01, 0x00};
    const uint16_t n = EZPI_improv_frame_build(frame, sizeof(frame),
                                               EZPI_IMPROV_TYPE_RPC, payload, sizeof(payload));
    CHECK(0 != n, "frame built");

    unsigned int frames = 0;
    unsigned int bad = 0;
    uint16_t pass_len = 0;
    uint8_t pass[64];

    /* n - 1 so the trailing newline is not fed: the frame is complete at the
     * checksum, and the newline is the next thing on the stream. */
    const e_ezpi_improv_rx_result_t last = feed_all(&rx, frame, (uint16_t)(n - 1),
                                                    pass, sizeof(pass), &pass_len,
                                                    &frames, &bad);

    CHECK(EZPI_IMPROV_RX_FRAME == last, "complete frame recognised");
    CHECK(1 == frames, "exactly one frame, got %u", frames);
    CHECK(0 == bad, "no checksum failures");
    CHECK(0 == pass_len, "nothing passed through, got %u", (unsigned)pass_len);

    CHECK(EZPI_IMPROV_TYPE_RPC == EZPI_improv_rx_type(&rx), "type read back");

    uint16_t got_len = 0;
    const uint8_t *got = EZPI_improv_rx_payload(&rx, &got_len);
    CHECK(NULL != got, "payload available");
    CHECK(sizeof(payload) == got_len, "payload length %u", (unsigned)got_len);
    CHECK((NULL != got) && (0 == memcmp(got, payload, sizeof(payload))), "payload matches");
}

static void test_rx_payload_with_nul_and_newline(void)
{
    /* The two byte values that break the obvious-but-wrong implementations. A
     * NUL defeats treating the stream as a C string, which is exactly what the
     * existing UART service does to everything it reads; a newline defeats
     * splitting the stream into lines. Both are legal payload bytes. */
    uint8_t frame[EZPI_IMPROV_FRAME_BUF_MAX];
    s_ezpi_improv_rx_t rx;
    memset(&rx, 0, sizeof(rx));

    const uint8_t payload[] = {0x01, 0x00, 0x0A, 0x00, 0x0D, 0xFF};
    const uint16_t n = EZPI_improv_frame_build(frame, sizeof(frame),
                                               EZPI_IMPROV_TYPE_RPC, payload, sizeof(payload));
    CHECK(0 != n, "frame built");

    unsigned int frames = 0;
    const e_ezpi_improv_rx_result_t last = feed_all(&rx, frame, (uint16_t)(n - 1),
                                                    NULL, 0, NULL, &frames, NULL);

    CHECK(EZPI_IMPROV_RX_FRAME == last, "frame with NUL and newline in payload recognised");
    CHECK(1 == frames, "one frame");

    uint16_t got_len = 0;
    const uint8_t *got = EZPI_improv_rx_payload(&rx, &got_len);
    CHECK(sizeof(payload) == got_len, "full payload length survived, got %u", (unsigned)got_len);
    CHECK((NULL != got) && (0 == memcmp(got, payload, sizeof(payload))),
          "payload bytes survived unchanged");
}

static void test_rx_passthrough(void)
{
    /* Improv has to share UART0 with the existing newline-delimited JSON
     * protocol, so anything that is not a frame must come back out intact. */
    s_ezpi_improv_rx_t rx;
    memset(&rx, 0, sizeof(rx));

    const char *json = "{\"cmd\":4}\n";
    uint8_t pass[64];
    uint16_t pass_len = 0;
    unsigned int frames = 0;

    feed_all(&rx, (const uint8_t *)json, (uint16_t)strlen(json),
             pass, sizeof(pass), &pass_len, &frames, NULL);

    CHECK(strlen(json) == pass_len, "all JSON bytes passed through, got %u of %u",
          (unsigned)pass_len, (unsigned)strlen(json));
    CHECK(0 == memcmp(pass, json, strlen(json)), "JSON bytes unchanged");
    CHECK(0 == frames, "no frames found in JSON");
}

static void test_rx_passthrough_partial_magic(void)
{
    /* A capital I inside ordinary traffic starts a candidate match. The bytes
     * must be handed back once the match fails, not swallowed. */
    s_ezpi_improv_rx_t rx;
    memset(&rx, 0, sizeof(rx));

    const char *text = "{\"name\":\"IMPORTANT\"}";
    uint8_t pass[64];
    uint16_t pass_len = 0;
    unsigned int frames = 0;

    feed_all(&rx, (const uint8_t *)text, (uint16_t)strlen(text),
             pass, sizeof(pass), &pass_len, &frames, NULL);

    /* "IMPRO" matches five bytes of the magic before the T breaks it. */
    CHECK(strlen(text) == pass_len, "every byte handed back, got %u of %u",
          (unsigned)pass_len, (unsigned)strlen(text));
    CHECK((pass_len == strlen(text)) && (0 == memcmp(pass, text, strlen(text))),
          "a near-miss prefix is handed back in order and unchanged");
    CHECK(0 == frames, "no frame");
}

static void test_rx_restart_on_repeated_magic(void)
{
    /* A partial magic followed by a real frame. The I that begins the frame
     * breaks the partial match and also starts the next one, so it must be
     * both handed back once and kept once.
     *
     * Every prefix length from 1 to 6 is tried, and that is the point of the
     * loop rather than thoroughness for its own sake. This test originally
     * used only the one-byte prefix "I", and mutation testing showed that
     * deleting the deferred-restart branch entirely still passed: after a bail
     * on a one-byte prefix the state is already len == 1 with buf[0] == 'I',
     * which is byte-for-byte what the restart branch would have set up. The
     * one case the test covered was the one case where the logic is a no-op.
     * At prefix length 2 or more the two diverge -- without the restart the
     * stale prefix is still in the buffer, so the frame's M is compared
     * against magic[prefix] and the frame is lost.
     *
     * Prefix 6 is worth having for a second reason: there the breaking byte
     * lands on the version field rather than inside the magic, which is the
     * other call site of the bail helper. */
    for (uint16_t prefix = 1; prefix <= EZPI_IMPROV_MAGIC_LEN; prefix++)
    {
        uint8_t frame[EZPI_IMPROV_FRAME_BUF_MAX];
        const uint8_t payload[] = {0x02, 0x00};
        const uint16_t n = EZPI_improv_frame_build(frame, sizeof(frame),
                                                   EZPI_IMPROV_TYPE_RPC,
                                                   payload, sizeof(payload));
        CHECK(0 != n, "frame built for prefix %u", (unsigned)prefix);

        uint8_t stream[EZPI_IMPROV_MAGIC_LEN + EZPI_IMPROV_FRAME_BUF_MAX];
        memcpy(stream, EZPI_IMPROV_MAGIC, prefix);
        memcpy(&stream[prefix], frame, n);

        s_ezpi_improv_rx_t rx;
        memset(&rx, 0, sizeof(rx));

        uint8_t pass[64];
        uint16_t pass_len = 0;
        unsigned int frames = 0;
        unsigned int bad = 0;

        /* prefix + (n - 1): the stray bytes plus the frame without its
         * trailing newline, which is the next thing on the stream rather than
         * part of the frame. */
        const e_ezpi_improv_rx_result_t last =
            feed_all(&rx, stream, (uint16_t)(prefix + n - 1u),
                     pass, sizeof(pass), &pass_len, &frames, &bad);

        CHECK(EZPI_IMPROV_RX_FRAME == last,
              "frame after a %u-byte stray prefix is still recognised", (unsigned)prefix);
        CHECK(1 == frames, "exactly one frame after a %u-byte prefix, got %u",
              (unsigned)prefix, frames);
        CHECK(0 == bad, "no checksum failures after a %u-byte prefix", (unsigned)prefix);
        CHECK(prefix == pass_len, "exactly the %u stray bytes handed back, got %u",
              (unsigned)prefix, (unsigned)pass_len);
        CHECK((prefix == pass_len) && (0 == memcmp(pass, EZPI_IMPROV_MAGIC, prefix)),
              "the bytes handed back are the %u-byte stray prefix", (unsigned)prefix);

        /* The frame really was parsed, not merely counted. */
        uint16_t got_len = 0;
        const uint8_t *got = EZPI_improv_rx_payload(&rx, &got_len);
        CHECK(sizeof(payload) == got_len, "payload length after a %u-byte prefix, got %u",
              (unsigned)prefix, (unsigned)got_len);
        CHECK((NULL != got) && (0 == memcmp(got, payload, sizeof(payload))),
              "payload intact after a %u-byte prefix", (unsigned)prefix);
    }
}

static void test_rx_bad_version(void)
{
    s_ezpi_improv_rx_t rx;
    memset(&rx, 0, sizeof(rx));

    /* Version 2 does not exist. Guessing at a future layout would be worse
     * than handing the bytes back. */
    uint8_t stream[] = {'I', 'M', 'P', 'R', 'O', 'V', 0x02, 0x03, 0x00, 0x00};
    uint8_t pass[64];
    uint16_t pass_len = 0;
    unsigned int frames = 0;

    feed_all(&rx, stream, (uint16_t)sizeof(stream), pass, sizeof(pass), &pass_len,
             &frames, NULL);

    CHECK(0 == frames, "unknown version is not accepted as a frame");
    CHECK(sizeof(stream) == pass_len, "all bytes handed back, got %u of %u",
          (unsigned)pass_len, (unsigned)sizeof(stream));
}

static void test_rx_bad_checksum(void)
{
    uint8_t frame[EZPI_IMPROV_FRAME_BUF_MAX];
    const uint8_t payload[] = {0x02, 0x00};
    const uint16_t n = EZPI_improv_frame_build(frame, sizeof(frame),
                                               EZPI_IMPROV_TYPE_RPC, payload, sizeof(payload));
    CHECK(0 != n, "frame built");

    /* Corrupt the checksum only. */
    frame[n - 2] = (uint8_t)(frame[n - 2] ^ 0xFFu);

    s_ezpi_improv_rx_t rx;
    memset(&rx, 0, sizeof(rx));

    unsigned int frames = 0;
    unsigned int bad = 0;
    const e_ezpi_improv_rx_result_t last = feed_all(&rx, frame, (uint16_t)(n - 1),
                                                    NULL, 0, NULL, &frames, &bad);

    CHECK(EZPI_IMPROV_RX_BAD_CHECKSUM == last, "wrong checksum reported");
    CHECK(0 == frames, "no frame accepted");
    CHECK(1 == bad, "one checksum failure");

    /* The stream must be back in sync: a good frame straight afterwards is
     * accepted. A parser that rescanned the rejected bytes would not manage
     * this. */
    uint8_t good[EZPI_IMPROV_FRAME_BUF_MAX];
    const uint16_t gn = EZPI_improv_frame_build(good, sizeof(good),
                                                EZPI_IMPROV_TYPE_RPC, payload, sizeof(payload));
    frames = 0;
    const e_ezpi_improv_rx_result_t after = feed_all(&rx, good, (uint16_t)(gn - 1),
                                                     NULL, 0, NULL, &frames, NULL);
    CHECK(EZPI_IMPROV_RX_FRAME == after, "resynchronised after a bad checksum");
    CHECK(1 == frames, "the following frame was accepted");
}

static void test_rx_max_frame_then_more_input(void)
{
    /* Regression test for a one-past-the-end write. A maximal payload puts the
     * checksum at the last index of the buffer; the state machine used to keep
     * its length set to the finished frame, so the next byte wrote one past
     * the end. ASan is what catches it, hence the sanitizers in the Makefile. */
    uint8_t frame[EZPI_IMPROV_FRAME_BUF_MAX];
    uint8_t payload[EZPI_IMPROV_DATA_MAX];
    memset(payload, 0xAB, sizeof(payload));

    const uint16_t n = EZPI_improv_frame_build(frame, sizeof(frame),
                                               EZPI_IMPROV_TYPE_RPC,
                                               payload, (uint16_t)sizeof(payload));
    CHECK(EZPI_IMPROV_FRAME_MAX + 1u == n, "maximal frame is the full size, got %u",
          (unsigned)n);

    s_ezpi_improv_rx_t rx;
    memset(&rx, 0, sizeof(rx));

    unsigned int frames = 0;
    const e_ezpi_improv_rx_result_t last = feed_all(&rx, frame, (uint16_t)(n - 1),
                                                    NULL, 0, NULL, &frames, NULL);
    CHECK(EZPI_IMPROV_RX_FRAME == last, "maximal frame recognised");
    CHECK(1 == frames, "one frame");

    uint16_t got_len = 0;
    const uint8_t *got = EZPI_improv_rx_payload(&rx, &got_len);
    CHECK(EZPI_IMPROV_DATA_MAX == got_len, "maximal payload length, got %u", (unsigned)got_len);
    CHECK((NULL != got) && (0 == memcmp(got, payload, sizeof(payload))), "maximal payload matches");

    /* The byte that used to overflow. Feeding without an intervening reset is
     * the whole point: the state machine must recover on its own. */
    const uint8_t *pt = NULL;
    uint16_t pt_len = 0;
    const e_ezpi_improv_rx_result_t next = EZPI_improv_rx_feed(&rx, (uint8_t)'\n', &pt, &pt_len);
    CHECK(EZPI_IMPROV_RX_NOT_IMPROV == next, "trailing newline handed back, not consumed");
    CHECK(1 == pt_len, "one byte handed back, got %u", (unsigned)pt_len);

    /* And a whole new frame still parses afterwards. */
    frames = 0;
    const e_ezpi_improv_rx_result_t again = feed_all(&rx, frame, (uint16_t)(n - 1),
                                                     NULL, 0, NULL, &frames, NULL);
    CHECK(EZPI_IMPROV_RX_FRAME == again, "a second maximal frame parses");
    CHECK(1 == frames, "one more frame");
}

static void test_rx_split_across_reads(void)
{
    /* The transport reads whatever happened to arrive, so a frame is routinely
     * delivered in two pieces. Driving the parser a byte at a time from two
     * separate buffers is the same thing. */
    uint8_t frame[EZPI_IMPROV_FRAME_BUF_MAX];
    const uint8_t payload[] = {0x01, 0x05, 'h', 'o', 'm', 'e', '!', 0x00};
    const uint16_t n = EZPI_improv_frame_build(frame, sizeof(frame),
                                               EZPI_IMPROV_TYPE_RPC, payload, sizeof(payload));

    for (uint16_t split = 1; split < (n - 1); split++)
    {
        s_ezpi_improv_rx_t rx;
        memset(&rx, 0, sizeof(rx));

        unsigned int frames_a = 0;
        unsigned int frames_b = 0;

        feed_all(&rx, frame, split, NULL, 0, NULL, &frames_a, NULL);
        const e_ezpi_improv_rx_result_t last =
            feed_all(&rx, &frame[split], (uint16_t)((n - 1) - split),
                     NULL, 0, NULL, &frames_b, NULL);

        CHECK(EZPI_IMPROV_RX_FRAME == last, "frame split at %u still parses", (unsigned)split);
        CHECK(1 == (frames_a + frames_b), "exactly one frame when split at %u", (unsigned)split);
    }
}

static void test_rx_idle_reset(void)
{
    /* A half-arrived frame is abandoned on an idle timeout -- the only
     * resynchronisation the protocol has. After that the partial bytes are
     * gone, which is the intended behaviour, not a leak into the other
     * protocol: they were already consumed as a frame candidate. */
    s_ezpi_improv_rx_t rx;
    memset(&rx, 0, sizeof(rx));

    const uint8_t partial[] = {'I', 'M', 'P', 'R'};
    feed_all(&rx, partial, (uint16_t)sizeof(partial), NULL, 0, NULL, NULL, NULL);

    EZPI_improv_rx_reset(&rx);

    uint16_t got_len = 0;
    CHECK(NULL == EZPI_improv_rx_payload(&rx, &got_len), "no payload after a reset");
    CHECK(0 == got_len, "no payload length after a reset");

    /* A complete frame after the reset is accepted. */
    uint8_t frame[EZPI_IMPROV_FRAME_BUF_MAX];
    const uint8_t payload[] = {0x02, 0x00};
    const uint16_t n = EZPI_improv_frame_build(frame, sizeof(frame),
                                               EZPI_IMPROV_TYPE_RPC, payload, sizeof(payload));
    unsigned int frames = 0;
    feed_all(&rx, frame, (uint16_t)(n - 1), NULL, 0, NULL, &frames, NULL);
    CHECK(1 == frames, "frame after reset accepted");
}

static void test_rpc_parse_send_wifi(void)
{
    /* [cmd][len][ssid len]["home"][pass len]["secret"], serial form with no
     * payload checksum of its own. */
    const uint8_t payload[] = {
        EZPI_IMPROV_RPC_SEND_WIFI, 12,
        4, 'h', 'o', 'm', 'e',
        6, 's', 'e', 'c', 'r', 'e', 't'};

    s_ezpi_improv_rpc_t rpc;
    memset(&rpc, 0, sizeof(rpc));

    const e_ezpi_improv_error_t err =
        EZPI_improv_rpc_parse(payload, sizeof(payload), false, &rpc);

    CHECK(EZPI_IMPROV_ERROR_NONE == err, "valid send-wifi accepted, err %u", (unsigned)err);
    CHECK(EZPI_IMPROV_RPC_SEND_WIFI == rpc.command, "command");
    CHECK(4 == rpc.ssid_len, "ssid length %u", (unsigned)rpc.ssid_len);
    CHECK((NULL != rpc.ssid) && (0 == memcmp(rpc.ssid, "home", 4)), "ssid bytes");
    CHECK(6 == rpc.passphrase_len, "passphrase length %u", (unsigned)rpc.passphrase_len);
    CHECK((NULL != rpc.passphrase) && (0 == memcmp(rpc.passphrase, "secret", 6)),
          "passphrase bytes");

    /* Pointers must be into the caller's buffer, not copies. */
    CHECK(rpc.ssid == &payload[3], "ssid points into the caller's buffer");
}

static void test_rpc_parse_open_network(void)
{
    /* An empty passphrase is legal: open networks exist. An empty SSID is not. */
    const uint8_t open_net[] = {EZPI_IMPROV_RPC_SEND_WIFI, 7, 5, 'g', 'u', 'e', 's', 't', 0};
    s_ezpi_improv_rpc_t rpc;

    CHECK(EZPI_IMPROV_ERROR_NONE == EZPI_improv_rpc_parse(open_net, sizeof(open_net), false, &rpc),
          "empty passphrase accepted");
    CHECK(0 == rpc.passphrase_len, "passphrase length zero");

    const uint8_t no_ssid[] = {EZPI_IMPROV_RPC_SEND_WIFI, 2, 0, 0};
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(no_ssid, sizeof(no_ssid), false, &rpc),
          "empty SSID refused");
}

static void test_rpc_parse_hostile_lengths(void)
{
    s_ezpi_improv_rpc_t rpc;

    /* Declared payload length longer than what arrived. Trusting it and
     * indexing the data would read past the buffer. */
    const uint8_t over[] = {EZPI_IMPROV_RPC_SEND_WIFI, 200, 4, 'h', 'o', 'm', 'e'};
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(over, sizeof(over), false, &rpc),
          "declared length longer than the payload refused");

    /* Declared shorter than what arrived. */
    const uint8_t under[] = {EZPI_IMPROV_RPC_SEND_WIFI, 2, 4, 'h', 'o', 'm', 'e'};
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(under, sizeof(under), false, &rpc),
          "declared length shorter than the payload refused");

    /* SSID length that runs past the end of the data, so the passphrase length
     * byte would be read from outside the payload. */
    const uint8_t ssid_over[] = {EZPI_IMPROV_RPC_SEND_WIFI, 5, 9, 'h', 'o', 'm', 'e'};
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(ssid_over, sizeof(ssid_over), false, &rpc),
          "SSID length past the end refused");

    /* SSID length that exactly consumes the data, leaving no passphrase length
     * byte -- the off-by-one worth testing explicitly. */
    const uint8_t ssid_exact[] = {EZPI_IMPROV_RPC_SEND_WIFI, 5, 4, 'h', 'o', 'm', 'e'};
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(ssid_exact, sizeof(ssid_exact), false, &rpc),
          "SSID consuming the whole payload refused");

    /* Passphrase length that disagrees with the remaining bytes. */
    const uint8_t pass_over[] = {EZPI_IMPROV_RPC_SEND_WIFI, 7, 4, 'h', 'o', 'm', 'e', 9, 'x'};
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(pass_over, sizeof(pass_over), false, &rpc),
          "passphrase length past the end refused");

    /* Too short to hold a command and a length at all. */
    const uint8_t runt[] = {EZPI_IMPROV_RPC_SEND_WIFI};
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(runt, sizeof(runt), false, &rpc),
          "runt payload refused");

    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(NULL, 4, false, &rpc),
          "NULL payload refused");
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(runt, sizeof(runt), false, NULL),
          "NULL output refused");
}

static void test_rpc_parse_wifi_limits(void)
{
    s_ezpi_improv_rpc_t rpc;
    uint8_t payload[2 + 1 + EZPI_IMPROV_SSID_MAX + 1 + EZPI_IMPROV_PASSPHRASE_MAX + 4];

    /* An SSID one byte over the 802.11 maximum. Accepting it would have the
     * Wi-Fi stack truncate it and then report success for a network the client
     * never asked to join. */
    memset(payload, 'a', sizeof(payload));
    payload[0] = EZPI_IMPROV_RPC_SEND_WIFI;
    payload[2] = (uint8_t)(EZPI_IMPROV_SSID_MAX + 1u);
    payload[3 + EZPI_IMPROV_SSID_MAX + 1] = 0; /* zero-length passphrase */
    payload[1] = (uint8_t)(1u + (EZPI_IMPROV_SSID_MAX + 1u) + 1u);
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC ==
              EZPI_improv_rpc_parse(payload, (uint32_t)(payload[1] + 2u), false, &rpc),
          "over-long SSID refused");

    /* Exactly the maximum is accepted. */
    memset(payload, 'a', sizeof(payload));
    payload[0] = EZPI_IMPROV_RPC_SEND_WIFI;
    payload[2] = (uint8_t)EZPI_IMPROV_SSID_MAX;
    payload[3 + EZPI_IMPROV_SSID_MAX] = 0;
    payload[1] = (uint8_t)(1u + EZPI_IMPROV_SSID_MAX + 1u);
    CHECK(EZPI_IMPROV_ERROR_NONE ==
              EZPI_improv_rpc_parse(payload, (uint32_t)(payload[1] + 2u), false, &rpc),
          "maximum-length SSID accepted");
    CHECK(EZPI_IMPROV_SSID_MAX == rpc.ssid_len, "maximum SSID length recorded");

    /* A passphrase one byte over the WPA maximum. */
    memset(payload, 'b', sizeof(payload));
    payload[0] = EZPI_IMPROV_RPC_SEND_WIFI;
    payload[2] = 4;
    payload[3 + 4] = (uint8_t)(EZPI_IMPROV_PASSPHRASE_MAX + 1u);
    payload[1] = (uint8_t)(1u + 4u + 1u + (EZPI_IMPROV_PASSPHRASE_MAX + 1u));
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC ==
              EZPI_improv_rpc_parse(payload, (uint32_t)(payload[1] + 2u), false, &rpc),
          "over-long passphrase refused");
}

static void test_rpc_parse_checksum_mode(void)
{
    /* BLE carries the checksum in the payload itself; serial does not, because
     * the frame checksum already covers it. Getting this distinction wrong
     * would make one transport reject every command the other accepts. */
    uint8_t payload[] = {EZPI_IMPROV_RPC_GET_CURRENT_STATE, 0, 0};
    payload[2] = EZPI_improv_checksum(payload, 2);

    s_ezpi_improv_rpc_t rpc;

    CHECK(EZPI_IMPROV_ERROR_NONE == EZPI_improv_rpc_parse(payload, sizeof(payload), true, &rpc),
          "BLE form with a correct checksum accepted");
    CHECK(EZPI_IMPROV_RPC_GET_CURRENT_STATE == rpc.command, "command read");

    payload[2] = (uint8_t)(payload[2] ^ 0xFFu);
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(payload, sizeof(payload), true, &rpc),
          "BLE form with a wrong checksum refused");

    /* The same three bytes read as a serial payload are a length mismatch,
     * since serial expects no trailing byte. */
    CHECK(EZPI_IMPROV_ERROR_INVALID_RPC == EZPI_improv_rpc_parse(payload, sizeof(payload), false, &rpc),
          "serial form rejects an unexpected trailing byte");

    /* And the two-byte serial form parses. */
    const uint8_t serial_form[] = {EZPI_IMPROV_RPC_GET_CURRENT_STATE, 0};
    CHECK(EZPI_IMPROV_ERROR_NONE == EZPI_improv_rpc_parse(serial_form, sizeof(serial_form), false, &rpc),
          "serial form accepted");
}

static void test_result_builder(void)
{
    uint8_t buf[64];
    s_ezpi_improv_result_t r;

    EZPI_improv_result_init(&r, buf, sizeof(buf), EZPI_IMPROV_RPC_SEND_WIFI);
    CHECK(EZPI_improv_result_add_str(&r, "http://192.168.1.5"), "entry added");

    const uint16_t n = EZPI_improv_result_finish(&r, false);

    CHECK(0 != n, "result finished");
    CHECK(EZPI_IMPROV_RPC_SEND_WIFI == buf[0], "command");
    CHECK(19 == buf[1], "data length is the entry plus its length byte, got %u", (unsigned)buf[1]);
    CHECK(18 == buf[2], "entry length");
    CHECK(0 == memcmp(&buf[3], "http://192.168.1.5", 18), "entry bytes");
    CHECK(0 == buf[n - 1], "trailing byte is zero when the frame checksums the payload");
    CHECK(22 == n, "total length, got %u", (unsigned)n);

    /* The BLE form checksums the payload itself. */
    EZPI_improv_result_init(&r, buf, sizeof(buf), EZPI_IMPROV_RPC_SEND_WIFI);
    CHECK(EZPI_improv_result_add_str(&r, "x"), "entry added");
    const uint16_t m = EZPI_improv_result_finish(&r, true);
    CHECK(0 != m, "checksummed result finished");
    CHECK(EZPI_improv_checksum(buf, (uint32_t)(m - 1u)) == buf[m - 1],
          "trailing byte is the payload checksum");

    /* A NULL string is an empty entry, which is how "no redirect URL" is
     * spelled. */
    EZPI_improv_result_init(&r, buf, sizeof(buf), EZPI_IMPROV_RPC_SEND_WIFI);
    CHECK(EZPI_improv_result_add_str(&r, NULL), "NULL string is an empty entry");
    const uint16_t k = EZPI_improv_result_finish(&r, false);
    CHECK(4 == k, "command, length, empty entry, trailing byte, got %u", (unsigned)k);
    CHECK(1 == buf[1], "one byte of data");
    CHECK(0 == buf[2], "the entry is empty");

    /* Several entries, as a scan result would have. */
    EZPI_improv_result_init(&r, buf, sizeof(buf), EZPI_IMPROV_RPC_GET_WIFI_NETWORKS);
    CHECK(EZPI_improv_result_add_str(&r, "ssid"), "first entry");
    CHECK(EZPI_improv_result_add_str(&r, "-50"), "second entry");
    CHECK(EZPI_improv_result_add_str(&r, "YES"), "third entry");
    CHECK(0 != EZPI_improv_result_finish(&r, false), "multi-entry result finished");
    CHECK((4u + 1u) + (3u + 1u) + (3u + 1u) == buf[1], "data length covers all three entries");
}

static void test_result_builder_overflow(void)
{
    uint8_t buf[8];
    s_ezpi_improv_result_t r;

    /* Overflow is sticky, so a caller may append several entries and check
     * once at the end rather than after each. */
    EZPI_improv_result_init(&r, buf, sizeof(buf), EZPI_IMPROV_RPC_GET_WIFI_NETWORKS);
    CHECK(EZPI_improv_result_add_str(&r, "abc"), "entry that fits");
    CHECK(!EZPI_improv_result_add_str(&r, "far too long for this buffer"), "entry that does not fit");
    CHECK(!EZPI_improv_result_add_str(&r, "a"), "overflow is sticky");
    CHECK(0 == EZPI_improv_result_finish(&r, false), "overflowed result reports zero length");

    /* A buffer too small even for the fixed fields. */
    EZPI_improv_result_init(&r, buf, 2, EZPI_IMPROV_RPC_GET_WIFI_NETWORKS);
    CHECK(!EZPI_improv_result_add_str(&r, "a"), "nothing fits in a 2-byte buffer");
    CHECK(0 == EZPI_improv_result_finish(&r, false), "zero length");

    EZPI_improv_result_init(&r, NULL, sizeof(buf), EZPI_IMPROV_RPC_GET_WIFI_NETWORKS);
    CHECK(!EZPI_improv_result_add_str(&r, "a"), "NULL buffer refused");
    CHECK(0 == EZPI_improv_result_finish(&r, false), "zero length for a NULL buffer");

    /* An entry exactly filling the buffer, and one byte more. The trailing
     * byte has to be accounted for, which is the easy thing to forget -- this
     * case was written with the arithmetic off by one and the implementation
     * was right, so the numbers are spelled out here.
     *
     *   buf[0] command, buf[1] data length, buf[2] entry length,
     *   buf[3..6] four data bytes, buf[7] trailing byte = 8 bytes.
     */
    uint8_t exact[8];
    EZPI_improv_result_init(&r, exact, sizeof(exact), EZPI_IMPROV_RPC_GET_WIFI_NETWORKS);
    CHECK(EZPI_improv_result_add_str(&r, "abcd"), "cmd+len+entrylen+4+trailing = 8");
    CHECK(8 == EZPI_improv_result_finish(&r, false), "exactly fills the buffer");

    EZPI_improv_result_init(&r, exact, sizeof(exact), EZPI_IMPROV_RPC_GET_WIFI_NETWORKS);
    CHECK(!EZPI_improv_result_add_str(&r, "abcde"), "one byte more does not fit");
}

static void test_result_then_frame(void)
{
    /* The two halves together, which is what the transport actually does:
     * build a result payload, wrap it in a frame, and have the parser get the
     * payload back unchanged. */
    uint8_t payload[64];
    uint8_t frame[EZPI_IMPROV_FRAME_BUF_MAX];
    s_ezpi_improv_result_t r;

    EZPI_improv_result_init(&r, payload, sizeof(payload), EZPI_IMPROV_RPC_GET_DEVICE_INFO);
    CHECK(EZPI_improv_result_add_str(&r, "EzloPi"), "firmware name");
    CHECK(EZPI_improv_result_add_str(&r, "1.0.0"), "version");
    CHECK(EZPI_improv_result_add_str(&r, "ESP32"), "chip");
    CHECK(EZPI_improv_result_add_str(&r, "EzloPi-1234"), "device name");
    const uint16_t plen = EZPI_improv_result_finish(&r, false);
    CHECK(0 != plen, "device info payload built");

    const uint16_t flen = EZPI_improv_frame_build(frame, sizeof(frame),
                                                  EZPI_IMPROV_TYPE_RPC_RESULT, payload, plen);
    CHECK(0 != flen, "device info frame built");

    s_ezpi_improv_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    unsigned int frames = 0;
    const e_ezpi_improv_rx_result_t last = feed_all(&rx, frame, (uint16_t)(flen - 1),
                                                    NULL, 0, NULL, &frames, NULL);
    CHECK(EZPI_IMPROV_RX_FRAME == last, "round trip parses");
    CHECK(1 == frames, "one frame");

    uint16_t got_len = 0;
    const uint8_t *got = EZPI_improv_rx_payload(&rx, &got_len);
    CHECK(plen == got_len, "payload length survived, %u vs %u", (unsigned)got_len, (unsigned)plen);
    CHECK((NULL != got) && (0 == memcmp(got, payload, plen)), "payload survived the round trip");
    CHECK(EZPI_IMPROV_TYPE_RPC_RESULT == EZPI_improv_rx_type(&rx), "type survived");
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

    test_checksum();
    test_frame_build();
    test_frame_build_limits();
    test_rx_round_trip();
    test_rx_payload_with_nul_and_newline();
    test_rx_passthrough();
    test_rx_passthrough_partial_magic();
    test_rx_restart_on_repeated_magic();
    test_rx_bad_version();
    test_rx_bad_checksum();
    test_rx_max_frame_then_more_input();
    test_rx_split_across_reads();
    test_rx_idle_reset();
    test_rpc_parse_send_wifi();
    test_rpc_parse_open_network();
    test_rpc_parse_hostile_lengths();
    test_rpc_parse_wifi_limits();
    test_rpc_parse_checksum_mode();
    test_result_builder();
    test_result_builder_overflow();
    test_result_then_frame();

    printf("%u check(s), %u failure(s)\n", tests_run, tests_failed);

    return (0 == tests_failed) ? 0 : 1;
}
