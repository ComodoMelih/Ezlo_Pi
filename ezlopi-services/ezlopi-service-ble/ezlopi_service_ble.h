
/**
 * @file    ezlopi_service_ble.h
 * @brief   Service related functionalities
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
 * @file    ezlopi_service_ble.h
 * @brief   Service related functionalities
 * @author  
 * @version 1.0
 * @date    Janyary 22, 2024
 */

#ifndef _EZLOPI_SERVICE_BLE_H_
#define _EZLOPI_SERVICE_BLE_H_

#ifdef CONFIG_EZPI_BLE_ENABLE

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

/*******************************************************************************
 *                          C++ Declaration Wrapper
 *******************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif // __cplusplus
    /*******************************************************************************
     *                          Type & Macro Declarations
     *******************************************************************************/
/**
 * @brief Checks for any error if found logs the message and returns
 *
 * @param[in] x error code
 * @param[in] msg messaage to log if error found
 *
 * @return void
 *
 * Example
 * @code
 * esp_err_t error = function_that_returns_error();
 * CHECK_PRINT_ERROR(error, "Error found");
 * @endcode
 *
 */
#define CHECK_PRINT_ERROR(x, msg)                                    \
    {                                                                \
        if (x)                                                       \
        {                                                            \
            TRACE_E("%s %s: %s", __func__, msg, esp_err_to_name(x)); \
            return;                                                  \
        }                                                            \
    }

/*******************************************************************************
 *                          WiFi Service Macros Start
 *******************************************************************************/
/**
 * @brief Serivce handle ID for the WiFi service
 *
 */
#define BLE_WIFI_SERVICE_HANDLE 0
/**
 * @brief Serivce UUID for the WiFi service
 *
 */
#define BLE_WIFI_SERVICE_UUID 0x00E1
/**
 * @brief WiFi service characteristics UUID for WiFi credential setter
 *
 */
#define BLE_WIFI_CHAR_CREDS_UUID 0xE101
/**
 * @brief WiFi service characteristics UUID for WiFi status
 *
 */
#define BLE_WIFI_CHAR_STATUS_UUID 0xE102
/**
 * @brief WiFi service characteristics UUID for WiFi error
 *
 */
#define BLE_WIFI_CHAR_ERROR_UUID 0xE103
/*******************************************************************************
 *                          WiFi Service Macros End
 *******************************************************************************/

/*******************************************************************************
 *                          Security Service Macros Start
 *******************************************************************************/
/**
 * @brief Serivce handle ID for the security service
 *
 */
#define BLE_SECURITY_SERVICE_HANDLE 1
/**
 * @brief Serivce UUID for the security service
 *
 */
#define BLE_SECURITY_SERVICE_UUID 0x00E2
/**
 * @brief Security service characteristics UUID for passkey
 *
 */
#define BLE_SECURITY_CHAR_PASSKEY_UUID 0xE201
/**
 * @brief Security service characteristics UUID for factory reset
 *
 */
#define BLE_SECURITY_FACTORY_RESET_CHAR_UUID 0xE202
/**
 * @brief Security service characteristics UUID for device reset
 *
 */
#define BLE_SECURITY_RESET_CHAR_UUID 0xE203
/*******************************************************************************
 *                          Security Service Macros End
 *******************************************************************************/

/*******************************************************************************
 *                          Provisioning Service Macros Start
 *******************************************************************************/
/**
 * @brief Serivce handle ID for the provisioning service
 *
 */
#define BLE_PROVISIONING_ID_HANDLE 2
/**
 * @brief Serivce UUID for the provisioning service
 *
 */
#define BLE_PROVISIONING_SERVICE_UUID 0x00E3
/**
 * @brief Provisioning service characteristics UUID for device proviosioning
 *
 */
#define BLE_PROVISIONING_CHAR_UUID 0xE301
/**
 * @brief Provisioning service characteristics UUID for device provisioning status
 *
 */
#define BLE_PROVISIONING_STATUS_CHAR_UUID 0xE302
/*******************************************************************************
 *                          Provisioning Service Macros End
 *******************************************************************************/

/*******************************************************************************
 *                          Device Info Service Macros Start
 *******************************************************************************/
/**
 * @brief Serivce handle ID for the device info service
 *
 */
#define BLE_DEVICE_INFO_ID_HANDLE 3
/**
 * @brief Serivce UUID for the device info service
 *
 */
#define BLE_DEVICE_INFO_SERVICE_UUID 0x00E4
/**
 * @brief Device info service characteristics UUID for device info getter
 *
 */
#define BLE_DEVICE_INFO_CHAR_UUID 0xE401
/**
 * @brief Device info service characteristics UUID for ezloPi Firmware API version Info
 *
 */
#define EZPI_BLE_CHAR_API_VERSION_INFO_UUID 0xE402
/**
 * @brief Device info service characteristics UUID for ezloPi Firmware Info
 *
 */
#define EZPI_BLE_CHAR_FIRMWARE_INFO_UUID 0xE403
/**
 * @brief Device info service characteristics UUID for ezloPi Chip Info
 *
 */
