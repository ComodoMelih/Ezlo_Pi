/**
 * @file    test_pms5003.c
 * @brief   Tests for the PMS5003 particulate-sensor UART frame parser.
 *
 * This compiles the real
 * ezlopi-sensors-devices/sensor-0037-pms5003-sensor/pms5003_parse.c -- not a
 * copy of it. That file exists because the rest of the driver cannot be built
 * on a host: pms5003.c configures GPIOs, drives a fan-control pin and creates
 * an esp_timer. Splitting the frame parser out is what makes the part worth
 * testing reachable, and is the pattern the remaining UART drivers still need.
 *
 * Unlike test_ze08.c, the upcall here is an extern symbol rather than one
 * captured through a HAL stand-in, because pms_init() -- the function that
 * would register it -- is the hardware half. The upcall is still called the way
 * the HAL calls it, through a s_ezlopi_uart_object handle whose .arg points at
 * the destination struct, so the handle-dereference path is covered.
 *
 * What is being regression-tested:
 *
 *   - test_short_frame_lengths() covers a 32-byte read from a heap block
 *     holding fewer than 32 bytes. The checksum walk covers indices 0..29 and
 *     the endian fix-up reads index 31, all unconditionally, while output_len
 *     was a parameter of the upcall that was never passed down. A 32-byte frame
 *     split across two UART events is the ordinary way this happens on a busy
 *     line, not a malformed-input case. Caught by ASan, which is why the
 *     buffers below are exact-size heap allocations: a short frame inside a
 *     64-byte stack array would pass while being the same defect.
 *
 *   - test_valid_frame() and the sweep pin the accept path, so that "reject
 *     everything" is not a passing fix for the above.
 *
 * Deliberately has no test-framework dependency -- plain C and a counter -- to
 * match the rest of tests/host.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pms5003_parse.h"

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static unsigned int tests_run = 0;
static unsigned int tests_failed = 0;

/* Stands in for the handle the UART HAL passes up. Only .arg is read by the
 * upcall, but the real struct is used rather than a local look-alike so the
 * field cannot drift. */
static struct s_ezlopi_uart_object fake_handle;

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
 * @brief Build a well-formed 32-byte Plantower frame.
 *
 * The frame is laid out and checksummed here independently of the driver, from
 * the part's own rule: two start bytes, then thirteen big-endian 16-bit fields,
 * then a checksum that is the sum of the first thirty bytes. If this shared the
 * driver's code every frame would pass by construction and the suite would be
 * worth nothing.
 *
 * Note which bytes the checksum covers: indices 0..29, i.e. it does NOT include
 * the two checksum bytes themselves at 30 and 31.
 */
static void build_frame(uint8_t frame[32], const uint16_t fields[14])
{
    uint16_t sum = 0;

    memset(frame, 0, 32);

    frame[0] = 0x42; /* start byte 1 */
    frame[1] = 0x4D; /* start byte 2 */

    /* fields[0] is framelen, then the twelve measurements, then "unused".
     * Stored big-endian starting at index 2. */
    for (int i = 0; i < 14; i++)
    {
        frame[2 + i * 2] = (uint8_t)(fields[i] >> 8);
        frame[2 + i * 2 + 1] = (uint8_t)(fields[i] & 0xFFu);
    }

    for (int i = 0; i < 30; i++)
    {
        sum = (uint16_t)(sum + frame[i]);
    }

    frame[30] = (uint8_t)(sum >> 8);
    frame[31] = (uint8_t)(sum & 0xFFu);
}

/**
 * @brief A set of plausible field values, distinct so a mix-up is visible.
 */
