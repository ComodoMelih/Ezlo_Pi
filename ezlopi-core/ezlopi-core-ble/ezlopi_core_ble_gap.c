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
 * @file    ezlopi_core_ble_gap.c
 * @brief   perform some function on ble-gap-operations
 * @author  Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 0.1
 * @date    12th DEC 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include "../../build/config/sdkconfig.h"

#ifdef CONFIG_EZPI_BLE_ENABLE

#include <string.h>

#include "ezlopi_util_trace.h"

#include "ezlopi_core_ble_gap.h"
#include "ezlopi_core_ble_profile.h"

#include "ezlopi_cloud_constants.h"
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
#define ADV_CONFIG_FLAG (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
#if (1 == ENABLE_TRACE)
static void show_bonded_devices(void);
static char *esp_key_type_to_str(esp_ble_key_type_t key_type);
static char *esp_auth_req_to_str(esp_ble_auth_req_t auth_req);
static char *EZPI_core_ble_gap_event_to_str(esp_gap_ble_cb_event_t event);
#endif
/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static uint8_t adv_config_done = 0;
// static uint8_t manufacturer[] = {'e', 'z', 'l', 'o', 'p', 'i'};

/* The advertisement carries the device name, as it always has: the shipped Ezlo
 * app discovers controllers by name prefix, so the name must stay in the
 * advertising payload and not move to the scan response.
 *
 * An earlier revision of this file moved the name into the scan response and
 * put the 0x00E1/0x00E3 service UUIDs here instead, so that
 * scanForPeripherals(withServices:) and ScanFilter.setServiceUuid() could
 * match. That has been reverted. The UUID list only helps clients that filter
 * by service UUID, and no shipped client does; against that it demoted the one
 * discovery key the Ezlo app actually uses to a field only an active scan
 * retrieves. Not a trade worth making by default.
 *
 * Compiled out when Improv is enabled, because improv_adv_data replaces it
 * wholesale and an unused file-scope static would warn. */
#ifndef CONFIG_EZPI_BLE_ENABLE_IMPROV
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    // .manufacturer_len = sizeof(manufacturer),
    // .p_manufacturer_data = manufacturer,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};
#endif // !CONFIG_EZPI_BLE_ENABLE_IMPROV

#ifdef CONFIG_EZPI_BLE_ENABLE_IMPROV
/* Improv Wi-Fi service UUID 00467768-6228-2272-4663-277478268000, as a 128-bit
 * little-endian array (byte 0 is the UUID's last octet).
 *
 * This is not a Bluetooth Base UUID expansion, so bluedroid cannot compact it
 * into a 2-byte element: btc_gap_ble.c takes the LEN_UUID_128 branch and emits
 * the full 16 bytes. That same branch is commented "Currently, only one
 * 128-bit UUID is supported", which is why nothing else can share this list. */
static uint8_t improv_service_uuid128[16] = {
    0x00, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46,
    0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00,
};

/* Improv requires service data under the 16-bit UUID 0x4677 holding
 * [current state][capabilities][0][0][0][0].
 *
 * bluedroid copies p_service_data into the AD element verbatim and does not
 * prepend the UUID itself (btc_gap_ble.c memcpy of service_data_len into a
 * BTM_BLE_AD_TYPE_SERVICE_DATA element), so the two UUID bytes are part of this
 * array, little-endian. */
static uint8_t improv_service_data[8] = {
    0x77, 0x46, /* 0x4677, little-endian */
    0x02,       /* current state: authorized */
    0x00,       /* capabilities: no identify */
    0x00, 0x00, 0x00, 0x00,
};

