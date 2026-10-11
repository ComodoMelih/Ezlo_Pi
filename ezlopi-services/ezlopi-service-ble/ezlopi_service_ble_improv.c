
/**
 * @file    ezlopi_service_ble_improv.c
 * @brief   Improv Wi-Fi provisioning over BLE
 * @authors
 * @version 1.0
 * @date    October 2026
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
 * @file    ezlopi_service_ble_improv.c
 * @brief   Implements the Improv Wi-Fi BLE service (improv-wifi.com) so that
 *          Home Assistant, the Improv web installer and any other Improv client
 *          can join this controller to a Wi-Fi network without the Ezlo app and
 *          without a cloud account.
 *
 *          This is an addition, not a replacement. The EzloPi WiFi (0x00E1) and
 *          provisioning (0x00E3) services are untouched and still the path the
 *          Ezlo app uses; Improv is a sixth GATT service alongside them.
 *
 *          Two things about this service are deliberate and worth reading
 *          before changing anything:
 *
 *          1. Improv has no authentication of any kind. The specification has
 *             an "authorization required" state, but nothing in the protocol
 *             proves who the client is. So this service only provisions while
 *             the controller is *unclaimed* -- no user_id in NVS. Once claimed,
 *             it reports BLE_IMPROV_STATE_PROVISIONED and rejects credential
 *             writes, so somebody in radio range cannot move an installed
 *             controller onto their own network. A factory reset clears the
 *             user_id and makes Improv usable again.
 *
 *          2. Improv does not carry a user_id, so the credentials it supplies
 *             cannot go through EZPI_ble_auth_check_user_id() the way the Ezlo
 *             provisioning path does. The Wi-Fi credentials are applied and
 *             persisted, but no user_id is invented or stored: the controller
 *             stays unclaimed and can still be claimed by the Ezlo app
 *             afterwards. Improv gets the device onto the network; it does not
 *             pretend to be an Ezlo claim.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

/* Not "../../build/config/sdkconfig.h". ESP-IDF puts the generated config
 * directory on every component's include path, so the plain name resolves; the
 * relative form pins both the build directory's name and its position relative
 * to this file, and breaks under "idf.py -B <dir>". Most of the tree still uses
 * the relative form -- see the known gap in the CHANGELOG -- but new code
 * should not add to it. */
#include "sdkconfig.h"

#if defined(CONFIG_EZPI_BLE_ENABLE) && defined(CONFIG_EZPI_BLE_ENABLE_IMPROV)

#include <string.h>

#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "ezlopi_util_trace.h"
#include "EZLOPI_USER_CONFIG.h"

#include "ezlopi_core_nvs.h"
#include "ezlopi_core_wifi.h"
#include "ezlopi_core_ble_gap.h"
#include "ezlopi_core_ble_gatt.h"
#include "ezlopi_core_ble_buffer.h"
#include "ezlopi_core_ble_profile.h"
#include "ezlopi_core_factory_info.h"

#include "ezlopi_service_ble.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/**
 * @brief Largest RPC command this service will accept
 *
 * The longest legitimate "send Wi-Fi settings" command is
 * 1 (command) + 1 (data length) + 1 + 32 (SSID) + 1 + 64 (passphrase) +
 * 1 (checksum) = 101 bytes. 128 leaves headroom without letting a client push
 * an arbitrary amount of data at us.
 */
#define IMPROV_RPC_MAX_LEN 128

/** @brief Field widths from IEEE 802.11: 32-byte SSID, 64-character passphrase */
#define IMPROV_SSID_MAX_LEN 32
#define IMPROV_PASSPHRASE_MAX_LEN 64

/**
 * @brief How long to wait for an IP before reporting the attempt failed
 *
 * The window has to cover association, the four-way handshake and DHCP, and it
 * has to outlast the retry loop in ezlopi-core-wifi, which calls
 * esp_wifi_connect() again on every disconnect up to EXAMPLE_ESP_MAXIMUM_RETRY
 * (5). Thirty seconds is comfortably past that on a network that works and
 * short enough that a client is not left guessing on one that does not.
 */
