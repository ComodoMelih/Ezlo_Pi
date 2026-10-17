/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    esp_sntp.h
 * @brief   Host-test stub for ESP-IDF's SNTP client, for test_tz.
 *
 * WHAT IS BEING TESTED AND WHAT IS NOT
 *
 * test_tz compiles the real ezlopi_core_sntp.c to test one thing: that the
 * stored timezone is resolved and installed into the process, so that every
 * localtime() caller in the firmware agrees about the wall clock. That is
 * testable on a host precisely because setenv("TZ")/tzset()/localtime() are
 * the host's own libc -- the test asserts a real conversion of a real epoch,
 * not that a function was called.
 *
 * None of that touches SNTP. The network half -- whether a time server
 * answers, whether the clock gets set -- is not tested here and could not
 * usefully be: it would be testing ESP-IDF and a UDP round trip. So these are
 * names, not behaviour, and they exist only so that EZPI_core_sntp_init()
 * compiles in the same translation unit as the functions under test.
 *
 * The one piece of behaviour worth having is the sync status. Returning
 * SNTP_SYNC_STATUS_RESET forever makes EZPI_core_sntp_init() take its retry
 * path to exhaustion and return EZPI_ERR_SNTP_INIT_FAILED, which is the
 * outcome a test can assert without pretending a server replied. A stub that
 * returned COMPLETED would be asserting a fiction.
 */

#ifndef _HOST_TEST_SNTP_ESP_SNTP_H_
#define _HOST_TEST_SNTP_ESP_SNTP_H_

#include <stdint.h>
#include <sys/time.h>

typedef enum
{
    SNTP_SYNC_STATUS_RESET,
    SNTP_SYNC_STATUS_COMPLETED,
    SNTP_SYNC_STATUS_IN_PROGRESS,
} sntp_sync_status_t;

typedef enum
{
    ESP_SNTP_OPMODE_POLL,
    ESP_SNTP_OPMODE_LISTENONLY,
} esp_sntp_operatingmode_t;

typedef void (*sntp_sync_time_cb_t)(struct timeval *tv);

/* Recorded so the test can show EZPI_core_sntp_init() was reached and how
 * many times it polled, rather than inferring it from a return value. */
extern unsigned fake_sntp_init_calls;
extern unsigned fake_sntp_status_polls;
extern sntp_sync_time_cb_t fake_sntp_notify_cb;

static inline void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t mode)
{
    (void)mode;
}

static inline void esp_sntp_setservername(unsigned idx, const char *server)
{
    (void)idx;
    (void)server;
}

static inline void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t cb)
{
    fake_sntp_notify_cb = cb;
}

static inline void sntp_set_sync_interval(uint32_t interval_ms)
{
    (void)interval_ms;
}

static inline void esp_sntp_init(void)
{
    fake_sntp_init_calls++;
}

static inline sntp_sync_status_t sntp_get_sync_status(void)
{
    fake_sntp_status_polls++;
    return SNTP_SYNC_STATUS_RESET;
}

#endif // _HOST_TEST_SNTP_ESP_SNTP_H_