/* Advertisement used when Improv is compiled in. The budget is exact:
 *
 *     flags                     1 + 1 + 1          =  3
 *     128-bit service UUID      1 + 1 + 16         = 18
 *     service data (0x4677)     1 + 1 + 8          = 10
 *                                                    --
 *                                                    31  of BTM_BLE_AD_DATA_LEN
 *
 * There is no room for the device name, so include_name is false here and the
 * name is served from the scan response instead. That is the documented cost of
 * CONFIG_EZPI_BLE_ENABLE_IMPROV.
 *
 * Nothing may be added to this structure. btm_ble_build_adv_data() guards every
 * element with `if (len > ...)` and simply stops emitting when the budget is
 * gone -- it returns no error. Service data is built last, so anything extra
 * here would silently drop the Improv state bytes and leave a service UUID
 * advertised with no state attached, which is precisely the field Improv
 * clients read. */
static esp_ble_adv_data_t improv_adv_data = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = false,
    /* Both intervals must stay zero. btc_gap_ble.c sets BTM_BLE_AD_BIT_INT_RANGE
     * whenever min and max are non-zero and max >= min, which emits a 6-byte
     * slave-connection-interval-range element. Carrying the 0x0006/0x0010 pair
     * that adv_data uses would make the payload 3 + 6 + 18 + 10 = 37 bytes, and
     * the 6 bytes over budget would be taken off the end -- silently costing us
     * the Improv service data. The range is advisory only; dropping it does not
     * change the advertising interval, which comes from esp_ble_adv_params_t. */
    .min_interval = 0x0000,
    .max_interval = 0x0000,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = sizeof(improv_service_data),
    .p_service_data = improv_service_data,
    .service_uuid_len = sizeof(improv_service_uuid128),
    .p_service_uuid = improv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

/* The two arrays above are the only variable-length parts of this payload, so
 * their sizes are what an edit here would realistically change. The budget is
 * asserted at compile time because the Bluetooth stack will not check it at
 * runtime: esp_ble_gap_config_adv_data() returns ESP_OK whether or not the
 * elements fitted. Each element costs 2 bytes of header (length + AD type) on
 * top of its payload; flags carries 1 byte of payload, so 3 in total.
 *
 * This does not and cannot cover the fixed fields -- include_name,
 * include_txpower, the interval pair, manufacturer data -- because reading a
 * member of a static struct is not a constant expression in C. Those are
 * guarded by the comments above and by ezpi_ble_gap_improv_adv_budget_ok()
 * below, which re-checks them on the device at startup. */
#define EZPI_IMPROV_ADV_BUDGET 31
#define EZPI_IMPROV_ADV_USED (3u /* flags: len + type + 1 byte */              \
                              + 2u + sizeof(improv_service_uuid128)            \
                              + 2u + sizeof(improv_service_data))

_Static_assert(EZPI_IMPROV_ADV_USED <= EZPI_IMPROV_ADV_BUDGET,
               "Improv advertisement exceeds the 31-byte legacy payload; the "
               "stack would silently drop the trailing service data element, "
               "leaving the Improv UUID advertised with no state behind it.");

/**
 * @brief Re-check at runtime the advert fields a compile-time assert cannot see
 *
 * Returns false if any field that would add bytes to the payload has been set,
 * which with an already-exact budget means the service data gets truncated
 * away. Logged rather than fatal: a controller that advertises a slightly wrong
 * Improv payload is still a working Ezlo controller, and bricking it over this
 * would be the worse failure.
 */
static bool ezpi_ble_gap_improv_adv_budget_ok(void)
{
    uint32_t used = EZPI_IMPROV_ADV_USED;

    if (improv_adv_data.include_name)
    {
        /* The name is set at runtime (esp_ble_gap_set_device_name), so its
         * length is not knowable here. It does not need to be: the budget is
         * already exact without it, so 2 bytes of header plus any name at all
         * overflows. */
        used += 3u;
    }
    if (improv_adv_data.include_txpower)
    {
        used += 3u;
    }
    if ((improv_adv_data.min_interval > 0) && (improv_adv_data.max_interval > 0) &&
        (improv_adv_data.max_interval >= improv_adv_data.min_interval))
    {
        used += 6u;
    }
    if (improv_adv_data.manufacturer_len > 0)
    {
        used += 2u + improv_adv_data.manufacturer_len;
    }
    if (improv_adv_data.appearance != 0x00)
    {
        used += 4u;
    }

    if (used > EZPI_IMPROV_ADV_BUDGET)
    {
        TRACE_E("improv: advertisement is %u bytes, budget is %d -- the service "
                "data will be silently dropped and clients will not offer "
                "provisioning",
                (unsigned)used, EZPI_IMPROV_ADV_BUDGET);
        return false;
    }

    return true;
}
#endif // CONFIG_EZPI_BLE_ENABLE_IMPROV

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    // .adv_type = ADV_TYPE_DIRECT_IND_HIGH,
    // .adv_type = ADV_TYPE_SCAN_IND,
    // .adv_type = ADV_TYPE_NONCONN_IND,
    // .adv_type = ADV_TYPE_DIRECT_IND_LOW,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};
