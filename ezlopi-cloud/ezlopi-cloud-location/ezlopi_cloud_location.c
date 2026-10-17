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
 * @file    ezlopi_cloud_location.c
 * @brief   Definitions for cloud location functions
 * @author  Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 1.0
 * @date    February 2nd, 2024 8:09 PM
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <string.h>
#include <time.h>

#include "cjext.h"
#include "zones.h"

#include "ezlopi_core_cjson_macros.h"
#include "ezlopi_core_errors.h"
#include "ezlopi_core_sntp.h"

#include "ezlopi_cloud_constants.h"
#include "ezlopi_util_trace.h"

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
static void __prepare_error(cJSON *cj_response, int error_code, const char *error_message);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
void EZPI_CLOUD_location_list(cJSON *cj_request, cJSON *cj_response)
{
    cJSON *cjson_result = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);
    if (cjson_result)
    {
        cJSON *json_array_tzs = cJSON_CreateArray(__FUNCTION__);

        if (NULL != json_array_tzs)
        {
            /* Every entry in the table, found by the terminator the table
            ** actually has. This was `while (i <= 50)` with the correct
            ** condition commented out directly above it, which published 51 of
            ** the 425 zones -- Africa/Abidjan through Africa/Tunis, stopping
            ** one short of Africa/Windhoek. Every zone in the Americas, Asia,
            ** Australia, Europe and the Pacific was missing, so for most of the
            ** world hub.location.list offered nothing that could be selected,
            ** and hub.location.set has no way to discover a name the list never
            ** returned.
            **
            ** The whole table is deliverable, which was measured rather than
            ** assumed, because the failure mode if it were not is silence: both
            ** websocket paths serialize a reply with cJSON_PrintPreallocated
            ** into the single 10240-byte buffer from EZPI_core_buffer_acquire
            ** (ezlopi_service_ws_server.c:850, ezlopi_service_webprov.c:495),
            ** and that returns false rather than truncating, after which
            ** neither path sends anything. All 425 names plus the id/sender/
            ** method/msg_id envelope serialize to 8006 bytes, 2234 under the
            ** buffer.
            **
            ** What that headroom is spent on is worth stating: `sender` is
            ** cJSON_Duplicate'd straight from the request
            ** (ezlopi_core_api.c:197), so the client chooses part of the reply
            ** size. At 51 entries a 9230-character sender still fit; at 425 the
            ** limit is 2248. Both are far past any real sender, and the 2248 is
            ** a measured number rather than a guess, but the margin did narrow.
            **
            ** The per-entry TRACE_E and vTaskDelay(1) are gone with it. The
            ** trace logged all 425 names at error severity to say it was
            ** looping, and the delay cost one tick per entry -- 1 ms on all
            ** four targets, which pin CONFIG_FREERTOS_HZ=1000 in their
            ** sdkconfig.defaults -- so the full list would have spent 425 ms
            ** yielding to build a few hundred small objects. */
            const micro_tz_db_pair *tz_list = get_tz_db();
            uint32_t i = 0;
            uint32_t added = 0;

            while (NULL != tz_list[i].name)
            {
                cJSON *json_location_name = cJSON_CreateString(__FUNCTION__, tz_list[i].name);

                if (NULL != json_location_name)
                {
                    cJSON_AddItemToArray(json_array_tzs, json_location_name);
                    added++;
                }
                i++;
            }

            if (added != i)
            {
                /* Short list rather than no list. Saying so, because a client
                ** cannot tell a truncated zone list from a complete one. */
                TRACE_E("hub.location.list: only %u of %u zones could be allocated", added, i);
            }

            cJSON_AddItemToObject(__FUNCTION__, cjson_result, ezlopi_locations_str, json_array_tzs);
        }
    }
}

void EZPI_CLOUD_location_get(cJSON *cj_request, cJSON *cj_response)
{
    cJSON *cjson_result = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);
    if (cjson_result)
    {
        char *location = EZPI_core_sntp_get_location();
        if (location)
        {
            cJSON_AddStringToObject(__FUNCTION__, cjson_result, ezlopi_location_str, location);
            ezlopi_free(__FUNCTION__, location);
        }
        else
        {
            cJSON_AddStringToObject(__FUNCTION__, cjson_result, ezlopi_location_str, ezlopi__str);
        }
    }
}

void EZPI_CLOUD_location_set(cJSON *cj_request, cJSON *cj_response)
{
    cJSON *cj_params = cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_params_str);
    if (cj_params)
    {
        char location_str[128];
        memset(location_str, 0, sizeof(location_str));
        CJSON_GET_VALUE_STRING_BY_COPY(cj_params, ezlopi_location_str, location_str, sizeof(location_str));

        if ('\0' != location_str[0])
        {
            TRACE_I("Location: %s", location_str);

            /* The return is the point. This used to be discarded, and the
            ** handler echoed the requested name back under "result" whatever
            ** happened -- so a client that set a name not in the zone table,
            ** or hit a failed NVS write, was told the timezone had been saved.
            ** The name it got back was the one it had just sent, which reads as
            ** confirmation. */
            ezlopi_error_t error = EZPI_core_sntp_set_location(location_str);

            cJSON *cjson_result = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);

            if (EZPI_SUCCESS == error)
            {
                if (cjson_result)
                {
                    cJSON_AddStringToObject(__FUNCTION__, cjson_result, ezlopi_location_str, location_str);
                }
            }
            else if (EZPI_ERR_NVS_WRITE_FAILED == error)
            {
                __prepare_error(cj_response, EZPI_ERR_NVS_WRITE_FAILED, "could not store timezone");
            }
            else
            {
                /* Naming hub.location.list, because the set of acceptable names
                ** is exactly what that method returns and it is not guessable
                ** from the rejection alone. */
                __prepare_error(cj_response, EZPI_ERR_SNTP_LOCATION_SET_FAILED,
                                "unknown timezone; use a name from hub.location.list");
            }
        }
    }
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/
/**
 * @brief Add a JSON-RPC style error object to a response.
 *
 * Same shape as ezlopi_cloud_coordinates.c and ezlopi_cloud_devcfg.c, including
 * their quirk: "result" has already been added by the time this runs, so a
 * refused request carries both an empty result and an error. That is not what
 * JSON-RPC describes and it is what every method in this tree does, so matching
 * it is the conservative choice -- a client reading "result" unconditionally
 * keeps working.
 *
 * @param cj_response   the response object
 * @param error_code    one of the EZPI_ERR_* values
 * @param error_message human-readable reason
 */
static void __prepare_error(cJSON *cj_response, int error_code, const char *error_message)
{
    cJSON *cj_error = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_error_str);

    if (cj_error)
    {
        cJSON_AddNumberToObject(__FUNCTION__, cj_error, ezlopi_code_str, error_code);
        cJSON_AddStringToObject(__FUNCTION__, cj_error, ezlopi_message_str, error_message);
        cJSON_AddStringToObject(__FUNCTION__, cj_error, ezlopi_data_str, ezlopi__str);
    }
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