#define IMPROV_PROVISION_TIMEOUT_US (30 * 1000 * 1000)

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static void ezpi_improv_state_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
static void ezpi_improv_error_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
static void ezpi_improv_caps_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
static void ezpi_improv_result_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);

static void ezpi_improv_rpc_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
static void ezpi_improv_rpc_write_exec_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);

static void ezpi_improv_cccd_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);
static void ezpi_improv_cccd_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param);

static void ezpi_improv_handle_rpc(const uint8_t *data, uint32_t len);
static void ezpi_improv_handle_send_wifi(const uint8_t *data, uint8_t data_len);
static bool ezpi_improv_is_claimed(void);
static void ezpi_improv_set_state(uint8_t state);
static void ezpi_improv_set_error(uint8_t error);
static void ezpi_improv_wifi_event_upcall(esp_event_base_t event, int32_t event_id, void *arg);
static void ezpi_improv_provision_timeout(void *arg);
static void ezpi_improv_provision_timer_start(void);
static void ezpi_improv_provision_timer_stop(void);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static s_gatt_service_t *sg_improv_service = NULL;
static s_gatt_char_t *sg_improv_state_char = NULL;
static s_gatt_char_t *sg_improv_error_char = NULL;
static s_gatt_char_t *sg_improv_result_char = NULL;

static uint8_t sg_improv_state = BLE_IMPROV_STATE_AUTHORIZED;
static uint8_t sg_improv_error = BLE_IMPROV_ERROR_NONE;

/** @brief Notification subscription state for the state/error characteristics */
static uint16_t sg_state_cccd = 0;
static uint16_t sg_error_cccd = 0;

/** @brief Buffer for a chunked (prepare-write) RPC command */
static s_linked_buffer_t *sg_rpc_linked_buffer = NULL;

/**
 * @brief One-shot timer bounding a provisioning attempt
 *
 * Created on the first attempt and reused afterwards, so repeated attempts do
 * not each allocate a timer.
 */
static esp_timer_handle_t sg_provision_timer = NULL;

/**
 * @brief Last RPC result, in the wire format the result characteristic returns
 *
 * Improv's success response to "send Wi-Fi settings" is a list of strings; for a
 * device with no local web UI the list is empty, which still has to be encoded
 * as command + length + checksum rather than returned as a zero-length read.
 */
static uint8_t sg_improv_result[4] = {0};
static uint8_t sg_improv_result_len = 0;

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