/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/
esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    .appearance = 0x00,
    // .manufacturer_len = sizeof(manufacturer), // TEST_MANUFACTURER_DATA_LEN,
    // .p_manufacturer_data = manufacturer,      //&manufacturer[0],
    .manufacturer_len = 0,       // TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data = NULL, //&manufacturer[0],
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};
/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
#if (1 == CONFIG_EZPI_BLE_ENABLE_PASSKEY)
void EZPI_core_ble_gap_set_passkey(uint32_t passkey)
{
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t));
}
#endif

void EZPI_core_ble_gap_start_advertising(void)
{
    esp_ble_gap_start_advertising(&adv_params);
}

/* Which advertisement this build advertises. Improv needs the whole 31-byte
 * budget, so the two layouts are alternatives rather than something that can be
 * merged -- see the comment on improv_adv_data. */
static esp_ble_adv_data_t *ezpi_ble_gap_active_adv_data(void)
{
#ifdef CONFIG_EZPI_BLE_ENABLE_IMPROV
    return &improv_adv_data;
#else
    return &adv_data;
#endif
}

void EZPI_core_ble_gap_config_adv_data(void)
{
#ifndef CONFIG_EZPI_UTIL_TRACE_EN
    esp_ble_gap_config_adv_data(ezpi_ble_gap_active_adv_data());
#else
    esp_err_t ret = esp_ble_gap_config_adv_data(ezpi_ble_gap_active_adv_data());
    if (ESP_OK != ret)
    {
        TRACE_E("config adv data failed, error code = %x", ret);
    }
#endif

    adv_config_done |= ADV_CONFIG_FLAG;
}

#ifdef CONFIG_EZPI_BLE_ENABLE_IMPROV
void EZPI_core_ble_gap_improv_set_adv_state(uint8_t improv_state, uint8_t capabilities)
{
    /* Bytes [0..1] are the 0x4677 UUID and bytes [4..7] are reserved zeroes;
     * only the state and capability bytes are ours to change. */
    if ((improv_service_data[2] == improv_state) && (improv_service_data[3] == capabilities))
    {
        /* Re-advertising an unchanged payload would cost a config round trip and
         * an advertising restart for nothing. */
        return;
    }

    improv_service_data[2] = improv_state;
    improv_service_data[3] = capabilities;

    /* The controller keeps its own copy of the payload, so mutating the array is
     * not enough -- the data has to be handed back to it. Advertising is then
     * restarted, because esp_ble_gap_config_adv_data() does not itself take
     * effect on an advertising set that is already running. */
    esp_err_t ret = esp_ble_gap_config_adv_data(&improv_adv_data);
    if (ESP_OK != ret)
    {
        TRACE_E("improv: config adv data failed, error code = %x", ret);
        return;
    }

    TRACE_I("improv: advertising state %d, capabilities 0x%02x", (int)improv_state, (unsigned)capabilities);
}
#endif // CONFIG_EZPI_BLE_ENABLE_IMPROV

