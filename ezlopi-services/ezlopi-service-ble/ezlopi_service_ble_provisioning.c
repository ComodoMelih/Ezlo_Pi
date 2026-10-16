

/**
 * @file    ezlopi_service_ble_provisioning.c
 * @brief   Provisioning service related functionalities
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
 * @file    ezlopi_service_ble_provisioning.c
 * @brief   Provisioning service related functionalities
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
#include <time.h>

#include "lwip/ip_addr.h"
#include "mbedtls/base64.h"

#include "esp_event_base.h"
#include "esp_gatt_common_api.h"

#include "cjext.h"
#include "ezlopi_util_trace.h"
#include "ezlopi_util_sanitize.h"
#include "EZLOPI_USER_CONFIG.h"

#include "ezlopi_core_nvs.h"
#include "ezlopi_core_sntp.h"
#include "ezlopi_core_wifi.h"
#include "ezlopi_core_devices.h"
#include "ezlopi_core_ble_gatt.h"
#include "ezlopi_core_ble_buffer.h"
#include "ezlopi_core_ble_profile.h"
#include "ezlopi_core_event_group.h"
#include "ezlopi_core_factory_info.h"
#include "ezlopi_core_cjson_macros.h"
#include "ezlopi_core_event_group.h"
#include "ezlopi_core_buffer.h"

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
 * @brief Returns string from the the json `root` which contains name member
 * @note root is the JOSN and should exist before being called
 *
 */
#define CJ_GET_STRING(name) cJSON_GetStringValue(cJSON_GetObjectItem(__FUNCTION__, root, name))
/**
 * @brief Returns number from the the json `root` which contains name member
 * @note root is the JOSN and should exist before being called
 *
 */
#define CJ_GET_NUMBER(name) cJSON_GetNumberValue(cJSON_GetObjectItem(__FUNCTION__, root, name))

/**
 * @brief Largest "total_len" a provisioning write may claim.
 *
 * total_len counts base64 characters -- see ezpi_base64_decode_provisioning_info(),
 * which sizes its base64 staging buffer at total_len + 1 and fills it with the
 * sum of the chunks' "len" fields. It is attacker-controlled: it arrives in a
 * BLE write before anything in this file has authenticated anybody.
 *
 * A bound is needed for correctness, not just for thrift. total_len + 1 is a
 * uint32 addition, so a claim of 0xFFFFFFFF wraps it to 0, ezlopi_malloc(0)
 * hands back either NULL or a minimal block, and the "total_size - pos"
 * argument to EZPI_util_clamp_copy_len() below is then ~4GB of apparent room.
 * The clamp would be computed from a number that no longer describes the
 * allocation and would permit the very overflow it exists to stop. Capping
 * total_len first is what makes that clamp meaningful.
 *
 * The value is derived rather than picked. Everything a provisioning payload
 * can persist lands in the connection-data region of the factory partition,
 * which ends where the hub-data region begins at EZLOPI_FINFO_OFFSET_HUB_DATA
 * (0x6000). A decoded config bigger than that region cannot be stored whatever
 * it claims, and base64 expands by 4/3, so 4/3 of the region is already more
 * than any usable payload. Legitimate writes are nowhere near it: the three
 * PEM fields are capped at 0x1000 each by EZLOPI_FINFO_LEN_CA_CERTIFICATE and
 * its siblings.
 */
#define EZPI_BLE_PROV_MAX_BASE64_LEN ((EZLOPI_FINFO_OFFSET_HUB_DATA / 3u) * 4u)

/* The largest the chunk accumulator may grow to before the transfer is
 * abandoned. A different limit from the one above, and both are needed: that
 * one bounds the buffer the chunks are decoded *into*, this one bounds the
 * linked list they arrive *in*, which nothing bounded at all.
 *
 * Every write appends a node and the only thing that freed the list was the
 * transfer completing -- on a test the client itself supplies the numbers for
 * (`(sequence - 1) * 400 + len >= tot_len`). A client in radio range, with no
 * authentication, could therefore send chunks whose arithmetic never completes
 * and grow this list until the heap ran out.
 *
 * Derived the same way as the dynamic-config equivalent, and note it cannot be
 * EZPI_BLE_PROV_MAX_BASE64_LEN: the nodes hold whole JSON envelopes rather than
 * the bare base64, so the bytes held always exceed the payload they encode. A
 * legitimate client sends ceil(total_len / 400) chunks of at most one 517-byte
 * ATT write each, so the bound is that product -- about 42 KB here, which is
 * large because the provisioning payload legitimately is: three PEM blobs at
 * 0x1000 each. */
