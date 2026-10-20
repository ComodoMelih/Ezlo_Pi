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
 * @file    main.c
 * @brief   perform some function on data
 * @author
 * @version 0.1
 * @date    xx
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdlib.h>
// #include "esp_err.h"

#include "ezlopi_core_cloud.h"
#include "ezlopi_core_nvs.h"
// #include "ezlopi_core_devices.h"
#include "ezlopi_core_device_value_updated.h"

#include "ezlopi_cloud_settings.h"
#include "ezlopi_cloud_constants.h"

#include "ens160.h"
#include "sensor_0068_ENS160_gas_sensor_settings.h"
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
static int __settings_sensor_0068_gas_sensor_setting_initialize_ambient_temperature_setting(l_ezlopi_device_t *device, void *user_arg);
static int __settings_sensor_0068_gas_sensor_setting_initialize_relative_humidity_setting(l_ezlopi_device_t *device, void *user_arg);
static int __settings_callback(e_ezlopi_settings_action_t action, struct l_ezlopi_device_settings_v3 *setting, void *arg, void *user_arg);
static int __settings_get_ens160_gas_sensor_ambient_temperature_get(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_get_ens160_gas_sensor_relative_humidity_get(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_get(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_set_ens160_gas_sensor_ambient_temperature_get(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_set_ens160_gas_sensor_relative_humidity_get(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_set(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_update_ens160_gas_sensor_ambient_temperature_get(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_update_ens160_gas_sensor_relative_humidity_get(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_update(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_reset_ens160_gas_sensor_ambient_temperature_get(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_reset_ens160_gas_sensor_relative_humidity_get(void *arg, l_ezlopi_device_settings_v3_t *setting);
static int __settings_reset(void *arg, l_ezlopi_device_settings_v3_t *setting);
static inline cJSON *__setting_add_text_and_lang_tag_ens160(const char *const object_text, const char *const object_lang_tag);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

// static const char *nvs_key_ens160_gas_sensor_ambient_temperature_setting = "enstemp";
// static const char *nvs_key_ens160_gas_sensor_relative_humidity_setting = "enshmd";

static uint32_t ens160_gas_sensor_settings_ids[2] = {0, 0};

static bool setting_changed = false;

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

l_ezlopi_device_settings_v3_t *ens160_gas_sensor_ambient_temperature_sensor_setting;
l_ezlopi_device_settings_v3_t *ens160_gas_sensor_relative_humidity_sensor_setting;
/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
/* Not ESP_ERROR_CHECK.
**
** Both initializers below return a plain int, 1 on failure -- not an esp_err_t.
** ESP_ERROR_CHECK aborts on any non-zero value, and this project builds with
** CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y and no NDEBUG, so the macro
** is the _esp_error_check_failed() variant: a panic and a reboot. Wrapping a
** "returns 1 on failure" function in it makes that failure a reboot switch.
**
** The only way either initializer fails is a settings allocation that did not
** fit, or a NULL device. Neither is a reason to restart the hub; the hub should
** come up with the sensor working and the setting reported as unavailable. So
** the failure is logged and returned.
**
** Both are attempted even if the first fails, so that one bad allocation does
** not silently cost the other setting as well.
**
** The return reaches sensor_0068_ENS160_gas_sensor.c, which currently discards
** it -- noted, not fixed here. The value of this change is that a failed
** allocation no longer reboots, not that anyone acts on the code yet. */
int ENS160_gas_sensor_settings_init(l_ezlopi_device_t *devices, void *user_arg)
{
    int ret = 0;

    if (0 != __settings_sensor_0068_gas_sensor_setting_initialize_ambient_temperature_setting(devices, user_arg))
    {
        TRACE_E("ENS160 ambient-temperature setting failed to initialize");
        ret = 1;
    }
    if (0 != __settings_sensor_0068_gas_sensor_setting_initialize_relative_humidity_setting(devices, user_arg))
    {
        TRACE_E("ENS160 relative-humidity setting failed to initialize");
        ret = 1;
    }

    return ret;
}

/* The payload is not the settings node. Initialization allocates it and hangs
** it off the node (`->user_arg = ambient_temperature;` below), which is how the
** get- and set-callbacks further down have always read it.
**
** These two getters used to cast the node itself. That is in bounds and
** aligned -- `void *user_arg` is the node's first member and the float is the
** payload's -- so it read the payload pointer as a float instead of trapping.
** On an ESP32 every internal-DRAM address decodes to a float in [1.9375, 2.0),
** so both getters returned about 1.96 where the defaults are 25 degrees C and
** 50 %RH, and DFROBOT_ens160_set_temp_and_hum() wrote that into the sensor's
** compensation registers at every boot. tests/host/test_ens160set.c links
** these functions and asserts the stored value, and sweeps both the DRAM and
** the PSRAM address ranges to show where that number came from.
**
** 0.0f is kept as the no-setting value rather than raised to the 25/50
** defaults, deliberately: the only way to get here with a null payload is an
** allocation failure during settings init, and returning the default would
** make that indistinguishable from a working default -- to a reader, and to
** the test above. The TRACE_E is so it is not silent. */
float ENS160_get_ambient_temp_setting()
{
    float ret = 0.0f;
    if (ens160_gas_sensor_ambient_temperature_sensor_setting)
    {
        s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *ambient_temperature = (s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *)ens160_gas_sensor_ambient_temperature_sensor_setting->user_arg;
        if (ambient_temperature)
        {
            ret = ambient_temperature->ambient_temperature;
        }
        else
        {
            TRACE_E("ENS160 ambient-temperature setting has no payload; reporting 0");
        }
    }
    return ret;
}
float ENS160_get_relative_humidity_setting()
{
    float ret = 0.0f;
    if (ens160_gas_sensor_relative_humidity_sensor_setting)
    {
        s_sensor_ens160_gas_sensor_setting_relative_humidity_t *relative_humidity = (s_sensor_ens160_gas_sensor_setting_relative_humidity_t *)ens160_gas_sensor_relative_humidity_sensor_setting->user_arg;
        if (relative_humidity)
        {
            ret = relative_humidity->relative_humidity;
        }
        else
        {
            TRACE_E("ENS160 relative-humidity setting has no payload; reporting 0");
        }
    }
    return ret;
}

bool ENS160_has_setting_changed()
{
    return setting_changed;
}

void ENS160_set_setting_changed_to_false()
{
    setting_changed = false;
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

// ********************************************* Setting initialization related start ********************************************* //

static int __settings_sensor_0068_gas_sensor_setting_initialize_ambient_temperature_setting(l_ezlopi_device_t *device, void *user_arg)
{
    int ret = 0;

    if (device)
    {
        ens160_gas_sensor_settings_ids[0] = EZPI_core_cloud_generate_settings_id();
        ens160_gas_sensor_ambient_temperature_sensor_setting = EZPI_core_device_add_settings_to_device_v3(device, __settings_callback);
        if (ens160_gas_sensor_ambient_temperature_sensor_setting)
        {
            ens160_gas_sensor_ambient_temperature_sensor_setting->cloud_properties.setting_id = ens160_gas_sensor_settings_ids[0];
            s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *ambient_temperature = (s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *)ezlopi_malloc(__FUNCTION__, sizeof(s_sensor_ens160_gas_sensor_setting_ambient_temperature_t));
            if (ambient_temperature)
            {
                memset(ambient_temperature, 0, sizeof(s_sensor_ens160_gas_sensor_setting_ambient_temperature_t));
                float read_value = 0;
                ezlopi_error_t error = EZPI_core_nvs_read_float32(&read_value, "enstemp");
                if (EZPI_SUCCESS == error)
                {
                    TRACE_D("Setting already exist");
                    ambient_temperature->ambient_temperature = read_value;
                }
                else
                {
                    TRACE_W("Not found saved setting for ambient temperature.");
                    ambient_temperature->ambient_temperature = 25;
                    if (EZPI_SUCCESS != EZPI_core_nvs_write_float32(ambient_temperature->ambient_temperature, "enstemp"))
                    {
                        TRACE_E("Failed to write to NVS");
                        ret = 1;
                    }
                }
                ambient_temperature->user_arg = user_arg;
                ens160_gas_sensor_ambient_temperature_sensor_setting->user_arg = ambient_temperature;
            }
            else
            {
                /* The node is not ours to free. EZPI_core_device_add_settings_to_device_v3
                ** has already appended it to device->settings, and freeing it
                ** here leaves that list holding a dangling pointer -- which the
                ** very next add_settings_to_device_v3 call walks, to find the
                ** tail to append to. So the use-after-free is immediate and
                ** unconditional, not something that waits for the cloud.
                **
                ** Leave it linked with user_arg still NULL. Every callback in
                ** this file already tests user_arg before using it and returns
                ** 1 when it is absent, and both getters report 0, so a setting
                ** that failed to initialize reads as unavailable rather than
                ** as a plausible default. */
                TRACE_E("ENS160 ambient-temperature payload allocation failed; setting left unavailable");
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

static int __settings_sensor_0068_gas_sensor_setting_initialize_relative_humidity_setting(l_ezlopi_device_t *device, void *user_arg)
{
    int ret = 0;

    if (device)
    {
        ens160_gas_sensor_settings_ids[1] = EZPI_core_cloud_generate_settings_id();
        ens160_gas_sensor_relative_humidity_sensor_setting = EZPI_core_device_add_settings_to_device_v3(device, __settings_callback);
        if (ens160_gas_sensor_relative_humidity_sensor_setting)
        {
            ens160_gas_sensor_relative_humidity_sensor_setting->cloud_properties.setting_id = ens160_gas_sensor_settings_ids[1];
            s_sensor_ens160_gas_sensor_setting_relative_humidity_t *relative_humidity = (s_sensor_ens160_gas_sensor_setting_relative_humidity_t *)ezlopi_malloc(__FUNCTION__, sizeof(s_sensor_ens160_gas_sensor_setting_relative_humidity_t));
            if (relative_humidity)
            {
                memset(relative_humidity, 0, sizeof(s_sensor_ens160_gas_sensor_setting_relative_humidity_t));
                float read_value = 0;
                ezlopi_error_t error = EZPI_core_nvs_read_float32(&read_value, "enshmd");
                if (EZPI_SUCCESS == error)
                {
                    TRACE_D("Setting already exist");
                    relative_humidity->relative_humidity = read_value;
                }
                else
                {
                    TRACE_W("Not found saved setting for ambient temperature.");
                    relative_humidity->relative_humidity = 50;
                    if (EZPI_SUCCESS != EZPI_core_nvs_write_float32(relative_humidity->relative_humidity, "enshmd"))
                    {
                        TRACE_E("Failed to write to NVS");
                        ret = 1;
                    }
                }
                relative_humidity->user_arg = user_arg;
                ens160_gas_sensor_relative_humidity_sensor_setting->user_arg = relative_humidity;
            }
            else
            {
                /* Left linked, for the reason given on the ambient-temperature
                ** path above. */
                TRACE_E("ENS160 relative-humidity payload allocation failed; setting left unavailable");
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

static int __settings_callback(e_ezlopi_settings_action_t action, struct l_ezlopi_device_settings_v3 *setting, void *arg, void *user_arg)
{
    int ret = 0;

    switch (action)
    {
    /* The four returns were discarded here, which made every `ret` in the four
    ** dispatchers write-only. Assigned now so the propagation added there is
    ** not dead code. It still ends at the cloud, which ignores func()'s value
    ** -- see the note above __settings_get. */
    case EZLOPI_SETTINGS_ACTION_GET_SETTING:
    {
        TRACE_D("EZLOPI_SETTINGS_ACTION_GET_SETTING");
        ret = __settings_get(arg, setting);
        break;
    }
    case EZLOPI_SETTINGS_ACTION_SET_SETTING:
    {
        TRACE_D("EZLOPI_SETTINGS_ACTION_SET_SETTING");
        ret = __settings_set(arg, setting);
        break;
    }
    case EZLOPI_SETTINGS_ACTION_RESET_SETTING:
    {
        TRACE_D("EZLOPI_SETTINGS_ACTION_RESET_SETTING");
        ret = __settings_reset(arg, setting);
        break;
    }
    case EZLOPI_SETTINGS_ACTION_UPDATE_SETTING:
    {
        TRACE_D("EZLOPI_SETTINGS_ACTION_UPDATE_SETTING");
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

// ********************************************* Setting initialization related end ********************************************* //

// ********************************************* Setting getter related start ********************************************* //

static int __settings_get_ens160_gas_sensor_ambient_temperature_get(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *ambient_temperature = (s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *)setting->user_arg;
    if (cj_properties && ambient_temperature)
    {
        cJSON_AddItemToObject(__FUNCTION__, cj_properties, ezlopi_label_str, __setting_add_text_and_lang_tag_ens160(SENSOR_ENS160_GAS_SENSOR_SETTING_AMBIENT_TEMPERATURE_LABEL_TEXT, SENSOR_ENS160_GAS_SENSOR_SETTING_AMBIENT_TEMPERATURE_LABEL_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_properties, ezlopi_description_str, __setting_add_text_and_lang_tag_ens160(SENSOR_ENS160_GAS_SENSOR_SETTING_AMBIENT_TEMPERATURE_DESCRIPTION_TEXT, SENSOR_ENS160_GAS_SENSOR_SETTING_AMBIENT_TEMPERATURE_DESCRIPTION_LANG_TAG));
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_status_str, ezlopi_synced_str);
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_valueType_str, value_type_temperature);

        cJSON *value = cJSON_AddObjectToObject(__FUNCTION__, cj_properties, ezlopi_value_str);
        cJSON *valueDefault = cJSON_AddObjectToObject(__FUNCTION__, cj_properties, ezlopi_valueDefault_str);
        if (value && valueDefault)
        {
            cJSON_AddNumberToObject(__FUNCTION__, value, ezlopi_value_str, ambient_temperature->ambient_temperature);
            cJSON_AddStringToObject(__FUNCTION__, value, ezlopi_scale_str, scales_celsius);

            cJSON_AddNumberToObject(__FUNCTION__, valueDefault, ezlopi_value_str, 25);
            cJSON_AddStringToObject(__FUNCTION__, valueDefault, ezlopi_scale_str, scales_celsius);
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}

static int __settings_get_ens160_gas_sensor_relative_humidity_get(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_sensor_ens160_gas_sensor_setting_relative_humidity_t *relative_humidity = (s_sensor_ens160_gas_sensor_setting_relative_humidity_t *)setting->user_arg;
    if (cj_properties && relative_humidity)
    {
        cJSON_AddItemToObject(__FUNCTION__, cj_properties, ezlopi_label_str, __setting_add_text_and_lang_tag_ens160(SENSOR_ENS160_GAS_SENSOR_SETTING_RELATIVE_HUMIDITY_LABEL_TEXT, SENSOR_ENS160_GAS_SENSOR_SETTING_RELATIVE_HUMIDITY_LABEL_LANG_TAG));
        cJSON_AddItemToObject(__FUNCTION__, cj_properties, ezlopi_description_str, __setting_add_text_and_lang_tag_ens160(SENSOR_ENS160_GAS_SENSOR_SETTING_RELATIVE_HUMIDITY_DESCRIPTION_TEXT, SENSOR_ENS160_GAS_SENSOR_SETTING_RELATIVE_HUMIDITY_DESCRIPTION_LANG_TAG));
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_status_str, ezlopi_synced_str);
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_valueType_str, value_type_humidity);

        cJSON *value = cJSON_AddObjectToObject(__FUNCTION__, cj_properties, ezlopi_value_str);
        cJSON *valueDefault = cJSON_AddObjectToObject(__FUNCTION__, cj_properties, ezlopi_valueDefault_str);
        if (value && valueDefault)
        {
            cJSON_AddNumberToObject(__FUNCTION__, value, ezlopi_value_str, relative_humidity->relative_humidity);
            cJSON_AddStringToObject(__FUNCTION__, value, ezlopi_scale_str, scales_percent);

            cJSON_AddNumberToObject(__FUNCTION__, valueDefault, ezlopi_value_str, 50);
            cJSON_AddStringToObject(__FUNCTION__, valueDefault, ezlopi_scale_str, scales_percent);
        }
    }
    else
    {
        ret = 1;
    }

    return ret;
}

/* The four dispatchers below used to wrap their per-setting helpers in
** ESP_ERROR_CHECK, and the comment on ENS160_gas_sensor_settings_init applies
** with more force here: these four are reached from the cloud, through
** l_ezlopi_device_settings_v3_t::func, when the hub serves hub.device.settings,
** hub.device.setting.value.set, hub.device.setting.reset or a settings
** broadcast. Each helper returns 1 when the setting has no payload, which is
** exactly the state a failed allocation leaves behind -- so with the macro in
** place the first cloud read of a setting that failed to initialize rebooted
** the hub, and would have gone on rebooting it on every retry.
**
** `int ret = ret = 0;` was also a self-assignment, and ret was never written
** again, so every one of these four returned 0 whatever happened. The value is
** now the OR of whatever ran.
**
** It stops here, and that is worth being plain about: every settings func()
** call site in ezlopi-cloud-settings discards the return. The cloud therefore
** receives a settings object with no value in it rather than an error. That is
** a defect in the cloud layer, shared by every device, and is not fixed in this
** file; the change that matters here is that the hub stays up. */
static int __settings_get(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;
    if (setting->cloud_properties.setting_id == ens160_gas_sensor_settings_ids[0])
    {
        ret |= __settings_get_ens160_gas_sensor_ambient_temperature_get(arg, setting);
    }
    if (setting->cloud_properties.setting_id == ens160_gas_sensor_settings_ids[1])
    {
        ret |= __settings_get_ens160_gas_sensor_relative_humidity_get(arg, setting);
    }
    if (0 != ret)
    {
        TRACE_E("ENS160 settings get failed");
    }
    return ret;
}
// ********************************************* Setting getter related end ********************************************* //

// ********************************************* Setting setter related start ********************************************* //

static int __settings_set_ens160_gas_sensor_ambient_temperature_get(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *ambient_temperature = (s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *)setting->user_arg;
    if (cj_properties && ambient_temperature)
    {
        CJSON_GET_VALUE_FLOAT(cj_properties, ezlopi_value_str, ambient_temperature->ambient_temperature);
        if (EZPI_SUCCESS != EZPI_core_nvs_write_float32(ambient_temperature->ambient_temperature, "enstemp"))
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

static int __settings_set_ens160_gas_sensor_relative_humidity_get(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_sensor_ens160_gas_sensor_setting_relative_humidity_t *relative_humidity = (s_sensor_ens160_gas_sensor_setting_relative_humidity_t *)setting->user_arg;
    if (cj_properties && relative_humidity)
    {
        CJSON_GET_VALUE_FLOAT(cj_properties, ezlopi_value_str, relative_humidity->relative_humidity);
        if (EZPI_SUCCESS != EZPI_core_nvs_write_float32(relative_humidity->relative_humidity, "enshmd"))
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
    if (setting->cloud_properties.setting_id == ens160_gas_sensor_settings_ids[0])
    {
        ret |= __settings_set_ens160_gas_sensor_ambient_temperature_get(arg, setting);
    }
    if (setting->cloud_properties.setting_id == ens160_gas_sensor_settings_ids[1])
    {
        ret |= __settings_set_ens160_gas_sensor_relative_humidity_get(arg, setting);
    }
    if (0 != ret)
    {
        TRACE_E("ENS160 settings set failed");
    }
    setting_changed = true;

    EZPI_core_device_value_updated_settings_broadcast(setting);
    // EZPI_cloud_settings_updated_from_devices_v3(NULL, setting);
    return ret;
}

// ********************************************* Setting setter related end ********************************************* //

// ********************************************* Setting updater related start ********************************************* //

static int __settings_update_ens160_gas_sensor_ambient_temperature_get(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *ambient_temperature = (s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *)setting->user_arg;
    if (cj_properties && ambient_temperature)
    {
        cJSON_AddNumberToObject(__FUNCTION__, cj_properties, ezlopi_value_str, ambient_temperature->ambient_temperature);
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_scale_str, scales_celsius);
    }
    else
    {
        ret = 1;
    }
    return ret;
}

static int __settings_update_ens160_gas_sensor_relative_humidity_get(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_sensor_ens160_gas_sensor_setting_relative_humidity_t *relative_humidity = (s_sensor_ens160_gas_sensor_setting_relative_humidity_t *)setting->user_arg;
    if (cj_properties && relative_humidity)
    {
        cJSON_AddNumberToObject(__FUNCTION__, cj_properties, ezlopi_value_str, relative_humidity->relative_humidity);
        cJSON_AddStringToObject(__FUNCTION__, cj_properties, ezlopi_scale_str, scales_percent);
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
    if (setting->cloud_properties.setting_id == ens160_gas_sensor_settings_ids[0])
    {
        ret |= __settings_update_ens160_gas_sensor_ambient_temperature_get(arg, setting);
    }
    if (setting->cloud_properties.setting_id == ens160_gas_sensor_settings_ids[1])
    {
        ret |= __settings_update_ens160_gas_sensor_relative_humidity_get(arg, setting);
    }
    if (0 != ret)
    {
        TRACE_E("ENS160 settings update failed");
    }
    return ret;
}

// ********************************************* Setting updater related end ********************************************* //

// ********************************************* Setting resetter related start ********************************************* //

static int __settings_reset_ens160_gas_sensor_ambient_temperature_get(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *ambient_temperature = (s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *)setting->user_arg;
    if (cj_properties && ambient_temperature)
    {
        ambient_temperature->ambient_temperature = 25;
        if (EZPI_SUCCESS != EZPI_core_nvs_write_float32(ambient_temperature->ambient_temperature, "enstemp"))
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

static int __settings_reset_ens160_gas_sensor_relative_humidity_get(void *arg, l_ezlopi_device_settings_v3_t *setting)
{
    int ret = 0;

    cJSON *cj_properties = (cJSON *)arg;
    s_sensor_ens160_gas_sensor_setting_relative_humidity_t *relative_humidity = (s_sensor_ens160_gas_sensor_setting_relative_humidity_t *)setting->user_arg;
    if (cj_properties && relative_humidity)
    {
        relative_humidity->relative_humidity = 50;
        if (EZPI_SUCCESS != EZPI_core_nvs_write_float32(relative_humidity->relative_humidity, "enshmd"))
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

static int __settings_reset(void *arg, l_ezlopi_device_settings_v3_t *setting)
{

    int ret = 0;
    if (setting->cloud_properties.setting_id == ens160_gas_sensor_settings_ids[0])
    {
        ret |= __settings_reset_ens160_gas_sensor_ambient_temperature_get(arg, setting);
    }
    if (setting->cloud_properties.setting_id == ens160_gas_sensor_settings_ids[1])
    {
        ret |= __settings_reset_ens160_gas_sensor_relative_humidity_get(arg, setting);
    }
    if (0 != ret)
    {
        TRACE_E("ENS160 settings reset failed");
    }
    setting_changed = true;

    EZPI_core_device_value_updated_settings_broadcast(setting);
    // EZPI_cloud_settings_updated_from_devices_v3(NULL, setting);
    return ret;
}

// ********************************************* Setting resetter related end ********************************************* //

static inline cJSON *__setting_add_text_and_lang_tag_ens160(const char *const object_text, const char *const object_lang_tag)
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

/*******************************************************************************
 *                          End of File
 *******************************************************************************/