void EZPI_ble_service_improv_init(void)
{
    esp_bt_uuid_t uuid;
    esp_gatt_perm_t permission;
    esp_gatt_char_prop_t properties;

    static const uint8_t service_uuid[16] = BLE_IMPROV_UUID128(0x00);
    static const uint8_t state_uuid[16] = BLE_IMPROV_UUID128(0x01);
    static const uint8_t error_uuid[16] = BLE_IMPROV_UUID128(0x02);
    static const uint8_t rpc_uuid[16] = BLE_IMPROV_UUID128(0x03);
    static const uint8_t result_uuid[16] = BLE_IMPROV_UUID128(0x04);
    static const uint8_t caps_uuid[16] = BLE_IMPROV_UUID128(0x05);

    /* A controller that is already claimed must not be re-provisioned by an
     * unauthenticated client, so start out reporting "provisioned" in that case.
     * This is the state an Improv client reads before it offers to configure
     * anything, so reporting it up front is what stops the attempt rather than
     * failing it later. */
    sg_improv_state = ezpi_improv_is_claimed() ? BLE_IMPROV_STATE_PROVISIONED
                                               : BLE_IMPROV_STATE_AUTHORIZED;

    uuid.len = ESP_UUID_LEN_128;
    memcpy(uuid.uuid.uuid128, service_uuid, sizeof(service_uuid));
    sg_improv_service = EZPI_core_ble_gatt_create_service(BLE_IMPROV_SERVICE_HANDLE, &uuid);

    /* Current state: read + notify. The spec also lists write, but a client
     * writing our own state to us is meaningless and would only give a remote
     * party a way to spoof "authorized", so it is not exposed. */
    uuid.len = ESP_UUID_LEN_128;
    memcpy(uuid.uuid.uuid128, state_uuid, sizeof(state_uuid));
    permission = ESP_GATT_PERM_READ;
    properties = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
    sg_improv_state_char = EZPI_core_ble_gatt_add_characteristic(sg_improv_service, &uuid, permission, properties,
                                                                ezpi_improv_state_read_func, NULL, NULL);

    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
    EZPI_core_ble_gatt_add_descriptor(sg_improv_state_char, &uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                      ezpi_improv_cccd_read_func, ezpi_improv_cccd_write_func, NULL);

    /* Error state: read + notify, same reasoning on write. */
    uuid.len = ESP_UUID_LEN_128;
    memcpy(uuid.uuid.uuid128, error_uuid, sizeof(error_uuid));
    permission = ESP_GATT_PERM_READ;
    properties = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
    sg_improv_error_char = EZPI_core_ble_gatt_add_characteristic(sg_improv_service, &uuid, permission, properties,
                                                                ezpi_improv_error_read_func, NULL, NULL);

    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
    EZPI_core_ble_gatt_add_descriptor(sg_improv_error_char, &uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                      ezpi_improv_cccd_read_func, ezpi_improv_cccd_write_func, NULL);

    /* RPC command: write only. Credentials can exceed a single ATT packet, so
     * the prepare/execute path is wired up as well as the single-packet one. */
    uuid.len = ESP_UUID_LEN_128;
    memcpy(uuid.uuid.uuid128, rpc_uuid, sizeof(rpc_uuid));
    permission = ESP_GATT_PERM_WRITE;
    properties = ESP_GATT_CHAR_PROP_BIT_WRITE;
    EZPI_core_ble_gatt_add_characteristic(sg_improv_service, &uuid, permission, properties,
                                          NULL, ezpi_improv_rpc_write_func, ezpi_improv_rpc_write_exec_func);

    /* RPC result: read + notify. */
    uuid.len = ESP_UUID_LEN_128;
    memcpy(uuid.uuid.uuid128, result_uuid, sizeof(result_uuid));
    permission = ESP_GATT_PERM_READ;
    properties = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
    sg_improv_result_char = EZPI_core_ble_gatt_add_characteristic(sg_improv_service, &uuid, permission, properties,
                                                                 ezpi_improv_result_read_func, NULL, NULL);

    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
    EZPI_core_ble_gatt_add_descriptor(sg_improv_result_char, &uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                      ezpi_improv_cccd_read_func, ezpi_improv_cccd_write_func, NULL);

    /* Capabilities: read only, constant. */
    uuid.len = ESP_UUID_LEN_128;
    memcpy(uuid.uuid.uuid128, caps_uuid, sizeof(caps_uuid));
    permission = ESP_GATT_PERM_READ;
    properties = ESP_GATT_CHAR_PROP_BIT_READ;
    EZPI_core_ble_gatt_add_characteristic(sg_improv_service, &uuid, permission, properties,
                                          ezpi_improv_caps_read_func, NULL, NULL);

    /* Publish the starting state in the advertisement, so a client knows
     * whether this controller is provisionable before it connects. */
    EZPI_core_ble_gap_improv_set_adv_state(sg_improv_state, BLE_IMPROV_CAP_NONE);

    /* Report the outcome from the Wi-Fi stack rather than guessing at the moment
     * the credentials are written. */
    EZPI_core_wifi_event_add(ezpi_improv_wifi_event_upcall, NULL);

    TRACE_I("improv: service registered, initial state %d", (int)sg_improv_state);
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

static bool ezpi_improv_is_claimed(void)
{
    /* "Claimed" means an Ezlo user_id has been stored, which is what the Ezlo
     * provisioning path writes once an account takes ownership. */
    char *stored_user_id = EZPI_core_nvs_read_user_id_str();
    bool claimed = false;

    if (NULL != stored_user_id)
    {
        /* An empty string is not ownership. The Ezlo path rejects empty ids at
         * the point of storage, but a controller flashed by an older build may
         * still have one, and treating that as claimed would lock Improv out
         * for no reason. */
        claimed = ('\0' != stored_user_id[0]);
        ezlopi_free(__FUNCTION__, stored_user_id);
    }

    return claimed;
}

static void ezpi_improv_set_state(uint8_t state)
{
    if (sg_improv_state == state)
    {
        return;
    }

    sg_improv_state = state;

    /* Improv clients read the state from the advertisement as well as from the
     * characteristic, so both have to move together. */
    EZPI_core_ble_gap_improv_set_adv_state(sg_improv_state, BLE_IMPROV_CAP_NONE);

    if ((NULL != sg_improv_state_char) && (0 != sg_state_cccd))
    {
        esp_gatt_value_t value = {0};
        value.len = 1;
        value.value[0] = sg_improv_state;
        EZPI_core_ble_gatts_characteristic_notify(sg_improv_service, sg_improv_state_char, &value);
    }

    TRACE_I("improv: state -> %d", (int)sg_improv_state);
}

static void ezpi_improv_set_error(uint8_t error)
{
    sg_improv_error = error;

    if ((NULL != sg_improv_error_char) && (0 != sg_error_cccd))
    {
        esp_gatt_value_t value = {0};
        value.len = 1;
        value.value[0] = sg_improv_error;
        EZPI_core_ble_gatts_characteristic_notify(sg_improv_service, sg_improv_error_char, &value);
    }

    if (BLE_IMPROV_ERROR_NONE != error)
    {
        TRACE_E("improv: error -> %d", (int)error);
    }
}

static void ezpi_improv_state_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (NULL != value)
    {
        value->len = 1;
        value->value[0] = sg_improv_state;
    }
}