#define EZPI_BLE_PROV_MAX_ACCUM_LEN ((((EZPI_BLE_PROV_MAX_BASE64_LEN + 399u) / 400u) * 517u))

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
#ifdef EZPI_SERV_BLE_ENABLE_READ_PROV
/**
 * @brief Function converts provisoning data of the device into JOSN str
 *
 * @return char* Pointer to the JSON string
 * @retval JSON string pointer, or NULL on error
 */
static char *ezpi_provisioning_info_jsonify(void);
/**
 * @brief Function returns base64 encoded value of device provisioning info
 *
 * @return char* Pointer to the base64 encoded string
 * @retval Base64 string or NULL on error
 */
static char *ezpi_provisioning_info_base64(void);
/**
 * @brief Function to read provisioning info of the device
 *
 * @param[in] value Pointer to the command value
 * @param[in] param Pointer to the gatts callback parameter
 */
static void ezpi_provisioning_info_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
#endif // EZPI_SERV_BLE_ENABLE_READ_PROV
#ifdef EZPI_SERV_BLE_ENABLE_STAT_PROV
/**
 * @brief Function responds to read provisioning status characteristics for provisioning service
 *
 * @param[in] value Pointer to the command value
 * @param[in] param Pointer to the gatts callback parameter
 */
static void ezpi_provisioning_status_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
#endif // EZPI_SERV_BLE_ENABLE_STAT_PROV
/**
 * @brief Function to write provisioning info of the device
 *
 * @param[in] value Pointer to the command value
 * @param[in] param Pointer to the gatts callback parameter
 */
static void ezpi_provisioning_info_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
/**
 * @brief Function to decode base64 encoded provisioning info
 *
 * @param[in] total_size total size to decode
 * @return char* Pointer to the decoded string
 * @retval Decoded string or NULL on error
 */
static char *ezpi_base64_decode_provisioning_info(uint32_t total_size);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static s_gatt_service_t *g_provisioning_service;
static s_linked_buffer_t *g_provisioning_linked_buffer = NULL;
/* Which connection the chunks in the accumulator above belong to, so chunks
** left behind by a dropped connection are discarded rather than appended to. */
static uint32_t g_provisioning_buffer_generation;

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

void EZPI_ble_service_provisioning_init(void)
{
    esp_bt_uuid_t uuid;
    esp_gatt_perm_t permission;
    esp_gatt_char_prop_t properties;

    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = BLE_PROVISIONING_SERVICE_UUID;
    g_provisioning_service = EZPI_core_ble_gatt_create_service(BLE_PROVISIONING_ID_HANDLE, &uuid);

    uuid.uuid.uuid16 = BLE_PROVISIONING_CHAR_UUID;
    uuid.len = ESP_UUID_LEN_16;

#ifdef EZPI_SERV_BLE_ENABLE_READ_PROV
    permission = ESP_GATT_PERM_WRITE | ESP_GATT_PERM_READ;
    properties = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE;
    EZPI_core_ble_gatt_add_characteristic(g_provisioning_service, &uuid, permission, properties, ezpi_provisioning_info_read_func, ezpi_provisioning_info_write_func, NULL); // reliable-write is not implemented for now
#else                                                                                                                                                                        // EZPI_SERV_BLE_ENABLE_READ_PROV
    permission = ESP_GATT_PERM_WRITE;
    properties = ESP_GATT_CHAR_PROP_BIT_WRITE;
    EZPI_core_ble_gatt_add_characteristic(g_provisioning_service, &uuid, permission, properties, NULL, ezpi_provisioning_info_write_func, NULL); // reliable-write is not implemented for now
#endif                                                                                                                                                                       // EZPI_SERV_BLE_ENABLE_READ_PROV

#ifdef EZPI_SERV_BLE_ENABLE_STAT_PROV
    uuid.uuid.uuid16 = BLE_PROVISIONING_STATUS_CHAR_UUID;
    uuid.len = ESP_UUID_LEN_16;
    permission = ESP_GATT_PERM_READ;
    properties = ESP_GATT_CHAR_PROP_BIT_READ;
    EZPI_core_ble_gatt_add_characteristic(g_provisioning_service, &uuid, permission, properties, ezpi_provisioning_status_read_func, NULL, NULL);
#endif // EZPI_SERV_BLE_ENABLE_STAT_PROV
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/
#ifdef EZPI_SERV_BLE_ENABLE_STAT_PROV

static char *__provisioning_status_jsonify(void)
{
    char *prov_status_jstr = NULL;
    cJSON *root = cJSON_CreateObject(__FUNCTION__);
    if (root)
    {
        uint32_t prov_stat = EZPI_core_factory_info_v3_get_provisioning_status();
        if (1 == prov_stat)
        {
            cJSON_AddNumberToObject(__FUNCTION__, root, ezlopi_version_str, EZPI_core_factory_info_v3_get_version());
            cJSON_AddNumberToObject(__FUNCTION__, root, ezlopi_status_str, prov_stat);
        }
        else
        {
            cJSON_AddNumberToObject(__FUNCTION__, root, ezlopi_version_str, 0);
            cJSON_AddNumberToObject(__FUNCTION__, root, ezlopi_status_str, 0);
        }

        char tmp_buffer[32];
        snprintf(tmp_buffer, sizeof(tmp_buffer), "%08x", EZPI_core_nvs_config_info_version_number_get());
        cJSON_AddStringToObject(__FUNCTION__, root, ezlopi_config_id_str, tmp_buffer);
        cJSON_AddNumberToObject(__FUNCTION__, root, ezlopi_config_time_str, EZPI_core_nvs_config_info_update_time_get());

        prov_status_jstr = cJSON_PrintBuffered(__FUNCTION__, root, 256, false);

        cJSON_Delete(__FUNCTION__, root);
    }

    return prov_status_jstr;
}

static void ezpi_provisioning_status_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (value)
    {
        static char *prov_status_jstr;
        if (NULL == prov_status_jstr)
        {
            prov_status_jstr = __provisioning_status_jsonify();
        }

        if (NULL != prov_status_jstr)
        {
            uint32_t total_data_len = strlen(prov_status_jstr);
            uint32_t max_data_buffer_size = EZPI_core_ble_gatt_get_max_data_size();
            uint32_t copy_size = ((total_data_len - param->read.offset) < max_data_buffer_size) ? (total_data_len - param->read.offset) : max_data_buffer_size;

            if ((0 != total_data_len) && (total_data_len > param->read.offset))
            {
                strncpy((char *)value->value, prov_status_jstr + param->read.offset, copy_size);
                value->len = copy_size;
            }
            else
            {
                value->len = 1;
                value->value[0] = 0; // Read 0 if the device not provisioned yet.
            }

            if ((param->read.offset + copy_size) >= total_data_len)
            {
                ezlopi_free(__FUNCTION__, prov_status_jstr);
                prov_status_jstr = NULL;
            }
        }
        else
        {
            TRACE_E("Unable to create json string");
            value->len = 1;
            value->value[0] = 0; // Read 0 if the device not provisioned yet.
        }
    }
    else
    {
        TRACE_E("VALUE IS NULL");
    }
}

