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
 * @file    ezlopi_service_system_temperature_sensor.c
 * @brief   Contains function definitions related to system temperature sensor
 * @authors Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 1.0
 * @date    September 9th, 2024 7:15 PM
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "sdkconfig.h"
#if defined(CONFIG_EZPI_ENABLE_SYSTEM_TEMPERATURE)

#include <math.h>

#include "driver/temp_sensor.h"
#include "esp_err.h"

#include "ezlopi_util_trace.h"

#include "ezlopi_core_setting_commands.h"
#include "ezlopi_core_actions.h"
#include "ezlopi_core_devices.h"
#include "ezlopi_core_valueformatter.h"
#include "ezlopi_core_device_value_updated.h"

#include "ezlopi_cloud_constants.h"

#include "ezlopi_service_system_temperature_sensor.h"
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
/**
 * @brief Function to prepare temperature sensor related cloud parameters and items
 *
 * @return ezlopi_error_t EZPI_SUCCESS, or EZPI_FAILED if the device or its
 *         item could not be added
 */
static ezlopi_error_t ezpi_prepare();
/**
 * @brief Function to initialize and start temperature data
 *
 * @param item Pointer to item linked to temperature sensor
 * @return ezlopi_error_t always EZPI_SUCCESS -- see the definition for why it
 *         cannot currently report a failure
 */
static ezlopi_error_t ezpi_init(l_ezlopi_item_t *item);
/**
 * @brief Function to populate JSON with temperature data and scale to send to cloud
 *
 * @param item Pointer to item linked to temperature sensor
 * @param arg User argument, contains JSON to be populated
 * @return ezlopi_error_t EZPI_SUCCESS, or EZPI_FAILED if either argument was
 *         NULL. The caller discards this, which is pre-existing.
 */
static ezlopi_error_t ezpi_get_cjson_value(l_ezlopi_item_t *item, void *arg);
/**
 * @brief Function to notify the cloud about the data
 *
 * @param item Pointer to item linked to temperature sensor
 * @return ezlopi_error_t always EZPI_SUCCESS, including on a failed sensor
 *         read -- see the definition
 */
static ezlopi_error_t ezpi_notify(l_ezlopi_item_t *item);
/**
 * @brief Function to prepare cloud properties for provided device
 *
 * @param device Pointer to device
 */
static void ezpi_prepare_device_cloud_properties(l_ezlopi_device_t *device);
/**
 * @brief Function to prepare item properties for provided item
 *
 * @param item pointer to item
 */
static void ezpi_prepare_item_properties(l_ezlopi_item_t *item);
/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static temp_sensor_config_t device_tempeature_sensor_configuration = TSENS_CONFIG_DEFAULT();
static bool system_temperature_initialized = false;
static float system_temperature = 0.0f;

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
/* ezlopi_error_t, not int, because that is what the item_func contract
** declares: EZPI_core_device_add_item_to_device() takes
** `ezlopi_error_t (*)(e_ezlopi_actions_t, l_ezlopi_item_t *, void *, void *)`.
** ezlopi_error_t is a typedef for int32_t, which is `int` on xtensa and
** `long int` on riscv32, so a function returning plain int was a compatible
** pointer on the three xtensa targets and -Wincompatible-pointer-types on
** esp32c3 -- the last of the four such sites in the tree.
**
** This is the type the rest of the tree already uses: of the 63 driver entry
** points declared against e_ezlopi_actions_t, 62 return ezlopi_error_t and
** this was the one that did not. The values do not change either, because
** EZPI_SUCCESS is 0 and EZPI_FAILED is -1, which is exactly what the helpers
** below were already returning as bare literals. */
ezlopi_error_t EZPI_system_temperature_device(e_ezlopi_actions_t action, l_ezlopi_item_t *item, void *arg, void *user_arg)
{
    ezlopi_error_t ret = EZPI_SUCCESS;
    switch (action)
    {
    case EZLOPI_ACTION_PREPARE:
    {
        ret = ezpi_prepare();
        break;
    }
    case EZLOPI_ACTION_INITIALIZE:
    {
        ret = ezpi_init(item);
        break;
    }
    case EZLOPI_ACTION_HUB_GET_ITEM:
    case EZLOPI_ACTION_GET_EZLOPI_VALUE:
    {
        ezpi_get_cjson_value(item, arg);
        break;
    }
    case EZLOPI_ACTION_NOTIFY_1000_MS:
    {
        ezpi_notify(item);
        break;
    }
    default:
    {
        break;
    }
    }
    return ret;
}
/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/
static void ezpi_prepare_device_cloud_properties(l_ezlopi_device_t *device)
{
    device->cloud_properties.category = category_temperature;
    device->cloud_properties.subcategory = subcategory_not_defined;
    device->cloud_properties.device_type = dev_type_sensor;
    device->cloud_properties.info = NULL;
    device->cloud_properties.device_type_id = NULL;
    snprintf(device->cloud_properties.device_name, sizeof(device->cloud_properties.device_name), "System temperature");
}

