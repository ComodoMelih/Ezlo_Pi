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
 * @file    ezlopi_core_ota.c
 * @brief   Function to perform ota operation
 * @author
 * @version 1.0
 * @date    March 7th, 2023 8:52 PM
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
/* ESP-IDF puts the generated config directory on every component's include
 * path, so the plain name is correct here. The previous
 * "../../build/config/sdkconfig.h" hard-coded both the build directory name
 * and its position relative to this file, so it broke for any build directory
 * other than <project>/build -- including `idf.py -B <dir>` and the
 * out-of-tree builds CI runs. */
#include "sdkconfig.h"

#ifdef CONFIG_EZPI_ENABLE_OTA

/* The CA chain for the OTA host is NOT in this repository: ca_mios.h is listed
 * in .gitignore, so a clean clone does not have it. Enabling OTA therefore
 * requires you to supply your own ca_mios.h defining:
 *
 *     static const char ca_mios[] = "-----BEGIN CERTIFICATE-----\n...";
 *
 * Without this guard the build failed with "ca_mios.h: No such file or
 * directory" pointing at a line in the middle of a function body, which gives
 * no hint that a file is expected to be supplied by hand. */
#if defined(__has_include)
#if !__has_include("ca_mios.h")
#error "CONFIG_EZPI_ENABLE_OTA is enabled but ezlopi-core-ota/ca_mios.h is missing. That header holds the CA chain for the OTA server and is deliberately not in this repository (see .gitignore); supply your own, or disable OTA in sdkconfig.defaults.<target>."
#endif
#endif

/* Was included inside ezlopi_ota_process()'s body, between statements. It
 * defines a variable, so at block scope it became a per-call local; at file
 * scope, where an include belongs, it is static storage as intended. */
#include "ca_mios.h"

#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_http_client.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"

#include "ezlopi_util_trace.h"

#include "ezlopi_core_ota.h"
#include "ezlopi_core_reset.h"
#include "ezlopi_core_factory_info.h"
#include "ezlopi_core_processes.h"

#include "ezlopi_service_ota.h"
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
#define HASH_LEN 32
#define OTA_URL_SIZE 512

typedef enum e_ezlopi_ota_state
{
    EZLOPI_OTA_STATE_FINISH = 0,
    EZLOPI_OTA_STATE_STARTED,
    EZLOPI_OTA_STATE_UPDATING,
    EZLOPI_OTA_STATE_FAILED,
    EZLOPI_OTA_STATE_MAX,
} e_ezlopi_ota_state_t;
/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static int32_t __byte_count = 0;

/* Reported OTA state, returned verbatim by EPZI_core_ota_get_state(). The
 * numeric values are part of the scene surface: ezlopi_core_scenes_when_methods.c
 * compares this against the literals 0, 1 and 2 for "done", "started" and
 * "updating", so neither the enum numbering nor what this holds after a failed
 * update may change. It stays at EZLOPI_OTA_STATE_FAILED after a failure,
 * exactly as before. */
static volatile uint32_t __ota_in_process = 0;

/* Whether an OTA task is currently alive. This used to be conflated with the
 * reported state above, which wedged OTA permanently: a failed download leaves
 * the state at EZLOPI_OTA_STATE_FAILED (3), nothing ever reset it, and the
 * launch guard demanded 0 -- so after one flaky download no further update
 * could be started until a power cycle. The two concerns are now separate, so
 * the reported state is unchanged while the guard clears correctly.
 *
 * Claimed under the spinlock below, and before xTaskCreate rather than in the
 * task body. Setting it inside the task left a window spanning task creation
 * and scheduling, in which two requests arriving close together both passed
 * the guard and started two concurrent esp_https_ota() calls against the same
 * OTA partition. */
static volatile bool __ota_task_alive = false;

/* Two different tasks can dispatch cloud methods -- the cloud websocket client
 * in ezlopi_service_webprov.c and the local websocket server in
 * ezlopi_service_ws_server.c both call EZPI_core_api_consume*(), and
 * "hub.firmware.update.start" lands here from either. A plain read-then-write
 * of the flag above is not atomic across them: both tasks could read false and
 * both then set it. The claim is therefore done under a critical section. */