#endif // EZPI_SERV_BLE_ENABLE_READ_PROV

static void ezpi_provisioning_info_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    // TRACE_D("Write function called!");

    TRACE_D("GATT_WRITE_EVT value: %.*s", param->write.len, param->write.value);

    /* The accumulator outlived its connection: a client that began a
    ** provisioning write and dropped the link left its chunks here for the rest
    ** of the boot, and the next client appended to them instead of starting
    ** fresh. See the longer note on the same change in
    ** ezlopi_service_ble_dynamic_config.c. */
    const uint32_t conn_generation = (NULL != g_provisioning_service) ? g_provisioning_service->conn_generation : 0;

    if ((NULL != g_provisioning_linked_buffer) && (conn_generation != g_provisioning_buffer_generation))
    {
        TRACE_W("discarding %d byte(s) of provisioning data left by an earlier connection", EZPI_core_ble_buffer_total_len(g_provisioning_linked_buffer));
        EZPI_core_ble_buffer_free_buffer(g_provisioning_linked_buffer);
        g_provisioning_linked_buffer = NULL;
    }

    g_provisioning_buffer_generation = conn_generation;

    /* See EZPI_BLE_PROV_MAX_ACCUM_LEN. The cap on total_len bounds the buffer
    ** the chunks are decoded into; this bounds the list they arrive in, which
    ** nothing limited. total_len() of NULL is 0, so the first chunk takes the
    ** same test as the rest. */
    if ((EZPI_core_ble_buffer_total_len(g_provisioning_linked_buffer) + param->write.len) > EZPI_BLE_PROV_MAX_ACCUM_LEN)
    {
        TRACE_E("provisioning transfer exceeded %d byte(s) without completing, abandoning it", (uint32_t)EZPI_BLE_PROV_MAX_ACCUM_LEN);
        EZPI_core_ble_buffer_free_buffer(g_provisioning_linked_buffer);
        g_provisioning_linked_buffer = NULL;
        return;
    }

    if (NULL == g_provisioning_linked_buffer)
    {
        g_provisioning_linked_buffer = EZPI_core_ble_buffer_create(param);
    }
    else if (false == EZPI_core_ble_buffer_add_to_buffer(g_provisioning_linked_buffer, param))
    {
        /* A dropped chunk leaves a hole that the end-of-transfer length test
        ** does not notice, so the certificates written to flash would be
        ** truncated and the hub would fail to reach the cloud with no
        ** indication of why. */
        TRACE_E("could not store provisioning chunk, abandoning the transfer");
        EZPI_core_ble_buffer_free_buffer(g_provisioning_linked_buffer);
        g_provisioning_linked_buffer = NULL;
        return;
    }

    if (g_provisioning_linked_buffer)
    {
        if ((NULL != param->write.value) && (param->write.len > 0))
        {
            cJSON *root = cJSON_ParseWithLength(__FUNCTION__, (const char *)param->write.value, param->write.len);
            if (root)
            {
                uint32_t len = CJ_GET_NUMBER(ezlopi_len_str);
                uint32_t tot_len = CJ_GET_NUMBER(ezlopi_total_len_str);
                uint32_t sequence = CJ_GET_NUMBER(ezlopi_sequence_str);

                TRACE_D("Len: %d", len);
                TRACE_D("tot_len: %d", tot_len);
                TRACE_D("sequence: %d", sequence);

                if (sequence && len && tot_len)
                {
                    if (((sequence - 1) * 400 + len) >= tot_len)
                    {
                        char *decoded_data = ezpi_base64_decode_provisioning_info(tot_len); // uncommente f
                        if (decoded_data)
                        {
                            cJSON *cj_config = cJSON_ParseWithRef(__FUNCTION__, decoded_data);

                            if (cj_config)
                            {
                                /* Zeroed, like every other buffer filled from
                                 * this config below. It was the one that was
                                 * not, and CJSON_GET_VALUE_STRING_BY_COPY
                                 * leaves its destination completely untouched
                                 * when the key is absent or not a string -- see
                                 * EZPI_core_cjson_get_value_string_by_copy in
                                 * ezlopi_core_cjson_macros.c, where the
                                 * snprintf sits behind
                                 * "if (o_item && o_item->valuestring ...)".
                                 *
                                 * So a BLE provisioning payload that simply
                                 * omitted "user_id" passed uninitialised stack
                                 * to the comparison below, and on a device that
                                 * had not been claimed yet that stack garbage
                                 * was written into NVS as the owning user. */
                                char user_id[32] = {0};
                                CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_user_id_str, user_id, sizeof(user_id));

                                /* An absent key now yields "" rather than
                                 * garbage, but "" must not be allowed through
                                 * either: storing an empty user id would make
                                 * strlen(stored) zero, and the strncmp in
                                 * EZPI_ble_auth_check_user_id compares zero
                                 * bytes -- after which every subsequent client
                                 * authenticates. Reject it here instead.
                                 *
                                 * This matches what the other BLE provisioning
                                 * path already does: ezlopi_service_ble_wifi.c
                                 * refuses a payload "missing wifi_ssid,
                                 * wifi_password or user_id". A config with no
                                 * user id was never serviceable. */
                                if ('\0' == user_id[0])
                                {
                                    TRACE_E("provisioning config has no user_id; rejecting");
                                }
                                else if (BLE_AUTH_SUCCESS == EZPI_ble_auth_check_user_id(user_id))
                                {
                                    s_basic_factory_info_t *ezlopi_config_basic = ezlopi_malloc(__FUNCTION__, sizeof(s_basic_factory_info_t));
                                    if (ezlopi_config_basic)
                                    {
                                        memset(ezlopi_config_basic, 0, sizeof(s_basic_factory_info_t));

                                        char device_name[EZLOPI_FINFO_LEN_DEVICE_NAME];
                                        char manufacturer[EZLOPI_FINFO_LEN_MANUF_NAME];
                                        char brand[EZLOPI_FINFO_LEN_BRAND_NAME];
                                        char model_number[EZLOPI_FINFO_LEN_MODEL_NAME];
                                        char device_uuid[EZLOPI_FINFO_LEN_DEVICE_UUID];
                                        char prov_uuid[EZLOPI_FINFO_LEN_PROV_UUID];
                                        char device_mac[EZLOPI_FINFO_LEN_DEVICE_MAC];
                                        char provision_server[EZLOPI_FINFO_LEN_PROVISIONING_SERVER_URL];
                                        char cloud_server[EZLOPI_FINFO_LEN_CLOUD_SERVER_URL];
                                        char provision_toke[EZLOPI_FINFO_LEN_PROVISIONING_TOKEN];
                                        char local_key[EZLOPI_FINFO_LEN_LOCAL_KEY];
                                        // char device_type[32];

                                        memset(device_name, 0, EZLOPI_FINFO_LEN_DEVICE_NAME);
                                        memset(manufacturer, 0, EZLOPI_FINFO_LEN_MANUF_NAME);
                                        memset(brand, 0, EZLOPI_FINFO_LEN_BRAND_NAME);
                                        memset(model_number, 0, EZLOPI_FINFO_LEN_MODEL_NAME);
                                        memset(device_uuid, 0, EZLOPI_FINFO_LEN_DEVICE_UUID);
                                        memset(prov_uuid, 0, EZLOPI_FINFO_LEN_PROV_UUID);
                                        memset(device_mac, 0, EZLOPI_FINFO_LEN_DEVICE_MAC);
                                        memset(provision_server, 0, EZLOPI_FINFO_LEN_PROVISIONING_SERVER_URL);
                                        memset(cloud_server, 0, EZLOPI_FINFO_LEN_CLOUD_SERVER_URL);
                                        memset(provision_toke, 0, EZLOPI_FINFO_LEN_PROVISIONING_TOKEN);
                                        memset(local_key, 0, EZLOPI_FINFO_LEN_LOCAL_KEY);
                                        // memset(device_type, 0, sizeof(device_type));

                                        uint32_t _id = 0;
                                        CJSON_GET_VALUE_UINT32(cj_config, ezlopi_serial_str, _id);
                                        ezlopi_config_basic->id = _id;

                                        CJSON_GET_VALUE_UINT16(cj_config, ezlopi_version_str, ezlopi_config_basic->config_version);

                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_device_name_str, device_name, sizeof(device_name));
                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_manufacturer_name_str, manufacturer, sizeof(manufacturer));
                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_brand_str, brand, sizeof(brand));
                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_model_number_str, model_number, sizeof(model_number));
                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_uuid_str, device_uuid, sizeof(device_uuid));
                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_uuid_provisioning_str, prov_uuid, sizeof(prov_uuid));
                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_mac_str, device_mac, sizeof(device_mac));
                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_provision_server_str, provision_server, sizeof(provision_server));
                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_cloud_server_str, cloud_server, sizeof(cloud_server));
                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_provision_token_str, provision_toke, sizeof(provision_toke));
                                        CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_local_key_str, local_key, sizeof(local_key));
                                        // CJSON_GET_VALUE_STRING_BY_COPY(cj_config, ezlopi_device_type_ezlopi_str, device_type, sizeof(device_type));

                                        ezlopi_config_basic->device_name = device_name;
                                        ezlopi_config_basic->manufacturer = manufacturer;
                                        ezlopi_config_basic->brand = brand;
                                        ezlopi_config_basic->model_number = model_number;
                                        ezlopi_config_basic->device_uuid = device_uuid;
                                        ezlopi_config_basic->prov_uuid = prov_uuid;
                                        ezlopi_config_basic->device_mac = device_mac;
                                        ezlopi_config_basic->provision_server = provision_server;
                                        ezlopi_config_basic->cloud_server = cloud_server;
                                        ezlopi_config_basic->provision_token = provision_toke;
                                        ezlopi_config_basic->local_key = local_key;
                                        ezlopi_config_basic->device_type = NULL;

                                        EZPI_core_factory_info_v3_set_basic(ezlopi_config_basic);
                                        uint32_t version_no = EZPI_core_nvs_config_info_version_number_get() + 1;
                                        EZPI_core_nvs_config_info_version_number_set(version_no);

                                        TRACE_D("time now{size: %u}: %lu", sizeof(time_t), EZPI_core_sntp_get_current_time_sec());
                                        EZPI_core_nvs_config_info_update_time_set(EZPI_core_sntp_get_current_time_sec());

                                        ezlopi_free(__FUNCTION__, ezlopi_config_basic);
                                    }

                                    EZPI_core_factory_info_v3_set_ca_cert(cJSON_GetObjectItem(__FUNCTION__, cj_config, ezlopi_signing_ca_certificate_str));
                                    EZPI_core_factory_info_v3_set_ssl_shared_key(cJSON_GetObjectItem(__FUNCTION__, cj_config, ezlopi_ssl_shared_key_str));
                                    EZPI_core_factory_info_v3_set_ssl_private_key(cJSON_GetObjectItem(__FUNCTION__, cj_config, ezlopi_ssl_private_key_str));
                                }
                                else
                                {
                                    TRACE_E("User verification failed!");

                                    char *curr_user_id = EZPI_core_nvs_read_user_id_str();
                                    if (curr_user_id)
                                    {
                                        TRACE_D("current user: %s", curr_user_id);
                                        ezlopi_free(__FUNCTION__, curr_user_id);
                                    }
                                }

                                cJSON_Delete(__FUNCTION__, cj_config);
                            }

                            ezlopi_free(__FUNCTION__, decoded_data);
                            decoded_data = NULL;
                        }

                        EZPI_core_ble_buffer_free_buffer(g_provisioning_linked_buffer);
                        g_provisioning_linked_buffer = NULL;
                    }
                }

                /* root holds a parsed copy of this chunk's envelope and nothing
                 * below keeps a pointer into it, but it was never released --
                 * so every chunk of every provisioning write leaked a whole
                 * cJSON tree. Unauthenticated: the leak happens on any write to
                 * this characteristic, valid or not, because the only thing
                 * between the write and the parse is a length check. */
                cJSON_Delete(__FUNCTION__, root);
            }
        }
    }
}

