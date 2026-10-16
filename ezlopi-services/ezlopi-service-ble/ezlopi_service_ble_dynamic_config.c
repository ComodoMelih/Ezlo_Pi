
/**
 * @file    ezlopi_service_ble_dynamic_config.c
 * @brief   Dynamic config service related functionalities
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
 * @file    ezlopi_service_ble_dynamic_config.c
 * @brief   Dynamic config service related functionalities
 * @author
 * @version 1.0
 * @date    January 22, 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include "sdkconfig.h"

#ifdef CONFIG_EZPI_BLE_ENABLE

#include <time.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/ip_addr.h"
#include "mbedtls/base64.h"

#include "esp_event_base.h"
#include "esp_gatt_common_api.h"

#include "ezlopi_util_trace.h"
#include "ezlopi_util_sanitize.h"
#include "ezlopi_util_devcfgval.h"
#include "cjext.h"
#include "EZLOPI_USER_CONFIG.h"

#include "ezlopi_core_nvs.h"
#include "ezlopi_core_wifi.h"
#include "ezlopi_core_sntp.h"
#include "ezlopi_core_reset.h"
#include "ezlopi_core_devices.h"
#include "ezlopi_core_ble_gatt.h"
#include "ezlopi_core_ble_buffer.h"
#include "ezlopi_core_ble_profile.h"
#include "ezlopi_core_cjson_macros.h"
#include "ezlopi_core_factory_info.h"

#include "ezlopi_cloud_constants.h"

#include "ezlopi_service_ble.h"
#include "ezlopi_service_ble_ble_auth.h"

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

/* The largest base64 payload a config write can legitimately carry.
 *
 * Needed for correctness and not only for thrift, the same way
 * EZPI_BLE_PROV_MAX_BASE64_LEN is in ezlopi_service_ble_provisioning.c. The
 * declared total_len arrives off the air and reaches
 * ezlopi_malloc(total_size + 1); that addition is on a uint32, so a claim of
 * 0xFFFFFFFF wraps it to 0. ezlopi_malloc(0) returns either NULL or a minimal
 * block, and if it returns a block then `base64_buffer[0] = '\0'` writes
 * outside it, and `total_size - pos` -- the room argument to
 * EZPI_util_clamp_copy_len() -- is ~4 GB of room that does not exist. The
 * clamp added to stop chunks overrunning this buffer is computed from
 * total_size, so leaving total_size unchecked disarms it. Bounding it first is
 * what makes that clamp mean anything.
 *
 * The number is derived, not chosen. A config written here is persisted by
 * EZPI_core_factory_info_v3_set_ezlopi_config(), which serialises into a 4 KB
 * buffer with cJSON_PrintPreallocated() and fails if the JSON does not fit --
 * EZLOPI_FINFO_LEN_EZLOPI_CONFIG_JSON is 0x1000 and the field in flash is that
 * size. So a config whose decoded form exceeds 0x1000 bytes cannot be stored
 * no matter what it claims, and base64 of 0x1000 bytes is ceil(0x1000/3)*4.
 *
 * The ceiling matters here and does not in the provisioning macro, which uses a
 * floor over a region 6x larger than any payload that can reach it. At this
 * size the two differ by 4 bytes, which is the difference between accepting and
 * refusing a config that exactly fills the field. */
#define EZPI_BLE_DYNCFG_MAX_BASE64_LEN ((((uint32_t)EZLOPI_FINFO_LEN_EZLOPI_CONFIG_JSON + 2u) / 3u) * 4u)