#define EZPI_BLE_CHAR_CHIP_INFO_UUID 0xE404
/**
 * @brief Device info service characteristics UUID for ezloPi firmware sdk Info
 *
 */
#define EZPI_BLE_CHAR_FIRMWARE_SDK_INFO_UUID 0xE405
/**
 * @brief Device info service characteristics UUID for ezloPi device state Info
 *
 */
#define EZPI_BLE_CHAR_DEV_STATE_INFO_UUID 0xE406
/**
 * @brief Device info service characteristics UUID for ezloPi serial config Info
 *
 */
#define EZPI_BLE_CHAR_SER_CONFIG_INFO_UUID 0xE407
/**
 * @brief Device info service characteristics UUID for ezloPi cloud Info
 *
 */
#define EZPI_BLE_CHAR_EZPI_CLOUD_INFO_UUID 0xE408
/**
 * @brief Device info service characteristics UUID for ezloPi oem Info
 *
 */
#define EZPI_BLE_CHAR_OEM_INFO_UUID 0xE409
/**
 * @brief Device info service characteristics UUID for ezloPi Network Info
 *
 */
#define EZPI_BLE_CHAR_NETWORK_INFO_UUID 0xE40A
/*******************************************************************************
 *                          Device Info Service Macros End
 *******************************************************************************/

/*******************************************************************************
 *                          Dynamic Config Service Macros Start
 *******************************************************************************/
/**
 * @brief Serivce handle ID for the dynamic config
 *
 */
#define BLE_DYNAMIC_CONFIG_HANDLE 4
/**
 * @brief Serivce UUID for the dynamic config
 *
 */
#define BLE_DYNAMIC_CONFIG_SERVICE_UUID 0x00E5
/**
 * @brief Dynamic config service characteristics UUID for device configuration setter
 *
 */
#define BLE_DYNAMIC_CONFIG_CHAR_UUID 0xE501
    /*******************************************************************************
     *                          Dynamic Config Service Macros end
     *******************************************************************************/

#ifdef CONFIG_EZPI_BLE_ENABLE_IMPROV
/*******************************************************************************
 *                          Improv Wi-Fi Service Macros Start
 *******************************************************************************/
/**
 * @brief Service handle ID for the Improv Wi-Fi service
 *
 * Handles 0..4 belong to the five EzloPi services; Improv is the sixth, and the
 * same value must be passed to esp_ble_gatts_app_register() in
 * EZPI_ble_service_init().
 */
#define BLE_IMPROV_SERVICE_HANDLE 5
/**
 * @brief Improv UUID builder, for the UUIDs fixed by improv-wifi.com
 *
 * The Improv UUIDs are 00467768-6228-2272-4663-2774782680xx, where xx is 00 for
 * the service and 01..05 for the characteristics. Unlike the EzloPi services,
 * which use 16-bit UUIDs, these have to be given to esp_bt_uuid_t as a full
 * uint8_t[16] in little-endian order -- byte 0 is the UUID's *last* octet, which
 * is exactly the octet that varies, hence the parameter.
 */
#define BLE_IMPROV_UUID128(last_octet)                  \
    {                                                   \
        (last_octet), 0x80, 0x26, 0x78, 0x74, 0x27,     \
            0x63, 0x46, 0x72, 0x22, 0x28, 0x62,         \
            0x68, 0x77, 0x46, 0x00,                     \
    }
/*
 * The Improv state, error, command and capability codes used to live here as
 * BLE_IMPROV_* macros. They are now in ezlopi-util-improv, which the BLE and
 * serial transports share, so the protocol has one definition rather than one
 * per transport.
 *
 * That is more than tidiness. The copy that was here defined
 * BLE_IMPROV_RPC_IDENTIFY as 0x02, and there is no identify command: the
 * Improv SDK gives IDENTIFY and GET_CURRENT_STATE the same value 0x02, and the
 * reference implementations treat 0x02 as get-current-state. A handler written
 * against that constant would have answered the wrong question, and a
 * capability bit advertising it would have put a dead button in the client's
 * UI. The two macros above are genuinely BLE-specific and stay.
 */
/*******************************************************************************
 *                          Improv Wi-Fi Service Macros end
 *******************************************************************************/
#endif // CONFIG_EZPI_BLE_ENABLE_IMPROV

    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief Function to initialize BLE services
     * @details This function performs following operations
     *  - Prepares the BLE services for initialization,
     *  - Starts the BLE stack
     *  - Sets MTU
     *  - Registers the BLE apps/services prepared prior providing service IDs.
     *  - Opens BLE gatt server for pairing/connection
     *
     */
    void EZPI_ble_service_init(void);

    /**
     * @brief Function to get the BLE MAC
     *
     * @param[out] mac uint8_t type array of size 6 to hold BLE MAC
     * @return int
     * @retval 0 on success, or 1 on success
     *
     */
    int EZPI_ble_service_get_ble_mac(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // CONFIG_EZPI_BLE_ENABLE

#endif //  _EZLOPI_SERVICE_BLE_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/