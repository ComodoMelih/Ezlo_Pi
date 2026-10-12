
/**
 * @file    ezlopi_service_ble_wifi.c
 * @brief   WiFi service related functionalities
 * @authors Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version
 * @date
 */
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
 * @file    ezlopi_service_ble_wifi.c
 * @brief   WiFi service related functionalities
 * @author
 * @version 1.0
 * @date    January 22, 2024
 */

#include "sdkconfig.h"

#ifdef CONFIG_EZPI_BLE_ENABLE

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include <string.h>

#include "lwip/ip_addr.h"

#include "esp_event_base.h"
#include "esp_wifi.h"

#include "cjext.h"
#include "ezlopi_util_trace.h"

#include "ezlopi_core_nvs.h"
#include "ezlopi_core_wifi.h"
#include "ezlopi_core_ping.h"
#include "ezlopi_core_ble_gatt.h"
#include "ezlopi_core_ble_buffer.h"
#include "ezlopi_core_ble_profile.h"
#include "ezlopi_core_factory_info.h"

#include "ezlopi_cloud_constants.h"

#include "ezlopi_service_ble_ble_auth.h"
#include "ezlopi_service_ble.h"

/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/
/**
 * @brief Provisioning progress reported over the WiFi status characteristic (0xE102)
 */
typedef enum e_ezpi_wifi_prov_state
{
    EZPI_WIFI_PROV_STATE_IDLE = 0,       /*!< No credentials received since boot */
    EZPI_WIFI_PROV_STATE_CONNECTING = 1, /*!< Credentials accepted, association in progress */
    EZPI_WIFI_PROV_STATE_CONNECTED = 2,  /*!< Associated and got an IP */
    EZPI_WIFI_PROV_STATE_FAILED = 3,     /*!< Association failed, see 0xE103 for the reason */
    EZPI_WIFI_PROV_STATE_REJECTED = 4,   /*!< Write was malformed or not authorized */
} e_ezpi_wifi_prov_state_t;

/**
 * @brief ATT payload overhead of a notification (1 byte opcode + 2 byte handle)
 */
#define EZPI_BLE_NOTIFY_ATT_OVERHEAD 3

/**
 * @brief ATT payload overhead of a read response (1 byte opcode). A response
 *        carrying a full MTU of data does not fit on the wire, so the stack
 *        would drop the last byte of every chunk and the client would reassemble
 *        invalid JSON.
 */
#define EZPI_BLE_READ_ATT_OVERHEAD 1

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
/**
 * @brief Function callback that responds to ESP_GATTS_WRITE_EVT gatts event for WiFi service
 */
static void ezlpi_wifi_creds_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
/**
 * @brief Function callback that responds to ESP_GATTS_EXEC_WRITE_EVT gatts event for WiFi service
 */
static void ezpi_wifi_creds_write_exec_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
/**
 * @brief Function to parse incoming WiFi credential JSON and connect with provided credentials
 *
 * @param[in] value Pointer to the credential JSON
 * @param[in] len  size of the credential JOSN
 */
static void ezpi_wifi_creds_parse_and_connect(uint8_t *value, uint32_t len);
/**
 * @brief Function callback that responds to ESP_GATTS_READ_EVT for the WiFi status characteristic
 */
static void ezpi_wifi_status_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
/**
 * @brief Function callback that responds to ESP_GATTS_READ_EVT for the WiFi error characteristic
 */
static void ezpi_wifi_error_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
/**
 * @brief Function callback that responds to ESP_GATTS_READ_EVT on the status CCCD
 */
static void ezpi_wifi_status_cccd_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
/**
 * @brief Function callback that responds to ESP_GATTS_READ_EVT on the error CCCD
 */
static void ezpi_wifi_error_cccd_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
/**
 * @brief Function callback that responds to ESP_GATTS_WRITE_EVT on the status CCCD
 */
static void ezpi_wifi_status_cccd_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
/**
 * @brief Function callback that responds to ESP_GATTS_WRITE_EVT on the error CCCD
 */
static void ezpi_wifi_error_cccd_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
/**
 * @brief Function to record the provisioning outcome and notify any subscribed client
 *
 * @param[in] state New provisioning state
 * @param[in] error Human readable failure reason, or NULL to clear it
 */
