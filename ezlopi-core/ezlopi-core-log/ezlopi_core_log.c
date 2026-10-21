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
 * @file    ezlopi_core_log.c
 * @brief   Function operaters on system-logs
 * @author
 * @date    12th DEC 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "sdkconfig.h"

#ifdef CONFIG_EZPI_UTIL_TRACE_EN

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
/* <stdint.h> for the uint32_t that EZPI_core_read_set_log_severities() now
** reads NVS into. It used to arrive by accident -- nvs_flash.h pulls it in on
** target -- which is not a property of this file and not one it should lean
** on; a host test compiling this translation unit against an empty nvs_flash.h
** stub is the include path where that stopped being true. */
#include <stdint.h>

#include "cjext.h"

#include "ezlopi_core_nvs.h"
#include "ezlopi_core_heap.h"
#include "ezlopi_core_sntp.h"
#include "ezlopi_core_broadcast.h"
#include "ezlopi_core_event_group.h"
// #include "ezlopi_core_errors.h"

#include "ezlopi_cloud_constants.h"

#include "ezlopi_core_log.h"

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
static ezlopi_error_t ezlopi_hub_cloud_log_set_severity(const char *severity_str);
static ezlopi_error_t ezlopi_hub_serial_log_set_severity(const char *severity_str);
static ezlopi_error_t EZPI_core_serial_log_upcall(int severity, const char *log_str);
static bool __is_a_severity(int severity);
static const char *__severity_str(int severity);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static e_trace_severity_t cloud_log_severity = ENUM_EZLOPI_TRACE_SEVERITY_WARNING;

/* ENUM_EZLOPI_TRACE_SEVERITY_MAX is the LENGTH of ezlopi_log_severity_enum[]
** below, not a severity. This default used to be MAX, which made
** EZPI_core_serial_log_get_current_severity_enum_str() evaluate
** ezlopi_log_severity_enum[6] on a six-element array -- the array's length
** used as an index into it.
**
** It was not reachable on a booted hub, and that is worth being precise about
** rather than overstating. Both callers of EZPI_core_read_set_log_severities()
**
**     ezlopi_core_ezlopi.c:135-137
**     ezlopi_core_setting_commands.c:256-258
**
** follow it immediately and unconditionally with
** EZPI_core_read_set_log_severities_internal(ENUM_EZLOPI_TRACE_SEVERITY_TRACE),
** so by the time anything reads the severity it is TRACE. What makes this a
** defect rather than a curiosity is the comment sitting above each of those
** two calls:
**
**     // #warning "remove this in release"
**
** The only thing standing between a cloud request and an out-of-bounds read is
** a debug override the source itself asks to have deleted. Delete it and the
** read goes live on any hub whose serial severity was never stored -- which is
** every hub out of the box, because EZPI_core_nvs_read_uint32() leaves its
** output parameter untouched when the key is absent.
**
** TRACE rather than NONE or WARNING, and the reason is that it is not a
** judgement call: TRACE is already what both boot paths force, so this makes
** the static default agree with the runtime default instead of inventing a
** third answer. It is also behaviour-identical for every severity that exists.
** The only consumer of this value is the (severity > NONE && severity <=
** serial_log_severity) gate in EZPI_core_serial_log_upcall(), so MAX and TRACE
** can differ only for a log emitted at severity exactly MAX, and nothing in
** the tree emits one -- the enum's own setters reject MAX at
** ezlopi_core_log.c:134 and :177, which is the clearest evidence that MAX was
** never meant to be a severity in the first place. */
static e_trace_severity_t serial_log_severity = ENUM_EZLOPI_TRACE_SEVERITY_TRACE;

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/
const char *ezlopi_log_severity_enum[ENUM_EZLOPI_TRACE_SEVERITY_MAX] = {
    "NONE",
    "ERROR",
    "WARNING",
    "INFO",
    "DEBUG",
    "TRACE"};

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
void EZPI_core_read_set_log_severities_internal(e_trace_severity_t severity)
{
    /* The last unguarded write to the index. Both of this function's callers
    ** pass ENUM_EZLOPI_TRACE_SEVERITY_TRACE today, so the check never fires --
    ** which is the point of adding it now rather than when a third caller
    ** arrives with something else.
    **
    ** Silent on rejection, deliberately. A TRACE_W here would route through
    ** ezlopi_util_trace.c straight back into EZPI_core_serial_log_upcall()
    ** below, i.e. this file logging about its own log configuration while
    ** halfway through changing it. There is no TRACE_* call anywhere in this
    ** translation unit and that is not an oversight. */
    if (__is_a_severity((int)severity))
    {
        serial_log_severity = severity;
    }
}

