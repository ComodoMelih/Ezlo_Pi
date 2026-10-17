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
 * @file    ezlopi_core_sntp.c
 * @brief   perform some function on sntp
 * @author  Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 0.1
 * @date    12th DEC 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "zones.h"
#include "esp_sntp.h"

#include "ezlopi_core_nvs.h"
#include "ezlopi_core_sntp.h"
#include "ezlopi_core_errors.h"

/* The four includes above and below this comment all used to arrive by
** accident. This file included ezlopi_core_wifi.h without naming a single wifi
** or netif symbol, and behind that one line came everything it actually uses
** but never asked for: freertos/task.h for vTaskDelay and the port tick
** macros, and -- via cjext.h -- ezlopi_util_trace.h for the TRACE_* macros and
** EZLOPI_USER_CONFIG.h for ezlopi_free.
**
** So the wifi include was not dead, despite looking dead to a symbol search;
** it was load-bearing, and for things a grep for wifi symbols could not show.
** Removing it on that evidence alone would have broken the build. Each of the
** four is named directly now, which is what makes the wifi line genuinely
** droppable -- along with the esp_wifi and esp_netif types it dragged in. */
#include "ezlopi_util_trace.h"
#include "EZLOPI_USER_CONFIG.h"

#include "ezlopi_util_version.h"

