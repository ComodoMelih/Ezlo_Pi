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
 * @file    hilink_presence_sensor_setting.c
 * @brief   perform some function on hilink_presence_sensor_setting
 * @author
 * @version 0.1
 * @date    xx
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <string.h>
#include <stdlib.h>

#include "ld2410.h"

#include "ezlopi_core_nvs.h"
// #include "ezlopi_core_cloud.h"
#include "ezlopi_core_device_value_updated.h"
#include "ezlopi_core_errors.h"

#include "ezlopi_cloud_settings.h"
#include "ezlopi_cloud_constants.h"

#include "hilink_presence_sensor_setting.h"
#include "sensor_0067_hilink_presence_sensor.h"
#include "EZLOPI_USER_CONFIG.h"

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
static int __settings_callback(e_ezlopi_settings_action_t action, struct l_ezlopi_device_settings_v3 *setting, void *arg, void *user_arg);
static int __settings_get(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_set(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_reset(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_update(void *arg, l_ezlopi_device_settings_v3_t *setting);
// Setting initialization declarations
static int __setting_initialize_hilink_presence_sensor_predefined_settings(l_ezlopi_device_t *device);
static int __setting_initialize_hilink_presence_sensor_userdefined_settings(l_ezlopi_device_t *device);
static int __setting_initialize_hilink_presence_sensor_radar_distance_sensitivity(l_ezlopi_device_t *device);
// Setting getters declarations
static int __setting_get_sensor_enum(cJSON *cj_enum);
static int __setting_get_pre_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __setting_get_user_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __setting_get_radar_distance_sensitivity_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);
// Setting setter declearations
static int __setting_set_change_user_defined_template(bool set_active);
static int __setting_set_change_setting_template(ld2410_template_t template, uint32_t setting_id);
static int __setting_set_find_predefined_setting_template(s_hilink_predefined_setting_value_t *setting_value, ld2410_template_t *template);
static int __setting_set_pre_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __setting_set_user_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);
// Setting reset declarations
static int __setting_reset_pre_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __setting_reset_user_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __setting_reset_radar_distance_sensitivity_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);
// Setting update declarations
static int __setting_update_pre_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __setting_update_user_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __setting_update_radar_distance_sensitivity_setting(void *arg, l_ezlopi_device_settings_v3_t *setting);

// common static function
static inline cJSON *__setting_add_text_and_lang_tag(const char *const object_text, const char *const object_lang_tag);
static inline void __setting_extract_user_defined_setting(cJSON *cj_value, s_hilink_userdefined_setting_value_t *user_defined_setting_val);
static inline int __prepare_user_defined_setting_cjson(cJSON *cj_value, s_hilink_userdefined_setting_value_t *setting_val);
static inline char *__prepare_user_defined_setting_str(s_hilink_userdefined_setting_value_t *setting_val);
static inline distance_t __setting_user_defined_setting_get_enum(float val);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

// static const char *nvs_key_hilink_presence_sensor_predefined_setting = "predef";
// static const char *nvs_key_hilink_presence_sensor_userdefined_setting = "userdef";
// static const char *nvs_key_hilink_presence_sensor_radar_distance_sensitivity = "rdrsens";
#warning "NABIN ; Need to remove these global variables";
static uint32_t hilink_presence_sensor_setting_ids[3] = {0, 0, 0};

static l_ezlopi_device_settings_v3_t *hilink_presence_sensor_radar_distance_sensitivity_setting;
static l_ezlopi_device_settings_v3_t *hilink_presence_sensor_user_defined_setting;
static l_ezlopi_device_settings_v3_t *hilink_presence_sensor_predefined_setting;

// static const char *hilink_presence_sensor_setting_enum[] = {
//     "sleep_mode_close",
//     "sleep_mode_mid",
//     "sleep_mode_long",
//     "movement_mode_close",
//     "movement_mode_mid",
//     "movement_mode_long",
//     "basic_routine_mode_close",
//     "basic_routine_mode_mid",
//     "basic_routine_mode_long",
//     "user_defined_mode",
// };