void EZPI_core_read_set_log_severities(void)
{
    /* Read into a uint32_t and validate before assigning, for three separate
    ** reasons that happen to share one fix:
    **
    ** 1. NVS holds a uint32_t and nothing narrowed it. The stored value
    **    survives a firmware downgrade, an enum renumbering and a partially
    **    written entry, and it was going straight into a variable used as an
    **    array index. Measured on a host: with 0xDEADBEEF in the entry, the
    **    unfixed version segfaults inside
    **    EZPI_core_cloud_log_get_current_severity_enum_str().
    **
    ** 2. These two calls used to pass &cloud_log_severity and
    **    &serial_log_severity -- e_trace_severity_t * -- to a parameter
    **    declared uint32_t *. On xtensa that is silent because size_t and the
    **    enum's underlying type are both unsigned int; on riscv32 it is
    **    -Wincompatible-pointer-types, and the esp32c3 build reported it at
    **    exactly these two lines. Writing through the pointer was always the
    **    type pun, not just a warning: the compiler is free to choose the
    **    enum's underlying type, and nothing here guaranteed it was four bytes
    **    wide. The temporary removes the pun rather than casting it away, which
    **    is what ezlopi_core_setting_commands.c:246 and :249 do to the same
    **    pattern -- and why those two do not warn while these two did.
    **
    ** 3. The return value was discarded, so a failed read was indistinguishable
    **    from a successful one. EZPI_core_nvs_read_uint32() does not touch its
    **    output parameter on failure, including when ezlopi_nvs_handle is NULL,
    **    so "the key is absent" silently meant "keep the initialiser" -- which
    **    only worked out because of the initialiser above. Now it is what the
    **    code says.
    **
    ** A value outside the enum leaves the static at whatever it already held.
    ** Clamping to the nearest valid severity was the alternative and is worse:
    ** it would turn an unreadable NVS entry into a confident claim about the
    ** hub's log level.
    **
    ** The bound is written as an unsigned comparison against the stored
    ** uint32_t rather than by casting it to int and reusing __is_a_severity():
    ** (int)0xDEADBEEF is an implementation-defined conversion, and a bounds
    ** check should not be the one place in the file that depends on one. */
    uint32_t stored = 0;

    if ((EZPI_SUCCESS == EZPI_core_nvs_read_cloud_log_severity(&stored)) &&
        (stored < (uint32_t)ENUM_EZLOPI_TRACE_SEVERITY_MAX))
    {
        cloud_log_severity = (e_trace_severity_t)stored;
    }

    stored = 0;

    if ((EZPI_SUCCESS == EZPI_core_nvs_read_serial_log_severity(&stored)) &&
        (stored < (uint32_t)ENUM_EZLOPI_TRACE_SEVERITY_MAX))
    {
        serial_log_severity = (e_trace_severity_t)stored;
    }
}

ezlopi_error_t EZPI_core_cloud_log_severity_process_str(bool severity_enable, const char *severity_str)
{
    ezlopi_error_t ret = EZPI_FAILED;
    if (severity_enable)
    {
        ezlopi_hub_cloud_log_set_severity(severity_str);
        ret = EZPI_SUCCESS;
    }
    else
    {
        cloud_log_severity = ENUM_EZLOPI_TRACE_SEVERITY_NONE;
        ret = EZPI_SUCCESS;
    }
    EZPI_core_nvs_write_cloud_log_severity(cloud_log_severity);
    return ret;
}

ezlopi_error_t EZPI_core_cloud_log_severity_process_id(const e_trace_severity_t severity_level_id)
{
    ezlopi_error_t ret = EZPI_FAILED;

    if ((ENUM_EZLOPI_TRACE_SEVERITY_MAX > severity_level_id) && (ENUM_EZLOPI_TRACE_SEVERITY_NONE <= severity_level_id))
    {
        if (severity_level_id <= ENUM_EZLOPI_TRACE_SEVERITY_WARNING)
        {
            cloud_log_severity = severity_level_id;
        }
        else
        {
            cloud_log_severity = ENUM_EZLOPI_TRACE_SEVERITY_WARNING;
        }
        EZPI_core_nvs_write_cloud_log_severity(cloud_log_severity);
        ret = EZPI_SUCCESS;
    }

    return ret;
}

const char **EZPI_core_cloud_log_get_severity_enums()
{
    return ezlopi_log_severity_enum;
}