/* The largest the chunk accumulator may grow to before the transfer is
 * abandoned.
 *
 * This is a separate limit from the one above and both are needed. The cap on
 * total_len bounds the buffer the chunks are copied *into*, and does nothing
 * about the list they are copied *from*: every write appends a node, and the
 * completion test is `(sequence - 1) * 400 + len >= tot_len` on three numbers
 * the client supplies. A client that keeps sending chunks whose arithmetic
 * never satisfies that test -- or simply never sends the last one -- grew this
 * list without limit, and the BLE characteristics take writes from anyone in
 * radio range with no authentication at all. Two ways that ended badly: the
 * heap ran out, or the transfer eventually completed and the accumulator was
 * freed by a function that recursed once per node.
 *
 * It cannot be EZPI_BLE_DYNCFG_MAX_BASE64_LEN, which is the trap worth naming:
 * the nodes hold whole JSON envelopes -- {"len":400,"total_len":...,
 * "sequence":...,"data":"..."} -- not the bare base64, so the bytes held always
 * exceed the payload they encode. Bounding the list at the payload size would
 * refuse a perfectly ordinary config somewhere around three quarters of the way
 * through.
 *
 * So it is derived from what a legitimate transfer actually does: a client
 * sends ceil(total_len / 400) chunks, because 400 is the data-per-chunk this
 * file's read path emits and the offset its write path assumes, and each chunk
 * is one ATT write, which this service already requires to be 517 bytes
 * (g_required_ble_prov_buffer_size). That is 14 * 517 = 7238 bytes for a config
 * that exactly fills the 4 KB field -- comfortably above any real one, and a
 * bound rather than no bound. */
#define EZPI_BLE_DYNCFG_MAX_ACCUM_LEN ((((EZPI_BLE_DYNCFG_MAX_BASE64_LEN + 399u) / 400u) * 517u))

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/

/**
 * @brief Function to encode dynamic config data to base64 string
 *
 * @return char* pointer to the base64 encoded string
 * @retval Base64 encoded string or NULL on error
 */
static char *ezpi_dynamic_config_base64(void);
/**
 * @brief Function to decode base64 encoded dynamic config data
 *
 * @param total_size total size to decode
 * @return char* Pointer to the decoded string
 * @retval Decoded string or NULL on error
 */
static char *ezpi_base64_decode_dynamic_config(uint32_t total_size);
/**
 * @brief Function responds to ESP_GATTS_WRITE_EVT event for dynamic config write characteristics
 *
 * @param value Pointer to the command value
 * @param param Pointer to the gatts callback parameter
 */
static void ezpi_dynamic_config_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
/**
 * @brief Function responds to ESP_GATTS_READ_EVT event for dynamic config read characteristics
 *
 * @param value Pointer to the command value
 * @param param Pointer to the gatts callback parameter
 */
static void ezpi_dynamic_config_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static s_gatt_service_t *g_dynamic_config_service = NULL;
static s_linked_buffer_t *g_dynamic_config_linked_buffer = NULL;
/* Which connection the chunks in the accumulator above belong to, so chunks
** left behind by a dropped connection are discarded rather than appended to. */
static uint32_t g_dynamic_config_buffer_generation;

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