#ifdef EZPI_SERV_BLE_ENABLE_READ_PROV
static void ezpi_provisioning_info_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    // TRACE_D("Read function called!");

    static const uint32_t _data_size = 400;
    static char *g_provisioning_info_base64;
    static uint32_t g_provisioning_sequence_no;
    static time_t g_provisioning_last_read_time;
    static uint32_t g_provisioning_number_of_sequence;

    static const time_t gc_provisioning_read_timeout_s = 2;
    static const uint32_t g_required_ble_prov_buffer_size = 517;

    // timeout logic
    int status = -1; // success for non negative, failed for negative

    if ((EZPI_core_sntp_get_current_time_sec() - g_provisioning_last_read_time) >= gc_provisioning_read_timeout_s)
    {
        g_provisioning_sequence_no = 0;
    }

    g_provisioning_last_read_time = EZPI_core_sntp_get_current_time_sec();

    if (value)
    {
        if (NULL == g_provisioning_info_base64)
        {
            g_provisioning_info_base64 = ezpi_provisioning_info_base64();

            g_provisioning_sequence_no = 0;
            g_provisioning_number_of_sequence = strlen(g_provisioning_info_base64) / EZPI_core_ble_gatt_get_max_data_size();
            g_provisioning_number_of_sequence = (strlen(g_provisioning_info_base64) % EZPI_core_ble_gatt_get_max_data_size()) ? (g_provisioning_number_of_sequence + 1) : g_provisioning_number_of_sequence;
        }

        if (NULL != g_provisioning_info_base64)
        {
            if (EZPI_core_ble_gatt_get_max_data_size() >= g_required_ble_prov_buffer_size)
            {
                uint32_t total_data_len = strlen(g_provisioning_info_base64);
                uint32_t copy_size = total_data_len - (g_provisioning_sequence_no * _data_size);
                copy_size = (copy_size > _data_size) ? _data_size : copy_size;

                cJSON *cj_response = cJSON_CreateObject(__FUNCTION__);
                if (cj_response)
                {
                    char data_buffer[_data_size + 4];
                    snprintf(data_buffer, sizeof(data_buffer), "%.*s", copy_size, g_provisioning_info_base64 + (g_provisioning_sequence_no * _data_size));

                    cJSON_AddNumberToObject(__FUNCTION__, cj_response, ezlopi_len_str, copy_size);
                    cJSON_AddNumberToObject(__FUNCTION__, cj_response, ezlopi_total_len_str, total_data_len);
                    cJSON_AddNumberToObject(__FUNCTION__, cj_response, ezlopi_sequence_str, g_provisioning_sequence_no);
                    cJSON_AddStringToObject(__FUNCTION__, cj_response, ezlopi_data_str, data_buffer);

                    const uint32_t buffer_len = 512;
                    char json_to_str_buffer[buffer_len];
                    memset(json_to_str_buffer, 0, buffer_len);
                    status = cJSON_PrintPreallocated(__FUNCTION__, cj_response, json_to_str_buffer, buffer_len, false);
                    cJSON_Delete(__FUNCTION__, cj_response);

                    if (true == status)
                    {
                        TRACE_D("data: %s", json_to_str_buffer);

                        if ((0 != total_data_len) && (total_data_len >= ((g_provisioning_sequence_no * _data_size) + copy_size)))
                        {
                            value->len = strlen(json_to_str_buffer);
                            strncpy((char *)value->value, json_to_str_buffer, value->len + 1);

                            TRACE_I("data: %s", (char *)value->value);

                            g_provisioning_sequence_no += 1;
                            status = 0;

                            if (copy_size < _data_size) // Done reading
                            {
                                status = 1; // non negative for done reading
                                ezlopi_free(__FUNCTION__, g_provisioning_info_base64);
                                g_provisioning_info_base64 = NULL;
                            }
                        }
                        else
                        {
                            TRACE_W("Check value: %d", ((g_provisioning_sequence_no * _data_size) + copy_size));
                            TRACE_W("total_data_len: %d", total_data_len);
                            status = -4;
                        }
                    }
                    else
                    {
                        status = -3;
                    }
                }
                else
                {
                    status = -2;
                }
            }
            else
            {
                TRACE_E("MTU size must be greater than %d!", g_required_ble_prov_buffer_size);
                TRACE_W("call SET-MTU API from client stack!");

                CHECK_PRINT_ERROR(esp_ble_gatt_set_local_mtu(g_required_ble_prov_buffer_size), "set local  MTU failed");
                status = -2;
            }
        }

        if (status)
        {
            if (status < 0)
            {
                TRACE_E("Error found: %d", status);
                value->len = 1;
                value->value[0] = 0; // Read 0 if the device not provisioned yet.
            }
            else
            {
                TRACE_S("Done Transmitting.");
            }

            g_provisioning_sequence_no = 0;
            g_provisioning_number_of_sequence = 0;

            if (g_provisioning_info_base64)
            {
                ezlopi_free(__FUNCTION__, g_provisioning_info_base64);
                g_provisioning_info_base64 = NULL;
            }
        }
    }
    else
    {
        TRACE_E("VALUE IS NULL");
    }
}

