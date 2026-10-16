
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
 * @file    ezlopi_core_api.c
 * @brief   These function perform operation on API-methods
 * @author  Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 0.1
 * @date    12th DEC 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <time.h>

/* For esp_restart(), called below when `register` has failed five times. None
** of this file's own includes named it; the declaration arrived down this
** edge, traced through the IDF checkout rather than guessed at:
**
**   cjext.h -> EZLOPI_USER_CONFIG.h -> driver/uart.h -> freertos/task.h
**           -> freertos/portmacro.h -> esp_system.h
**
** Unconditional in both the xtensa and the riscv port, so it held for all four
** targets. It is also explicitly temporary at the far end: both ports carry
** IDF's own "required by esp_get_...() functions in portable.h.
** [refactor-todo] Update portable.h" against that very include -- xtensa
** portmacro.h line 50, riscv line 45. When portable.h is updated and the
** include goes, esp_restart() here becomes an implicit declaration returning
** int rather than a compile error, which is the quiet kind of breakage. Named
** here because this is the file that calls it. */
#include "esp_system.h"

// #include "ezlopi_util_trace.h"

#include "ezlopi_core_api.h"
#include "ezlopi_core_sntp.h"
#include "ezlopi_core_broadcast.h"
#include "ezlopi_core_api_methods.h"
#include "ezlopi_core_event_group.h"
#include "ezlopi_core_cjson_macros.h"

#include "ezlopi_cloud_constants.h"
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
static int __check_for_no_error(cJSON *cj_request);
static cJSON *__execute_method(cJSON *cj_request, f_method_func_t method_func);
/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
cJSON *EZPI_core_api_consume(const char *who, const char *payload, uint32_t len, time_t time_stamp)
{
    cJSON *cj_response = NULL;

    if (payload && len)
    {
        cJSON *cj_request = cJSON_ParseWithLength(who, payload, len);

        /* Do not swap the line above for the one below without reading this.
        **
        ** cJSON_ParseWithRefWithLength() avoids copying every string by
        ** pointing valuestring into the payload, which looks like free memory
        ** on a device this size. Two things in this tree depend on it not
        ** being used:
        **
        ** 1. A referenced string is terminated by its closing quote, not by a
        **    NUL. EZPI_STRNCMP_IF_EQUAL() is strncmp() over the longer of the
        **    two lengths, so it reads past the value and into the rest of the
        **    payload.
        ** 2. The two parsers disagree about str_value_len. parse_string()
        **    stores an allocation size, which for an unescaped string is
        **    strlen + 1; parse_string_with_ref() stores the exact length. The
        **    whole tree is written for the first convention -- see the note on
        **    EZPI_STRNCMP_IF_EQUAL in ezlopi_cloud_constants.h, which says to
        **    add one for the terminator, and every caller that passes
        **    strlen(literal) + 1 alongside str_value_len.
        **
        ** Together those mean method dispatch in ezlopi_core_api_methods.c
        ** stops matching anything: the recorded length is one short, the
        ** literal's is one long, strncmp() takes the longer, and the
        ** comparison runs off the end of the value into a quote. Every request
        ** would be answered "method not found".
        **
        ** Measured, not deduced: a parsed 128-character string reports
        ** str_value_len 129 through cJSON_ParseWithLength and 128 through the
        ** reference parser. */
        // cJSON *cj_request = cJSON_ParseWithRefWithLength(who, payload, len);
        if (cj_request)
        {
            cj_response = EZPI_core_api_consume_cjson(who, cj_request, time_stamp);
            cJSON_Delete(__FUNCTION__, cj_request);
        }
    }
    return cj_response;
}