static void ezpi_improv_error_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (NULL != value)
    {
        value->len = 1;
        value->value[0] = sg_improv_error;
    }
}

static void ezpi_improv_caps_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (NULL != value)
    {
        /* No identify handler yet, so no capability bits are claimed. Claiming
         * identify without implementing it would make a client offer the user a
         * button that does nothing. */
        value->len = 1;
        value->value[0] = BLE_IMPROV_CAP_NONE;
    }
}

static void ezpi_improv_result_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (NULL != value)
    {
        value->len = sg_improv_result_len;
        if (0 != sg_improv_result_len)
        {
            memcpy(value->value, sg_improv_result, sg_improv_result_len);
        }
    }
}

static void ezpi_improv_cccd_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (NULL != value)
    {
        /* Which descriptor this read belongs to is not distinguished here: the
         * value is only an echo of the client's own subscription, and reporting
         * the state subscription for all three is harmless. Notifications
         * themselves are gated on the correct flag at send time. */
        value->len = 2;
        value->value[0] = (uint8_t)(sg_state_cccd & 0xFF);
        value->value[1] = (uint8_t)((sg_state_cccd >> 8) & 0xFF);
    }
}

static void ezpi_improv_cccd_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if ((NULL == param) || (NULL == param->write.value) || (param->write.len < 2))
    {
        return;
    }

    uint16_t cccd = (uint16_t)param->write.value[0] | ((uint16_t)param->write.value[1] << 8);

    /* Both subscriptions are set together. The alternative is matching on the
     * descriptor handle, which this GATT layer does not pass through to the
     * write callback; since a client that subscribes to Improv at all wants
     * both state and error, tracking them jointly costs nothing. */
    sg_state_cccd = cccd;
    sg_error_cccd = cccd;

    TRACE_I("improv: notifications %s", (0 != cccd) ? "enabled" : "disabled");
}

