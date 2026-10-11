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
 * @file    ezlopi_service_improv.c
 * @brief   Transport-independent half of Improv Wi-Fi provisioning.
 *
 * See ezlopi_service_improv.h for why this is a separate component and what
 * each transport is expected to supply.
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

#if defined(CONFIG_EZPI_ENABLE_IMPROV_SERIAL) || defined(CONFIG_EZPI_BLE_ENABLE_IMPROV)

#include <string.h>

#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_timer.h"

#include "ezlopi_util_trace.h"
#include "ezlopi_util_version.h"
#include "EZLOPI_USER_CONFIG.h"

#include "ezlopi_core_nvs.h"
#include "ezlopi_core_wifi.h"
#include "ezlopi_core_factory_info.h"

#include "ezlopi_service_improv.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/**
 * @brief How many transports may be registered.
 *
 * BLE and Serial, and nothing else is planned. Sized exactly rather than
 * generously so that adding a third is a deliberate edit here instead of
 * something that silently works and is never thought about.
 */
#define IMPROV_MAX_TRANSPORTS 2

/**
 * @brief Buffer for an RPC result payload.
 *
 * The largest answer this core builds is the device info: four entries, each a
 * length byte plus its string, and the longest of those is the device name at
 * EZLOPI_FINFO_LEN_DEVICE_NAME. 256 covers it with room spare and matches the
 * protocol's own ceiling -- the RPC data length is a single byte, so a payload
 * can never usefully exceed 255 plus the command, length and trailing bytes.
 */
#define IMPROV_RESULT_BUF_LEN 264

/**
 * @brief Bytes in a result that carries no entries.
 *
 * Command, a zero data length and the trailing checksum slot. The protocol
 * uses this shape for two different things: the end-of-list marker for a
 * network scan, and the empty URL list that answers a successful provision.
 */
#define RPC_EMPTY_RESULT_LEN 3

/**
 * @brief Human-readable chip family for the device-info answer.
 *
 * CONFIG_IDF_TARGET is already a string ("esp32"), but clients show this to a
 * person, so the conventional spelling is used where it is known and the raw
 * target is the fallback. The fallback matters: a target added by a future IDF
 * must still produce a valid frame rather than failing to compile.
 */
#if defined(CONFIG_IDF_TARGET_ESP32)
#define IMPROV_CHIP_FAMILY "ESP32"
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
#define IMPROV_CHIP_FAMILY "ESP32-S2"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define IMPROV_CHIP_FAMILY "ESP32-S3"
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define IMPROV_CHIP_FAMILY "ESP32-C3"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#define IMPROV_CHIP_FAMILY "ESP32-C6"
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
#define IMPROV_CHIP_FAMILY "ESP32-H2"
#else
#define IMPROV_CHIP_FAMILY CONFIG_IDF_TARGET
#endif

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static void ezpi_improv_set_state(uint8_t state);
static void ezpi_improv_publish_state(const s_ezpi_improv_transport_t *only_to);
static void ezpi_improv_set_error(uint8_t error);
static void ezpi_improv_handle_send_wifi(const s_ezpi_improv_rpc_t *rpc);
static void ezpi_improv_answer_current_state(const s_ezpi_improv_transport_t *origin);
static void ezpi_improv_answer_device_info(const s_ezpi_improv_transport_t *origin);
static void ezpi_improv_answer_wifi_networks(const s_ezpi_improv_transport_t *origin);
static void ezpi_improv_send_empty_result(const s_ezpi_improv_transport_t *origin,
                                          uint8_t command);
static void ezpi_improv_wifi_event_upcall(esp_event_base_t event, int32_t event_id, void *arg);
static void ezpi_improv_provision_timeout(void *arg);
static void ezpi_improv_provision_timer_start(void);
static void ezpi_improv_provision_timer_stop(void);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static const s_ezpi_improv_transport_t *sg_transports[IMPROV_MAX_TRANSPORTS] = {NULL};
static uint32_t sg_transport_count = 0;

static uint8_t sg_state = EZPI_IMPROV_STATE_AUTHORIZED;
static uint8_t sg_error = EZPI_IMPROV_ERROR_NONE;

static esp_timer_handle_t sg_provision_timer = NULL;
static bool sg_core_started = false;

/*******************************************************************************
 *                          Public Function Definitions
 *******************************************************************************/