static void ezpi_wifi_prov_set_state(e_ezpi_wifi_prov_state_t state, const char *error);
/**
 * @brief WiFi/IP event upcall used to drive the provisioning state machine
 */
static void ezpi_wifi_prov_event_upcall(esp_event_base_t event, int32_t event_id, void *arg);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static s_linked_buffer_t *wifi_creds_linked_buffer = NULL;
static s_gatt_service_t *wifi_ble_service;
static s_gatt_char_t *wifi_status_char;
static s_gatt_char_t *wifi_error_char;
static bool sg_status_notify_enabled;
static bool sg_error_notify_enabled;
static uint32_t sg_status_notify_generation;
static uint32_t sg_error_notify_generation;
static bool sg_prov_ignore_disconnect;
static e_ezpi_wifi_prov_state_t sg_prov_state = EZPI_WIFI_PROV_STATE_IDLE;
static char sg_prov_error[96];
static char sg_prov_ssid[33];

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

void EZPI_ble_service_wifi_profile_init(void)
{
    esp_bt_uuid_t uuid;
    esp_gatt_perm_t permission;
    esp_gatt_char_prop_t properties;

    // wifi credentials
    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = BLE_WIFI_SERVICE_UUID;
    wifi_ble_service = EZPI_core_ble_gatt_create_service(BLE_WIFI_SERVICE_HANDLE, &uuid);

    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = BLE_WIFI_CHAR_CREDS_UUID;
    permission = ESP_GATT_PERM_WRITE;
    properties = ESP_GATT_CHAR_PROP_BIT_WRITE;
    EZPI_core_ble_gatt_add_characteristic(wifi_ble_service, &uuid, permission, properties, NULL, ezlpi_wifi_creds_write_func, ezpi_wifi_creds_write_exec_func);

    /* Status (0xE102) and error (0xE103) have been declared in ezlopi_service_ble.h
     * since the service was written but were never created, so a client had no way
     * to learn whether a credential write succeeded. Both are readable and
     * notifiable; each carries a CCCD so the client can subscribe. */
    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = BLE_WIFI_CHAR_STATUS_UUID;
    permission = ESP_GATT_PERM_READ;
    properties = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
    wifi_status_char = EZPI_core_ble_gatt_add_characteristic(wifi_ble_service, &uuid, permission, properties, ezpi_wifi_status_read_func, NULL, NULL);

    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
    EZPI_core_ble_gatt_add_descriptor(wifi_status_char, &uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                      ezpi_wifi_status_cccd_read_func, ezpi_wifi_status_cccd_write_func, NULL);

    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = BLE_WIFI_CHAR_ERROR_UUID;
    permission = ESP_GATT_PERM_READ;
    properties = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
    wifi_error_char = EZPI_core_ble_gatt_add_characteristic(wifi_ble_service, &uuid, permission, properties, ezpi_wifi_error_read_func, NULL, NULL);

    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
    EZPI_core_ble_gatt_add_descriptor(wifi_error_char, &uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                      ezpi_wifi_error_cccd_read_func, ezpi_wifi_error_cccd_write_func, NULL);

    /* Drive the reported state from the WiFi stack rather than guessing at write time. */
    EZPI_core_wifi_event_add(ezpi_wifi_prov_event_upcall, NULL);
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

static void ezlpi_wifi_creds_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (0 == param->write.is_prep) // Data received in single packet
    {
        if ((NULL != param->write.value) && (param->write.len > 0))
        {
            ezpi_wifi_creds_parse_and_connect(param->write.value, param->write.len);
        }
    }
    else
    {
        if (NULL == wifi_creds_linked_buffer)
        {
            wifi_creds_linked_buffer = EZPI_core_ble_buffer_create(param);
        }
        else
        {
            EZPI_core_ble_buffer_add_to_buffer(wifi_creds_linked_buffer, param);
        }
    }
}