static void ezpi_improv_rpc_write_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (NULL == param)
    {
        return;
    }

    if (0 == param->write.is_prep) // single packet
    {
        if ((NULL != param->write.value) && (param->write.len > 0))
        {
            ezpi_improv_handle_rpc(param->write.value, param->write.len);
        }
    }
    else
    {
        if (NULL == sg_rpc_linked_buffer)
        {
            sg_rpc_linked_buffer = EZPI_core_ble_buffer_create(param);
        }
        else
        {
            EZPI_core_ble_buffer_add_to_buffer(sg_rpc_linked_buffer, param);
        }
    }
}

static void ezpi_improv_rpc_write_exec_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (NULL != sg_rpc_linked_buffer)
    {
        EZPI_core_ble_buffer_accumulate_to_start(sg_rpc_linked_buffer);
        ezpi_improv_handle_rpc(sg_rpc_linked_buffer->buffer, sg_rpc_linked_buffer->len);
        EZPI_core_ble_buffer_free_buffer(sg_rpc_linked_buffer);
        sg_rpc_linked_buffer = NULL;
    }
}

/**
 * @brief Validate and dispatch one Improv RPC command
 *
 * Wire format is [command][data length][data ...][checksum], where the checksum
 * is the low byte of the sum of every preceding byte.
 */
static void ezpi_improv_handle_rpc(const uint8_t *data, uint32_t len)
{
    if ((NULL == data) || (len < 3))
    {
        /* Shortest possible valid command is command + length + checksum. */
        ezpi_improv_set_error(BLE_IMPROV_ERROR_INVALID_RPC);
        return;
    }

    if (len > IMPROV_RPC_MAX_LEN)
    {
        ezpi_improv_set_error(BLE_IMPROV_ERROR_INVALID_RPC);
        return;
    }

    const uint8_t command = data[0];
    const uint8_t data_len = data[1];

    /* The declared payload length has to agree with what actually arrived
     * before any of it is indexed. Trusting data[1] and reading data[2 + i]
     * would be an out-of-bounds read on a truncated or hostile write. */
    if ((uint32_t)data_len + 3u != len)
    {
        ezpi_improv_set_error(BLE_IMPROV_ERROR_INVALID_RPC);
        return;
    }

    uint32_t sum = 0;
    for (uint32_t i = 0; i < (len - 1u); i++)
    {
        sum += data[i];
    }

    if ((uint8_t)(sum & 0xFFu) != data[len - 1u])
    {
        ezpi_improv_set_error(BLE_IMPROV_ERROR_INVALID_RPC);
        return;
    }

    switch (command)
    {
    case BLE_IMPROV_RPC_SEND_WIFI:
    {
        ezpi_improv_handle_send_wifi(&data[2], data_len);
        break;
    }
    default:
    {
        /* Identify, device info and Wi-Fi scan are not implemented, and the
         * capabilities characteristic does not advertise them, so a conforming
         * client will not ask. Anything that does ask gets the spec's
         * "unknown command" rather than silence. */
        ezpi_improv_set_error(BLE_IMPROV_ERROR_UNKNOWN_RPC);
        break;
    }
    }
}

/**
 * @brief Apply credentials from an Improv "send Wi-Fi settings" command
 *
 * Payload is [ssid length][ssid ...][passphrase length][passphrase ...].
 */