cJSON *EZPI_core_api_consume_cjson(const char *who, cJSON *cj_request, time_t time_stamp)
{
    cJSON *cj_response = NULL;

    if (cj_request)
    {
        if (__check_for_no_error(cj_request))
        {
            cJSON *cj_id = cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_id_str);
            cJSON *cj_sender = cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_sender_str);
            cJSON *cj_method = cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_method_str);

            uint32_t method_id = EZPI_core_ezlopi_methods_search_in_list(cj_method);

            if (UINT32_MAX != method_id)
            {
                f_method_func_t method = EZPI_core_ezlopi_methods_get_by_id(method_id);
                if (method)
                {
                    cj_response = __execute_method(cj_request, method);

                    if (cj_response)
                    {
                        /* Only when the handler did not report one. This used
                        ** to be unconditional, and cjext's add_item_to_object()
                        ** ends in add_item_to_array() -- it appends, with no
                        ** replace and no duplicate check -- so a handler that
                        ** had already failed produced two "error" members on
                        ** the wire:
                        **
                        **   {"result":{...},"error":{"code":-32602,...},"error":null}
                        **
                        ** Which one a client sees depends on its parser, and
                        ** the two disagree. cjext's get_object_item() returns
                        ** the FIRST match, so C callers saw the failure. A
                        ** browser's JSON.parse keeps the LAST duplicate, as do
                        ** Python's json and Go's encoding/json, so every error
                        ** reply read as a success with error == null. That is
                        ** the direction that matters now there is a local web
                        ** UI: it would have reported a refused configuration as
                        ** saved.
                        **
                        ** Success replies still carry an explicit error: null,
                        ** so the contract a client checks is unchanged. */
                        if (NULL == cJSON_GetObjectItem(__FUNCTION__, cj_response, ezlopi_error_str))
                        {
                            cJSON_AddNullToObject(__FUNCTION__, cj_response, ezlopi_error_str);
                        }

                        cJSON_AddItemToObject(__FUNCTION__, cj_response, ezlopi_id_str, cJSON_Duplicate(__FUNCTION__, cj_id, true));
                        cJSON_AddItemToObject(__FUNCTION__, cj_response, ezlopi_sender_str, cJSON_Duplicate(__FUNCTION__, cj_sender, true));
                        cJSON_AddItemToObject(__FUNCTION__, cj_response, ezlopi_method_str, cJSON_Duplicate(__FUNCTION__, cj_method, true));
                    }
                }

                f_method_func_t updater = EZPI_core_ezlopi_methods_get_updater_by_id(method_id);
                if (updater)
                {
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
                    TRACE_W("updater function: %p", updater);
#endif

                    cJSON *cj_update_response = __execute_method(cj_request, updater);

                    if (cj_update_response)
                    {
                        // cJSON_AddNumberToObject(__FUNCTION__, cj_update_response, ezlopi_startTime_str, EZPI_core_sntp_get_current_time_sec());
                        // cJSON_AddNullToObject(__FUNCTION__, cj_update_response, ezlopi_error_str);

                        if (EZPI_SUCCESS != EZPI_core_broadcast_add_to_queue(cj_update_response, time_stamp))
                        {
                            cJSON_Delete(__FUNCTION__, cj_update_response);
                        }
                    }
                }
            }
            else
            {
                cj_response = __execute_method(cj_request, EZPI_core_ezlopi_methods_rpc_method_notfound);

                if (cj_response)
                {
                    cJSON_AddItemToObject(__FUNCTION__, cj_response, ezlopi_id_str, cJSON_Duplicate(__FUNCTION__, cj_id, true));
                    cJSON_AddItemToObject(__FUNCTION__, cj_response, ezlopi_sender_str, cJSON_Duplicate(__FUNCTION__, cj_sender, true));
                    cJSON_AddItemToObject(__FUNCTION__, cj_response, ezlopi_method_str, cJSON_Duplicate(__FUNCTION__, cj_method, true));
                }

#ifdef CONFIG_EZPI_UTIL_TRACE_EN
                CJSON_TRACE("x-cj_response", cj_response);
#endif
            }
        }

        /* No else. A request that arrived carrying its own `error` member is
        ** dropped without a reply, which is what __check_for_no_error() is for.
        **
        ** There was a TRACE_EN-only else here whose entire body was a
        ** cJSON_GetObjectItem() feeding a commented-out TRACE_E -- so with
        ** tracing on it computed a lookup it never read, and with tracing off
        ** it was an empty branch. The commented-out line could not be revived
        ** either: it referenced `len` and `payload`, which are parameters of
        ** EZPI_core_api_consume (above), not of this function, so uncommenting
        ** it would not compile. Plumbing them in to make it compile is the
        ** wrong fix twice over -- it would print the whole request body, which
        ** is the credential-logging class check_no_secret_logging.py exists to
        ** reject. If this branch ever needs a diagnostic, log the method name
        ** and the id, both of which are already in scope above. */
    }

    return cj_response;
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/
static cJSON *__execute_method(cJSON *cj_request, f_method_func_t method_func)
{
    cJSON *cj_response = NULL;
    if (method_func)
    {
        if (EZPI_core_ezlopi_methods_check_method_register(method_func))
        {
            method_func(cj_request, NULL);
            EZPI_core_event_group_set_event(EZLOPI_EVENT_NMA_REG);
        }
        else
        {
            cj_response = cJSON_CreateObject(__FUNCTION__);
            if (NULL != cj_response)
            {
                method_func(cj_request, cj_response);
            }
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
            else
            {
                /* This branch is the `else` of `if (NULL != cj_response)`, so
                ** cj_response is NULL by construction and the old message --
                ** TRACE_E("Error - cj_response: %d", (uint32_t)cj_response) --
                ** could only ever print 0. It was also %d against a uint32_t
                ** cast of a pointer, the invalidPrintfArgType_sint pattern;
                ** cppcheck does not see it because TRACE_E is an unexpanded
                ** macro to it. The fact worth logging is which allocation
                ** failed, and there are no arguments to get wrong. */
                TRACE_E("cJSON_CreateObject failed; dropping request without a reply");
            }
#endif
        }
    }

    return cj_response;
}

static uint32_t __register_fail_count = 0;

static int __check_for_no_error(cJSON *cj_request)
{
    int ret = 0;

    if (cj_request)
    {
        cJSON *cj_error = cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_error_str);
        if ((NULL == cj_error) || (cJSON_NULL == cj_error->type) ||
            ((NULL != cj_error->valuestring) && (EZPI_STRNCMP_IF_EQUAL(cj_error->valuestring, ezlopi_null_str, cj_error->str_value_len, 5))))
        {
            ret = 1;
        }
#if (1 == ENABLE_TRACE)
        else
        {
            TRACE_E("cj_error: %p, cj_error->type: %u, cj_error->value_string: %s", cj_error, cj_error->type, cj_error ? (cj_error->valuestring ? cj_error->valuestring : ezlopi_null_str) : ezlopi_null_str);
        }
#endif
        cJSON *cj_method = cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_method_str);
        if (cj_method && cj_method->valuestring)
        {
            if (0 == strncmp(cj_method->valuestring, "register", cj_method->str_value_len > 8 ? cj_method->str_value_len : 8))
            {
                __register_fail_count++;
                if (__register_fail_count > 5)
                {
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
                    TRACE_E("Registration failed for 5 times! Rebooting device!");
#endif
                    esp_restart();
                }
            }
        }
    }

    return ret;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