static void default_fields(uint16_t fields[14])
{
    fields[0] = 28;   /* framelen        */
    fields[1] = 10;   /* pm10_standard   */
    fields[2] = 25;   /* pm25_standard   */
    fields[3] = 100;  /* pm100_standard  */
    fields[4] = 11;   /* pm10_env        */
    fields[5] = 26;   /* pm25_env        */
    fields[6] = 101;  /* pm100_env       */
    fields[7] = 300;  /* particles_03um  */
    fields[8] = 500;  /* particles_05um  */
    fields[9] = 1000; /* particles_10um  */
    fields[10] = 250; /* particles_25um  */
    fields[11] = 50;  /* particles_50um  */
    fields[12] = 10;  /* particles_100um */
    fields[13] = 0;   /* unused          */
}

/**
 * @brief Hand a buffer to the upcall through an exact-size heap allocation.
 *
 * Exact-size and heap are both load-bearing: the UART HAL allocates a block
 * sized to the bytes that arrived and frees it once the upcall returns, and
 * ASan can only see a read past the end when there is nothing valid past the
 * end.
 */
static void feed(const uint8_t *bytes, size_t len, PM25_AQI_Data *data)
{
    uint8_t *heap = NULL;

    memset(&fake_handle, 0, sizeof(fake_handle));
    fake_handle.arg = data;

    if (len > 0)
    {
        heap = (uint8_t *)malloc(len);
        if (NULL == heap)
        {
            printf("FAIL %s:%d: out of memory\n", __FILE__, __LINE__);
            tests_failed++;
            return;
        }
        memcpy(heap, bytes, len);
    }

    EZPI_pms5003_uart_upcall(heap, (uint32_t)len, &fake_handle);

    free(heap);
}

/*******************************************************************************
 *                          Test Cases
 *******************************************************************************/

/**
 * @brief A frame built to the part's own rule must be accepted and decoded.
 *
 * Checks the decoded fields rather than only the return code, which is what
 * catches an endian or offset mistake in the fix-up loop.
 */
static void test_valid_frame(void)
{
    PM25_AQI_Data data;
    uint16_t fields[14];
    uint8_t frame[32];

    memset(&data, 0, sizeof(data));
    default_fields(fields);
    build_frame(frame, fields);

    CHECK(ESP_OK == pms_read_upcall(frame, sizeof(frame), &data), "well-formed frame was rejected");

    CHECK(28 == data.framelen, "framelen: expected 28, got %u", data.framelen);
    CHECK(10 == data.pm10_standard, "pm10_standard: expected 10, got %u", data.pm10_standard);
    CHECK(25 == data.pm25_standard, "pm25_standard: expected 25, got %u", data.pm25_standard);
    CHECK(100 == data.pm100_standard, "pm100_standard: expected 100, got %u", data.pm100_standard);
    CHECK(11 == data.pm10_env, "pm10_env: expected 11, got %u", data.pm10_env);
    CHECK(26 == data.pm25_env, "pm25_env: expected 26, got %u", data.pm25_env);
    CHECK(101 == data.pm100_env, "pm100_env: expected 101, got %u", data.pm100_env);
    CHECK(300 == data.particles_03um, "particles_03um: expected 300, got %u", data.particles_03um);
    CHECK(500 == data.particles_05um, "particles_05um: expected 500, got %u", data.particles_05um);
    CHECK(1000 == data.particles_10um, "particles_10um: expected 1000, got %u", data.particles_10um);
    CHECK(250 == data.particles_25um, "particles_25um: expected 250, got %u", data.particles_25um);
    CHECK(50 == data.particles_50um, "particles_50um: expected 50, got %u", data.particles_50um);
    CHECK(10 == data.particles_100um, "particles_100um: expected 10, got %u", data.particles_100um);
}

/**
 * @brief Sweep one field across every 16-bit value.
 *
 * A single frame could pass by coincidence under a broken checksum; 65536
 * cannot. Also exercises the carry behaviour of the 16-bit sum, which a
 * single low-valued frame would never reach.
 */