static portMUX_TYPE __ota_guard_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void ezlopi_ota_process(void *pv);
static esp_err_t _http_event_handler(esp_http_client_event_t *evt);

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

uint32_t EPZI_core_ota_get_state(void)
{
    return ((__ota_in_process < EZLOPI_OTA_STATE_MAX) ? __ota_in_process : EZLOPI_OTA_STATE_FINISH);
}

void EZPI_core_ota_start(cJSON *url)
{
    if (url && url->valuestring)
    {
        /* The URL length is measured rather than assumed. This was
         * memcpy(ota_url, url->valuestring, OTA_URL_SIZE) with OTA_URL_SIZE of
         * 512, copying a fixed 512 bytes out of a cJSON string allocated to
         * the length of the actual URL -- typically 60 to 100 bytes. Every OTA
         * read roughly 400 bytes past the end of that allocation. It did not
         * crash in practice only because the heap happened to be readable
         * there, and it could copy unrelated heap contents into the URL. */
        const size_t url_len = strlen(url->valuestring);
        if (url_len >= OTA_URL_SIZE)
        {
            TRACE_E("OTA url too long: %u bytes, max %d", (unsigned)url_len, OTA_URL_SIZE - 1);
            return;
        }

        /* Atomic test-and-set: exactly one caller can observe the flag clear
         * and claim it, even if both websocket tasks arrive together. */
        bool claimed = false;
        portENTER_CRITICAL(&__ota_guard_spinlock);
        if (false == __ota_task_alive)
        {
            __ota_task_alive = true;
            claimed = true;
        }
        portEXIT_CRITICAL(&__ota_guard_spinlock);

        if (false == claimed)
        {
            TRACE_W("Ota in progress...");
            return;
        }

        char *ota_url = (char *)ezlopi_malloc(__FUNCTION__, url_len + 1);
        if (ota_url)
        {
            memcpy(ota_url, url->valuestring, url_len + 1);

            TaskHandle_t ezlopi_core_ota_process_task_handle = NULL;
            if (pdPASS == xTaskCreate(ezlopi_ota_process, "EzpiOTAProcess", EZLOPI_CORE_OTA_PROCESS_TASK_DEPTH, ota_url, 3, &ezlopi_core_ota_process_task_handle))
            {
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
                EZPI_core_process_set_process_info(ENUM_EZLOPI_CORE_OTA_PROCESS_TASK, &ezlopi_core_ota_process_task_handle, EZLOPI_CORE_OTA_PROCESS_TASK_DEPTH);
#endif
            }
            else
            {
                /* xTaskCreate was previously called without checking its
                 * return value. On failure the task never runs, so nothing
                 * would ever have released the guard or the buffer. */
                TRACE_E("Failed to create OTA task");
                ezlopi_free(__FUNCTION__, ota_url);
                ota_url = NULL;
                __ota_task_alive = false;
            }
        }
        else
        {
            /* The claim has to be released here too. Without this, a single
             * failed allocation would leave the flag set with no task running
             * to clear it -- reintroducing the permanent wedge this change
             * exists to remove, just by a different route. */
            TRACE_E("Failed to allocate OTA url buffer");
            __ota_task_alive = false;
        }
    }
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/
static void ezlopi_ota_process(void *pv)
{
    __ota_in_process = EZLOPI_OTA_STATE_STARTED;
    char *url = (char *)pv;

    TRACE_S("Starting OTA ");
#ifdef CONFIG_FIRMWARE_UPGRADE_BIND_IF
    esp_netif_t *netif = get_example_netif_from_desc(bind_interface_name);
    if (netif == NULL)
    {
        TRACE_E("Can't find netif from interface description");
        abort();
    }
    struct ifreq ifr;
    esp_netif_get_netif_impl_name(netif, ifr.ifr_name);
    TRACE_S("Bind interface name is %s", ifr.ifr_name);
#endif

    /* ca_mios.h is now included at the top of this file, with the other
     * includes, rather than here in the middle of a function body. */
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .cert_pem = ca_mios, // ezlopi_factory_info_v3_get_ssl_shared_key(),
                             // .buffer_size = 1024,
                             // .buffer_size_tx = 1024,
    // .client_cert_pem = ezlopi_factory_info_v3_get_ssl_shared_key(),
    // .client_key_pem = ezlopi_factory_info_v3_get_ssl_private_key(),

#ifdef CONFIG_FIRMWARE_UPGRADE_BIND_IF
        .if_name = &ifr,
#endif
    };

#ifdef CONFIG_FIRMWARE_UPGRADE_URL_FROM_STDIN
    char url_buf[OTA_URL_SIZE];
    if (strncmp(config.url, "FROM_STDIN", ((strlen(config.url) + 1) > 11 ? (strlen(config.url) + 1) : 11)) == 0)
    {
        example_configure_stdin_stdout();
        fgets(url_buf, OTA_URL_SIZE, stdin);
        int len = strlen(url_buf);
        url_buf[len - 1] = '\0';
        config.url = url_buf;
    }
    else
    {
        TRACE_E("Configuration mismatch: wrong firmware upgrade image url");
        abort();
    }
#endif

    __byte_count = 0;

    esp_err_t ret = esp_https_ota(&config);
    if (ret == ESP_OK)
    {
        __ota_in_process = EZLOPI_OTA_STATE_FINISH;
        TRACE_W("Firmware Upgrade Successful, restarting !");
        EZPI_core_reset_reboot();
    }
    else
    {
        __ota_in_process = EZLOPI_OTA_STATE_FAILED;
        TRACE_E("Firmware upgrade failed");
    }

    /* Freed here, before vTaskDelete, and no longer referenced by config.url
     * now that esp_https_ota() has returned. The free used to sit *after*
     * vTaskDelete(NULL), which never returns, so it was unreachable and every
     * OTA attempt leaked the URL buffer. */
    if (url)
    {
        ezlopi_free(__FUNCTION__, url);
        url = NULL;
        config.url = NULL;
    }

    /* Released on every path that reaches here. The success path above reboots
     * and never gets this far; the failure path does, and this is what makes a
     * later OTA attempt possible. __ota_in_process is deliberately left at
     * EZLOPI_OTA_STATE_FAILED so the state reported to scenes and to the cloud
     * is exactly what it was before this change. */
    __ota_task_alive = false;

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
    EZPI_core_process_set_is_deleted(ENUM_EZLOPI_CORE_OTA_PROCESS_TASK);
#endif
    vTaskDelete(NULL);
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
    {
        TRACE_E("HTTP_EVENT_ERROR");
        TRACE_E("HTTP_EVENT_ERROR, error code: %d", esp_http_client_get_errno(evt));
        break;
    }
    case HTTP_EVENT_ON_CONNECTED:
    {
        TRACE_D("HTTP_EVENT_ON_CONNECTED");
        break;
    }
    case HTTP_EVENT_HEADER_SENT:
    {
        TRACE_D("HTTP_EVENT_HEADER_SENT");
        break;
    }
    case HTTP_EVENT_ON_HEADER:
    {
        __ota_in_process = EZLOPI_OTA_STATE_STARTED;
        TRACE_D("HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    }
    case HTTP_EVENT_ON_DATA:
    {
        __ota_in_process = EZLOPI_OTA_STATE_UPDATING;

        TRACE_D("HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        __byte_count += evt->data_len;
        TRACE_D("Received Bytes: %d", __byte_count);
        break;
    }
    case HTTP_EVENT_ON_FINISH:
    {
        TRACE_D("HTTP_EVENT_ON_FINISH");
        break;
    }
    case HTTP_EVENT_DISCONNECTED:
    {
        TRACE_D("HTTP_EVENT_DISCONNECTED");
        break;
    }
    }
    return ESP_OK;
}
/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

#endif // CONFIG_EZPI_ENABLE_OTA

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
