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
 * @file    ezlopi_core_sntp.h
 * @brief   perform some function on sntp
 * @author  Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 0.1
 * @date    12th DEC 2024
 */

#ifndef _EZLOPI_CORE_SNTP_H_
#define _EZLOPI_CORE_SNTP_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include <time.h>
#include "ezlopi_core_errors.h"

/*******************************************************************************
 *                          C++ Declaration Wrapper
 *******************************************************************************/
#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/
    /**
     * @brief Function to initialize sntp service
     *
     * @return ezlopi_error_t
     */
    ezlopi_error_t EZPI_core_sntp_init(void);
    /**
     * @brief Install the stored timezone into the process environment.
     *
     * Reads the location saved by EZPI_core_sntp_set_location, resolves it to a
     * POSIX TZ string, and applies it with setenv()+tzset(). Falls back to GMT0
     * when nothing is stored or the stored name is not in the zone table.
     *
     * Must be called once at boot, before anything calls localtime(). TZ is
     * process-global and libc reads it through tzset(), so every localtime()
     * caller in the firmware shares one setting -- including the four
     * time-of-day scene evaluators in ezlopi_core_scenes_when_methods.c, which
     * do not go anywhere near this component. Nothing used to set TZ at boot:
     * it was installed only as a side effect of EZPI_core_sntp_get_local_time,
     * whose one caller is hub.info.get. So until a client happened to ask for
     * hub.info.get, every scene ran on UTC.
     *
     * Safe to call when no network is up: resolving a name to a POSIX string is
     * a table lookup, and needs neither SNTP nor a synchronised clock.
     *
     * @return EZPI_SUCCESS when a stored zone was resolved and applied,
     *         EZPI_ERR_SNTP_LOCATION_SET_FAILED when GMT0 was applied instead.
     */
    ezlopi_error_t EZPI_core_sntp_apply_stored_timezone(void);
    /**
     * @brief Function to set location
     *
     * Validates the name against the zone table before storing it, and applies
     * it on success, so the stored location and the active TZ cannot disagree.
     *
     * @param location Pointer to target location
     * @return EZPI_SUCCESS, or EZPI_ERR_SNTP_LOCATION_SET_FAILED when the name
     *         is not a known timezone, and EZPI_ERR_NVS_WRITE_FAILED when it is
     *         known but could not be stored. Nothing is stored and the active TZ
     *         is left alone in either failure case.
     */
    ezlopi_error_t EZPI_core_sntp_set_location(const char *location);
    /**
     * @brief Function to get current location
     *
     * @return char*
     */
    char *EZPI_core_sntp_get_location(void);
    /**
     * @brief Function to get uptime
     *
     * @param up_time_buf Pointer to destination , to store extract data
     * @param buf_len Buffer len
     */
    void EZPI_core_sntp_get_up_time(char *up_time_buf, uint32_t buf_len);
    /**
     * @brief Function to get local time
     *
     * @param local_time_buf Pointer to local time buffer
     * @param buf_len Buffer len
     */
    void EZPI_core_sntp_get_local_time(char *local_time_buf, uint32_t buf_len);
    /**
     * @brief Function to get corresponding standard time
     *
     * @param time_buf Pointer to buffer to store extract val
     * @param buf_len buffer len
     * @param t Time val to convert
     */
    void EZPI_core_sntp_epoch_to_iso8601(char *time_buf, uint32_t buf_len, time_t t);
    /**
     * @brief Function to get current time in ms
     *
     * @return time_t
     */
    time_t EZPI_core_sntp_get_current_time_ms(void);
    /**
     * @brief Function to get current time in sec
     *
     * @return time_t
     */
    // time_t EZPI_core_sntp_get_current_time_sec(void);
    time_t EZPI_core_sntp_get_current_time_sec_abc(const char *filename);
#define EZPI_core_sntp_get_current_time_sec() EZPI_core_sntp_get_current_time_sec_abc(__FUNCTION__)

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_CORE_SNTP_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/