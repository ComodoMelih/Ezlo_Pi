
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
 * @brief   The BLE transport for Improv Wi-Fi provisioning (improv-wifi.com),
 *          so that Home Assistant, the Improv web installer and any other
 *          Improv client can join this controller to a Wi-Fi network without
 *          the Ezlo app and without a cloud account.
 *
 *          This is an addition, not a replacement. The EzloPi WiFi (0x00E1) and
 *          provisioning (0x00E3) services are untouched and still the path the
 *          Ezlo app uses; Improv is a sixth GATT service alongside them.
 *
 *          Only the BLE half is here: the GATT service, its characteristics,
 *          notification subscriptions and the prepare/execute path for a
 *          command too long for one ATT packet. Everything that is not specific
 *          to BLE -- whether the controller may be provisioned at all, the
 *          state machine, applying credentials, deciding that an attempt has
 *          failed, and what each RPC answers -- is in ezlopi-service-improv and
 *          shared with the serial transport. See that component's header for
 *          why; in short, the provisioning gate below is the entire security
 *          argument for shipping Improv, and it is only as good as its least
 *          careful copy, so there is one copy.
 *
 *          Two things about this remain worth reading before changing anything:
 *
 *          1. Improv has no authentication of any kind. The specification has
 *             an "authorization required" state, but nothing in the protocol
 *             proves who the client is. So provisioning is only accepted while
 *             the controller is *unclaimed* -- no user_id in NVS. Once claimed,
 *             it reports EZPI_IMPROV_STATE_PROVISIONED and rejects credential
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
 *
 *          One detail of the BLE transport differs from serial and is easy to
 *          get wrong: BLE has no frame, so an Improv payload carries its own
 *          checksum in both directions. Serial frames carry one instead, and
 *          the payload inside them does not. That is why the shared parser
 *          takes a has_checksum flag and why results are re-checksummed on the
 *          way out of here.
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

#include "ezlopi_util_trace.h"
#include "EZLOPI_USER_CONFIG.h"

#include "ezlopi_core_ble_gap.h"
#include "ezlopi_core_ble_gatt.h"
#include "ezlopi_core_ble_buffer.h"
#include "ezlopi_core_ble_profile.h"

#include "ezlopi_service_ble.h"
#include "ezlopi_service_improv.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/**
 * @brief Buffer for the last RPC result, in the form the result characteristic
 *        returns it
 *
 * The result is kept rather than only notified because the characteristic is
 * readable: a client may subscribe late, or read instead of subscribing, and
 * either way it should get the last answer rather than nothing.
 *
 * Sized to match IMPROV_RESULT_BUF_LEN in ezlopi-service-improv, which is what
 * the core builds into, so that a result the core considers valid can always be
 * stored here. It is also well inside ESP_GATT_MAX_ATTR_LEN, which bounds what
 * a notification can carry -- but the copy below is length-checked anyway,
 * because "the other component's buffer is no bigger" is an invariant that
 * lives in a different file and can change without this one being recompiled.
 */
#define IMPROV_BLE_RESULT_MAX 264

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

static void ezpi_improv_ble_publish_state(uint8_t state);
static void ezpi_improv_ble_publish_error(uint8_t error);
static void ezpi_improv_ble_publish_result(const uint8_t *payload, uint16_t len);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static s_gatt_service_t *sg_improv_service = NULL;
static s_gatt_char_t *sg_improv_state_char = NULL;
static s_gatt_char_t *sg_improv_error_char = NULL;
static s_gatt_char_t *sg_improv_result_char = NULL;

/** @brief Notification subscription state for the state/error characteristics */
static uint16_t sg_state_cccd = 0;
static uint16_t sg_error_cccd = 0;

/** @brief Buffer for a chunked (prepare-write) RPC command */
static s_linked_buffer_t *sg_rpc_linked_buffer = NULL;

static uint8_t sg_improv_result[IMPROV_BLE_RESULT_MAX] = {0};
static uint16_t sg_improv_result_len = 0;

/**
 * @brief This transport, as the shared core sees it
 *
 * Static storage, which the core's register call requires: it stores the
 * pointer rather than the contents.
 */