ezlopi_error_t EZPI_core_serial_log_severity_process_str(const char *severity_str)
{
    ezlopi_error_t ret = EZPI_FAILED;
    if (severity_str)
    {
        ezlopi_hub_serial_log_set_severity(severity_str);
        ret = EZPI_SUCCESS;
    }
    else
    {
        serial_log_severity = ENUM_EZLOPI_TRACE_SEVERITY_NONE;
        ret = EZPI_SUCCESS;
    }
    EZPI_core_nvs_write_serial_log_severity(serial_log_severity);
    return ret;
}

ezlopi_error_t EZPI_core_serial_log_severity_process_id(const e_trace_severity_t severity_level_id)
{
    ezlopi_error_t ret = EZPI_FAILED;

    if ((ENUM_EZLOPI_TRACE_SEVERITY_MAX > severity_level_id) && (ENUM_EZLOPI_TRACE_SEVERITY_NONE <= severity_level_id))
    {
        serial_log_severity = severity_level_id;
        EZPI_core_nvs_write_serial_log_severity(serial_log_severity);
        ret = EZPI_SUCCESS;
    }

    return ret;
}

const char *EZPI_core_cloud_log_get_current_severity_enum_str()
{
    return __severity_str((int)cloud_log_severity);
}

const char *EZPI_core_serial_log_get_current_severity_enum_str()
{
    return __severity_str((int)serial_log_severity);
}

e_trace_severity_t EZPI_core_cloud_log_get_current_severity_enum_val()
{
    return cloud_log_severity;
}

e_trace_severity_t EZPI_core_serial_log_get_current_severity_enum_val()
{
    return serial_log_severity;
}

ezlopi_error_t EZPI_core_send_cloud_log(int severity, const char *log_str)
{
    ezlopi_error_t ret = EZPI_FAILED;

    /* `severity` is a plain int handed over by a caller, and the bottom of
    ** this function indexes ezlopi_log_severity_enum[] with it. The threshold
    ** comparison bounds it above; it bounded it below only by accident, because
    ** `int <= e_trace_severity_t` promotes the int to the enum's underlying
    ** type and -1 therefore became a huge unsigned that failed the test. That
    ** does work, and it is also the -Wsign-compare the esp32c3 build reports
    ** here -- but the C standard lets the implementation choose the enum's
    ** underlying type, so an index being in range rested on a choice no line
    ** of this file made. Both bounds are now explicit and the cast makes the
    ** comparison signed-vs-signed, which is the same outcome stated rather
    ** than inherited: every severity the old code admitted, it still admits. */
    if (__is_a_severity(severity) && (severity <= (int)EZPI_core_cloud_log_get_current_severity_enum_val()))
    {
        e_ezlopi_event_t event = EZPI_core_event_group_get_eventbit_status();
        if ((event & EZLOPI_EVENT_NMA_REG) == EZLOPI_EVENT_NMA_REG)
        {
            cJSON *cj_log_broadcast = cJSON_CreateObject(__FUNCTION__);
            if (cj_log_broadcast)
            {
                cJSON_AddStringToObject(__FUNCTION__, cj_log_broadcast, ezlopi_id_str, ezlopi_ui_broadcast_str);
                cJSON_AddStringToObject(__FUNCTION__, cj_log_broadcast, ezlopi_msg_subclass_str, "hub.log");
                cJSON *cj_result = cJSON_AddObjectToObject(__FUNCTION__, cj_log_broadcast, ezlopi_result_str);
                if (cj_result)
                {
                    time_t timestamp = EZPI_core_sntp_get_current_time_sec();

                    char timestamp_str[64];

                    EZPI_core_sntp_epoch_to_iso8601(timestamp_str, sizeof(timestamp_str), (time_t)timestamp);

                    size_t total_len = sizeof(timestamp_str) + strlen(log_str) + 2;
                    char message[total_len];
                    memset(message, 0, total_len);
                    snprintf(message, total_len, "%s: %s", timestamp_str, log_str);

                    cJSON_AddStringToObject(__FUNCTION__, cj_result, ezlopi_message_str, message);

                    char severity_str[10];
                    memset(severity_str, 0, 10);
                    snprintf(severity_str, 10, "%s", __severity_str(severity));
                    cJSON_AddStringToObject(__FUNCTION__, cj_result, ezlopi_severity_str, severity_str);
                    if (EZPI_SUCCESS != EZPI_core_broadcast_add_to_queue(cj_log_broadcast, timestamp))
                    {
                        cJSON_Delete(__FUNCTION__, cj_log_broadcast);
                    }
                    ret = EZPI_SUCCESS;
                }
            }
        }
    }
    return ret;
}

