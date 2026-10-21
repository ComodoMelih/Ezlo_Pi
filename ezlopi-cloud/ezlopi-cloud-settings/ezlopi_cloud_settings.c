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
 * @file    ezlopi_cloud_settings.c
 * @brief   Definitions for cloud settings functions
 * @author
 * @version 1.0
 * @date    January 20, 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <string.h>
#include <stdint.h>

#include "ezlopi_util_trace.h"

#include "ezlopi_cloud_settings.h"
#include "ezlopi_core_devices_list.h"
#include "ezlopi_core_cjson_macros.h"
#include "ezlopi_cloud_constants.h"
#include "ezlopi_cloud_methods_str.h"

#include "ezlopi_service_webprov.h"
/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/* Error "data" tokens, in the dotted form the rest of the cloud layer uses
** (compare DEVCFG_TOKEN_* in ezlopi_cloud_devcfg.c). A client matches on these
** rather than on the message text, which is why they are named constants and
** not inline strings: a token that changes spelling between two call sites is
** worse than no token.
*/
#define SETTINGS_TOKEN_PARAMS_MISSING "settings.params.missing"
#define SETTINGS_TOKEN_ID_NOT_FOUND "settings.id.not_found"
#define SETTINGS_TOKEN_SET_FAILED "settings.set.failed"
#define SETTINGS_TOKEN_RESET_FAILED "settings.reset.failed"

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/

static void __prepare_error(cJSON *cj_response, int error_code,
                            const char *error_message, const char *error_data);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
void EZPI_device_settings_list_v3(cJSON *cj_request, cJSON *cj_response)
{
    cJSON *cj_result = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);
    if (cj_result)
    {
        cJSON *cj_settings_array = cJSON_AddArrayToObject(__FUNCTION__, cj_result, ezlopi_settings_str);
        if (cj_settings_array)
        {
            l_ezlopi_device_t *curr_device = EZPI_core_device_get_head();
            while (curr_device)
            {
                l_ezlopi_device_settings_v3_t *curr_setting = curr_device->settings;
                while (curr_setting)
                {
                    cJSON *cj_properties = cJSON_CreateObject(__FUNCTION__);
                    if (cj_properties)
                    {
                        char tmp_string[64];
                        snprintf(tmp_string, sizeof(tmp_string), "%08x", curr_setting->cloud_properties.setting_id);
                        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi__id_str, tmp_string);
                        snprintf(tmp_string, sizeof(tmp_string), "%08x", curr_device->cloud_properties.device_id);
                        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_deviceId_str, tmp_string);

                        /* "synced" used to be stamped here, before the handler
                        ** ran. A handler that fails has added no value, no
                        ** valueType and no label, so the entry went out
                        ** claiming to be in sync with a device state it had
                        ** never read. Claimed now only if the handler agreed.
                        **
                        ** On failure the member is omitted rather than given
                        ** some other value: "synced" is the only status string
                        ** this API has ever sent, and inventing a second one
                        ** here would be guessing at a vocabulary the clients
                        ** share. Omitting lets a client test for the member,
                        ** which is the same reasoning __prepare_error() in
                        ** ezlopi_cloud_devcfg.c gives for leaving out
                        ** "badIndex" rather than sending -1.
                        */
                        if (0 == curr_setting->func(EZLOPI_SETTINGS_ACTION_GET_SETTING, curr_setting, cj_properties, curr_setting->user_arg))
                        {
                            cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_status_str, ezlopi_synced_str);
                        }
                        else
                        {
                            TRACE_W("settings: GET failed for setting %08x; entry sent without a status",
                                    curr_setting->cloud_properties.setting_id);
                        }

                        if (!cJSON_AddItemToArray(cj_settings_array, cj_properties))
                        {
                            cJSON_Delete(__FUNCTION__, cj_properties);
                        }
                    }
                    curr_setting = curr_setting->next;
                }
                curr_device = curr_device->next;
            }
        }
    }
}

void EZPI_device_settings_value_set_v3(cJSON *cj_request, cJSON *cj_response)
{
    cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);

    cJSON *cj_params = cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_params_str);
    if (cj_params)
    {
        uint32_t setting_id = EZPI_core_cjson_get_id(cj_params, ezlopi__id_str);
        // CJSON_GET_ID(setting_id, cJSON_GetObjectItem(__FUNCTION__, cj_params, ezlopi__id_str));

        l_ezlopi_device_t *curr_device = EZPI_core_device_get_head();
        uint32_t found_setting = 0;
        int set_ret = 0;
        while (curr_device)
        {
            l_ezlopi_device_settings_v3_t *curr_setting = curr_device->settings;
            while (curr_setting)
            {
                if (setting_id == curr_setting->cloud_properties.setting_id)
                {
                    set_ret = curr_setting->func(EZLOPI_SETTINGS_ACTION_SET_SETTING, curr_setting, cj_params, curr_setting->user_arg);
                    found_setting = 1;
                    break;
                }
                curr_setting = curr_setting->next;
            }
            if (found_setting)
            {
                break;
            }
            curr_device = curr_device->next;
        }

        /* Two separate silences, both of which looked identical to a client:
        ** an id matching no setting on any device, and a handler that ran and
        ** refused. Either way the reply was an empty result, which is this
        ** API's way of saying the setting was applied. Someone changing a
        ** setting in the app was told it worked and then watched the device
        ** not change -- with nothing anywhere to distinguish "no such setting"
        ** from "that value was rejected".
        */
        if (0 == found_setting)
        {
            TRACE_W("settings: set requested for unknown settingId %08x", setting_id);
            __prepare_error(cj_response, -32602, "Wrong params", SETTINGS_TOKEN_ID_NOT_FOUND);
        }
        else if (0 != set_ret)
        {
            TRACE_E("settings: handler refused set for settingId %08x (returned %d)", setting_id, set_ret);
            __prepare_error(cj_response, -32500, "Internal error", SETTINGS_TOKEN_SET_FAILED);
        }
    }
    else
    {
        __prepare_error(cj_response, -32600, "Bad request", SETTINGS_TOKEN_PARAMS_MISSING);
    }
}