static void ezpi_wifi_creds_write_exec_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (wifi_creds_linked_buffer)
    {
        EZPI_core_ble_buffer_accumulate_to_start(wifi_creds_linked_buffer);
        ezpi_wifi_creds_parse_and_connect(wifi_creds_linked_buffer->buffer, wifi_creds_linked_buffer->len);
        EZPI_core_ble_buffer_free_buffer(wifi_creds_linked_buffer);
        wifi_creds_linked_buffer = NULL;
    }
}

static void ezpi_wifi_creds_parse_and_connect(uint8_t *value, uint32_t len)
{
    if ((NULL == value) || (0 == len))
    {
        ezpi_wifi_prov_set_state(EZPI_WIFI_PROV_STATE_REJECTED, "empty credential write");
        return;
    }

    TRACE_I("ble-provisioning: credential write of %d byte(s) received", (int)len);

    cJSON *root = cJSON_Parse(__FUNCTION__, (const char *)value);

    if (NULL == root)
    {
        ezpi_wifi_prov_set_state(EZPI_WIFI_PROV_STATE_REJECTED, "malformed credential json");
        return;
    }

    cJSON *cj_ssid = cJSON_GetObjectItemCaseSensitive(root, ezlopi_wifi_ssid_str);
    cJSON *cj_user_id = cJSON_GetObjectItemCaseSensitive(root, ezlopi_user_id_str);
    cJSON *cj_password = cJSON_GetObjectItemCaseSensitive(root, ezlopi_wifi_password_str);

    if ((NULL == cj_ssid) || (NULL == cj_ssid->valuestring) ||
        (NULL == cj_user_id) || (NULL == cj_user_id->valuestring) ||
        (NULL == cj_password) || (NULL == cj_password->valuestring))
    {
        ezpi_wifi_prov_set_state(EZPI_WIFI_PROV_STATE_REJECTED, "missing wifi_ssid, wifi_password or user_id");
    }
    else
    {
        char *ssid = cj_ssid->valuestring;
        char *password = cj_password->valuestring;
        char *user_id_str = cj_user_id->valuestring;
        e_auth_status_t l_ble_auth_status = EZPI_ble_auth_check_user_id(user_id_str);

        /* The password is deliberately reduced to a length: this log goes to the
         * serial console and, at cloud severity, off the device. */
        TRACE_I("ble-provisioning: ssid '%s', password %d char(s), user_id '%s', auth_status %d",
                ssid, (int)strlen(password), user_id_str, (int)l_ble_auth_status);

        if ((BLE_AUTH_SUCCESS == l_ble_auth_status) || (BLE_AUTH_USER_ID_NOT_FOUND == l_ble_auth_status))
        {
            snprintf(sg_prov_ssid, sizeof(sg_prov_ssid), "%s", ssid);

            /* EZPI_core_wifi_connect() calls esp_wifi_stop() first, so re-provisioning
             * an already associated controller emits a disconnect for the OLD network.
             * Without this the teardown would be reported as a failure to join the new
             * one. */
            sg_prov_ignore_disconnect = (0 != EZPI_core_wifi_got_ip());

            ezpi_wifi_prov_set_state(EZPI_WIFI_PROV_STATE_CONNECTING, NULL);

            EZPI_core_wifi_connect(ssid, password);
            EZPI_core_factory_info_v3_set_wifi(ssid, password);

            if (BLE_AUTH_USER_ID_NOT_FOUND == l_ble_auth_status)
            {
                EZPI_ble_auth_store_user_id(user_id_str);
            }
        }
        else
        {
            /* A different user_id once the controller is already claimed. This
             * previously fell through both branches, so the client was told
             * nothing at all and simply waited forever. */
            ezpi_wifi_prov_set_state(EZPI_WIFI_PROV_STATE_REJECTED, "user_id not authorized for this controller");
        }
    }

    cJSON_Delete(__FUNCTION__, root);
}

static const char *ezpi_wifi_prov_state_str(e_ezpi_wifi_prov_state_t state)
{
    switch (state)
    {
    case EZPI_WIFI_PROV_STATE_CONNECTING:
        return "connecting";
    case EZPI_WIFI_PROV_STATE_CONNECTED:
        return "connected";
    case EZPI_WIFI_PROV_STATE_FAILED:
        return "failed";
    case EZPI_WIFI_PROV_STATE_REJECTED:
        return "rejected";
    case EZPI_WIFI_PROV_STATE_IDLE:
    default:
        return "idle";
    }
}