#endif // EZPI_SERV_BLE_ENABLE_READ_PROV

static char *ezpi_base64_decode_provisioning_info(uint32_t total_size)
{
    char *decoded_config_json = NULL;
    char *base64_buffer = NULL;

    TRACE_W("tatal data length: %d", total_size);

    /* See EZPI_BLE_PROV_MAX_BASE64_LEN. This has to happen before the
    ** allocation below, because total_size + 1 is what wraps. */
    if ((0 == total_size) || (total_size > EZPI_BLE_PROV_MAX_BASE64_LEN))
    {
        TRACE_E("total_len %d is outside 1..%d, refusing", total_size, (uint32_t)EZPI_BLE_PROV_MAX_BASE64_LEN);
        return NULL;
    }

    base64_buffer = ezlopi_malloc(__FUNCTION__, total_size + 1);

    if (base64_buffer)
    {
        uint32_t pos = 0;
        s_linked_buffer_t *tmp_prov_buffer = g_provisioning_linked_buffer;

        /* If the very first chunk carries no data member, the strlen() and the
         * trace below would both read this buffer uninitialised. */
        base64_buffer[0] = '\0';

        while (tmp_prov_buffer)
        {
            cJSON *root = cJSON_ParseWithLength(__FUNCTION__, (const char *)tmp_prov_buffer->buffer, tmp_prov_buffer->len);
            if (root)
            {
                uint32_t len = CJ_GET_NUMBER(ezlopi_len_str);
                char *data = CJ_GET_STRING(ezlopi_data_str);
                if (data)
                {
                    /* len and the data member both come straight off the wire,
                     * and base64_buffer is only total_size + 1 bytes. Copying
                     * len bytes unchecked let a client whose chunks claim more
                     * than total_len -- or whose len exceeds the string it
                     * actually sent -- read past the end of the parsed string
                     * and write past the end of this heap block. "len" is a
                     * JSON number with no enforced relation to the length of
                     * "data" at all, so a single 517-byte BLE write claiming
                     * {"total_len":1,"sequence":1,"len":100000,"data":"AA"}
                     * was enough to reach it.
                     *
                     * This is the same fix already carried by the near-identical
                     * ezpi_base64_decode_dynamic_config(); the two functions are
                     * copies of each other and only one of them had been
                     * corrected. */
                    uint32_t safe_len = EZPI_util_clamp_copy_len(len, (uint32_t)strlen(data), total_size - pos);
                    if (safe_len != len)
                    {
                        TRACE_E("chunk claimed %d byte(s), only %d safe at pos %d of %d, truncating", len, safe_len, pos, total_size);
                    }
                    len = safe_len;

                    memcpy(base64_buffer + pos, data, len);
                    pos += len;
                    base64_buffer[pos] = '\0';
                }
                else
                {
                    TRACE_E("DATA IS NULL");
                }

                /* data pointed into root, so this has to come after the copy.
                 * It was missing entirely, leaking one tree per chunk. */
                cJSON_Delete(__FUNCTION__, root);
            }
            else
            {
                TRACE_E("Failed to parse");
            }

            tmp_prov_buffer = tmp_prov_buffer->next;
        }

        TRACE_D("base64_buffer: %s", base64_buffer);

        decoded_config_json = ezlopi_malloc(__FUNCTION__, total_size);
        // decoded_config_json = EZPI_core_buffer_acquire(__FUNCTION__, &buffer_len, 5000);

        if (decoded_config_json)
        {
            size_t o_len = 0;
            bzero(decoded_config_json, total_size);
            mbedtls_base64_decode((uint8_t *)decoded_config_json, (size_t)total_size, &o_len, (uint8_t *)base64_buffer, strlen(base64_buffer));
            TRACE_D("Decoded data: %s", decoded_config_json);
        }
        else
        {
            TRACE_E("decoding bufffer allocation failed!");
        }

        ezlopi_free(__FUNCTION__, base64_buffer);
    }

    return decoded_config_json;
}