void EZPI_ble_service_dynamic_config_init(void)
{
    esp_bt_uuid_t uuid;
    esp_gatt_perm_t permission;
    esp_gatt_char_prop_t properties;

    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = BLE_DYNAMIC_CONFIG_SERVICE_UUID;
    g_dynamic_config_service = EZPI_core_ble_gatt_create_service(BLE_DYNAMIC_CONFIG_HANDLE, &uuid);

    uuid.uuid.uuid16 = BLE_DYNAMIC_CONFIG_CHAR_UUID;
    uuid.len = ESP_UUID_LEN_16;
    permission = ESP_GATT_PERM_WRITE | ESP_GATT_PERM_READ;
    properties = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE;
    EZPI_core_ble_gatt_add_characteristic(g_dynamic_config_service, &uuid, permission, properties, ezpi_dynamic_config_read_func, ezpi_dynamic_config_write_func, NULL); // reliable-write is not implemented for now
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/* Devices are instantiated exactly once per boot: EZPI_device_prepare() ->
 * ezlopi_device_parse_json_v3() is reachable only from ezlopi_core_ezlopi.c,
 * and nothing re-reads the stored config afterwards. So a config written here
 * is persisted to flash but has no effect on the running hub until it reboots
 * -- which is why adding a device over BLE appeared to do nothing.
 *
 * The reboot cannot happen inline in the write upcall. The GATT layer sends the
 * ATT write response only after the upcall returns (see
 * EZPI_core_ble_gatt_call_write_by_handle in ezlopi_core_ble_gatt.c), so
 * restarting inside it swallows the response to the final chunk: the client's
 * write request goes unanswered and the link just drops. Hence the short-lived
 * task -- it lets the upcall return and the response reach the phone first. */
static void ezpi_dynamic_config_reboot_task(void *arg)
{
    vTaskDelay(1500 / portTICK_PERIOD_MS);
    TRACE_W("New device config stored, restarting to apply it .....");
    EZPI_core_reset_reboot();
    vTaskDelete(NULL);
}

static void ezpi_dynamic_config_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    bool reboot_to_apply = false;

    TRACE_D("Write function called!");
    TRACE_D("GATT_WRITE_EVT value: %.*s", param->write.len, param->write.value);

    /* Nothing used to end a part-finished transfer except its own completion,
    ** so a client that started a config write and dropped the link left its
    ** chunks in this global for the lifetime of the boot. The next client did
    ** not start a new transfer, it appended to the abandoned one: the decode
    ** then concatenated both and produced base64 that does not parse, and the
    ** only way back to a working config write was a reboot. On a product whose
    ** point is to be plugged in and configured from a phone, "retry after the
    ** connection dropped" is the common case, not an edge case.
    **
    ** conn_generation is bumped on every CONNECT_EVT, which is what makes a
    ** new connection distinguishable -- conn_id alone is reused by the
    ** controller. Treating generation 0 (no service object) as "same" is
    ** deliberate: the fallback should lose the reset, not discard a transfer
    ** that is in progress. */
    const uint32_t conn_generation = (NULL != g_dynamic_config_service) ? g_dynamic_config_service->conn_generation : 0;

    if ((NULL != g_dynamic_config_linked_buffer) && (conn_generation != g_dynamic_config_buffer_generation))
    {
        TRACE_W("discarding %d byte(s) of config left by an earlier connection", EZPI_core_ble_buffer_total_len(g_dynamic_config_linked_buffer));
        EZPI_core_ble_buffer_free_buffer(g_dynamic_config_linked_buffer);
        g_dynamic_config_linked_buffer = NULL;
    }

    g_dynamic_config_buffer_generation = conn_generation;

    /* See EZPI_BLE_DYNCFG_MAX_ACCUM_LEN. total_len() of NULL is 0, so the
    ** first chunk of a transfer goes through the same test as the rest. */
    if ((EZPI_core_ble_buffer_total_len(g_dynamic_config_linked_buffer) + param->write.len) > EZPI_BLE_DYNCFG_MAX_ACCUM_LEN)
    {
        TRACE_E("config transfer exceeded %d byte(s) without completing, abandoning it", (uint32_t)EZPI_BLE_DYNCFG_MAX_ACCUM_LEN);
        EZPI_core_ble_buffer_free_buffer(g_dynamic_config_linked_buffer);
        g_dynamic_config_linked_buffer = NULL;
        return;
    }

    if (NULL == g_dynamic_config_linked_buffer)
    {
        g_dynamic_config_linked_buffer = EZPI_core_ble_buffer_create(param);
    }
    else if (false == EZPI_core_ble_buffer_add_to_buffer(g_dynamic_config_linked_buffer, param))
    {
        /* The chunk could not be stored. Carrying on would leave a hole in the
        ** middle of the base64 while the length test at the end still passed,
        ** so what landed in flash would be a config that decodes to nothing
        ** useful -- and the failure would surface one reboot later as a hub
        ** with no devices. Abandoning the transfer is the honest outcome. */
        TRACE_E("could not store config chunk, abandoning the transfer");
        EZPI_core_ble_buffer_free_buffer(g_dynamic_config_linked_buffer);
        g_dynamic_config_linked_buffer = NULL;
        return;
    }

    if (g_dynamic_config_linked_buffer)
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
                        char *decoded_data = ezpi_base64_decode_dynamic_config(tot_len); // uncommente f
                        if (decoded_data)
                        {
                            cJSON *cjson_config = cJSON_Parse(__FUNCTION__, decoded_data);
                            if (cjson_config)
                            {
                                /* Refuse a document that cannot produce a
                                ** device list before it reaches flash, and
                                ** before the reboot below applies it. Until
                                ** this check "cJSON parsed it" was the only
                                ** requirement on the most exposed path in the
                                ** firmware -- this characteristic takes writes
                                ** from anyone in radio range, with no pairing.
                                ** Storing `{"hello":"world"}` here costs the
                                ** user every configured sensor: the next boot
                                ** finds no "chipset", the recovery rule stamps
                                ** one and restarts, and the boot after that
                                ** reports success with an empty device list.
                                **
                                ** Size arguments are 0 deliberately; see the
                                ** matching comment in ezlopi_service_uart.c.
                                **
                                ** reboot_to_apply is left false, so a refused
                                ** configuration does not restart the hub. */
                                int bad_index = -1;
                                const e_ezpi_devcfgval_t reason =
                                    EZPI_util_devcfgval_check(cjson_config,
                                                              EZPI_core_device_expected_chipset_name(),
                                                              0, 0, &bad_index);

                                if (EZPI_DEVCFGVAL_OK != reason)
                                {
                                    TRACE_E("refusing device config from BLE (%s), entry %d",
                                            EZPI_util_devcfgval_token(reason), bad_index);
                                }
                                else if (EZPI_core_factory_info_v3_set_ezlopi_config(cjson_config))
                                {
                                    reboot_to_apply = true;
                                }
                                else
                                {
                                    TRACE_E("ERROR : Failed storing device config.");
                                }
                                cJSON_Delete(__FUNCTION__, cjson_config);
                            }
                            else
                            {
                                TRACE_E("ERROR : Failed parsing JSON for config.");
                            }
                            ezlopi_free(__FUNCTION__, decoded_data);
                        }

                        EZPI_core_ble_buffer_free_buffer(g_dynamic_config_linked_buffer);
                        g_dynamic_config_linked_buffer = NULL;
                    }
                }

                /* root holds a parsed copy of this chunk's envelope and nothing
                 * below keeps a pointer into it, but it was never released --
                 * so every chunk of every config write leaked a whole cJSON
                 * tree. */
                cJSON_Delete(__FUNCTION__, root);
            }
        }
    }

    if (reboot_to_apply)
    {
        if (pdPASS != xTaskCreate(ezpi_dynamic_config_reboot_task, "ezpi_cfg_reboot", 2048, NULL, 1, NULL))
        {
            /* Without the deferred task the config would sit in flash unapplied
             * until the next power cycle, which is the failure this fix exists
             * to remove. Rebooting inline costs the write response but at least
             * applies the config. */
            TRACE_E("Could not defer restart, restarting inline.");
            EZPI_core_reset_reboot();
        }
    }
}