static char *ezpi_wifi_status_jsonify(void)
{
    char *status_jstr = NULL;
    cJSON *root = cJSON_CreateObject(__FUNCTION__);

    if (root)
    {
        cJSON_AddStringToObject(__FUNCTION__, root, ezlopi_status_str, ezpi_wifi_prov_state_str(sg_prov_state));
        cJSON_AddNumberToObject(__FUNCTION__, root, "code", sg_prov_state);

        if (0 != sg_prov_ssid[0])
        {
            cJSON_AddStringToObject(__FUNCTION__, root, ezlopi_wifi_ssid_str, sg_prov_ssid);
        }

        status_jstr = cJSON_PrintBuffered(__FUNCTION__, root, 128, false);
        cJSON_Delete(__FUNCTION__, root);
    }

    return status_jstr;
}

static char *ezpi_wifi_error_jsonify(void)
{
    char *error_jstr = NULL;
    cJSON *root = cJSON_CreateObject(__FUNCTION__);

    if (root)
    {
        cJSON_AddStringToObject(__FUNCTION__, root, ezlopi_error_str, sg_prov_error);
        error_jstr = cJSON_PrintBuffered(__FUNCTION__, root, 128, false);
        cJSON_Delete(__FUNCTION__, root);
    }

    return error_jstr;
}

/**
 * @brief Copies a (possibly offset) slice of 'jstr' into the read response and
 *        frees 'jstr'. Reads larger than the MTU are fetched by the client in
 *        successive offsets, matching the other ezlopi BLE read handlers.
 */
static void ezpi_wifi_read_jstr(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param, char *jstr)
{
    if (NULL == jstr)
    {
        value->len = 1;
        value->value[0] = 0;
        return;
    }

    uint32_t total_data_len = strlen(jstr);
    uint32_t max_data_buffer_size = EZPI_core_ble_gatt_get_max_data_size() - EZPI_BLE_READ_ATT_OVERHEAD;

    if (total_data_len > param->read.offset)
    {
        uint32_t remaining = total_data_len - param->read.offset;
        uint32_t copy_size = (remaining < max_data_buffer_size) ? remaining : max_data_buffer_size;
        memcpy(value->value, jstr + param->read.offset, copy_size);
        value->len = copy_size;
    }
    else
    {
        value->len = 0;
    }

    ezlopi_free(__FUNCTION__, jstr);
}

static void ezpi_wifi_status_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (value && param)
    {
        ezpi_wifi_read_jstr(value, param, ezpi_wifi_status_jsonify());
    }
}

static void ezpi_wifi_error_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (value && param)
    {
        ezpi_wifi_read_jstr(value, param, ezpi_wifi_error_jsonify());
    }
}

static bool ezpi_wifi_cccd_notify_requested(esp_ble_gatts_cb_param_t *param)
{
    /* CCCD is a little endian bitfield; bit 0 enables notifications. */
    return ((NULL != param) && (NULL != param->write.value) && (param->write.len >= 1) && (0 != (param->write.value[0] & 0x01)));
}

static void ezpi_wifi_cccd_fill_read(esp_gatt_value_t *value, bool enabled)
{
    if (value)
    {
        value->len = 2;
        value->value[0] = enabled ? 0x01 : 0x00;
        value->value[1] = 0x00;
    }
}

static void ezpi_wifi_status_cccd_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    ezpi_wifi_cccd_fill_read(value, sg_status_notify_enabled);
}

static void ezpi_wifi_error_cccd_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    ezpi_wifi_cccd_fill_read(value, sg_error_notify_enabled);
}

static uint32_t ezpi_wifi_current_generation(void)
{
    return (NULL != wifi_ble_service) ? wifi_ble_service->conn_generation : 0;
}

static void ezpi_wifi_status_cccd_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    sg_status_notify_enabled = ezpi_wifi_cccd_notify_requested(param);
    sg_status_notify_generation = ezpi_wifi_current_generation();
}

static void ezpi_wifi_error_cccd_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    sg_error_notify_enabled = ezpi_wifi_cccd_notify_requested(param);
    sg_error_notify_generation = ezpi_wifi_current_generation();
}