static void test_valid_frame_sweep(void)
{
    unsigned int accepted = 0;
    unsigned int total = 0;

    for (unsigned int v = 0; v < 65536u; v++)
    {
        PM25_AQI_Data data;
        uint16_t fields[14];
        uint8_t frame[32];

        memset(&data, 0, sizeof(data));
        default_fields(fields);
        fields[2] = (uint16_t)v; /* pm25_standard */
        build_frame(frame, fields);

        total++;
        if (ESP_OK == pms_read_upcall(frame, sizeof(frame), &data))
        {
            accepted++;
            if (data.pm25_standard != (uint16_t)v)
            {
                CHECK(0, "pm25_standard round-trip failed at %u, got %u", v, data.pm25_standard);
                return;
            }
        }
    }

    CHECK(accepted == total, "%u of %u well-formed frames were rejected", total - accepted, total);
}

/**
 * @brief A frame whose checksum is wrong must be rejected.
 *
 * Without this, deleting the checksum test entirely would pass every other
 * case here -- it is what stops "accept everything" from being a valid fix.
 */
static void test_bad_checksum(void)
{
    PM25_AQI_Data data;
    uint16_t fields[14];
    uint8_t frame[32];

    memset(&data, 0, sizeof(data));
    default_fields(fields);
    build_frame(frame, fields);
    frame[31] = (uint8_t)(frame[31] + 1u);

    CHECK(ESP_FAIL == pms_read_upcall(frame, sizeof(frame), &data), "frame with a corrupt checksum was accepted");

    /* Corrupting a payload byte without fixing the checksum must also fail,
     * which catches a checksum over the wrong range of bytes. */
    memset(&data, 0, sizeof(data));
    default_fields(fields);
    build_frame(frame, fields);
    frame[5] = (uint8_t)(frame[5] ^ 0xFFu);

    CHECK(ESP_FAIL == pms_read_upcall(frame, sizeof(frame), &data), "frame with a corrupt payload byte was accepted");
}

/**
 * @brief The start byte must be checked.
 */
static void test_bad_start_byte(void)
{
    PM25_AQI_Data data;
    uint16_t fields[14];
    uint8_t frame[32];

    memset(&data, 0, sizeof(data));
    default_fields(fields);
    build_frame(frame, fields);
    frame[0] = 0x43; /* not 0x42 */

    CHECK(ESP_FAIL == pms_read_upcall(frame, sizeof(frame), &data), "frame with a bad start byte was accepted");
}

/**
 * @brief Every short length must be rejected without reading past the block.
 *
 * This is the case the clamp was added for. Lengths 1..31 are all shorter than
 * the 32 bytes the parser touches unconditionally. Against the unguarded
 * version ASan aborts the process on each one, so the result does not depend
 * on the return code being right.
 */
static void test_short_frame_lengths(void)
{
    uint16_t fields[14];
    uint8_t frame[32];

    default_fields(fields);
    build_frame(frame, fields);

    for (size_t len = 1; len < PMS_MIN_FRAME_LEN; len++)
    {
        PM25_AQI_Data data;

        memset(&data, 0, sizeof(data));
        feed(frame, len, &data);

        CHECK(0 == data.available, "a %zu-byte frame produced an available reading", len);
        CHECK(0 == data.pm25_standard, "a %zu-byte frame left a decoded value behind", len);
    }
}

/**
 * @brief A read of exactly PMS_MIN_FRAME_LEN must be accepted.
 *
 * Guards against the clamp being written as a strict inequality, which would
 * reject every real frame -- the part emits exactly 32 bytes. A fix that is
 * off by one here passes test_short_frame_lengths() and breaks the sensor.
 */
static void test_exact_minimum_length(void)
{
    PM25_AQI_Data data;
    uint16_t fields[14];
    uint8_t frame[32];

    memset(&data, 0, sizeof(data));
    default_fields(fields);
    build_frame(frame, fields);

    CHECK(ESP_OK == pms_read_upcall(frame, PMS_MIN_FRAME_LEN, &data), "a frame of exactly PMS_MIN_FRAME_LEN was rejected");
    CHECK(25 == data.pm25_standard, "exact-length frame decoded wrongly: got %u", data.pm25_standard);
}

/**
 * @brief A zero-length read must be dropped.
 */