bool EZPI_SERV_improv_transport_register(const s_ezpi_improv_transport_t *transport)
{
    if (NULL == transport)
    {
        return false;
    }

    for (uint32_t i = 0; i < sg_transport_count; i++)
    {
        if (sg_transports[i] == transport)
        {
            /* Already registered. Not an error: both transports call this from
             * their own init and the order they run in is not fixed. */
            return true;
        }
    }

    if (sg_transport_count >= IMPROV_MAX_TRANSPORTS)
    {
        TRACE_E("improv: no room for transport '%s'",
                (NULL != transport->name) ? transport->name : "?");
        return false;
    }

    sg_transports[sg_transport_count] = transport;
    sg_transport_count++;

    if (false == sg_core_started)
    {
        sg_core_started = true;

        /* A claimed controller reports PROVISIONED from the outset, which is
         * what makes a conforming client not offer a Wi-Fi form it would only
         * be refused on. The refusal itself is enforced again at the moment
         * credentials arrive -- this is the courtesy, not the control. */
        sg_state = EZPI_SERV_improv_is_claimed() ? EZPI_IMPROV_STATE_PROVISIONED
                                                 : EZPI_IMPROV_STATE_AUTHORIZED;

        /* Registered once for the whole core rather than once per transport, so
         * that a got-IP is not counted twice. */
        EZPI_core_wifi_event_add(ezpi_improv_wifi_event_upcall, NULL);
    }

    TRACE_I("improv: transport '%s' registered, state %d",
            (NULL != transport->name) ? transport->name : "?", (int)sg_state);

    return true;
}

uint8_t EZPI_SERV_improv_state(void)
{
    return sg_state;
}

uint8_t EZPI_SERV_improv_error(void)
{
    return sg_error;
}

uint8_t EZPI_SERV_improv_capabilities(void)
{
    /* No capability bits are claimed. The only bit the protocol defines is
     * identify, and claiming it without implementing it would make a client
     * offer the user a button that does nothing. */
    return EZPI_IMPROV_CAP_NONE;
}

bool EZPI_SERV_improv_is_claimed(void)
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

void EZPI_SERV_improv_report_error(uint8_t error)
{
    ezpi_improv_set_error(error);
}