void EZPI_core_set_log_upcalls()
{
    ezlopi_util_set_log_upcalls(EZPI_core_send_cloud_log, EZPI_core_serial_log_upcall);
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/
/**
 * @brief   Is this int a severity, i.e. a valid index into
 *          ezlopi_log_severity_enum[]?
 *
 * ENUM_EZLOPI_TRACE_SEVERITY_MAX is the array's length, so the last valid
 * index is MAX - 1 and the comparison is strictly less-than. Written against
 * the symbols so renumbering the enum cannot invalidate it.
 */
static bool __is_a_severity(int severity)
{
    return ((severity >= (int)ENUM_EZLOPI_TRACE_SEVERITY_NONE) &&
            (severity < (int)ENUM_EZLOPI_TRACE_SEVERITY_MAX));
}

/**
 * @brief   ezlopi_log_severity_enum[severity], for a severity that may not be
 *          one.
 *
 * The three sites that index the table all reached it from a value this file
 * did not own: two statics loaded from NVS and one int parameter. They now go
 * through here.
 *
 * An out-of-range severity answers "NONE" rather than NULL, and that is a
 * choice about the callers rather than about this function. All three of them
 * hand the result straight to cJSON_AddStringToObject() or snprintf("%s") --
 * ezlopi_cloud_log.c:132, ezlopi_core_setting_commands.c:159 and :532, and
 * EZPI_core_send_cloud_log() below -- every one of which would read through a
 * NULL. Returning NULL would trade an out-of-bounds read for a null
 * dereference, which is not an improvement, and pushing the check out to five
 * call sites is how the original defect got five chances to happen.
 *
 * "NONE" is the honest answer of the six available: it is the entry that
 * claims nothing is being logged, which is the safest thing to report about a
 * severity the hub cannot identify. It should also be unreachable -- every
 * write path into both statics is now range-checked -- so if it is ever
 * observed on the wire, something above this function is wrong.
 */
static const char *__severity_str(int severity)
{
    return __is_a_severity(severity)
               ? ezlopi_log_severity_enum[severity]
               : ezlopi_log_severity_enum[ENUM_EZLOPI_TRACE_SEVERITY_NONE];
}

static ezlopi_error_t ezlopi_hub_cloud_log_set_severity(const char *severity_str)
{
    ezlopi_error_t ret = EZPI_FAILED;
    if (severity_str)
    {
        for (int i = 0; i < ENUM_EZLOPI_TRACE_SEVERITY_MAX; i++)
        {
            if (0 == strncmp(ezlopi_log_severity_enum[i], severity_str, strlen(ezlopi_log_severity_enum[i])))
            {
                if (i <= ENUM_EZLOPI_TRACE_SEVERITY_WARNING)
                {
                    cloud_log_severity = i;
                }
                else
                {
                    cloud_log_severity = ENUM_EZLOPI_TRACE_SEVERITY_WARNING;
                }
                ret = EZPI_SUCCESS;
                break;
            }
        }
    }
    return ret;
}

static ezlopi_error_t ezlopi_hub_serial_log_set_severity(const char *severity_str)
{
    ezlopi_error_t ret = EZPI_FAILED;
    if (severity_str)
    {
        for (int i = 0; i < ENUM_EZLOPI_TRACE_SEVERITY_MAX; i++)
        {
            if (0 == strncmp(ezlopi_log_severity_enum[i], severity_str, strlen(ezlopi_log_severity_enum[i])))
            {
                serial_log_severity = i;
                ret = EZPI_SUCCESS;
                break;
            }
        }
    }
    return ret;
}

static ezlopi_error_t EZPI_core_serial_log_upcall(int severity, const char *log_str)
{
    ezlopi_error_t ret = EZPI_FAILED;
    /* The cast is the -Wsign-compare the esp32c3 build reports here. Unlike
    ** the cloud gate above, the lower bound was already explicit and already
    ** correct -- `severity > NONE` excludes negatives as a signed comparison --
    ** so this is the warning fixed and nothing else: the admitted set is
    ** unchanged for every value of `severity`. */
    if ((severity > (int)ENUM_EZLOPI_TRACE_SEVERITY_NONE) && (severity <= (int)EZPI_core_serial_log_get_current_severity_enum_val()))
    {
        printf("%s\n", log_str);
        ret = EZPI_SUCCESS;
    }
    return ret;
}

#endif // CONFIG_EZPI_UTIL_TRACE_EN

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