static const s_ezpi_improv_transport_t sg_ble_transport = {
    .name = "ble",
    .publish_state = ezpi_improv_ble_publish_state,
    .publish_error = ezpi_improv_ble_publish_error,
    .publish_result = ezpi_improv_ble_publish_result,
};

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

    /* Registered before the service is built, not after, because registration is
     * what settles the initial state -- the core reads the claim flag there --
     * and the advertisement below has to carry that state. */
    if (false == EZPI_SERV_improv_transport_register(&sg_ble_transport))
    {
        /* Without the core there is nothing behind the characteristics: an RPC
         * would be accepted and silently do nothing, and a client would sit
         * through its own timeout. Better to not advertise the service at all. */
        TRACE_E("improv-ble: core refused the transport, service not registered");
        return;
    }

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
    EZPI_core_ble_gap_improv_set_adv_state(EZPI_SERV_improv_state(),
                                           EZPI_SERV_improv_capabilities());

    TRACE_I("improv-ble: service registered, initial state %d",
            (int)EZPI_SERV_improv_state());
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * @brief Deliver a new provisioning state over BLE
 *
 * Called by the shared core, from whichever task caused the change -- the Wi-Fi
 * event task, the esp_timer task, or the GATT callback task when a client asks
 * for the current state. Nothing here needs the GATT task specifically: both
 * calls below are the same ones the write callbacks make.
 */
static void ezpi_improv_ble_publish_state(uint8_t state)
{
    /* Improv clients read the state from the advertisement as well as from the
     * characteristic, so both have to move together. A client that is only
     * scanning, and has never connected, sees nothing else. */
    EZPI_core_ble_gap_improv_set_adv_state(state, EZPI_SERV_improv_capabilities());

    if ((NULL != sg_improv_state_char) && (0 != sg_state_cccd))
    {
        esp_gatt_value_t value = {0};
        value.len = 1;
        value.value[0] = state;
        EZPI_core_ble_gatts_characteristic_notify(sg_improv_service, sg_improv_state_char, &value);
    }
}

static void ezpi_improv_ble_publish_error(uint8_t error)
{
    if ((NULL != sg_improv_error_char) && (0 != sg_error_cccd))
    {
        esp_gatt_value_t value = {0};
        value.len = 1;
        value.value[0] = error;
        EZPI_core_ble_gatts_characteristic_notify(sg_improv_service, sg_improv_error_char, &value);
    }
}

/**
 * @brief Deliver an RPC result over BLE, adding the payload checksum
 *
 * The core builds results without a checksum, because on serial the enclosing
 * frame carries one and a second would be a payload byte the client does not
 * expect. BLE has no frame, so the checksum has to be in the payload, and
 * EZPI_improv_result_finish() has already left the last byte reserved for it --
 * so this fills that slot in rather than appending to the payload.
 */
static void ezpi_improv_ble_publish_result(const uint8_t *payload, uint16_t len)
{
    if ((NULL == payload) || (0 == len) || (len > sizeof(sg_improv_result)))
    {
        TRACE_E("improv-ble: result of %d bytes cannot be published", (int)len);
        return;
    }

    memcpy(sg_improv_result, payload, len);

    /* Over every byte before it, which for a result payload is the command, the
     * data length and the entries. */
    sg_improv_result[len - 1u] = EZPI_improv_checksum(sg_improv_result, (uint32_t)(len - 1u));
    sg_improv_result_len = len;

    /* Gated on the state subscription because this GATT layer does not tell the
     * descriptor write callback which descriptor it belongs to, so all three are
     * tracked as one. See ezpi_improv_cccd_write_func(). */
    if ((NULL != sg_improv_result_char) && (0 != sg_state_cccd))
    {
        esp_gatt_value_t value = {0};
        value.len = sg_improv_result_len;
        memcpy(value.value, sg_improv_result, sg_improv_result_len);
        EZPI_core_ble_gatts_characteristic_notify(sg_improv_service, sg_improv_result_char, &value);
    }
}

static void ezpi_improv_state_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (NULL != value)
    {
        value->len = 1;
        value->value[0] = EZPI_SERV_improv_state();
    }
}

static void ezpi_improv_error_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (NULL != value)
    {
        value->len = 1;
        value->value[0] = EZPI_SERV_improv_error();
    }
}

static void ezpi_improv_caps_read_func(esp_gatt_value_t *value, esp_ble_gatts_cb_param_t *param)
{
    if (NULL != value)
    {
        value->len = 1;
        value->value[0] = EZPI_SERV_improv_capabilities();
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
            /* true: a BLE payload carries its own checksum, there being no
             * frame around it to carry one. */
            EZPI_SERV_improv_handle_rpc(&sg_ble_transport, param->write.value,
                                        param->write.len, true);
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
        EZPI_SERV_improv_handle_rpc(&sg_ble_transport, sg_rpc_linked_buffer->buffer,
                                    sg_rpc_linked_buffer->len, true);
        EZPI_core_ble_buffer_free_buffer(sg_rpc_linked_buffer);
        sg_rpc_linked_buffer = NULL;
    }
}

#endif // CONFIG_EZPI_BLE_ENABLE && CONFIG_EZPI_BLE_ENABLE_IMPROV

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