void EZPI_core_ble_gap_config_scan_rsp_data(void)
{
    /* The scan response carries only the device name and TX power. Service
     * UUIDs are deliberately left out of it: name (up to 26 bytes) plus TX
     * power (3) plus a UUID list (6) would exceed the 31-byte limit for
     * longer provisioned names such as "ezlopi_generic_105200261".
     *
     * With CONFIG_EZPI_BLE_ENABLE_IMPROV the name here is the only copy, since
     * the Improv advertisement has no room for it. Without Improv the name is
     * in both, which is harmless and matches the long-standing behaviour. */
#ifndef CONFIG_EZPI_UTIL_TRACE_EN
    esp_ble_gap_config_adv_data(&scan_rsp_data);
#else
    esp_err_t ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
    if (!ret)
    {
        TRACE_S("Scan response set complete");
    }
#endif
    adv_config_done |= SCAN_RSP_CONFIG_FLAG;
}

void EZPI_core_ble_gap_dissociate_bonded_devices(void)
{
    int dev_num = esp_ble_get_bond_device_num();

    esp_ble_bond_dev_t *dev_list = (esp_ble_bond_dev_t *)ezlopi_malloc(__FUNCTION__, sizeof(esp_ble_bond_dev_t) * dev_num);
    if (dev_list)
    {
        esp_ble_get_bond_device_list(&dev_num, dev_list);
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
        TRACE_S("Bonded devices number : %d", dev_num);
        TRACE_S("Bonded devices list : %d", dev_num);
#endif
        for (int i = 0; i < dev_num; i++)
        {
            // dump("dev_list[i].bd_addr", dev_list[i].bd_addr, 0, sizeof(esp_bd_addr_t));
            esp_ble_gap_disconnect(dev_list[i].bd_addr);
            esp_ble_remove_bond_device(dev_list[i].bd_addr);
        }

        ezlopi_free(__FUNCTION__, dev_list);
    }
}

void EZPI_core_ble_setup_adv_config(void)
{
#ifdef CONFIG_EZPI_BLE_ENABLE_IMPROV
    /* Checked here rather than left to review, because the stack reports
     * success either way and the failure is invisible without a sniffer. */
    (void)ezpi_ble_gap_improv_adv_budget_ok();
#endif // CONFIG_EZPI_BLE_ENABLE_IMPROV

    esp_err_t ret = esp_ble_gap_config_adv_data(ezpi_ble_gap_active_adv_data());
    if (!ret)
    {
        adv_config_done |= ADV_CONFIG_FLAG;
    }
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
    else
    {
        TRACE_E("config adv data failed, error code = %x", ret);
    }
#endif

    ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
    if (!ret)
    {
        adv_config_done |= SCAN_RSP_CONFIG_FLAG;
    }
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
    else
    {
        TRACE_E("config adv data failed, error code = %x", ret);
    }
#endif
}

