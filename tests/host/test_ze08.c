/**
 * @file    test_ze08.c
 * @brief   Tests for the ZE08-CH2O UART frame parser.
 *
 * This compiles the real ezlopi-sensors-devices/sensor-0069-UART-ze08-ch02-gas-sensor
 * /ze08_ch2o.c -- not a copy of it -- which is possible because that driver's
 * only dependency on ESP-IDF is the type declarations in ezlopi_hal_uart.h. The
 * headers *underneath* that one are what tests/host/stubs replaces, so the real
 * ezlopi_hal_uart.h is the header being compiled and its callback signature and
 * struct layout cannot drift away from what this file assumes.
 *
 * The parser and its upcall are both static, so there is no symbol to call.
 * They are reached the same way the firmware reaches them: the test supplies
 * EZPI_hal_uart_init(), ZE08_ch2o_sensor_init() hands it the upcall pointer,
 * and the test keeps that pointer. Every case below therefore runs the exact
 * registration path the firmware runs.
 *
 * Two regression tests for defects found by reading this driver:
 *
 *   - test_valid_frame() covers the checksum helper, which had two nested
 *     loops with identical bounds and a shadowed induction variable, and the
 *     accumulate line written twice in the body. It therefore summed every
 *     byte fourteen times. A sweep of all 65536 concentration/range pairs put
 *     the rejection rate for well-formed frames at 99.61%, so this sensor had
 *     never reported a reading unless its byte-sum happened to be a multiple
 *     of 256. Any single-count loop passes this test; the old one does not.
 *
 *   - test_short_frame_*() cover a nine-byte read from a heap block holding
 *     fewer than nine bytes. output_len was a parameter of the upcall and was
 *     never passed down or consulted, while the checksum walk and the header
 *     test between them touch indices 0 to 8 unconditionally. A frame split
 *     across two UART events is the ordinary way a short read happens, not a
 *     malformed-input case. Caught here by ASan, which is why this suite is
 *     built with sanitizers on, and only because the buffers below are exact-
 *     size heap allocations -- a short frame inside a 16-byte stack array
 *     would pass while still being the same defect.
 *
 * Deliberately has no test-framework dependency -- plain C and a counter -- to
 * match the rest of tests/host.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ze08_ch2o.h"

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static unsigned int tests_run = 0;
static unsigned int tests_failed = 0;

/* Captured by the EZPI_hal_uart_init() below, which stands in for the UART
 * HAL. Holding the upcall pointer is the whole reason this stub exists. */
static __uart_upcall captured_upcall = NULL;
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
 *                          HAL stand-in
 *******************************************************************************/
/* The real one creates a queue, starts a task and configures a peripheral.
 * Here it only records what the driver registered. Returning non-NULL is what
 * makes ZE08_ch2o_sensor_init() report success, so the test can also check
 * that the driver propagates that. */