static void ezpi_dynamic_config_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    TRACE_D("Read function called!");

    static char *g_dynamic_config_base64;
    static uint32_t g_dynamic_config_sequence_no;
    static time_t g_provisioning_last_read_time;
    static uint32_t g_dynamic_config_number_of_sequence;
    static const time_t gc_provisioning_read_timeout_s = 2;
    static const uint32_t g_required_ble_prov_buffer_size = 517;

    // timeout logic
    int status = -1; // success for non negative, failed for negative
    time_t time_now = EZPI_core_sntp_get_current_time_sec();

    if ((time_now - g_provisioning_last_read_time) >= gc_provisioning_read_timeout_s)
    {
        g_dynamic_config_sequence_no = 0;
    }

    g_provisioning_last_read_time = EZPI_core_sntp_get_current_time_sec();

    if (value)
    {
        if (NULL == g_dynamic_config_base64)
        {
            g_dynamic_config_base64 = ezpi_dynamic_config_base64();

            if (g_dynamic_config_base64)
            {
                g_dynamic_config_sequence_no = 0;
                g_dynamic_config_number_of_sequence = strlen(g_dynamic_config_base64) / EZPI_core_ble_gatt_get_max_data_size();
                g_dynamic_config_number_of_sequence = (strlen(g_dynamic_config_base64) % EZPI_core_ble_gatt_get_max_data_size()) ? (g_dynamic_config_number_of_sequence + 1) : g_dynamic_config_number_of_sequence;
            }
        }

        if (NULL != g_dynamic_config_base64)
        {
            if (EZPI_core_ble_gatt_get_max_data_size() >= g_required_ble_prov_buffer_size)
            {
                uint32_t total_data_len = strlen(g_dynamic_config_base64);
                uint32_t copy_size = total_data_len - (g_dynamic_config_sequence_no * 400);
                copy_size = (copy_size > 400) ? 400 : copy_size;

                TRACE_I("copy_size: %d", copy_size);
                TRACE_I("total_data_len: %d", total_data_len);

                cJSON *cj_response = cJSON_CreateObject(__FUNCTION__);
                if (cj_response)
                {
                    static char data_buffer[400 + 1];
                    snprintf(data_buffer, sizeof(data_buffer), "%.*s", copy_size, g_dynamic_config_base64 + (g_dynamic_config_sequence_no * 400));

                    cJSON_AddNumberToObject(__FUNCTION__, cj_response, ezlopi_len_str, copy_size);
                    cJSON_AddNumberToObject(__FUNCTION__, cj_response, ezlopi_total_len_str, total_data_len);
                    /* The wire protocol numbers chunks from 1, not 0: the write
                     * counterpart in this same file rejects sequence 0 outright
                     * (`if (sequence && len && tot_len)`) and offsets with
                     * `(sequence - 1) * 400`. Emitting the raw 0-based counter
                     * made the very first chunk look invalid to the client,
                     * which then reports the characteristic as not offered at
                     * all. g_dynamic_config_sequence_no stays 0-based because
                     * the local offset arithmetic above depends on it. */
                    cJSON_AddNumberToObject(__FUNCTION__, cj_response, ezlopi_sequence_str, g_dynamic_config_sequence_no + 1);
                    cJSON_AddStringToObject(__FUNCTION__, cj_response, ezlopi_data_str, data_buffer);

                    bool ret = cJSON_PrintPreallocated(__FUNCTION__, cj_response, (char *)value->value, 512, false);
                    cJSON_Delete(__FUNCTION__, cj_response);

                    if (true == ret)
                    {
                        TRACE_D("data: %s", (char *)value->value);
                        TRACE_D("length of 'send_data': %d", strlen((char *)value->value));

                        if ((0 != total_data_len) && (total_data_len >= ((g_dynamic_config_sequence_no * 400) + copy_size)))
                        {
                            value->len = strlen((char *)value->value);
                            g_dynamic_config_sequence_no += 1;
                            status = 0;

                            if (copy_size < 400) // Done reading
                            {
                                status = 1; // non negative for done reading
                                ezlopi_free(__FUNCTION__, g_dynamic_config_base64);
                                g_dynamic_config_base64 = NULL;
                            }
                        }
                        else
                        {
                            TRACE_W("Check value: %d", ((g_dynamic_config_sequence_no * 400) + copy_size));
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
                TRACE_E("MTU size must be greater than or equal to %d!", g_required_ble_prov_buffer_size);
                TRACE_W("call SET-MTU API from client stack!");

                CHECK_PRINT_ERROR(esp_ble_gatt_set_local_mtu(517), "set local  MTU failed");
                status = -5;
            }
        }

        if (status)
        {
            if (status == -5)
            {
                const char *mtu_fail_resp = "{\"req_mtu_size\":517}";
                strcpy((char *)value->value, mtu_fail_resp);
            }
            else if (status < 0)
            {
                TRACE_E("Error found: %d", status);
                value->len = 1;
                value->value[0] = 0; // Read 0 if the device not provisioned yet.
            }
            else
            {
                TRACE_S("Done Transmitting.");
            }

            g_dynamic_config_sequence_no = 0;
            g_dynamic_config_number_of_sequence = 0;

            if (g_dynamic_config_base64)
            {
                ezlopi_free(__FUNCTION__, g_dynamic_config_base64);
                g_dynamic_config_base64 = NULL;
            }
        }
    }
    else
    {
        TRACE_E("VALUE IS NULL");
    }
}

static char *ezpi_base64_decode_dynamic_config(uint32_t total_size)
{
    char *decoded_config_json = NULL;
    char *base64_buffer = NULL;

    /* See EZPI_BLE_DYNCFG_MAX_BASE64_LEN. This has to happen before the
    ** allocation below, because total_size + 1 is what wraps. */
    if ((0 == total_size) || (total_size > EZPI_BLE_DYNCFG_MAX_BASE64_LEN))
    {
        TRACE_E("total_len %d is outside 1..%d, refusing", total_size, (uint32_t)EZPI_BLE_DYNCFG_MAX_BASE64_LEN);
        return NULL;
    }

    base64_buffer = ezlopi_malloc(__FUNCTION__, total_size + 1);

    if (base64_buffer)
    {
        uint32_t pos = 0;
        s_linked_buffer_t *tmp_prov_buffer = g_dynamic_config_linked_buffer;

        /* If the very first chunk carries no data member, the strlen() and the
         * trace below would both read this buffer uninitialised. */
        base64_buffer[0] = '\0';

        while (tmp_prov_buffer)
        {
            // TRACE_W("tmp_prov_buffer->buffer[%d]: %.*s", tmp_prov_buffer->len, tmp_prov_buffer->len, (char *)tmp_prov_buffer->buffer);
            cJSON *root = cJSON_ParseWithLength(__FUNCTION__, (const char *)tmp_prov_buffer->buffer, tmp_prov_buffer->len);
            if (root)
            {
                uint32_t len = CJ_GET_NUMBER(ezlopi_len_str);
                // uint32_t tot_len = CJ_GET_NUMBER(ezlopi_total_len_str);
                // uint32_t sequence = CJ_GET_NUMBER(ezlopi_sequence_str);
                char *data = CJ_GET_STRING(ezlopi_data_str);
                if (data)
                {
                    /* len and the data member both come straight off the wire,
                     * and base64_buffer is only total_size + 1 bytes. Copying
                     * len bytes unchecked let a client whose chunks claim more
                     * than total_len -- or whose len exceeds the string it
                     * actually sent -- walk off the end of the heap block. */
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
        if (decoded_config_json)
        {
            size_t o_len = 0;
            bzero(decoded_config_json, total_size);
            mbedtls_base64_decode((uint8_t *)decoded_config_json, (size_t)total_size, &o_len, (uint8_t *)base64_buffer, strlen(base64_buffer));
            TRACE_D("Decoded data: %s", decoded_config_json);
        }
        else
        {
            TRACE_E("mALLOC FAILED");
        }

        ezlopi_free(__FUNCTION__, base64_buffer);
    }

    return decoded_config_json;
}

static char *ezpi_dynamic_config_base64(void)
{
    const uint32_t base64_data_len = 4096;
    char *base64_data = ezlopi_malloc(__FUNCTION__, base64_data_len);
    if (base64_data)
    {
        uint32_t out_put_len = 0;
        char *str_ezlopi_config = EZPI_core_factory_info_v3_get_ezlopi_config(); // do not free 'str_provisioning_data', it is used by other modules
        if (str_ezlopi_config)
        {
            TRACE_D("device-config: [len: %d]\n%s", strlen(str_ezlopi_config), str_ezlopi_config);

#ifndef CONFIG_EZPI_UTIL_TRACE_EN
            mbedtls_base64_encode((unsigned char *)base64_data, base64_data_len, &out_put_len,
                                  (const unsigned char *)str_ezlopi_config, strlen(str_ezlopi_config));
#else
            int ret = mbedtls_base64_encode((unsigned char *)base64_data, base64_data_len, &out_put_len,
                                            (const unsigned char *)str_ezlopi_config, strlen(str_ezlopi_config));
            TRACE_D("'mbedtls_base64_encode' returned: %04x", ret);
#endif

            EZPI_core_factory_info_v3_free(str_ezlopi_config);
        }

        TRACE_D("out-put-len: %d", out_put_len);
        TRACE_D("base64_data[len: %d]: %s", strlen(base64_data), base64_data);

        if (0 == out_put_len)
        {
            ezlopi_free(__FUNCTION__, base64_data);
            base64_data = NULL;
        }
    }

    return base64_data;
}

#endif // CONFIG_EZPI_BLE_ENABLE

/*******************************************************************************
 *                          End of File
 *******************************************************************************/