static void ezpi_improv_handle_send_wifi(const uint8_t *data, uint8_t data_len)
{
    /* The claim check is repeated here rather than relied upon from init: the
     * controller can be claimed through the Ezlo app while this BLE connection
     * is open, and the decision has to reflect the state at the moment the
     * credentials arrive. */
    if (ezpi_improv_is_claimed())
    {
        TRACE_W("improv: credential write refused, controller already claimed");
        ezpi_improv_set_error(BLE_IMPROV_ERROR_NOT_AUTHORIZED);
        ezpi_improv_set_state(BLE_IMPROV_STATE_PROVISIONED);
        return;
    }

    if (data_len < 2)
    {
        ezpi_improv_set_error(BLE_IMPROV_ERROR_INVALID_RPC);
        return;
    }

    const uint8_t ssid_len = data[0];

    /* Every offset below is bounds-checked against data_len before use. */
    if (((uint32_t)ssid_len + 1u) >= (uint32_t)data_len)
    {
        ezpi_improv_set_error(BLE_IMPROV_ERROR_INVALID_RPC);
        return;
    }

    const uint8_t pass_len = data[1 + ssid_len];

    if (((uint32_t)ssid_len + (uint32_t)pass_len + 2u) != (uint32_t)data_len)
    {
        ezpi_improv_set_error(BLE_IMPROV_ERROR_INVALID_RPC);
        return;
    }

    if ((0 == ssid_len) || (ssid_len > IMPROV_SSID_MAX_LEN) || (pass_len > IMPROV_PASSPHRASE_MAX_LEN))
    {
        ezpi_improv_set_error(BLE_IMPROV_ERROR_INVALID_RPC);
        return;
    }

    /* Both are NUL-terminated copies: the wire format is length-prefixed and
     * not terminated, but EZPI_core_wifi_connect() takes C strings. */
    char ssid[IMPROV_SSID_MAX_LEN + 1] = {0};
    char passphrase[IMPROV_PASSPHRASE_MAX_LEN + 1] = {0};

    memcpy(ssid, &data[1], ssid_len);
    memcpy(passphrase, &data[2 + ssid_len], pass_len);

    /* The passphrase is reduced to a length for the log: this goes to the serial
     * console and, at cloud severity, off the device. */
    TRACE_I("improv: provisioning ssid '%s', passphrase %d char(s)", ssid, (int)pass_len);

    ezpi_improv_set_error(BLE_IMPROV_ERROR_NONE);
    ezpi_improv_set_state(BLE_IMPROV_STATE_PROVISIONING);

    /* Started before the connect call, not after: EZPI_core_wifi_connect() is
     * synchronous through esp_wifi_start() and the got-IP that ends the attempt
     * can be dispatched on the Wi-Fi task while we are still inside it. Arming
     * the timer first means there is no window in which the attempt is running
     * with nothing bounding it. */
    ezpi_improv_provision_timer_start();

    EZPI_core_wifi_connect(ssid, passphrase);
    EZPI_core_factory_info_v3_set_wifi(ssid, passphrase);
}

static void ezpi_improv_wifi_event_upcall(esp_event_base_t event, int32_t event_id, void *arg)
{
    /* Only transitions out of PROVISIONING are ours to report. Wi-Fi events also
     * arrive for ordinary roaming and for provisioning driven by the Ezlo app,
     * and reporting those as Improv results would make a connected Improv client
     * display progress for something it did not start. */
    if (BLE_IMPROV_STATE_PROVISIONING != sg_improv_state)
    {
        return;
    }

    /* Success is the only verdict taken from an event. Failure is decided by the
     * timeout below, deliberately:
     *
     * WIFI_EVENT_STA_DISCONNECTED is not a failure signal in this firmware. Two
     * separate reasons, either of which alone would make a disconnect-driven
     * failure report wrong:
     *
     *  - EZPI_core_wifi_connect() begins with esp_wifi_stop(), so applying any
     *    credentials to a station that was already associated raises a
     *    disconnect for our own teardown. Treating that as a failure would make
     *    every re-provision report "unable to connect" immediately, and because
     *    the state would then have left PROVISIONING, the got-IP that followed
     *    would be ignored -- a successful provision reported as a failure.
     *
     *  - __event_wifi_disconnected() in ezlopi-core-wifi calls esp_wifi_connect()
     *    again on every disconnect and keeps doing so up to
     *    EXAMPLE_ESP_MAXIMUM_RETRY (5). A disconnect is therefore the start of a
     *    retry, not the end of the attempt, and a network that associates on the
     *    second try is a success.
     *
     * So the attempt is given a fixed window and judged on whether an IP
     * arrived, which is also what the Improv reference implementations do. */
    if ((IP_EVENT == event) && (IP_EVENT_STA_GOT_IP == event_id))
    {
        ezpi_improv_provision_timer_stop();

        /* Improv's success response is a list of strings -- a URL to the
         * device's own web UI. EzloPi has no local web UI to point at, so
         * the list is empty: command, zero data length, checksum. */
        sg_improv_result[0] = BLE_IMPROV_RPC_SEND_WIFI;
        sg_improv_result[1] = 0x00;
        sg_improv_result[2] = (uint8_t)((BLE_IMPROV_RPC_SEND_WIFI + 0x00) & 0xFF);
        sg_improv_result_len = 3;

        ezpi_improv_set_error(BLE_IMPROV_ERROR_NONE);
        ezpi_improv_set_state(BLE_IMPROV_STATE_PROVISIONED);

        if ((NULL != sg_improv_result_char) && (0 != sg_state_cccd))
        {
            esp_gatt_value_t value = {0};
            value.len = sg_improv_result_len;
            memcpy(value.value, sg_improv_result, sg_improv_result_len);
            EZPI_core_ble_gatts_characteristic_notify(sg_improv_service, sg_improv_result_char, &value);
        }

        TRACE_S("improv: provisioned");
    }
}