/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static void EZPI_core_sntp_sync_time_call_back(struct timeval *tv);
static ezlopi_error_t __install_tz(const char *posix_str);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static time_t start_time = 0;

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
ezlopi_error_t EZPI_core_sntp_init(void)
{
    int retry = 0;
    const int retry_count = 10;

    TRACE_I("Initializing SNTP");

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    // esp_sntp_setservername(1, "ntp-b.nist.gov");
    esp_sntp_setservername(2, "ntp-wwv.nist.gov");
    sntp_set_time_sync_notification_cb(EZPI_core_sntp_sync_time_call_back);

    // sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);

    sntp_set_sync_interval(15 * 1000);
    esp_sntp_init();

    sntp_sync_status_t sync_status = SNTP_SYNC_STATUS_RESET;
    while ((sync_status == SNTP_SYNC_STATUS_RESET) && (++retry < retry_count))
    {
        sync_status = sntp_get_sync_status();
        TRACE_I("Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(700 / portTICK_PERIOD_MS);
    }
    return (sync_status == SNTP_SYNC_STATUS_COMPLETED) ? EZPI_SUCCESS : EZPI_ERR_SNTP_INIT_FAILED;
}

ezlopi_error_t EZPI_core_sntp_apply_stored_timezone(void)
{
    ezlopi_error_t error = EZPI_SUCCESS;

    char *location = EZPI_core_nvs_read_time_location();
    const char *posix_str = (location) ? micro_tz_db_get_posix_str(location) : NULL;

    if (NULL == posix_str)
    {
        /* Naming what to do about it, rather than silently standing in for a
        ** choice the user never made. GMT0 is applied because localtime() has
        ** to mean something, but it is announced: a hub quietly running on UTC
        ** reports plausible times for the wrong place, and a scene set for 7am
        ** fires at 7am UTC without anything looking broken. */
        TRACE_W("timezone: %s -- applying GMT0; set one with hub.location.set",
                (location) ? "stored location is not in the zone table" : "none stored");
        posix_str = "GMT0";
        error = EZPI_ERR_SNTP_LOCATION_SET_FAILED;
    }

    if (EZPI_SUCCESS != __install_tz(posix_str))
    {
        error = EZPI_ERR_SNTP_LOCATION_SET_FAILED;
    }

    if (location)
    {
        ezlopi_free(__FUNCTION__, location);
    }

    return error;
}

ezlopi_error_t EZPI_core_sntp_set_location(const char *location)
{
    ezlopi_error_t error = EZPI_SUCCESS;

    if (location)
    {
        /* Resolve before storing. This used to write NVS first and only then
        ** look the name up, so an unknown timezone was persisted anyway: after
        ** that, hub.location.get reported the bogus name while every
        ** localtime() caller silently ran on the GMT0 fallback. Two readers of
        ** one datum, two answers -- so the name is only stored once it is known
        ** to resolve. */
        const char *posix_str = micro_tz_db_get_posix_str(location);

        if (NULL == posix_str)
        {
            TRACE_W("timezone: '%s' is not a known timezone; not stored", location);
            error = EZPI_ERR_SNTP_LOCATION_SET_FAILED;
        }
        else if (EZPI_SUCCESS != EZPI_core_nvs_write_time_location(location, strlen(location)))
        {
            /* Deliberately not applied: the active TZ stays with what is still
            ** in flash, so a failed write leaves the two agreeing. */
            TRACE_E("timezone: '%s' resolved to %s but could not be stored", location, posix_str);
            error = EZPI_ERR_NVS_WRITE_FAILED;
        }
        else
        {
            TRACE_I("timezone: '%s' -> %s", location, posix_str);
            error = __install_tz(posix_str);
        }
    }
    else
    {
        error = EZPI_ERR_SNTP_LOCATION_SET_FAILED;
    }

    return error;
}

char *EZPI_core_sntp_get_location(void)
{
    return EZPI_core_nvs_read_time_location();
}

void EZPI_core_sntp_get_local_time(char *time_buf, uint32_t buf_len)
{
    if (time_buf && buf_len)
    {
        /* Formats only. This used to read NVS, resolve the zone and call
        ** setenv()+tzset() itself on every call, which made hub.info.get the
        ** only thing in the firmware that ever installed TZ -- and therefore
        ** made every other localtime() caller's correctness depend on whether a
        ** client had happened to ask for hub.info.get yet.
        **
        ** TZ is now installed by EZPI_core_sntp_apply_stored_timezone at boot
        ** and again after any successful hub.location.set, so there is one
        ** place that sets it and this reader agrees with the scene evaluators
        ** by construction rather than by both doing the same work. */
        struct tm timeinfo;
        time_t now = EZPI_core_sntp_get_current_time_sec();
        localtime_r(&now, &timeinfo);

        if (strftime(time_buf, buf_len, "%Y-%m-%dT%H:%M:%S%z", &timeinfo))
        {
            TRACE_I("local time: %s", time_buf);
        }
        else
        {
            TRACE_E("local time: strftime failed into %u bytes", (unsigned)buf_len);
        }
    }
}

void EZPI_core_sntp_get_up_time(char *up_time_buf, uint32_t buf_len)
{
    if (up_time_buf && buf_len)
    {
        struct tm timeinfo;
        localtime_r(&start_time, &timeinfo);
        strftime(up_time_buf, buf_len, "%c", &timeinfo);
    }
}

void EZPI_core_sntp_epoch_to_iso8601(char *time_buf, uint32_t buf_len, time_t t)
{
    if (time_buf && buf_len)
    {
        struct tm *timeinfo;
        // timeinfo = gmtime(&t);
        timeinfo = localtime(&t);
        strftime(time_buf, buf_len, "%Y-%m-%dT%H:%M:%S%z", timeinfo);
        // TRACE_I("Build Time: %s", time_buf);
    }
}

time_t EZPI_core_sntp_get_current_time_ms(void)
{
    return (EZPI_core_sntp_get_current_time_sec() * 1000LLU);
}

time_t EZPI_core_sntp_get_current_time_sec_abc(const char *function_name)
{
    time_t now = 0;
    int retries = 3;

    while (retries--)
    {
        time(&now);

        /* The upper bound this used to carry -- now < BUILD_DATE + 1576800000lu,
        ** fifty years past the build -- was unreachable, and not by a little.
        ** 1738320733 + 1576800000 is 3315120733, and time_t on every target
        ** this project builds for is 4-byte signed: measured, not assumed, with
        ** a _Static_assert compiled under all four of xtensa-esp32-elf-gcc,
        ** xtensa-esp32s2-elf-gcc, xtensa-esp32s3-elf-gcc and riscv32-esp-elf-gcc.
        ** So the largest value `now` can hold is 2147483647 and the bound sits
        ** 1.17 billion seconds beyond it.
        **
        ** Because the literal was unsigned, `now` was converted to unsigned long
        ** for the comparison, which made it true for every input rather than
        ** false: positive clocks compare below 3315120733 directly, and a
        ** negative one wraps to at least 2147483648 and still compares below it.
        ** The left-hand test is what rejected those. The whole right-hand half
        ** of the window contributed nothing on any target, in any case.
        **
        ** It is not replaced with a smaller number, because a representable
        ** upper bound would be a new policy rather than a repair -- and a narrow
        ** one, since a signed 32-bit time_t expires in January 2038, about
        ** thirteen years past this BUILD_DATE. What is left is the half that
        ** worked: reject a clock that predates the firmware, which is what an
        ** unsynchronised ESP32 reports. */
        if (now > (time_t)BUILD_DATE)
        {
            break;
        }
        else
        {
            vTaskDelay(5 / portTICK_RATE_MS);
        }
    }
    return now;
}

/**
 * @brief Install a POSIX TZ string as the process timezone.
 *
 * The one place in this component that calls setenv("TZ")+tzset(). Both matter:
 * setenv alone changes the environment without libc noticing, because libc
 * caches the parsed zone and only re-reads it in tzset().
 *
 * @param posix_str a POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
 * @return EZPI_SUCCESS, or EZPI_ERR_SNTP_LOCATION_SET_FAILED if setenv failed
 */
static ezlopi_error_t __install_tz(const char *posix_str)
{
    ezlopi_error_t error = EZPI_SUCCESS;

    if (0 != setenv("TZ", posix_str, 1))
    {
        TRACE_E("timezone: setenv(TZ=%s) failed", posix_str);
        error = EZPI_ERR_SNTP_LOCATION_SET_FAILED;
    }
    else
    {
        tzset();
    }

    return error;
}

static void EZPI_core_sntp_sync_time_call_back(struct timeval *tv)
{
    char strftime_buf[64];
    struct tm timeinfo;

    sntp_set_sync_interval(60 * 60 * 1000); // Sync every hour
    TRACE_I("Notification of a time synchronization event");

    time_t now = EZPI_core_sntp_get_current_time_sec();
    if (0 == start_time)
    {
        start_time = now;
    }

    localtime_r(&now, &timeinfo);

    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    TRACE_I("Time now[%ld]: %s", now, strftime_buf);
}