void EZPI_SERV_improv_handle_rpc(const s_ezpi_improv_transport_t *origin,
                                 const uint8_t *payload, uint32_t len,
                                 bool has_checksum)
{
    s_ezpi_improv_rpc_t rpc;
    const e_ezpi_improv_error_t parse_err =
        EZPI_improv_rpc_parse(payload, len, has_checksum, &rpc);

    if (EZPI_IMPROV_ERROR_NONE != parse_err)
    {
        ezpi_improv_set_error((uint8_t)parse_err);
        return;
    }

    switch (rpc.command)
    {
    case EZPI_IMPROV_RPC_SEND_WIFI:
    {
        ezpi_improv_handle_send_wifi(&rpc);
        break;
    }
    case EZPI_IMPROV_RPC_GET_CURRENT_STATE:
    {
        ezpi_improv_answer_current_state(origin);
        break;
    }
    case EZPI_IMPROV_RPC_GET_DEVICE_INFO:
    {
        ezpi_improv_answer_device_info(origin);
        break;
    }
    case EZPI_IMPROV_RPC_GET_WIFI_NETWORKS:
    {
        ezpi_improv_answer_wifi_networks(origin);
        break;
    }
    default:
    {
        ezpi_improv_set_error(EZPI_IMPROV_ERROR_UNKNOWN_RPC);
        break;
    }
    }
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

static void ezpi_improv_publish_state(const s_ezpi_improv_transport_t *only_to)
{
    for (uint32_t i = 0; i < sg_transport_count; i++)
    {
        const s_ezpi_improv_transport_t *t = sg_transports[i];

        if ((NULL != only_to) && (t != only_to))
        {
            continue;
        }

        if ((NULL != t) && (NULL != t->publish_state))
        {
            t->publish_state(sg_state);
        }
    }
}

static void ezpi_improv_set_state(uint8_t state)
{
    if (sg_state == state)
    {
        return;
    }

    sg_state = state;

    /* Broadcast, not sent to one transport: there is one radio and one
     * provisioning attempt, so a client watching over BLE while someone
     * provisions over the cable should see the truth rather than a stale
     * AUTHORIZED. */
    ezpi_improv_publish_state(NULL);

    TRACE_I("improv: state -> %d", (int)sg_state);
}

static void ezpi_improv_set_error(uint8_t error)
{
    sg_error = error;

    for (uint32_t i = 0; i < sg_transport_count; i++)
    {
        const s_ezpi_improv_transport_t *t = sg_transports[i];

        if ((NULL != t) && (NULL != t->publish_error))
        {
            t->publish_error(sg_error);
        }
    }

    if (EZPI_IMPROV_ERROR_NONE != error)
    {
        TRACE_E("improv: error -> %d", (int)error);
    }
}

/**
 * @brief Apply credentials from an Improv "send Wi-Fi settings" command.
 *
 * The payload has already been validated by EZPI_improv_rpc_parse(), which is
 * where every bound on the client-supplied lengths is enforced and which is
 * covered by the host tests. What is left here is policy, not parsing.
 */
static void ezpi_improv_handle_send_wifi(const s_ezpi_improv_rpc_t *rpc)
{
    /* The claim check is repeated here rather than relied upon from
     * registration: the controller can be claimed through the Ezlo app while
     * an Improv client is connected, and the decision has to reflect the state
     * at the moment the credentials arrive. */
    if (EZPI_SERV_improv_is_claimed())
    {
        TRACE_W("improv: credential write refused, controller already claimed");
        ezpi_improv_set_error(EZPI_IMPROV_ERROR_NOT_AUTHORIZED);
        ezpi_improv_set_state(EZPI_IMPROV_STATE_PROVISIONED);
        return;
    }

    /* Both are NUL-terminated copies: the wire format is length-prefixed and
     * not terminated, but EZPI_core_wifi_connect() takes C strings. The sizes
     * are the parser's own ceilings, which it has already enforced. */
    char ssid[EZPI_IMPROV_SSID_MAX + 1] = {0};
    char passphrase[EZPI_IMPROV_PASSPHRASE_MAX + 1] = {0};

    memcpy(ssid, rpc->ssid, rpc->ssid_len);

    if (0 != rpc->passphrase_len)
    {
        memcpy(passphrase, rpc->passphrase, rpc->passphrase_len);
    }

    /* The passphrase is reduced to a length for the log: this goes to the
     * serial console and, at cloud severity, off the device. */
    TRACE_I("improv: provisioning ssid '%s', passphrase %d char(s)",
            ssid, (int)rpc->passphrase_len);

    ezpi_improv_set_error(EZPI_IMPROV_ERROR_NONE);
    ezpi_improv_set_state(EZPI_IMPROV_STATE_PROVISIONING);

    /* Started before the connect call, not after: EZPI_core_wifi_connect() is
     * synchronous through esp_wifi_start() and the got-IP that ends the attempt
     * can be dispatched on the Wi-Fi task while we are still inside it. Arming
     * the timer first means there is no window in which the attempt is running
     * with nothing bounding it. */
    ezpi_improv_provision_timer_start();

    EZPI_core_wifi_connect(ssid, passphrase);
    EZPI_core_factory_info_v3_set_wifi(ssid, passphrase);
}

static void ezpi_improv_answer_current_state(const s_ezpi_improv_transport_t *origin)
{
    /* Sent to the asker alone and unconditionally, even when the state has not
     * changed: this is an answer to a question, so the no-change shortcut in
     * ezpi_improv_set_state() must not apply. */
    ezpi_improv_publish_state(origin);

    /* A provisioned device follows its state with the same result it would
     * have sent at the end of a successful provision, which is how a client
     * that connects to an already-configured device learns where to find it.
     * EzloPi has no local web UI to point at, so the list is empty. */
    if (EZPI_IMPROV_STATE_PROVISIONED == sg_state)
    {
        ezpi_improv_send_empty_result(origin, EZPI_IMPROV_RPC_GET_CURRENT_STATE);
    }
}

static void ezpi_improv_answer_device_info(const s_ezpi_improv_transport_t *origin)
{
    uint8_t buf[IMPROV_RESULT_BUF_LEN];
    s_ezpi_improv_result_t result;
    char *device_name = EZPI_core_factory_info_v3_get_name();

    EZPI_improv_result_init(&result, buf, sizeof(buf), EZPI_IMPROV_RPC_GET_DEVICE_INFO);

    /* Order is fixed by the protocol: firmware name, firmware version, chip
     * family, then the device's own name. */
    (void)EZPI_improv_result_add_str(&result, "EzloPi");
    (void)EZPI_improv_result_add_str(&result, VERSION_STR);
    (void)EZPI_improv_result_add_str(&result, IMPROV_CHIP_FAMILY);

    /* A blank factory-info name is reported as an empty entry rather than
     * omitted: the client indexes the list by position, so dropping an entry
     * would shift nothing into its place and leave the name reading as
     * whatever came after it. */
    (void)EZPI_improv_result_add_str(&result, (NULL != device_name) ? device_name : "");

    if (NULL != device_name)
    {
        ezlopi_free(__FUNCTION__, device_name);
    }

    /* false: the transport's frame carries the checksum. BLE, which has no
     * frame, re-checksums on the way out -- see the transport. */
    const uint16_t len = EZPI_improv_result_finish(&result, false);

    if ((0 != len) && (NULL != origin) && (NULL != origin->publish_result))
    {
        origin->publish_result(buf, len);
    }
    else if (0 == len)
    {
        /* Only reachable if the device name is long enough to overflow the
         * buffer, which the sizing above should prevent. Reported rather than
         * dropped so a client is not left waiting out its timeout. */
        ezpi_improv_set_error(EZPI_IMPROV_ERROR_UNKNOWN);
    }
}

static void ezpi_improv_answer_wifi_networks(const s_ezpi_improv_transport_t *origin)
{
    /* The protocol answers this with one result per network and then an empty
     * result to mark the end of the list. This firmware sends only the
     * terminator for now, which is a complete and conforming answer meaning
     * "no networks to offer" -- esp-web-tools takes an empty list and falls
     * back to asking the user to type the SSID, so provisioning still works.
     *
     * The terminator is sent rather than EZPI_IMPROV_ERROR_UNKNOWN_RPC, even
     * though the installer handles that too, because an unknown-command error
     * is not true: the command is understood, the list is simply empty.
     *
     * Filling the list in needs a blocking scan, and a blocking scan while the
     * station is associated has side effects of its own, so it is a separate
     * change rather than something to get half-right here. */
    ezpi_improv_send_empty_result(origin, EZPI_IMPROV_RPC_GET_WIFI_NETWORKS);
}

static void ezpi_improv_send_empty_result(const s_ezpi_improv_transport_t *origin,
                                          uint8_t command)
{
    uint8_t buf[RPC_EMPTY_RESULT_LEN];
    s_ezpi_improv_result_t result;

    EZPI_improv_result_init(&result, buf, sizeof(buf), command);

    const uint16_t len = EZPI_improv_result_finish(&result, false);

    if ((0 != len) && (NULL != origin) && (NULL != origin->publish_result))
    {
        origin->publish_result(buf, len);
    }
}

static void ezpi_improv_wifi_event_upcall(esp_event_base_t event, int32_t event_id, void *arg)
{
    /* Only transitions out of PROVISIONING are ours to report. Wi-Fi events also
     * arrive for ordinary roaming and for provisioning driven by the Ezlo app,
     * and reporting those as Improv results would make a connected Improv client
     * display progress for something it did not start. */
    if (EZPI_IMPROV_STATE_PROVISIONING != sg_state)
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

        ezpi_improv_set_error(EZPI_IMPROV_ERROR_NONE);
        ezpi_improv_set_state(EZPI_IMPROV_STATE_PROVISIONED);

        /* Improv's success response is a list of URLs for the device's own web
         * UI. EzloPi has no local web UI to point at, so the list is empty.
         *
         * Broadcast rather than addressed, because by the time an IP arrives
         * the originating RPC is long finished and the core no longer knows
         * which transport asked. A transport that nobody is listening on
         * discards it. */
        for (uint32_t i = 0; i < sg_transport_count; i++)
        {
            ezpi_improv_send_empty_result(sg_transports[i], EZPI_IMPROV_RPC_SEND_WIFI);
        }

        TRACE_S("improv: provisioned");
    }
}