#ifdef EZPI_SERV_BLE_ENABLE_READ_PROV
static char *ezpi_provisioning_info_jsonify(void)
{
    char *str_json_prov_info = NULL;

    cJSON *cj_prov_info = cJSON_CreateObject(__FUNCTION__);
    if (cj_prov_info)
    {
        char tmp_buffer[32];
        char *device_name = EZPI_core_factory_info_v3_get_name();
        char *brand = EZPI_core_factory_info_v3_get_brand();
        char *manufacturer_name = EZPI_core_factory_info_v3_get_manufacturer();
        char *model_number = EZPI_core_factory_info_v3_get_model();
        char *uuid = EZPI_core_factory_info_v3_get_device_uuid();
        char *uuid_provisioning = EZPI_core_factory_info_v3_get_provisioning_uuid();
        char *cloud_server = EZPI_core_factory_info_v3_get_cloud_server();
        char *ssl_private_key = EZPI_core_factory_info_v3_get_ssl_private_key();
        char *ssl_shared_key = EZPI_core_factory_info_v3_get_ssl_shared_key();
        char *ca_cert = EZPI_core_factory_info_v3_get_ca_certificate();

        snprintf(tmp_buffer, sizeof(tmp_buffer), "%08x", EZPI_core_nvs_config_info_version_number_get());
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_config_id_str, tmp_buffer);
        cJSON_AddNumberToObject(__FUNCTION__, cj_prov_info, ezlopi_config_time_str, EZPI_core_nvs_config_info_update_time_get());
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_device_name_str, device_name);
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_brand_str, brand);
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_manufacturer_name_str, manufacturer_name);
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_model_number_str, model_number);
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_uuid_str, uuid);
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_uuid_provisioning_str, uuid_provisioning);
        cJSON_AddNumberToObject(__FUNCTION__, cj_prov_info, ezlopi_serial_str, EZPI_core_factory_info_v3_get_id());
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_cloud_server_str, cloud_server);
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_ssl_private_key_str, ssl_private_key);
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_ssl_shared_key_str, ssl_shared_key);
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_ca_cert_str, ca_cert);
        cJSON_AddStringToObject(__FUNCTION__, cj_prov_info, ezlopi_device_type_ezlopi_str, EZPI_core_factory_info_v3_get_device_type());

        EZPI_core_factory_info_v3_free(device_name);
        EZPI_core_factory_info_v3_free(brand);
        EZPI_core_factory_info_v3_free(manufacturer_name);
        EZPI_core_factory_info_v3_free(model_number);
        EZPI_core_factory_info_v3_free(uuid);
        EZPI_core_factory_info_v3_free(uuid_provisioning);
        EZPI_core_factory_info_v3_free(cloud_server);
        // EZPI_core_factory_info_v3_free(ssl_private_key); // allocated once for all, do not free
        // EZPI_core_factory_info_v3_free(ssl_shared_key); // allocated once for all, do not free
        // EZPI_core_factory_info_v3_free(ca_cert); // allocated once for all, do not free

        str_json_prov_info = cJSON_PrintBuffered(__FUNCTION__, cj_prov_info, 6 * 1024, false);
        cJSON_Delete(__FUNCTION__, cj_prov_info);
    }

    return str_json_prov_info;
}

static char *ezpi_provisioning_info_base64(void)
{
    char *base64_data = NULL;
    char *str_provisioning_data = ezpi_provisioning_info_jsonify();

    if (str_provisioning_data)
    {
        const uint32_t base64_data_len = 6 * 1024;
        base64_data = ezlopi_malloc(__FUNCTION__, base64_data_len);

        if (base64_data)
        {
            uint32_t out_put_len = 0;
            TRACE_D("str_provisioning_data[len: %d]: %s", strlen(str_provisioning_data), str_provisioning_data);

            int ret = mbedtls_base64_encode((unsigned char *)base64_data, base64_data_len, &out_put_len,
                                            (const unsigned char *)str_provisioning_data, strlen(str_provisioning_data));

            if (0 == out_put_len)
            {
                ezlopi_free(__FUNCTION__, base64_data);
                base64_data = NULL;
            }
        }

        ezlopi_free(__FUNCTION__, str_provisioning_data);
    }

    return base64_data;
}

#endif // CONFIG_EZPI_BLE_ENABLE

#endif // EZPI_SERV_BLE_ENABLE_READ_PROV

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