void EZPI_device_settings_reset_v3(cJSON *cj_request, cJSON *cj_response)
{
    cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);

    cJSON *cj_params = cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_params_str);
    if (cj_params)
    {
        l_ezlopi_device_t *curr_device = EZPI_core_device_get_head();

        /* Accumulated rather than kept as "the last one", because both loops
        ** below reset every matching setting and carry on. Keeping only the
        ** most recent return would let a failure be overwritten by a success
        ** that happened to come after it, which is a reply that is wrong in
        ** the one direction that matters.
        */
        int reset_failures = 0;

        while (curr_device)
        {
            // check for deviceId first
            if (cJSON_HasObjectItem(__FUNCTION__, cj_params, ezlopi_deviceId_str))
            {
                uint32_t device_id = EZPI_core_cjson_get_id(cj_params, ezlopi_deviceId_str);
                // CJSON_GET_ID(device_id, cJSON_GetObjectItem(__FUNCTION__, cj_params, ezlopi_deviceId_str));

                if (device_id == curr_device->cloud_properties.device_id)
                {
                    l_ezlopi_device_settings_v3_t *curr_setting = curr_device->settings;
                    while (curr_setting)
                    {
                        if (0 != curr_setting->func(EZLOPI_SETTINGS_ACTION_RESET_SETTING, curr_setting, cj_params, curr_setting->user_arg))
                        {
                            TRACE_E("settings: reset refused for settingId %08x on deviceId %08x",
                                    curr_setting->cloud_properties.setting_id, device_id);
                            reset_failures++;
                        }
                        curr_setting = curr_setting->next;
                    }
                }
                else
                {
                    TRACE_OTEL(ENUM_EZLOPI_TRACE_SEVERITY_WARNING, "deviceId: %04X not found!", device_id);
                }
            }
            // checking for settingId
            else if (cJSON_HasObjectItem(__FUNCTION__, cj_params, ezlopi__id_str))
            {
                bool found_id = false;
                uint32_t setting_id = EZPI_core_cjson_get_id(cj_params, ezlopi__id_str);
                // CJSON_GET_ID(setting_id, cJSON_GetObjectItem(__FUNCTION__, cj_params, ezlopi__id_str));

                l_ezlopi_device_settings_v3_t *curr_setting = curr_device->settings;
                while (curr_setting)
                {
                    if (setting_id == curr_setting->cloud_properties.setting_id)
                    {
                        found_id = true;
                        if (0 != curr_setting->func(EZLOPI_SETTINGS_ACTION_RESET_SETTING, curr_setting, cj_params, curr_setting->user_arg))
                        {
                            TRACE_E("settings: reset refused for settingId %08x", setting_id);
                            reset_failures++;
                        }
                    }
                    curr_setting = curr_setting->next;
                }

                if (false == found_id)
                {
                    TRACE_OTEL(ENUM_EZLOPI_TRACE_SEVERITY_WARNING, "settingId: %04X not found!", setting_id);
                }
            }

            curr_device = curr_device->next;
        }

        if (reset_failures > 0)
        {
            __prepare_error(cj_response, -32500, "Internal error", SETTINGS_TOKEN_RESET_FAILED);
        }
    }
    else
    {
        __prepare_error(cj_response, -32600, "Bad request", SETTINGS_TOKEN_PARAMS_MISSING);
    }
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * @brief Add the cloud layer's standard error object to a response.
 *
 * Same shape and the same JSON-RPC code vocabulary as __prepare_error() in
 * ezlopi_cloud_devcfg.c, minus its "badIndex" member, which is specific to a
 * request carrying an array. Kept as a local static rather than shared with
 * that file for now: lifting it would mean choosing where it lives and
 * changing two call sites' includes, and this change is about the three
 * methods below reporting failure at all.
 *
 * As that function's comment records, "result" has already been added to the
 * response by the time this runs, so a refused request carries both an empty
 * result and an error. That is not what JSON-RPC describes, and it is what
 * every other method in this layer already does -- which is the point: a
 * client reading "result" unconditionally is unaffected by this change, and
 * only a client that looks for "error" sees anything new. Sending an error
 * *instead* of the result would have been the more correct shape and a
 * breaking one.
 */
static void __prepare_error(cJSON *cj_response, int error_code,
                            const char *error_message, const char *error_data)
{
    cJSON *cj_error = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_error_str);

    if (cj_error)
    {
        cJSON_AddNumberToObject(__FUNCTION__, cj_error, ezlopi_code_str, error_code);
        cJSON_AddStringToObject(__FUNCTION__, cj_error, ezlopi_message_str, error_message);
        cJSON_AddStringToObject(__FUNCTION__, cj_error, ezlopi_data_str, error_data);
    }
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/