static void ezpi_prepare_item_properties(l_ezlopi_item_t *item)
{
    item->interface_type = EZLOPI_DEVICE_INTERFACE_ANALOG_INPUT;
    item->cloud_properties.show = true;
    item->cloud_properties.has_getter = true;
    item->cloud_properties.has_setter = false;
    item->cloud_properties.item_name = ezlopi_item_name_temp;
    item->cloud_properties.value_type = value_type_temperature;
    item->cloud_properties.item_id = EZPI_core_cloud_generate_item_id();

    e_enum_temperature_scale_t scale_to_use = EZPI_core_setting_get_temperature_scale();
    item->cloud_properties.scale = (TEMPERATURE_SCALE_FAHRENHEIT == scale_to_use) ? scales_fahrenheit : scales_celsius;

    item->interface.adc.gpio_num = 0;
    item->interface.adc.resln_bit = 0;
}

static ezlopi_error_t ezpi_prepare()
{
    ezlopi_error_t ret = EZPI_FAILED;
    l_ezlopi_device_t *device = EZPI_core_device_add_device(NULL, NULL);
    if (device)
    {
        ezpi_prepare_device_cloud_properties(device);
        l_ezlopi_item_t *temp_item = EZPI_core_device_add_item_to_device(device, EZPI_system_temperature_device);
        if (temp_item)
        {
            ezpi_prepare_item_properties(temp_item);
            ret = EZPI_SUCCESS;
        }
        else
        {
            EZPI_core_device_free_device(device);
        }
    }
    return ret;
}

/* Still cannot fail, and that is not fixed here. Both ESP_ERROR_CHECK() calls
** below abort the hub rather than returning, so the only value this can ever
** produce is EZPI_SUCCESS. Unlike the hilink initialisers, the macro is at
** least being used on esp_err_t-returning functions here, so it is not the
** same defect -- it is the "no driver can report an initialisation failure"
** gap, recorded under Known gaps and deliberately out of scope for a
** pointer-type commit. */
static ezlopi_error_t ezpi_init(l_ezlopi_item_t *item)
{
    ezlopi_error_t ret = EZPI_SUCCESS;
    if (!system_temperature_initialized)
    {
        ESP_ERROR_CHECK(temp_sensor_set_config(device_tempeature_sensor_configuration));
        ESP_ERROR_CHECK(temp_sensor_start());
        system_temperature_initialized = true;
    }
    return ret;
}

static ezlopi_error_t ezpi_get_cjson_value(l_ezlopi_item_t *item, void *arg)
{
    ezlopi_error_t ret = EZPI_FAILED;
    if (item && arg)
    {
        cJSON *cj_result = (cJSON *)arg;
        EZPI_core_valueformatter_float_to_cjson(cj_result, system_temperature, item->cloud_properties.scale);
        ret = EZPI_SUCCESS;
    }
    return ret;
}

/* EZPI_SUCCESS unconditionally, including when temp_sensor_read_celsius()
** fails, which is how it already behaved as `int ret = 0`. Made explicit
** rather than changed: making a failed read report EZPI_FAILED would be a
** behaviour change in the notify path, and the caller discards this value
** anyway. Both halves of that are worth someone deciding on deliberately. */
static ezlopi_error_t ezpi_notify(l_ezlopi_item_t *item)
{
    ezlopi_error_t ret = EZPI_SUCCESS;
    float system_temperature_current_value = 0;
    float _eplison = 1.0;
    esp_err_t error = temp_sensor_read_celsius(&system_temperature_current_value);
    if (ESP_OK == error)
    {
        e_enum_temperature_scale_t scale_to_use = EZPI_core_setting_get_temperature_scale();
        item->cloud_properties.scale = (TEMPERATURE_SCALE_FAHRENHEIT == scale_to_use) ? scales_fahrenheit : scales_celsius;

        if (TEMPERATURE_SCALE_FAHRENHEIT == scale_to_use)
        {
            system_temperature_current_value = (system_temperature_current_value * (9.0f / 5.0f)) + 32.0f;
            _eplison = 2.5;
        }

        if (fabs(system_temperature - system_temperature_current_value) > _eplison)
        {
            system_temperature = system_temperature_current_value;
            EZPI_core_device_value_updated_from_device_broadcast(item);
        }
    }
    return ret;
}

#endif // CONFIG_EZPI_ENABLE_SYSTEM_TEMPERATURE

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