static void test_zero_length(void)
{
    PM25_AQI_Data data;

    memset(&data, 0, sizeof(data));
    feed(NULL, 0, &data);

    CHECK(0 == data.available, "a zero-length read produced a reading");
}

/**
 * @brief A read longer than one frame must still parse the frame at offset 0.
 *
 * Confirms the guard is a minimum and not an equality test, which would
 * silently drop readings whenever the line coalesced two frames.
 */
static void test_longer_than_one_frame(void)
{
    PM25_AQI_Data data;
    uint16_t fields[14];
    uint8_t buf[64];

    memset(&data, 0, sizeof(data));
    memset(buf, 0xA5, sizeof(buf));
    default_fields(fields);
    build_frame(buf, fields);

    feed(buf, sizeof(buf), &data);

    CHECK(25 == data.pm25_standard, "a frame followed by extra bytes decoded wrongly: got %u", data.pm25_standard);
}

/**
 * @brief NULL buffer, NULL destination and NULL handle must not be dereferenced.
 */
static void test_null_arguments(void)
{
    PM25_AQI_Data data;
    uint16_t fields[14];
    uint8_t frame[32];

    memset(&data, 0, sizeof(data));
    default_fields(fields);
    build_frame(frame, fields);

    /* NULL buffer with a non-zero length -- what the HAL used to pass on a
     * break condition, where event.size was stale but the buffer was NULL. */
    CHECK(ESP_FAIL == pms_read_upcall(NULL, PMS_MIN_FRAME_LEN, &data), "a NULL buffer produced a reading");

    /* NULL destination. */
    CHECK(ESP_FAIL == pms_read_upcall(frame, PMS_MIN_FRAME_LEN, NULL), "a NULL destination was accepted");

    /* NULL handle through the upcall. */
    EZPI_pms5003_uart_upcall(frame, PMS_MIN_FRAME_LEN, NULL);
    CHECK(1, "a NULL handle did not fault");

    /* Handle present but carrying no user arg, which is what a driver that
     * failed to allocate its data struct would look like. */
    memset(&fake_handle, 0, sizeof(fake_handle));
    fake_handle.arg = NULL;
    EZPI_pms5003_uart_upcall(frame, PMS_MIN_FRAME_LEN, &fake_handle);
    CHECK(1, "a NULL user arg did not fault");
}

/**
 * @brief A rejected frame must not leave a stale reading behind.
 *
 * The parser zeroes its destination before validating, so a good frame
 * followed by a bad one reports nothing rather than repeating the old value.
 * Pinned because it is the behaviour the sensor's consumer depends on and it
 * is easy to "optimise" away by moving the memset after the checks.
 */
static void test_reject_clears_previous(void)
{
    PM25_AQI_Data data;
    uint16_t fields[14];
    uint8_t frame[32];

    memset(&data, 0, sizeof(data));
    default_fields(fields);
    build_frame(frame, fields);

    CHECK(ESP_OK == pms_read_upcall(frame, sizeof(frame), &data), "setup frame was rejected");
    CHECK(25 == data.pm25_standard, "setup frame decoded wrongly");

    frame[0] = 0x00; /* now invalid */
    CHECK(ESP_FAIL == pms_read_upcall(frame, sizeof(frame), &data), "invalid frame was accepted");
    CHECK(0 == data.pm25_standard, "a rejected frame left the previous reading in place: %u", data.pm25_standard);
}

/*******************************************************************************
 *                          Entry Point
 *******************************************************************************/
int main(void)
{
    test_valid_frame();
    test_valid_frame_sweep();
    test_bad_checksum();
    test_bad_start_byte();
    test_short_frame_lengths();
    test_exact_minimum_length();
    test_zero_length();
    test_longer_than_one_frame();
    test_null_arguments();
    test_reject_clears_previous();

    printf("%u check(s), %u failure(s)\n", tests_run, tests_failed);

    return (0 == tests_failed) ? 0 : 1;
}