/**
 * @brief Give up on the current provisioning attempt
 *
 * Runs on the esp_timer task once IMPROV_PROVISION_TIMEOUT_US has passed without
 * an IP. The state is checked again here because the timer and the Wi-Fi event
 * run on different tasks: a got-IP that landed between the timer firing and this
 * callback being scheduled must not be overwritten with a failure.
 */
static void ezpi_improv_provision_timeout(void *arg)
{
    if (BLE_IMPROV_STATE_PROVISIONING != sg_improv_state)
    {
        return;
    }

    /* The Wi-Fi layer keeps the reason string from the last disconnect, which is
     * the difference between "wrong password" and "no such network" for whoever
     * is reading the serial log. Improv's own error set cannot express either,
     * so it only goes to the log. */
    TRACE_E("improv: unable to connect within %d s, last disconnect reason: %s",
            (int)(IMPROV_PROVISION_TIMEOUT_US / 1000000), EZPI_core_wifi_get_last_disconnect_reason());

    ezpi_improv_set_error(BLE_IMPROV_ERROR_UNABLE_TO_CONNECT);

    /* Back to authorized rather than staying in provisioning, so the client can
     * offer the user another attempt instead of waiting on a state that will
     * never advance. */
    ezpi_improv_set_state(BLE_IMPROV_STATE_AUTHORIZED);
}

static void ezpi_improv_provision_timer_start(void)
{
    if (NULL == sg_provision_timer)
    {
        const esp_timer_create_args_t timer_args = {
            .callback = ezpi_improv_provision_timeout,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "improv_prov",
        };

        if (ESP_OK != esp_timer_create(&timer_args, &sg_provision_timer))
        {
            /* Without the timer a failed attempt would sit in PROVISIONING for
             * ever. Reporting it now is better than leaving the client waiting
             * on a state that cannot change. */
            TRACE_E("improv: could not create the provisioning timer");
            sg_provision_timer = NULL;
            return;
        }
    }

    /* A second attempt while the first is still pending restarts the window
     * rather than stacking a timer. */
    esp_timer_stop(sg_provision_timer);
    esp_timer_start_once(sg_provision_timer, IMPROV_PROVISION_TIMEOUT_US);
}

static void ezpi_improv_provision_timer_stop(void)
{
    if (NULL != sg_provision_timer)
    {
        esp_timer_stop(sg_provision_timer);
    }
}

#endif // CONFIG_EZPI_BLE_ENABLE && CONFIG_EZPI_BLE_ENABLE_IMPROV

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