s_ezlopi_uart_object_handle_t EZPI_hal_uart_init(uint32_t baudrate, uint32_t tx, uint32_t rx, __uart_upcall upcall, void *arg)
{
    (void)baudrate;
    (void)tx;
    (void)rx;

    captured_upcall = upcall;

    memset(&fake_handle, 0, sizeof(fake_handle));
    fake_handle.arg = arg;
    fake_handle.upcall = upcall;

    return &fake_handle;
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * @brief The datasheet checksum: bytes 1..7 and the checksum byte sum to zero.
 *
 * Written out here independently of the driver so that a test frame is correct
 * by the part's rule rather than by whatever the driver happens to compute. If
 * this and the driver's helper were the same code, every frame would pass by
 * construction and the suite would be worthless.
 */
static uint8_t datasheet_checksum(const uint8_t frame[9])
{
    uint8_t sum = 0;
    for (int i = 1; i <= 7; i++)
    {
        sum += frame[i];
    }
    return (uint8_t)((uint8_t)(~sum) + 1u);
}

/**
 * @brief Build a well-formed frame carrying a given concentration in ppb.
 */
static void build_frame(uint8_t frame[9], uint16_t ppb)
{
    frame[0] = 0xFF;                      /* start                     */
    frame[1] = 0x17;                      /* gas name: CH2O            */
    frame[2] = 0x04;                      /* unit: ppb                 */
    frame[3] = 0x00;                      /* decimal places            */
    frame[4] = (uint8_t)(ppb >> 8);       /* concentration high        */
    frame[5] = (uint8_t)(ppb & 0xFFu);    /* concentration low         */
    frame[6] = 0x13;                      /* full range high (5000)    */
    frame[7] = 0x88;                      /* full range low            */
    frame[8] = datasheet_checksum(frame); /* checksum                  */
}

/**
 * @brief Hand a frame to the driver through an exact-size heap allocation.
 *
 * Exact-size and heap are both load-bearing. The UART HAL allocates a block
 * sized to the bytes that arrived and frees it after the upcall returns, so
 * this reproduces the real lifetime; and ASan can only see a read past the end
 * if there is nothing valid past the end, which a generously sized buffer
 * would quietly provide.
 */
static void feed(const uint8_t *bytes, size_t len, ze08_ch2o_sensor_data_t *data)
{
    uint8_t *heap = NULL;

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

    captured_upcall(heap, (uint32_t)len, &fake_handle);

    free(heap);
}

/*******************************************************************************
 *                          Test Cases
 *******************************************************************************/

/**
 * @brief The driver must register an upcall at all. Everything else depends
 *        on this, so if it ever stops happening the rest of the suite would
 *        pass vacuously rather than fail.
 */
static void test_registration(void)
{
    ze08_ch2o_sensor_data_t data;
    s_ezlopi_uart_t cfg;

    memset(&data, 0, sizeof(data));
    memset(&cfg, 0, sizeof(cfg));
    cfg.enable = true;
    cfg.baudrate = ZE08_CH2O_UART_BUAD_RATE;
    cfg.tx = ZE08_CH2O_TX_PIN;
    cfg.rx = ZE08_CH2O_RX_PIN;

    CHECK(true == ZE08_ch2o_sensor_init(&cfg, &data), "init should report success when the HAL returns a handle");
    CHECK(NULL != captured_upcall, "driver did not register an upcall");

    /* A disabled config must not touch the HAL at all. */
    captured_upcall = NULL;
    cfg.enable = false;
    CHECK(false == ZE08_ch2o_sensor_init(&cfg, &data), "init should fail when the config is disabled");
    CHECK(NULL == captured_upcall, "disabled config still registered an upcall");

    /* Leave the upcall captured for the remaining tests. */
    cfg.enable = true;
    (void)ZE08_ch2o_sensor_init(&cfg, &data);
}

/**
 * @brief A frame built to the part's own checksum rule must be accepted.
 *
 * This is the one that fails against the doubled-loop checksum.
 */
static void test_valid_frame(void)
{
    ze08_ch2o_sensor_data_t data;
    uint8_t frame[9];

    memset(&data, 0, sizeof(data));
    build_frame(frame, 1500);
    feed(frame, sizeof(frame), &data);

    CHECK(true == data.available, "well-formed frame was rejected");
    /* 1500 ppb -> 1.5 after the driver's divide by 1000. Compared with a
     * tolerance because the driver stores a float. */
    CHECK(data.ppm > 1.4999 && data.ppm < 1.5001, "expected 1.5 ppm, got %f", (double)data.ppm);
}

/**
 * @brief Sweep concentrations so the pass does not depend on one lucky frame.
 *
 * The doubled-loop checksum agreed with the real one only when the byte-sum
 * was a multiple of 256, so a single frame could in principle have passed by
 * coincidence. Sweeping every concentration removes that possibility: 65536
 * frames cannot all be coincidences.
 */
static void test_valid_frame_sweep(void)
{
    unsigned int accepted = 0;
    unsigned int total = 0;

    for (unsigned int hi = 0; hi < 256u; hi++)
    {
        for (unsigned int lo = 0; lo < 256u; lo++)
        {
            ze08_ch2o_sensor_data_t data;
            uint8_t frame[9];

            memset(&data, 0, sizeof(data));
            build_frame(frame, (uint16_t)((hi << 8) | lo));
            feed(frame, sizeof(frame), &data);

            total++;
            if (data.available)
            {
                accepted++;
            }
        }
    }

    CHECK(accepted == total, "%u of %u well-formed frames were rejected", total - accepted, total);
}

/**
 * @brief A frame whose checksum byte is wrong must be rejected.
 *
 * Without this, a checksum helper that always returned the byte it was
 * compared against -- or one deleted entirely -- would pass every other test
 * here. It is what stops "accept everything" from being a valid fix.
 */
static void test_bad_checksum(void)
{
    ze08_ch2o_sensor_data_t data;
    uint8_t frame[9];

    memset(&data, 0, sizeof(data));
    build_frame(frame, 1500);
    frame[8] = (uint8_t)(frame[8] + 1u);
    feed(frame, sizeof(frame), &data);

    CHECK(false == data.available, "frame with a corrupt checksum was accepted");

    /* Corrupting a payload byte without fixing the checksum must also fail,
     * which is the case that catches a helper summing the wrong range. */
    memset(&data, 0, sizeof(data));
    build_frame(frame, 1500);
    frame[4] = (uint8_t)(frame[4] ^ 0xFFu);
    feed(frame, sizeof(frame), &data);

    CHECK(false == data.available, "frame with a corrupt payload byte was accepted");
}

/**
 * @brief A frame with the wrong header must be rejected.
 */
static void test_bad_header(void)
{
    const int header_indices[3] = {0, 1, 2};

    for (int k = 0; k < 3; k++)
    {
        ze08_ch2o_sensor_data_t data;
        uint8_t frame[9];

        memset(&data, 0, sizeof(data));
        build_frame(frame, 1500);
        frame[header_indices[k]] = (uint8_t)(frame[header_indices[k]] ^ 0xFFu);
        /* Recompute so the checksum is not what rejects it. */
        frame[8] = datasheet_checksum(frame);
        feed(frame, sizeof(frame), &data);

        CHECK(false == data.available, "frame with a corrupt header byte %d was accepted", header_indices[k]);
    }
}

/**
 * @brief Every short length must be dropped without reading past the block.
 *
 * Lengths 1..8 are all shorter than the nine bytes the parser touches. Under
 * ASan each one aborts the process against the unguarded version, so this does
 * not rely on data.available to detect the fault.
 */
static void test_short_frame_lengths(void)
{
    uint8_t frame[9];

    build_frame(frame, 1500);

    for (size_t len = 1; len < 9; len++)
    {
        ze08_ch2o_sensor_data_t data;

        memset(&data, 0, sizeof(data));
        feed(frame, len, &data);

        CHECK(false == data.available, "a %zu-byte frame was accepted as a reading", len);
    }
}

/**
 * @brief A zero-length read must be dropped.
 *
 * The UART HAL no longer dispatches these, but this driver is reachable from
 * any future caller and the guard is cheap, so it is pinned here rather than
 * left to depend on a different file's behaviour.
 */
static void test_zero_length(void)
{
    ze08_ch2o_sensor_data_t data;

    memset(&data, 0, sizeof(data));
    feed(NULL, 0, &data);

    CHECK(false == data.available, "a zero-length read was accepted as a reading");
}

/**
 * @brief NULL buffer and NULL handle must not be dereferenced.
 */
static void test_null_arguments(void)
{
    ze08_ch2o_sensor_data_t data;
    uint8_t frame[9];

    memset(&data, 0, sizeof(data));
    build_frame(frame, 1500);

    /* NULL buffer with a non-zero length -- what the HAL used to pass on a
     * break condition, where event.size was stale but the buffer was NULL. */
    fake_handle.arg = &data;
    captured_upcall(NULL, 9, &fake_handle);
    CHECK(false == data.available, "a NULL buffer produced a reading");

    /* NULL handle. */
    captured_upcall(frame, 9, NULL);
    CHECK(true, "a NULL handle did not fault");

    /* Handle present but carrying no user arg, which is what a driver that
     * failed to allocate its data struct would look like. */
    fake_handle.arg = NULL;
    captured_upcall(frame, 9, &fake_handle);
    CHECK(true, "a NULL user arg did not fault");
}

/**
 * @brief A read longer than one frame must still parse the frame at offset 0.
 *
 * Confirms the length guard is a minimum and not an equality test, which would
 * silently drop every reading on a line busy enough to coalesce frames.
 */
static void test_longer_than_one_frame(void)
{
    ze08_ch2o_sensor_data_t data;
    uint8_t buf[20];

    memset(&data, 0, sizeof(data));
    memset(buf, 0xA5, sizeof(buf));
    build_frame(buf, 2500);
    feed(buf, sizeof(buf), &data);

    CHECK(true == data.available, "a frame followed by extra bytes was rejected");
    CHECK(data.ppm > 2.4999 && data.ppm < 2.5001, "expected 2.5 ppm, got %f", (double)data.ppm);
}

/*******************************************************************************
 *                          Entry Point
 *******************************************************************************/
int main(void)
{
    test_registration();

    if (NULL == captured_upcall)
    {
        printf("FAIL %s:%d: no upcall captured; the rest of the suite cannot run\n", __FILE__, __LINE__);
        return 1;
    }

    test_valid_frame();
    test_valid_frame_sweep();
    test_bad_checksum();
    test_bad_header();
    test_short_frame_lengths();
    test_zero_length();
    test_null_arguments();
    test_longer_than_one_frame();

    printf("%u check(s), %u failure(s)\n", tests_run, tests_failed);

    return (0 == tests_failed) ? 0 : 1;
}