/**
 * @brief A subscription only applies to the connection that made it, so a
 *        reconnecting client does not inherit the previous client's.
 */
static bool ezpi_wifi_notify_allowed(bool enabled, uint32_t subscribed_generation)
{
    return (enabled && (NULL != wifi_ble_service) && wifi_ble_service->connected && (subscribed_generation == wifi_ble_service->conn_generation));
}

static void ezpi_wifi_notify_char(s_gatt_char_t *characteristic, char *jstr, uint8_t fallback_code)
{
    if ((NULL == characteristic) || (NULL == jstr))
    {
        return;
    }

    esp_gatt_value_t *notify_value = ezlopi_malloc(__FUNCTION__, sizeof(esp_gatt_value_t));

    if (notify_value)
    {
        uint16_t max_notify_len = EZPI_core_ble_gatt_get_max_data_size() - EZPI_BLE_NOTIFY_ATT_OVERHEAD;
        uint32_t jstr_len = strlen(jstr);

        memset(notify_value, 0, sizeof(esp_gatt_value_t));

        if (jstr_len <= max_notify_len)
        {
            memcpy(notify_value->value, jstr, jstr_len);
            notify_value->len = jstr_len;
        }
        else
        {
            /* Truncating would emit invalid JSON, so send the single byte state
             * code instead and let the client read the characteristic for detail. */
            notify_value->value[0] = fallback_code;
            notify_value->len = 1;
        }

        EZPI_core_ble_gatts_characteristic_notify(wifi_ble_service, characteristic, notify_value);
        ezlopi_free(__FUNCTION__, notify_value);
    }
}

static void ezpi_wifi_prov_set_state(e_ezpi_wifi_prov_state_t state, const char *error)
{
    sg_prov_state = state;

    if (NULL == error)
    {
        sg_prov_error[0] = 0;
    }
    else
    {
        snprintf(sg_prov_error, sizeof(sg_prov_error), "%s", error);
    }

    TRACE_I("ble-provisioning state: %s (%s)", ezpi_wifi_prov_state_str(state), (0 != sg_prov_error[0]) ? sg_prov_error : "-");

    if (ezpi_wifi_notify_allowed(sg_status_notify_enabled, sg_status_notify_generation))
    {
        char *status_jstr = ezpi_wifi_status_jsonify();
        ezpi_wifi_notify_char(wifi_status_char, status_jstr, (uint8_t)state);

        if (status_jstr)
        {
            ezlopi_free(__FUNCTION__, status_jstr);
        }
    }

    if ((0 != sg_prov_error[0]) && ezpi_wifi_notify_allowed(sg_error_notify_enabled, sg_error_notify_generation))
    {
        char *error_jstr = ezpi_wifi_error_jsonify();
        ezpi_wifi_notify_char(wifi_error_char, error_jstr, (uint8_t)state);

        if (error_jstr)
        {
            ezlopi_free(__FUNCTION__, error_jstr);
        }
    }
}

static void ezpi_wifi_prov_event_upcall(esp_event_base_t event, int32_t event_id, void *arg)
{
    if (WIFI_EVENT == event)
    {
        if (WIFI_EVENT_STA_DISCONNECTED == event_id)
        {
            if (sg_prov_ignore_disconnect)
            {
                /* Teardown of the previous association, not a failure of the new one. */
                sg_prov_ignore_disconnect = false;
            }
            else if (EZPI_WIFI_PROV_STATE_CONNECTING == sg_prov_state)
            {
                /* Only report a provisioning failure while a credential write is in
                 * flight; a later drop on an already provisioned device is a
                 * roaming/outage event, not a provisioning error. */
                ezpi_wifi_prov_set_state(EZPI_WIFI_PROV_STATE_FAILED, EZPI_core_wifi_get_last_disconnect_reason());
            }
        }
    }
    else if (IP_EVENT == event)
    {
        if (IP_EVENT_STA_GOT_IP == event_id)
        {
            ezpi_wifi_prov_set_state(EZPI_WIFI_PROV_STATE_CONNECTED, NULL);
        }
    }
}
#endif // CONFIG_EZPI_BLE_ENABLE

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