static const s_hilink_userdefined_setting_value_t hilink_user_defined_setting_default_value = {
    .min_move_distance = 0.75,
    .max_move_distance = 6.0,
    .min_still_distance = 0.75,
    .max_still_distance = 6.0,
    .timeout = 50,
    .is_active = false,
};

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
int HILINK_presence_sensor_apply_settings(void)
{
    int ret = 0;
    if (hilink_presence_sensor_predefined_setting && hilink_presence_sensor_user_defined_setting)
    {
        // Only the predefined setting mode is required to be accessed, if it is set to user_defined_mode, the settings are changed according to the user defined setting automatically.
        s_hilink_predefined_setting_value_t *predef_setting_value = (s_hilink_predefined_setting_value_t *)hilink_presence_sensor_predefined_setting->user_arg;
        if (predef_setting_value)
        {
            ld2410_template_t template;
            if (!__setting_set_find_predefined_setting_template(predef_setting_value, &template))
            {
                ret = __setting_set_change_setting_template(template, hilink_presence_sensor_predefined_setting->cloud_properties.setting_id);
            }
        }
        else
        {
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }
    return ret;
}
/* Not ESP_ERROR_CHECK, and the return value is now the one that is returned.
**
** All three initializers below return a plain int, 1 on failure -- not an
** esp_err_t. ESP_ERROR_CHECK aborts on any non-zero value, and this project
** builds with CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y and no NDEBUG,
** so the macro is the _esp_error_check_failed() variant: a panic and a reboot.
** Wrapping three "returns 1 on failure" functions in it made each of them a
** reboot switch.
**
** That was not theoretical here. The first initializer tested its NVS write
** with `EZPI_SUCCESS ==` where it meant `!=` (fixed below), so the *successful*
** first-ever write of the predefined setting returned 1, and ESP_ERROR_CHECK
** turned it into a panic. Configuring a presence sensor for the first time
** rebooted the hub. It rebooted only once, because the value had already been
** committed to NVS before the panic, so the next boot took the "setting already
** exists" path -- which is how this survived: it looks like one unexplained
** restart during onboarding and then never happens again.
**
** The remaining ways any of the three can fail are a settings allocation that
** did not fit, a NULL device, or a genuinely failed NVS write. None is a reason
** to restart the hub; the hub should come up with the sensor working and the
** setting reported as unavailable. So each failure is logged and returned.
**
** All three are attempted even if an earlier one fails, so that one bad
** allocation does not silently cost the other two settings as well.
**
** `int ret = 0; ... return ret;` with nothing in between assigning it meant
** this function returned 0 unconditionally -- the three results were consumed
** by the macro and otherwise thrown away. The value is now the OR of what ran.
** Its one caller, sensor_0067_hilink_presence_sensor.c, currently discards it;
** noted, not fixed here. The value of this change is that the hub stays up. */
int HILINK_presence_sensor_initialize_settings(l_ezlopi_device_t *device)
{
    int ret = 0;

    if (0 != __setting_initialize_hilink_presence_sensor_predefined_settings(device))
    {
        TRACE_E("hilink predefined setting failed to initialize");
        ret = 1;
    }
    if (0 != __setting_initialize_hilink_presence_sensor_userdefined_settings(device))
    {
        TRACE_E("hilink user-defined setting failed to initialize");
        ret = 1;
    }
    if (0 != __setting_initialize_hilink_presence_sensor_radar_distance_sensitivity(device))
    {
        TRACE_E("hilink radar distance-sensitivity setting failed to initialize");
        ret = 1;
    }

    return ret;
}
/* The node is checked, not just the payload behind it.
**
** This read `...->user_arg` and then tested the result for NULL, which covers
** the case where the payload allocation failed but not the case where the node
** itself was never allocated -- and the node pointer is a file-scope static
** that starts NULL. So if EZPI_core_device_add_settings_to_device_v3 returns
** NULL during initialization, the first presence report from the radar
** dereferences NULL and panics the hub, on a code path that has nothing to do
** with the setting.
**
** The caller is the UART frame handler in sensor_0067_hilink_presence_sensor.c,
** which runs on every frame the radar sends, so this is not a rare window: once
** that allocation has failed the hub cannot process presence at all. Reporting
** out-of-range is the conservative answer -- the same answer this already gave
** for a missing payload. */
bool HILINK_presence_sensor_target_in_detectable_range(const uint16_t moving_target_distance)
{
    bool in_range = false;

    if (hilink_presence_sensor_radar_distance_sensitivity_setting)
    {
        s_hilink_radar_distance_sensitivity_value_t *setting_val = (s_hilink_radar_distance_sensitivity_value_t *)hilink_presence_sensor_radar_distance_sensitivity_setting->user_arg;
        if (setting_val)
        {
            if (setting_val->distance_sensitivity_value <= moving_target_distance)
            {
                in_range = true;
            }
        }
    }

    return in_range;
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

// ********************************************* Setting initialization related start ********************************************* //
static int __setting_initialize_hilink_presence_sensor_predefined_settings(l_ezlopi_device_t *device)
{
    int ret = 0;

    hilink_presence_sensor_setting_ids[0] = EZPI_core_cloud_generate_settings_id();
    hilink_presence_sensor_predefined_setting = EZPI_core_device_add_settings_to_device_v3(device, __settings_callback);
    if (hilink_presence_sensor_predefined_setting)
    {
        hilink_presence_sensor_predefined_setting->cloud_properties.setting_id = hilink_presence_sensor_setting_ids[0];
        s_hilink_predefined_setting_value_t *hilink_presence_sensor_setting_value = (s_hilink_predefined_setting_value_t *)ezlopi_malloc(__FUNCTION__, sizeof(s_hilink_predefined_setting_value_t));
        if (hilink_presence_sensor_setting_value)
        {
            memset(hilink_presence_sensor_setting_value, 0, sizeof(s_hilink_predefined_setting_value_t));
            char *read_value = EZPI_core_nvs_read_str("predef");
            if (NULL != read_value)
            {
                TRACE_I("Setting already exist");
                snprintf(hilink_presence_sensor_setting_value->setting_value, 50, "%s", read_value);
                /* EZPI_core_nvs_read_str returns ezlopi_malloc'd memory the
                ** caller owns. The value has been copied into the payload
                ** above, so the string is finished with here. The user-defined
                ** initializer below already did this; this one did not, so
                ** every boot with a stored predefined mode leaked it. */
                ezlopi_free(__FUNCTION__, read_value);
            }
            else
            {
                TRACE_W("Not found saved setting for predefined setting value!");
                snprintf(hilink_presence_sensor_setting_value->setting_value, 50, "%s", "basic_routine_mode_mid");
                /* `!=`, not `==`. As written this tested for *success* and then
                ** logged "Failed to write to NVS" and returned 1, so the
                ** ordinary first-ever write -- the one that works -- was
                ** reported as a failure. With the ESP_ERROR_CHECK that used to
                ** wrap this function, that failure rebooted the hub the first
                ** time a presence sensor was configured. */
                if (EZPI_SUCCESS != EZPI_core_nvs_write_str(hilink_presence_sensor_setting_value->setting_value, strlen(hilink_presence_sensor_setting_value->setting_value), "predef"))
                {
                    TRACE_E("Failed to write to NVS");
                    ret = 1;
                }
            }
            hilink_presence_sensor_predefined_setting->user_arg = (void *)hilink_presence_sensor_setting_value;
        }
        else
        {
            /* The node is not this function's to free. EZPI_core_device_add_settings_to_device_v3
            ** has already linked it into device->settings; freeing it here left
            ** a dangling pointer in that list, and the very next initializer
            ** walks the list to append its own node -- `while (curr->next)` --
            ** so the read happens immediately, not at some later shutdown.
            **
            ** AddressSanitizer reports it as a heap-use-after-free from inside
            ** add_settings_to_device_v3, reached from the user-defined
            ** initializer below, which is exactly how it would behave on the
            ** hub. The node stays linked with a NULL user_arg, which is what
            ** the get-callback already treats as "no value". */
            ret = 1;
        }
    }
    else
    {
        /* This `else` was missing. A node that could not be allocated left
        ** ret at 0, so initialization reported success for a setting that does
        ** not exist. The other two initializers both report it. */
        TRACE_E("Could not add the predefined setting to the device");
        ret = 1;
    }

    return ret;
}
static int __setting_initialize_hilink_presence_sensor_userdefined_settings(l_ezlopi_device_t *device)
{
    int ret = 0;

    hilink_presence_sensor_setting_ids[1] = EZPI_core_cloud_generate_settings_id();
    hilink_presence_sensor_user_defined_setting = EZPI_core_device_add_settings_to_device_v3(device, __settings_callback);
    if (hilink_presence_sensor_user_defined_setting)
    {
        hilink_presence_sensor_user_defined_setting->cloud_properties.setting_id = hilink_presence_sensor_setting_ids[1];
        s_hilink_userdefined_setting_value_t *hilink_presence_sensor_user_defined_setting_val = (s_hilink_userdefined_setting_value_t *)ezlopi_malloc(__FUNCTION__, sizeof(s_hilink_userdefined_setting_value_t));
        if (hilink_presence_sensor_user_defined_setting_val)
        {
            memset(hilink_presence_sensor_user_defined_setting_val, 0, sizeof(s_hilink_userdefined_setting_value_t));
            char *read_value = EZPI_core_nvs_read_str("userdef");
            if (NULL != read_value)
            {
                TRACE_I("Setting already exist.");
                cJSON *cj_value = cJSON_Parse(__FUNCTION__, read_value);
                ezlopi_free(__FUNCTION__, read_value);

                if (cj_value)
                {
                    __setting_extract_user_defined_setting(cj_value, hilink_presence_sensor_user_defined_setting_val);
                    cJSON_Delete(__FUNCTION__, cj_value);
                }
            }
            else
            {
                TRACE_W("Not found saved setting for predefined setting value!");
                memcpy(hilink_presence_sensor_user_defined_setting_val, &hilink_user_defined_setting_default_value, sizeof(s_hilink_userdefined_setting_value_t));
                char *user_defined_value = __prepare_user_defined_setting_str(hilink_presence_sensor_user_defined_setting_val);
                if (user_defined_value)
                {
                    /* `!=`, not `==`. This tested for success and logged
                    ** "Failed to write to NVS" -- see the predefined
                    ** initializer above for what that cost. */
                    if (EZPI_SUCCESS != EZPI_core_nvs_write_str(user_defined_value, strlen(user_defined_value), "userdef"))
                    {
                        TRACE_E("Failed to write to NVS");
                        ret = 1;
                    }
                    /* The string is this function's to release.
                    ** __prepare_user_defined_setting_str hands over a
                    ** cJSON_PrintBuffered buffer and nothing freed it, so every
                    ** first-ever configuration of a presence sensor leaked
                    ** 1024 bytes. */
                    ezlopi_free(__FUNCTION__, user_defined_value);
                }
                else
                {
                    ret = 1;
                }
            }
            hilink_presence_sensor_user_defined_setting->user_arg = (void *)hilink_presence_sensor_user_defined_setting_val;
        }
        else
        {
            /* Not this function's node to free -- see the predefined
            ** initializer above. The same defect, in the same shape. */
            ret = 1;
        }
    }
    else
    {
        /* The free that stood here could not do anything: this branch is
        ** reached precisely because hilink_presence_sensor_user_defined_setting
        ** is NULL, and ezlopi_free is a guarded no-op on NULL. Removed as dead
        ** code rather than as a bug -- no test can show a difference, and this
        ** is recorded so that it is not mistaken for one of the frees above. */
        ret = 1;
    }

    return ret;
}
static int __setting_initialize_hilink_presence_sensor_radar_distance_sensitivity(l_ezlopi_device_t *device)
{
    int ret = 0;
    if (device)
    {
        hilink_presence_sensor_setting_ids[2] = EZPI_core_cloud_generate_settings_id();
        hilink_presence_sensor_radar_distance_sensitivity_setting = EZPI_core_device_add_settings_to_device_v3(device, __settings_callback);
        if (hilink_presence_sensor_radar_distance_sensitivity_setting)
        {
            hilink_presence_sensor_radar_distance_sensitivity_setting->cloud_properties.setting_id = hilink_presence_sensor_setting_ids[2];
            s_hilink_radar_distance_sensitivity_value_t *distance_sensitivity_value = (s_hilink_radar_distance_sensitivity_value_t *)ezlopi_malloc(__FUNCTION__, sizeof(s_hilink_radar_distance_sensitivity_value_t));
            if (distance_sensitivity_value)
            {
                memset(distance_sensitivity_value, 0, sizeof(s_hilink_radar_distance_sensitivity_value_t));
                int read_value = 0;
                ezlopi_error_t error = EZPI_core_nvs_read_int32(&read_value, "rdrsens");
                if (EZPI_SUCCESS == error)
                {
                    TRACE_I("Setting already exist.");
                    distance_sensitivity_value->distance_sensitivity_value = read_value;
                }
                else
                {
                    TRACE_W("Not found saved setting for predefined setting value!");
                    distance_sensitivity_value->distance_sensitivity_value = 10;
                    if (EZPI_SUCCESS != EZPI_core_nvs_write_int32(distance_sensitivity_value->distance_sensitivity_value, "rdrsens"))
                    {
                        TRACE_E("Failed to write to NVS");
                        ret = 1;
                    }
                }
                hilink_presence_sensor_radar_distance_sensitivity_setting->user_arg = distance_sensitivity_value;
            }
            else
            {
                /* `ret = 1`. This said `ret = 0` -- a payload allocation that
                ** failed was reported as success, where both sibling
                ** initializers report 1. The ezlopi_free that stood here freed
                ** distance_sensitivity_value, which is NULL in this branch by
                ** construction; removed as dead code, like the one below. */
                TRACE_E("Could not allocate the radar distance-sensitivity payload");
                ret = 1;
            }
        }
        else
        {
            /* No free: this branch is reached because the node pointer is
            ** NULL. Dead code, removed. */
            TRACE_E("Could not add the radar distance-sensitivity setting to the device");
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }
    return ret;
}

// ********************************************* Setting initialization related end ********************************************* //
/* Each dispatcher's answer is now kept.
**
** This is the function the cloud reaches, through setting->func(), and it
** called all four dispatchers for their side effects and returned a `ret` it
** never assigned. So even once the dispatchers below stopped rebooting and
** started returning a value, this function was where that value was thrown
** away. Fixing the dispatchers without fixing this would have been a change
** with no observable effect at all.
**
** The caller in ezlopi-cloud-settings still discards what it gets back -- see
** the note at __settings_get. One layer at a time. */
static int __settings_callback(e_ezlopi_settings_action_t action, struct l_ezlopi_device_settings_v3 *setting, void *arg, void *user_arg)
{
    int ret = 0;
    switch (action)
    {
    case EZLOPI_SETTINGS_ACTION_GET_SETTING:
    {
        TRACE_I("%s", stringify(EZLOPI_SETTINGS_ACTION_GET_SETTING));
        ret = __settings_get(arg, setting);
        break;
    }
    case EZLOPI_SETTINGS_ACTION_SET_SETTING:
    {
        TRACE_I("%s", stringify(EZLOPI_SETTINGS_ACTION_SET_SETTING));
        ret = __settings_set(arg, setting);
        break;
    }
    case EZLOPI_SETTINGS_ACTION_RESET_SETTING:
    {
        TRACE_I("%s", stringify(EZLOPI_SETTINGS_ACTION_RESET_SETTING));
        ret = __settings_reset(arg, setting);
        break;
    }
    case EZLOPI_SETTINGS_ACTION_UPDATE_SETTING:
    {
        TRACE_I("%s", stringify(EZLOPI_SETTINGS_ACTION_UPDATE_SETTING));
        ret = __settings_update(arg, setting);
        break;
    }

    default:
    {
        break;
    }
    }

    return ret;
}
// ********************************************* hub.device.settings.list related start ********************************************* //
static int __setting_get_sensor_enum(cJSON *cj_enum)
{
    int ret = 0;

    if (cj_enum)
    {
        cJSON_AddItemToObject(__FUNCTION__, cj_enum, "sleep_mode_close", __setting_add_text_and_lang_tag(SLEEP_MODE_CLOSE_TEXT, SLEEP_MODE_CLOSE_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_enum, "sleep_mode_mid", __setting_add_text_and_lang_tag(SLEEP_MODE_MID_TEXT, SLEEP_MODE_MID_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_enum, "sleep_mode_long", __setting_add_text_and_lang_tag(SLEEP_MODE_LONG_TEXT, SLEEP_MODE_LONG_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_enum, "movement_mode_close", __setting_add_text_and_lang_tag(MOVEMENT_MODE_CLOSE_TEXT, MOVEMENT_MODE_CLOSE_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_enum, "movement_mode_mid", __setting_add_text_and_lang_tag(MOVEMENT_MODE_MID_TEXT, MOVEMENT_MODE_MID_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_enum, "movement_mode_long", __setting_add_text_and_lang_tag(MOVEMENT_MODE_LONG_TEXT, MOVEMENT_MODE_LONG_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_enum, "basic_routine_mode_close", __setting_add_text_and_lang_tag(BASIC_ROUTINE_MODE_CLOSE_TEXT, BASIC_ROUTINE_MODE_CLOSE_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_enum, "basic_routine_mode_mid", __setting_add_text_and_lang_tag(BASIC_ROUTINE_MODE_MID_TEXT, BASIC_ROUTINE_MODE_MID_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_enum, "basic_routine_mode_long", __setting_add_text_and_lang_tag(BASIC_ROUTINE_MODE_LONG_TEXT, BASIC_ROUTINE_MODE_LONG_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_enum, "user_defined_mode", __setting_add_text_and_lang_tag(USER_DEFINED_MODE_TEXT, USER_DEFINED_MODE_LANG_TAG));
    }
    else
    {
        ret = 1;
    }
    return ret;
}
static int __setting_get_pre_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_hilink_predefined_setting_value_t *setting_value = (s_hilink_predefined_setting_value_t *)setting->user_arg;
    if (cj_properties && setting_value)
    {
        cJSON_AddItemToObject(__FUNCTION__, cj_properties, ezlopi_label_str, __setting_add_text_and_lang_tag(HILINK_PRESENCE_SENSOR_SETTING_PREDEFINED_LABEL_TEXT, HILINK_PRESENCE_SENSOR_SETTING_PREDEFINED_LABEL_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_properties, ezlopi_description_str, __setting_add_text_and_lang_tag(HILINK_PRESENCE_SENSOR_SETTING_PREDEFINED_DESCRIPTION_TEXT, HILINK_PRESENCE_SENSOR_SETTING_PREDEFINED_DESCRIPTION_LANG_TAG));

        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_status_str, ezlopi_synced_str);
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_valueType_str, value_type_token);

        cJSON *enum_object = cJSON_AddObjectToObject(__FUNCTION__, cj_properties, ezlopi_enum_str);
        if (enum_object)
        {

            if (ESP_OK == __setting_get_sensor_enum(enum_object))
            {
                ret = 0;
            }
            else
            {
                ret = 1;
            }
        }
        else
        {
            ret = 1;
        }
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_value_str, setting_value->setting_value);
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_valueDefault_str, "basic_routine_mode_mid");
    }
    else
    {
        ret = 1;
    }
    return ret;
}
static int __setting_get_user_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_hilink_userdefined_setting_value_t *setting_value = (s_hilink_userdefined_setting_value_t *)setting->user_arg;
    if (cj_properties && setting_value)
    {
        cJSON_AddItemToObject(__FUNCTION__, cj_properties, ezlopi_label_str, __setting_add_text_and_lang_tag(HILINK_PRESENCE_SENSOR_SETTING_USER_DEFINED_LABEL_TEXT, HILINK_PRESENCE_SENSOR_SETTING_USER_DEFINED_LABEL_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_properties, ezlopi_description_str, __setting_add_text_and_lang_tag(HILINK_PRESENCE_SENSOR_SETTING_USER_DEFINED_DESCRIPTION_TEXT, HILINK_PRESENCE_SENSOR_SETTING_USER_DEFINED_DESCRIPTION_LANG_TAG));

        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_status_str, ezlopi_synced_str);
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_valueType_str, value_type_presence_operation_mode);

        cJSON *cj_value = cJSON_AddObjectToObject(__FUNCTION__, cj_properties, ezlopi_value_str);
        if (cj_value)
        {
            ret |= __prepare_user_defined_setting_cjson(cj_value, setting_value);
        }
        else
        {
            ret = 1;
        }
        cJSON *cj_value_default = cJSON_AddObjectToObject(__FUNCTION__, cj_properties, ezlopi_valueDefault_str);
        if (cj_value_default)
        {
            ret |= __prepare_user_defined_setting_cjson(cj_value_default, (s_hilink_userdefined_setting_value_t *)&hilink_user_defined_setting_default_value);
        }
        else
        {
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}
static int __setting_get_radar_distance_sensitivity_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_hilink_radar_distance_sensitivity_value_t *setting_value = (s_hilink_radar_distance_sensitivity_value_t *)setting->user_arg;
    if (cj_properties && setting_value)
    {
        cJSON_AddItemToObject(__FUNCTION__, cj_properties, ezlopi_label_str, __setting_add_text_and_lang_tag(HILINK_PRESENCE_SENSOR_SETTING_RADAR_DISTANCE_SENSITIVITY_LABEL_TEXT, HILINK_PRESENCE_SENSOR_SETTING_RADAR_DISTANCE_SENSITIVITY_LABEL_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_properties, ezlopi_description_str, __setting_add_text_and_lang_tag(HILINK_PRESENCE_SENSOR_SETTING_RADAR_DISTANCE_SENSITIVITY_DESCRIPTION_TEXT, HILINK_PRESENCE_SENSOR_SETTING_RADAR_DISTANCE_SENSITIVITY_DESCRIPTION_LANG_TAG));

        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_status_str, ezlopi_synced_str);
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_valueType_str, value_type_int);
        cJSON_AddNumberToObject(__FUNCTION__, cj_properties, ezlopi_value_str, setting_value->distance_sensitivity_value);
        cJSON_AddNumberToObject(__FUNCTION__, cj_properties, ezlopi_valueMin_str, 5);
        cJSON_AddNumberToObject(__FUNCTION__, cj_properties, ezlopi_valueMax_str, 30);
        cJSON_AddNumberToObject(__FUNCTION__, cj_properties, ezlopi_valueDefault_str, 10);
    }
    else
    {
        ret = 1;
    }

    return ret;
}
/* The four dispatchers below -- get, set, reset and update -- all had the same
** two faults as HILINK_presence_sensor_initialize_settings: every branch was
** wrapped in ESP_ERROR_CHECK on a function returning a plain int, and `ret` was
** returned without ever being assigned. So each of them rebooted the hub on any
** failure, and reported success on every path that did not reboot.
**
** These are reached from the cloud, which makes them worse than the init path:
** a malformed or unexpected settings request from the far end became a reboot.
** They are now the OR of whatever branch matched.
**
** It stops there, and that is worth being plain about, as it was for the ENS160
** file: every settings func() call site in ezlopi-cloud-settings discards the
** return. The cloud therefore gets a settings object with nothing in it rather
** than an error. That is a defect in the cloud layer, shared by every device,
** and is not fixed here; what changes here is that the hub stays up. */
static int __settings_get(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    if (hilink_presence_sensor_setting_ids[0] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_get_pre_defined_setting(arg, setting);
    }
    if (hilink_presence_sensor_setting_ids[1] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_get_user_defined_setting(arg, setting);
    }
    if (hilink_presence_sensor_setting_ids[2] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_get_radar_distance_sensitivity_setting(arg, setting);
    }
    if (0 != ret)
    {
        TRACE_E("hilink presence sensor settings get failed");
    }
    return ret;
}
// ********************************************* hub.device.settings.list related end ********************************************* //

// ********************************************* hub.device.setting.value.set related start ********************************************* //
// If set_active is true, is_active member is set to true and sent to the cloud accordingly.
static int __setting_set_change_user_defined_template(bool set_active)
{
    int ret = 1;

    /* The node, not just the payload. Same defect as the one described at
    ** HILINK_presence_sensor_target_in_detectable_range: the pointer is a
    ** file-scope static that is NULL until initialization allocates it, and
    ** this read of `->user_arg` happened before anything checked it.
    ** HILINK_presence_sensor_apply_settings above is the one place in the file
    ** that already guarded both statics; this follows it.
    **
    ** `ret` starts at 1 here rather than 0, because every path that does not
    ** reach a successful write is a path on which nothing was stored -- and the
    ** outer `if (setting_val)` had no else at all, so a missing payload
    ** returned success. */
    if (NULL == hilink_presence_sensor_user_defined_setting)
    {
        TRACE_E("The user-defined setting does not exist yet");
        return ret;
    }

    s_hilink_userdefined_setting_value_t *setting_val = (s_hilink_userdefined_setting_value_t *)hilink_presence_sensor_user_defined_setting->user_arg;
    if (setting_val)
    {
        ret = 0;
        if (set_active)
        {
            ld2410_settings_t ld2410_setting = {
                .max_move_distance = __setting_user_defined_setting_get_enum(setting_val->max_move_distance),
                .max_still_distance = __setting_user_defined_setting_get_enum(setting_val->max_still_distance),
                .min_move_distance = __setting_user_defined_setting_get_enum(setting_val->min_move_distance),
                .min_still_distance = __setting_user_defined_setting_get_enum(setting_val->min_still_distance),
                .no_one_duration = setting_val->timeout,
            };
            /* The driver's answer was discarded. Its sibling
            ** __setting_set_change_setting_template below checks the same call,
            ** so the radar failing to accept a custom template was reported
            ** there and silently ignored here. */
            if (ESP_OK != ld2410_set_template(CUSTOM_TEMPLATE, &ld2410_setting))
            {
                TRACE_E("The radar did not accept the custom template");
                ret = 1;
            }
            setting_val->is_active = true;
            char *value_str = __prepare_user_defined_setting_str(setting_val);
            if (value_str)
            {
                /* `!=`, not `==` -- the third of these in the file. */
                if (EZPI_SUCCESS != EZPI_core_nvs_write_str(value_str, strlen(value_str), "userdef"))
                {
                    TRACE_E("Failed to write to NVS");
                    ret = 1;
                }
                /* Owned here, as at every other call of this function. */
                ezlopi_free(__FUNCTION__, value_str);
            }
            else
            {
                /* No else stood here, so a setting that could not be
                ** serialized -- and so was never stored -- returned success. */
                ret = 1;
            }
        }
    }

    return ret;
}
static int __setting_set_change_setting_template(ld2410_template_t template, uint32_t setting_id)
{
    int ret = 0;

    if (template != CUSTOM_TEMPLATE)
    {
        // Directly change the setting if template is other predefined templates.
        ret = 0;
        esp_err_t error = ld2410_set_template(template, NULL);
        if (ESP_OK != error)
        {
            ret = 1;
        }
    }
    else
    {
        /* All three of these discarded __setting_set_change_user_defined_template's
        ** return value, and this function then returned a `ret` that nothing in
        ** this branch had ever written -- so the whole custom-template path
        ** reported success unconditionally.
        **
        ** That is not just one more dropped return. It is the reason five
        ** separate defects inside __setting_set_change_user_defined_template
        ** could not be observed from outside it at all: a failed NVS write, a
        ** radar that refused the custom template, a missing node, a missing
        ** payload and an unserializable setting all set `ret = 1` in there, and
        ** every one of them was thrown away here. Re-breaking each of those
        ** five against the test suite left it green, which is what pointed at
        ** this function -- the fixes in the callee were real but unreachable
        ** evidence until this line kept the answer. */
        // Check if the setting change request comes from predefined mode setting.
        if (setting_id == hilink_presence_sensor_setting_ids[0])
        {
            ret = __setting_set_change_user_defined_template(true);
            EZPI_core_device_value_updated_settings_broadcast(hilink_presence_sensor_user_defined_setting);
        }
        else
        {
            /* Guarded, like every other read of these file-scope statics in
            ** this file now. The node is NULL until the predefined initializer
            ** has allocated it, and this read of `->user_arg` came before
            ** anything tested it. */
            s_hilink_predefined_setting_value_t *predef_value = hilink_presence_sensor_predefined_setting
                                                                    ? (s_hilink_predefined_setting_value_t *)hilink_presence_sensor_predefined_setting->user_arg
                                                                    : NULL;
            // If setting change request comes from user defined setting, check if predefined setting mode is set to user defined mode; in this case 9th index in enum str array.
            if (predef_value && (0 == strncmp(predef_value->setting_value, "user_defined_mode", ((strlen(predef_value->setting_value) + 1) > 18 ? (strlen(predef_value->setting_value) + 1) : 18))))
            {
                ret = __setting_set_change_user_defined_template(true);
                EZPI_core_device_value_updated_settings_broadcast(hilink_presence_sensor_predefined_setting);
            }
            else
            {
                // Don't change is_active member if user defined setting is changed but predefined setting is not set to user_defined_mode.
                ret = __setting_set_change_user_defined_template(false);
            }
        }
    }
    return ret;
}
static int __setting_set_find_predefined_setting_template(s_hilink_predefined_setting_value_t *setting_value, ld2410_template_t *template)
{
    int ret = 0;
    if (0 == strncmp(setting_value->setting_value, "sleep_mode_close", ((strlen(setting_value->setting_value) + 1) > 17 ? (strlen(setting_value->setting_value) + 1) : 17))) // sleep_mode_close
    {
        *template = SLEEP_TEMPLATE_CLOSE_RANGE;
    }
    else if (0 == strncmp(setting_value->setting_value, "sleep_mode_mid", ((strlen(setting_value->setting_value) + 1) > 15 ? (strlen(setting_value->setting_value) + 1) : 15))) // sleep_mode_mid
    {
        *template = SLEEP_TEMPLATE_MID_RANGE;
    }
    else if (0 == strncmp(setting_value->setting_value, "sleep_mode_long", ((strlen(setting_value->setting_value) + 1) > 16 ? (strlen(setting_value->setting_value) + 1) : 16))) // sleep_mode_long
    {
        *template = SLEEP_TEMPLATE_LONG_RANGE;
    }
    else if (0 == strncmp(setting_value->setting_value, "movement_mode_close", ((strlen(setting_value->setting_value) + 1) > 20 ? (strlen(setting_value->setting_value) + 1) : 20))) // movement_mode_close
    {
        *template = MOVEMENT_TEMPLATE_CLOSE_RANGE;
    }
    else if (0 == strncmp(setting_value->setting_value, "movement_mode_mid", ((strlen(setting_value->setting_value) + 1) > 18 ? (strlen(setting_value->setting_value) + 1) : 18))) // movement_mode_mid
    {
        *template = MOVEMENT_TEMPLATE_MID_RANGE;
    }
    else if (0 == strncmp(setting_value->setting_value, "movement_mode_long", ((strlen(setting_value->setting_value) + 1) > 19 ? (strlen(setting_value->setting_value) + 1) : 19))) // movement_mode_long
    {
        *template = MOVEMENT_TEMPLATE_LONG_RANGE;
    }
    else if (0 == strncmp(setting_value->setting_value, "basic_routine_mode_close", ((strlen(setting_value->setting_value) + 1) > 25 ? (strlen(setting_value->setting_value) + 1) : 25))) // basic_routine_mode_close
    {
        *template = BASIC_TEMPLATE_CLOSE_RANGE;
    }
    else if (0 == strncmp(setting_value->setting_value, "basic_routine_mode_mid", ((strlen(setting_value->setting_value) + 1) > 23 ? (strlen(setting_value->setting_value) + 1) : 23))) // basic_routine_mode_mid
    {
        *template = BASIC_TEMPLATE_MID_RANGE;
    }
    else if (0 == strncmp(setting_value->setting_value, "basic_routine_mode_long", ((strlen(setting_value->setting_value) + 1) > 24 ? (strlen(setting_value->setting_value) + 1) : 24))) // basic_routine_mode_long
    {
        *template = BASIC_TEMPLATE_LONG_RANGE;
    }
    else if (0 == strncmp(setting_value->setting_value, "user_defined_mode", ((strlen(setting_value->setting_value) + 1) > 18 ? (strlen(setting_value->setting_value) + 1) : 18))) // user_defined_mode
    {
        *template = CUSTOM_TEMPLATE;
    }
    else
    {
        TRACE_E("Provided predefined setting is unreconized.");
        ret = 1;
    }

    return ret;
}
static int __setting_set_pre_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    ld2410_template_t template;
    if (cj_properties && setting)
    {
        s_hilink_predefined_setting_value_t *setting_value = (s_hilink_predefined_setting_value_t *)setting->user_arg;
        if (setting_value)
        {
            CJSON_GET_VALUE_STRING_BY_COPY(cj_properties, ezlopi_value_str, setting_value->setting_value, sizeof(setting_value->setting_value));
            if (EZPI_SUCCESS != EZPI_core_nvs_write_str(setting_value->setting_value, strlen(setting_value->setting_value), "predef"))
            {
                TRACE_E("Failed to write to NVS");
                ret = 1;
            }
            /* `|=`, not `=`. This is the one write-result test in the file that
            ** was written the right way round -- and the assignment on the next
            ** line threw its answer away again, on every call where the
            ** template lookup succeeded, which is every call with a mode name
            ** the app could actually send. So the correct test cost nothing: a
            ** presence mode that could not be stored still reported success,
            ** and came back as the old mode on the next boot. */
            if (!__setting_set_find_predefined_setting_template(setting_value, &template))
            {
                ret |= __setting_set_change_setting_template(template, setting->cloud_properties.setting_id);
            }
            else
            {
                ret = 1;
            }
        }
        else
        {
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}
static int __setting_set_user_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_hilink_userdefined_setting_value_t *setting_val = (s_hilink_userdefined_setting_value_t *)setting->user_arg;
    if (cj_properties && setting && setting_val)
    {
        cJSON *cj_value = cJSON_GetObjectItem(__FUNCTION__, cj_properties, ezlopi_value_str);
        if (cj_value)
        {
            __setting_extract_user_defined_setting(cj_value, setting_val);

            char *value_str = __prepare_user_defined_setting_str(setting_val);
            if (value_str)
            {
                /* `!=`, not `==`, and this one did more damage than a wrong
                ** return value. As written, a *successful* write returned 1 and
                ** took the branch that does nothing, while only a *failed*
                ** write went on to program the radar. So choosing a
                ** user-defined presence mode from the app stored the value and
                ** never applied it -- the sensor kept its previous behaviour,
                ** and the only way the radar was ever reconfigured was if NVS
                ** had just failed. */
                if (EZPI_SUCCESS != EZPI_core_nvs_write_str(value_str, strlen(value_str), "userdef"))
                {
                    TRACE_E("Failed to write to NVS");
                    ret = 1;
                }
                else
                {
                    ret = __setting_set_change_setting_template(CUSTOM_TEMPLATE, setting->cloud_properties.setting_id); // user_defined_mode
                }
                ezlopi_free(__FUNCTION__, value_str);
            }
            else
            {
                ret = 1;
            }
        }
        else
        {
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}
static int __setting_set_radar_distance_sensitivity_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_hilink_radar_distance_sensitivity_value_t *setting_val = (s_hilink_radar_distance_sensitivity_value_t *)setting->user_arg;
    if (cj_properties && setting && setting_val)
    {
        CJSON_GET_VALUE_INT(cj_properties, ezlopi_value_str, setting_val->distance_sensitivity_value);
        if (EZPI_SUCCESS != EZPI_core_nvs_write_int32(setting_val->distance_sensitivity_value, "rdrsens"))
        {
            TRACE_E("Failed to write to NVS");
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}
static int __settings_set(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    if (hilink_presence_sensor_setting_ids[0] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_set_pre_defined_setting(arg, setting);
    }
    if (hilink_presence_sensor_setting_ids[1] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_set_user_defined_setting(arg, setting);
    }
    if (hilink_presence_sensor_setting_ids[2] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_set_radar_distance_sensitivity_setting(arg, setting);
    }
    if (0 != ret)
    {
        TRACE_E("hilink presence sensor settings set failed");
    }

    /* Broadcast either way, as before. A set that partly succeeded still
    ** changed the setting, and the cloud's copy should follow the hub's. */
    EZPI_core_device_value_updated_settings_broadcast(setting);

    return ret;
}
// ********************************************* hub.device.setting.value.set related end ********************************************* //

// ********************************************* hub.device.setting.reset related start ********************************************* //
static int __setting_reset_pre_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_params = (cJSON *)arg;
    if (cj_params && setting)
    {
        s_hilink_predefined_setting_value_t *setting_val = (s_hilink_predefined_setting_value_t *)setting->user_arg;
        if (setting_val)
        {
            snprintf(setting_val->setting_value, 50, "%s", "basic_routine_mode_mid");
            /* `!=`, not `==`. Inverted the same way as the set path, and with
            ** the same consequence: resetting the predefined setting stored the
            ** default and then skipped the branch that tells the radar about
            ** it, so a reset from the app had no effect on the sensor. */
            if (EZPI_SUCCESS != EZPI_core_nvs_write_str(setting_val->setting_value, strlen(setting_val->setting_value), "predef"))
            {
                TRACE_E("Failed to write to NVS");
                ret = 1;
            }
            else
            {
                ret = __setting_set_change_setting_template(BASIC_TEMPLATE_MID_RANGE, setting->cloud_properties.setting_id);
            }
        }
        else
        {
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}

static int __setting_reset_user_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_params = (cJSON *)arg;
    if (cj_params && setting)
    {
        s_hilink_userdefined_setting_value_t *setting_val = (s_hilink_userdefined_setting_value_t *)setting->user_arg;
        /* `setting_val`, not `setting`. This tested the pointer it had already
        ** tested on the line above -- so the guard was always true, and the
        ** memset below wrote 20 bytes through a NULL payload pointer whenever
        ** the setting had been added to the device without one. That is the
        ** state a failed payload allocation leaves behind, which is the state
        ** this file's initializers now deliberately keep rather than freeing
        ** the node. Every sibling guard in the file tests the payload. */
        if (setting_val)
        {
            /* The memset is redundant: the memcpy on the next line overwrites
            ** the whole struct. Left alone -- it is harmless and not what this
            ** change is about. */
            memset(setting_val, 0, sizeof(s_hilink_userdefined_setting_value_t));
            memcpy(setting_val, &hilink_user_defined_setting_default_value, sizeof(s_hilink_userdefined_setting_value_t));
            char *setting_val_str = __prepare_user_defined_setting_str(setting_val);
            if (setting_val_str)
            {
                /* `!=`, not `==` -- and TRACE_I, not TRACE_E, for a message
                ** that says "Failed". Both corrected. As with the other reset
                ** path, the inversion meant a successful write skipped the
                ** branch that applies the default to the radar. */
                if (EZPI_SUCCESS != EZPI_core_nvs_write_str(setting_val_str, strlen(setting_val_str), "userdef"))
                {
                    TRACE_E("Failed to write to nvs.");
                    ret = 1;
                }
                else
                {
                    ret = __setting_set_change_setting_template(CUSTOM_TEMPLATE, setting->cloud_properties.setting_id);
                }
                ezlopi_free(__FUNCTION__, setting_val_str);
            }
            else
            {
                ret = 1;
            }
        }
        else
        {
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}

static int __setting_reset_radar_distance_sensitivity_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_params = (cJSON *)arg;
    if (cj_params && setting)
    {
        s_hilink_radar_distance_sensitivity_value_t *setting_val = (s_hilink_radar_distance_sensitivity_value_t *)setting->user_arg;
        if (setting_val)
        {
            setting_val->distance_sensitivity_value = 10;
            if (EZPI_SUCCESS != EZPI_core_nvs_write_int32(setting_val->distance_sensitivity_value, "rdrsens"))
            {
                TRACE_E("Failed to write to NVS");
                ret = 1;
            }
            else
            {
                ret = 0;
            }
        }
        else
        {
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}

static int __settings_reset(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    if (hilink_presence_sensor_setting_ids[0] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_reset_pre_defined_setting(arg, setting);
    }
    if (hilink_presence_sensor_setting_ids[1] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_reset_user_defined_setting(arg, setting);
    }
    if (hilink_presence_sensor_setting_ids[2] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_reset_radar_distance_sensitivity_setting(arg, setting);
    }
    if (0 != ret)
    {
        TRACE_E("hilink presence sensor settings reset failed");
    }

    EZPI_core_device_value_updated_settings_broadcast(setting);

    return ret;
}
// ********************************************* hub.device.setting.reset related end ********************************************* //

// ********************************************* hub.device.setting.updated related start ********************************************* //
static int __setting_update_pre_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    if (setting)
    {
        cJSON *cj_params = (cJSON *)arg;
        s_hilink_predefined_setting_value_t *setting_value = (s_hilink_predefined_setting_value_t *)setting->user_arg;
        if (cj_params && setting_value)
        {
            cJSON_AddStringToObject(__FUNCTION__, cj_params, ezlopi_value_str, setting_value->setting_value);
        }
        else
        {
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}

/* Reads the payload from `setting`, not from the file-scope global.
**
** This took its value from hilink_presence_sensor_user_defined_setting->user_arg
** while being handed the node as a parameter -- so it dereferenced a static
** that is NULL until initialization succeeds, before anything checked it, and
** it answered about the global rather than about the setting it was asked
** about. Both sibling update functions use `setting->user_arg`; so does this
** one now. The `setting &&` test that used to sit in the condition below has
** moved ahead of the dereference, which is the only reason it was ever
** reachable in the right order. */
static int __setting_update_user_defined_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_params = (cJSON *)arg;
    if (cj_params && setting)
    {
        s_hilink_userdefined_setting_value_t *setting_value = (s_hilink_userdefined_setting_value_t *)setting->user_arg;
        if (setting_value)
        {
            cJSON *cj_value = cJSON_AddObjectToObject(__FUNCTION__, cj_params, ezlopi_value_str);
            if (cj_value)
            {
                if (setting_value->is_active)
                {
                    ret |= __prepare_user_defined_setting_cjson(cj_value, setting_value);
                }
                else
                {
                    cJSON_AddBoolToObject(__FUNCTION__, cj_value, ezlopi_is_active_str, setting_value->is_active);
                }
            }
            else
            {
                ret = 1;
            }
        }
        else
        {
            ret = 1;
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}

static int __setting_update_radar_distance_sensitivity_setting(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_params = (cJSON *)arg;
    s_hilink_radar_distance_sensitivity_value_t *setting_value = (s_hilink_radar_distance_sensitivity_value_t *)setting->user_arg;
    if (cj_params && setting && setting_value)
    {
        cJSON_AddNumberToObject(__FUNCTION__, cj_params, ezlopi_value_str, setting_value->distance_sensitivity_value);
    }
    else
    {
        ret = 1;
    }

    return ret;
}

static int __settings_update(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    if (hilink_presence_sensor_setting_ids[0] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_update_pre_defined_setting(arg, setting);
    }
    if (hilink_presence_sensor_setting_ids[1] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_update_user_defined_setting(arg, setting);
    }
    if (hilink_presence_sensor_setting_ids[2] == setting->cloud_properties.setting_id)
    {
        ret |= __setting_update_radar_distance_sensitivity_setting(arg, setting);
    }
    if (0 != ret)
    {
        TRACE_E("hilink presence sensor settings update failed");
    }

    return ret;
}
// ********************************************* hub.device.setting.updated related end ********************************************* //

static inline cJSON *__setting_add_text_and_lang_tag(const char *const object_text, const char *const object_lang_tag)
{

    cJSON *cj_object = cJSON_CreateObject(__FUNCTION__);
    if (cj_object)
    {
        cJSON_AddStringToObject(__FUNCTION__, cj_object, ezlopi_text_str, object_text);
        cJSON_AddStringToObject(__FUNCTION__, cj_object, ezlopi_lang_tag_str, object_lang_tag);
    }
    else
    {
        ezlopi_free(__FUNCTION__, cj_object);
        cj_object = NULL;
    }

    return cj_object;
}
static inline void __setting_extract_user_defined_setting(cJSON *cj_value, s_hilink_userdefined_setting_value_t *user_defined_setting_val)
{
    CJSON_GET_VALUE_FLOAT(cj_value, ezlopi_min_move_distance_str, user_defined_setting_val->min_move_distance);
    CJSON_GET_VALUE_FLOAT(cj_value, ezlopi_max_move_distance_str, user_defined_setting_val->max_move_distance);
    CJSON_GET_VALUE_FLOAT(cj_value, ezlopi_min_still_distance_str, user_defined_setting_val->min_still_distance);
    CJSON_GET_VALUE_FLOAT(cj_value, ezlopi_max_still_distance_str, user_defined_setting_val->max_still_distance);
    CJSON_GET_VALUE_UINT8(cj_value, ezlopi_timeout_str, user_defined_setting_val->timeout);
    CJSON_GET_VALUE_BOOL(cj_value, ezlopi_is_active_str, user_defined_setting_val->is_active);
}
static inline int __prepare_user_defined_setting_cjson(cJSON *cj_value, s_hilink_userdefined_setting_value_t *setting_val)
{
    int ret = 0;
    if (cj_value && setting_val)
    {
        cJSON_AddNumberToObject(__FUNCTION__, cj_value, ezlopi_min_move_distance_str, setting_val->min_move_distance);
        cJSON_AddNumberToObject(__FUNCTION__, cj_value, ezlopi_max_move_distance_str, setting_val->max_move_distance);
        cJSON_AddNumberToObject(__FUNCTION__, cj_value, ezlopi_min_still_distance_str, setting_val->min_still_distance);
        cJSON_AddNumberToObject(__FUNCTION__, cj_value, ezlopi_max_still_distance_str, setting_val->max_still_distance);
        cJSON_AddNumberToObject(__FUNCTION__, cj_value, ezlopi_timeout_str, setting_val->timeout);
        cJSON_AddBoolToObject(__FUNCTION__, cj_value, ezlopi_is_active_str, setting_val->is_active);
    }
    else
    {
        ret = 1;
    }
    return ret;
}
/* Serializes the user-defined setting. The returned string belongs to the
** caller, who must ezlopi_free it; the only caller is the user-defined
** initializer, which did not, and LeakSanitizer reported it as a 1024-byte
** leak out of cJSON_PrintBuffered on every first-ever configuration.
**
** Three other things were wrong in these ten lines:
**
**   - cj_setting was never deleted, on any path. The object and every child it
**     had been given leaked alongside the string.
**   - strlen(ret) ran before anything checked ret. cJSON_PrintBuffered returns
**     NULL when it cannot allocate, which is the one case this trace was most
**     likely to be read in.
**   - ESP_ERROR_CHECK wrapped __prepare_user_defined_setting_cjson, which
**     returns a plain int, 1 on failure -- another reboot switch of the kind
**     described at HILINK_presence_sensor_initialize_settings. A setting that
**     could not be built is now reported as NULL, which the caller already
**     treats as a failure. */
static inline char *__prepare_user_defined_setting_str(s_hilink_userdefined_setting_value_t *setting_val)
{
    char *ret = NULL;

    cJSON *cj_setting = cJSON_CreateObject(__FUNCTION__);
    if (cj_setting)
    {
        if (setting_val && (0 == __prepare_user_defined_setting_cjson(cj_setting, setting_val)))
        {
            ret = cJSON_PrintBuffered(__FUNCTION__, cj_setting, 1024, false);
        }
        else
        {
            TRACE_E("Failed to build the user-defined setting object");
        }
        cJSON_Delete(__FUNCTION__, cj_setting);
    }

    if (ret)
    {
        TRACE_D("length of 'ret': %u", (unsigned)strlen(ret));
    }

    return ret;
}
// Sensor's distance parameters are in the multiple of 0.75 which are encoded as enum in the driver.
// Starting from 0.75cm ranges goes up to 6.0cm, there are total of 8 different distance ranges.
static inline distance_t __setting_user_defined_setting_get_enum(float val)
{
    distance_t dist = DISTANCE_0CM;
    if (val == 0.75)
    {
        dist = DISTANCE_75CM;
    }
    else if (val == 1.5)
    {
        dist = DISTANCE_150CM;
    }
    else if (val == 2.25)
    {
        dist = DISTANCE_225CM;
    }
    else if (val == 3.0)
    {
        dist = DISTANCE_300CM;
    }
    else if (val == 3.75)
    {
        dist = DISTANCE_375CM;
    }
    else if (val == 4.5)
    {
        dist = DISTANCE_450CM;
    }
    else if (val == 5.25)
    {
        dist = DISTANCE_525CM;
    }
    else if (val == 6.0)
    {
        dist = DISTANCE_600CM;
    }
    else
    {
        dist = DISTANCE_0CM;
    }
    return dist;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/