void EZPI_core_ble_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    // TRACE_I("BLE GAP Eevent: [%d]-%s", event, EZPI_core_ble_gap_event_to_str(event));
    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT: // 0
    {
        adv_config_done &= (~ADV_CONFIG_FLAG);
        if (0 == adv_config_done)
        {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    }
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT: // 1
    {
        adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
        if (0 == adv_config_done)
        {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    }
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT: // 6
    {
        // advertising start complete event to indicate advertising start successfully or failed
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            TRACE_E("Advertising start failed, error status = %x", param->adv_start_cmpl.status);
        }
        else
        {
            // TRACE_S("Start adv success.");
        }
        break;
    }
#endif

#if (1 == CONFIG_EZPI_BLE_ENABLE_PAIRING)
    case ESP_GAP_BLE_PASSKEY_REQ_EVT: // 12
    {
        break;
    }
    case ESP_GAP_BLE_OOB_REQ_EVT: // 13
    {
        uint8_t tk[16] = {1};
        esp_ble_oob_req_reply(param->ble_security.ble_req.bd_addr, tk, sizeof(tk));
        break;
    }
    case ESP_GAP_BLE_NC_REQ_EVT: // 16
    {
        esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
        break;
    }
    case ESP_GAP_BLE_SEC_REQ_EVT: // 10
    {
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    }
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: // 11
    {
        break;
    }
    case ESP_GAP_BLE_KEY_EVT: // 9
    {
        break;
    }
#endif
    case ESP_GAP_BLE_AUTH_CMPL_EVT: // 8
    {
        esp_bd_addr_t bd_addr;
        memcpy(bd_addr, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
        // dump("remote BD_ADDR", bd_addr, 0, 6);
        // TRACE_I("address type = %d", param->ble_security.auth_cmpl.addr_type);
        TRACE_OTEL(ENUM_EZLOPI_TRACE_SEVERITY_INFO, "BLE: '%2X-%2X-%2X-%2X-%2X-%2X' connected!", bd_addr[0], bd_addr[1], bd_addr[2], bd_addr[3], bd_addr[4], bd_addr[5]);
        if (!param->ble_security.auth_cmpl.success)
        {
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
            TRACE_W("pair status = fail");
            TRACE_W("fail reason = 0x%x", param->ble_security.auth_cmpl.fail_reason);
#endif
            esp_ble_gap_disconnect(bd_addr);
        }
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
        else
        {
            TRACE_S("pair status = success");
            TRACE_S("auth mode =  %s", esp_auth_req_to_str(param->ble_security.auth_cmpl.auth_mode));
        }
#endif
#if (1 == ENABLE_TRACE)
        show_bonded_devices();
#endif // 1 == ENABLE_TRACE
        break;
    }
    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT: // 23
    {
        uint8_t *client_mac = param->remove_bond_dev_cmpl.bd_addr;
        TRACE_OTEL(ENUM_EZLOPI_TRACE_SEVERITY_INFO, "BLE: '%2X-%2X-%2X-%2X-%2X-%2X' dis-connected!", client_mac[0], client_mac[1], client_mac[2], client_mac[3], client_mac[4], client_mac[5]);
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
        TRACE_I("ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT status = %d", param->remove_bond_dev_cmpl.status);
        TRACE_I("ESP_GAP_BLE_REMOVE_BOND_DEV");
        TRACE_I("-----ESP_GAP_BLE_REMOVE_BOND_DEV----");
        // dump("bd_adrr", param->remove_bond_dev_cmpl.bd_addr, 0, sizeof(esp_bd_addr_t));
        TRACE_I("------------------------------------");
#endif
        break;
    }
#endif
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT: // 17
    {
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            TRACE_E("Advertising stop failed");
        }
        else
        {
            TRACE_S("Stop adv successfully");
        }
        break;
    }
#endif
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT: // 20
    {
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
        TRACE_S("update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                param->update_conn_params.status,
                param->update_conn_params.min_int,
                param->update_conn_params.max_int,
                param->update_conn_params.conn_int,
                param->update_conn_params.latency,
                param->update_conn_params.timeout);
        EZPI_core_ble_setup_adv_config();
#endif
        break;
    }
#if (1 == CONFIG_EZPI_BLE_ENABLE_PAIRING)
    case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT: // 22
    {
        EZPI_core_ble_setup_adv_config();
        break;
    }
#endif

    default:
    {
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
        TRACE_W("BLE GAP Event: %s Not Implemented!", EZPI_core_ble_gap_event_to_str(event));
#endif
        break;
    }
    }
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/
#if (1 == ENABLE_TRACE)
static char *EZPI_core_ble_gap_event_to_str(esp_gap_ble_cb_event_t event)
{
    char *ret = "BLE GAP Event Not defined!";
    switch (event)
    {
#if (BLE_42_FEATURE_SUPPORT == TRUE)
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT: /*!< When advertising data set complete, the event comes */
    {
        ret = "ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT: /*!< When scan response data set complete, the event comes */
    {
        ret = "ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: /*!< When scan parameters set complete, the event comes */
    {
        ret = "ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_SCAN_RESULT_EVT: /*!< When one scan result ready, the event comes each time */
    {
        ret = "ESP_GAP_BLE_SCAN_RESULT_EVT";
        break;
    }
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT: /*!< When raw advertising data set complete, the event comes */
    {
        ret = "ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT: /*!< When raw advertising data set complete, the event comes */
    {
        ret = "ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT: /*!< When start advertising complete, the event comes */
    {
        ret = "ESP_GAP_BLE_ADV_START_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT: /*!< When start scan complete, the event comes */
    {
        ret = "ESP_GAP_BLE_SCAN_START_COMPLETE_EVT";
        break;
    }
#endif                              // #if (BLE_42_FEATURE_SUPPORT == TRUE)
    case ESP_GAP_BLE_AUTH_CMPL_EVT: /* Authentication complete indication. */
    {
        ret = "ESP_GAP_BLE_AUTH_CMPL_EVT";
        break;
    }
    case ESP_GAP_BLE_KEY_EVT: /* BLE  key event for peer device keys */
    {
        ret = "ESP_GAP_BLE_KEY_EVT";
        break;
    }
    case ESP_GAP_BLE_SEC_REQ_EVT: /* BLE  security request */
    {
        ret = "ESP_GAP_BLE_SEC_REQ_EVT";
        break;
    }
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: /* passkey notification event */
    {
        ret = "ESP_GAP_BLE_PASSKEY_NOTIF_EVT";
        break;
    }
    case ESP_GAP_BLE_PASSKEY_REQ_EVT: /* passkey request event */
    {
        ret = "ESP_GAP_BLE_PASSKEY_REQ_EVT";
        break;
    }
    case ESP_GAP_BLE_OOB_REQ_EVT: /* OOB request event */
    {
        ret = "ESP_GAP_BLE_OOB_REQ_EVT";
        break;
    }
    case ESP_GAP_BLE_LOCAL_IR_EVT: /* BLE local IR event */
    {
        ret = "ESP_GAP_BLE_LOCAL_IR_EVT";
        break;
    }
    case ESP_GAP_BLE_LOCAL_ER_EVT: /* BLE local ER event */
    {
        ret = "ESP_GAP_BLE_LOCAL_ER_EVT";
        break;
    }
    case ESP_GAP_BLE_NC_REQ_EVT: /* Numeric Comparison request event */
    {
        ret = "ESP_GAP_BLE_NC_REQ_EVT";
        break;
    }
#if (BLE_42_FEATURE_SUPPORT == TRUE)
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT: /*!< When stop adv complete, the event comes */
    {
        ret = "ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: /*!< When stop scan complete, the event comes */
    {
        ret = "ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT";
        break;
    }
#endif                                         // #if (BLE_42_FEATURE_SUPPORT == TRUE)
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT: /*!< When set the static rand address complete, the event comes */
    {
        ret = "ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT";
        break;
    }
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT: /*!< When update connection parameters complete, the event comes */
    {
        ret = "ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT";
        break;
    }
    case ESP_GAP_BLE_SET_PKT_LENGTH_COMPLETE_EVT: /*!< When set pkt length complete, the event comes */
    {
        ret = "ESP_GAP_BLE_SET_PKT_LENGTH_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT: /*!< When  Enable/disable privacy on the local device complete, the event comes */
    {
        ret = "ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT: /*!< When remove the bond device complete, the event comes */
    {
        ret = "ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_CLEAR_BOND_DEV_COMPLETE_EVT: /*!< When clear the bond device clear complete, the event comes */
    {
        ret = "ESP_GAP_BLE_CLEAR_BOND_DEV_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_GET_BOND_DEV_COMPLETE_EVT: /*!< When get the bond device list complete, the event comes */
    {
        ret = "ESP_GAP_BLE_GET_BOND_DEV_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT: /*!< When read the rssi complete, the event comes */
    {
        ret = "ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_UPDATE_WHITELIST_COMPLETE_EVT: /*!< When add or remove whitelist complete, the event comes */
    {
        ret = "ESP_GAP_BLE_UPDATE_WHITELIST_COMPLETE_EVT";
        break;
    }
#if (BLE_42_FEATURE_SUPPORT == TRUE)
    case ESP_GAP_BLE_UPDATE_DUPLICATE_EXCEPTIONAL_LIST_COMPLETE_EVT: /*!< When update duplicate exceptional list complete, the event comes */
    {
        ret = "ESP_GAP_BLE_UPDATE_DUPLICATE_EXCEPTIONAL_LIST_COMPLETE_EVT";
        break;
    }
#endif                                 // #if (BLE_42_FEATURE_SUPPORT == TRUE)
    case ESP_GAP_BLE_SET_CHANNELS_EVT: /*!< When setting BLE channels complete, the event comes */
    {
        ret = "ESP_GAP_BLE_SET_CHANNELS_EVT";
        break;
    }
#if (BLE_50_FEATURE_SUPPORT == TRUE)
    case ESP_GAP_BLE_READ_PHY_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_READ_PHY_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_SET_PREFERED_DEFAULT_PHY_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_SET_PREFERED_DEFAULT_PHY_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_SET_PREFERED_PHY_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_SET_PREFERED_PHY_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_ADV_SET_RAND_ADDR_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_ADV_SET_RAND_ADDR_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_SCAN_RSP_DATA_SET_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_SCAN_RSP_DATA_SET_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_ADV_START_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_ADV_START_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_ADV_STOP_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_ADV_STOP_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_ADV_SET_REMOVE_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_ADV_SET_REMOVE_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_ADV_SET_CLEAR_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_ADV_SET_CLEAR_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_SET_PARAMS_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_SET_PARAMS_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_DATA_SET_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_DATA_SET_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_START_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_START_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_STOP_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_STOP_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_CREATE_SYNC_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_CREATE_SYNC_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_SYNC_CANCEL_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_SYNC_CANCEL_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_SYNC_TERMINATE_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_SYNC_TERMINATE_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_ADD_DEV_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_ADD_DEV_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_REMOVE_DEV_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_REMOVE_DEV_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_CLEAR_DEV_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_CLEAR_DEV_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_SCAN_START_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_SCAN_START_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_SCAN_STOP_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_SCAN_STOP_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PREFER_EXT_CONN_PARAMS_SET_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PREFER_EXT_CONN_PARAMS_SET_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_PHY_UPDATE_COMPLETE_EVT:
    {
        ret = "ESP_GAP_BLE_PHY_UPDATE_COMPLETE_EVT";
        break;
    }
    case ESP_GAP_BLE_EXT_ADV_REPORT_EVT:
    {
        ret = "ESP_GAP_BLE_EXT_ADV_REPORT_EVT";
        break;
    }
    case ESP_GAP_BLE_SCAN_TIMEOUT_EVT:
    {
        ret = "ESP_GAP_BLE_SCAN_TIMEOUT_EVT";
        break;
    }
    case ESP_GAP_BLE_ADV_TERMINATED_EVT:
    {
        ret = "ESP_GAP_BLE_ADV_TERMINATED_EVT";
        break;
    }
    case ESP_GAP_BLE_SCAN_REQ_RECEIVED_EVT:
    {
        ret = "ESP_GAP_BLE_SCAN_REQ_RECEIVED_EVT";
        break;
    }
    case ESP_GAP_BLE_CHANNEL_SELETE_ALGORITHM_EVT:
    {
        ret = "ESP_GAP_BLE_CHANNEL_SELETE_ALGORITHM_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_REPORT_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_REPORT_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_SYNC_LOST_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_SYNC_LOST_EVT";
        break;
    }
    case ESP_GAP_BLE_PERIODIC_ADV_SYNC_ESTAB_EVT:
    {
        ret = "ESP_GAP_BLE_PERIODIC_ADV_SYNC_ESTAB_EVT";
        break;
    }

#endif // #if (BLE_50_FEATURE_SUPPORT == TRUE)
       // case ESP_GAP_BLE_SC_OOB_REQ_EVT:
       // {
       //     ret = "ESP_GAP_BLE_SC_OOB_REQ_EVT";
       //     break;
       // }
       // case ESP_GAP_BLE_EVT_MAX:
       // {
       //     ret = "ESP_GAP_BLE_EVT_MAX";
       //     break;
       // }
       // case ESP_GAP_BLE_SC_CR_LOC_OOB_EVT:
       // {
       //     ret = "ESP_GAP_BLE_SC_CR_LOC_OOB_EVT";
       //     break;
       // }
       // case ESP_GAP_BLE_GET_DEV_NAME_COMPLETE_EVT:
       // {
       //     ret = "ESP_GAP_BLE_GET_DEV_NAME_COMPLETE_EVT";
       //     break;
       // }

    default:
        break;
    }

    return ret;
}
static char *esp_auth_req_to_str(esp_ble_auth_req_t auth_req)
{
    char *auth_str = NULL;
    switch (auth_req)
    {
    case ESP_LE_AUTH_NO_BOND:
        auth_str = "ESP_LE_AUTH_NO_BOND";
        break;
    case ESP_LE_AUTH_BOND:
        auth_str = "ESP_LE_AUTH_BOND";
        break;
    case ESP_LE_AUTH_REQ_MITM:
        auth_str = "ESP_LE_AUTH_REQ_MITM";
        break;
    case ESP_LE_AUTH_REQ_BOND_MITM:
        auth_str = "ESP_LE_AUTH_REQ_BOND_MITM";
        break;
    case ESP_LE_AUTH_REQ_SC_ONLY:
        auth_str = "ESP_LE_AUTH_REQ_SC_ONLY";
        break;
    case ESP_LE_AUTH_REQ_SC_BOND:
        auth_str = "ESP_LE_AUTH_REQ_SC_BOND";
        break;
    case ESP_LE_AUTH_REQ_SC_MITM:
        auth_str = "ESP_LE_AUTH_REQ_SC_MITM";
        break;
    case ESP_LE_AUTH_REQ_SC_MITM_BOND:
        auth_str = "ESP_LE_AUTH_REQ_SC_MITM_BOND";
        break;
    default:
        auth_str = "INVALID BLE AUTH REQ";
        break;
    }

    return auth_str;
}
static char *esp_key_type_to_str(esp_ble_key_type_t key_type)
{
    char *key_str = NULL;
    switch (key_type)
    {
    case ESP_LE_KEY_NONE:
        key_str = "ESP_LE_KEY_NONE";
        break;
    case ESP_LE_KEY_PENC:
        key_str = "ESP_LE_KEY_PENC";
        break;
    case ESP_LE_KEY_PID:
        key_str = "ESP_LE_KEY_PID";
        break;
    case ESP_LE_KEY_PCSRK:
        key_str = "ESP_LE_KEY_PCSRK";
        break;
    case ESP_LE_KEY_PLK:
        key_str = "ESP_LE_KEY_PLK";
        break;
    case ESP_LE_KEY_LLK:
        key_str = "ESP_LE_KEY_LLK";
        break;
    case ESP_LE_KEY_LENC:
        key_str = "ESP_LE_KEY_LENC";
        break;
    case ESP_LE_KEY_LID:
        key_str = "ESP_LE_KEY_LID";
        break;
    case ESP_LE_KEY_LCSRK:
        key_str = "ESP_LE_KEY_LCSRK";
        break;
    default:
        key_str = "INVALID BLE KEY TYPE";
        break;
    }

    return key_str;
}
static void show_bonded_devices(void)
{
    int dev_num = esp_ble_get_bond_device_num();

    esp_ble_bond_dev_t *dev_list = (esp_ble_bond_dev_t *)ezlopi_malloc(__FUNCTION__, sizeof(esp_ble_bond_dev_t) * dev_num);
    if (dev_list)
    {
        esp_ble_get_bond_device_list(&dev_num, dev_list);
        TRACE_S("Bonded devices number : %d", dev_num);

        TRACE_S("Bonded devices list : %d", dev_num);
        for (int i = 0; i < dev_num; i++)
        {
            // dump("dev_list[i].bd_addr", dev_list[i].bd_addr, 0, sizeof(esp_bd_addr_t));
        }

        ezlopi_free(__FUNCTION__, dev_list);
    }
}
#endif // 1 == ENABLE_TRACE

#endif // CONFIG_EZPI_BLE_ENABLE
/*******************************************************************************
 *                          End of File
 *******************************************************************************/