/**
 * @brief Give up on the current provisioning attempt
 *
 * Runs on the esp_timer task once EZPI_IMPROV_PROVISION_TIMEOUT_US has passed
 * without an IP. The state is checked again here because the timer and the
 * Wi-Fi event run on different tasks: a got-IP that landed between the timer
 * firing and this callback being scheduled must not be overwritten with a
 * failure.
 */
static void ezpi_improv_provision_timeout(void *arg)
{
    if (EZPI_IMPROV_STATE_PROVISIONING != sg_state)
    {
        return;
    }

    /* The Wi-Fi layer keeps the reason string from the last disconnect, which is
     * the difference between "wrong password" and "no such network" for whoever
     * is reading the serial log. Improv's own error set cannot express either,
     * so it only goes to the log. */
    TRACE_E("improv: unable to connect within %d s, last disconnect reason: %s",
            (int)(EZPI_IMPROV_PROVISION_TIMEOUT_US / 1000000),
            EZPI_core_wifi_get_last_disconnect_reason());

    ezpi_improv_set_error(EZPI_IMPROV_ERROR_UNABLE_TO_CONNECT);

    /* Back to authorized rather than staying in provisioning, so the client can
     * offer the user another attempt instead of waiting on a state that will
     * never advance. */
    ezpi_improv_set_state(EZPI_IMPROV_STATE_AUTHORIZED);
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
    esp_timer_start_once(sg_provision_timer, EZPI_IMPROV_PROVISION_TIMEOUT_US);
}

static void ezpi_improv_provision_timer_stop(void)
{
    if (NULL != sg_provision_timer)
    {
        esp_timer_stop(sg_provision_timer);
    }
}

#endif // CONFIG_EZPI_ENABLE_IMPROV_SERIAL || CONFIG_EZPI_BLE_ENABLE_IMPROV

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
