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
 * @file    ezlopi_service_improv_serial.c
 * @brief   Improv Wi-Fi provisioning over UART0.
 *
 * See ezlopi_service_improv_serial.h for why this is a filter the UART reader
 * calls rather than another case in the EzloPi serial parser, and
 * ezlopi_util_improv.h for where the wire format was established from.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

/* Not "../../build/config/sdkconfig.h" -- see ezlopi_service_improv.c. */
#include "sdkconfig.h"

#if defined(CONFIG_EZPI_ENABLE_IMPROV_SERIAL)

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"

#include "ezlopi_util_trace.h"
#include "EZLOPI_USER_CONFIG.h"

#include "ezlopi_util_improv.h"
#include "ezlopi_service_improv.h"
#include "ezlopi_service_improv_serial.h"

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static void ezpi_improv_serial_publish_state(uint8_t state);
static void ezpi_improv_serial_publish_error(uint8_t error);
static void ezpi_improv_serial_publish_result(const uint8_t *payload, uint16_t len);
static void ezpi_improv_serial_emit(e_ezpi_improv_type_t type,
                                    const uint8_t *data, uint16_t data_len);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static s_ezpi_improv_rx_t sg_rx;

/**
 * @brief Transmit buffer, shared under sg_tx_lock.
 *
 * One buffer rather than one per call site because a frame is at most 266
 * bytes and three of the four callers are not on a task of their own; a
 * stack copy that size on the esp_timer task would be unwelcome.
 */
static uint8_t sg_tx_frame[EZPI_IMPROV_FRAME_BUF_MAX];

/**
 * @brief Serialises access to sg_tx_frame and to the write itself.
 *
 * Needed, not defensive: the frame parser runs on the UART task, while state
 * and error changes are published from the Wi-Fi event task and from the
 * esp_timer task. Without this, two of those can be inside
 * EZPI_improv_frame_build() on the same buffer at once and emit one frame with
 * the other's payload and a checksum belonging to neither.
 */
static SemaphoreHandle_t sg_tx_lock = NULL;

static const s_ezpi_improv_transport_t sg_serial_transport = {
    .name = "serial",
    .publish_state = ezpi_improv_serial_publish_state,
    .publish_error = ezpi_improv_serial_publish_error,
    .publish_result = ezpi_improv_serial_publish_result,
};

/*******************************************************************************
 *                          Public Function Definitions
 *******************************************************************************/

void EZPI_SERV_improv_serial_init(void)
{
    EZPI_improv_rx_reset(&sg_rx);

    if (NULL == sg_tx_lock)
    {
        sg_tx_lock = xSemaphoreCreateMutex();

        if (NULL == sg_tx_lock)
        {
            /* Refusing to register is the safe failure. Emitting frames without
             * the lock would interleave them, and a client that receives a
             * frame whose payload belongs to another frame has no way to tell
             * -- the checksum would be wrong, so it would look like line noise
             * and provisioning would fail for no discoverable reason. */
            TRACE_E("improv-serial: no mutex, transport not registered");
            return;
        }
    }

    if (EZPI_SERV_improv_transport_register(&sg_serial_transport))
    {
        /* Logged at info because this is the one line that tells whoever is
         * watching the console that the port now carries two protocols. */
        TRACE_I("improv-serial: listening on UART%d", (int)EZPI_SERV_UART_NUM_DEFAULT);
    }
}

uint32_t EZPI_SERV_improv_serial_filter(const uint8_t *in, uint32_t len,
                                        uint8_t *out, uint32_t out_cap)
{
    uint32_t written = 0;

    if ((NULL == in) || (NULL == out))
    {
        return 0;
    }

    for (uint32_t i = 0; i < len; i++)
    {
        const uint8_t *pass = NULL;
        uint16_t pass_len = 0;

        const e_ezpi_improv_rx_result_t res =
            EZPI_improv_rx_feed(&sg_rx, in[i], &pass, &pass_len);

        switch (res)
        {
        case EZPI_IMPROV_RX_NOT_IMPROV:
        {
            /* Writing back into the caller's own buffer is supported. It is
             * safe because the parser only ever hands back bytes it has
             * already consumed, so `written` can never exceed `i + 1` and
             * every byte written lands at an index that has already been
             * read. */
            for (uint16_t j = 0; j < pass_len; j++)
            {
                if (written < out_cap)
                {
                    out[written] = pass[j];
                    written++;
                }
            }
            break;
        }
        case EZPI_IMPROV_RX_FRAME:
        {
            uint16_t payload_len = 0;
            const uint8_t *payload = EZPI_improv_rx_payload(&sg_rx, &payload_len);

            if (EZPI_IMPROV_TYPE_RPC == EZPI_improv_rx_type(&sg_rx))
            {
                /* false: this transport's frame checksum already covers the
                 * payload, so the payload does not carry a second one. That
                 * distinction is the whole reason the parser takes a flag --
                 * see ezlopi_util_improv.h. */
                EZPI_SERV_improv_handle_rpc(&sg_serial_transport, payload,
                                            payload_len, false);
            }
            else
            {
                /* A well-formed frame of a type only a device sends. The
                 * client is echoing us, or something else on the line speaks
                 * Improv. Dropped silently: answering an error to a frame we
                 * may well have sent ourselves would be worse than ignoring
                 * it. */
                TRACE_D("improv-serial: ignoring frame of type %d",
                        (int)EZPI_improv_rx_type(&sg_rx));
            }
            break;
        }
        case EZPI_IMPROV_RX_BAD_CHECKSUM:
        {
            /* Reported rather than dropped, because the client is waiting for
             * an answer and a corrupted frame is exactly the case where it
             * would otherwise sit out its timeout. The parser has already
             * cleared itself, so the stream stays in sync. */
            TRACE_W("improv-serial: frame failed its checksum");
            EZPI_SERV_improv_report_error(EZPI_IMPROV_ERROR_INVALID_RPC);
            break;
        }
        case EZPI_IMPROV_RX_NEED_MORE:
        default:
        {
            /* Mid-frame. Nothing is passed through: the bytes so far may yet
             * turn out to be a frame, and handing them to the EzloPi parser as
             * well would double-deliver them. */
            break;
        }
        }
    }

    return written;
}

void EZPI_SERV_improv_serial_idle(void)
{
    EZPI_improv_rx_reset(&sg_rx);
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

static void ezpi_improv_serial_publish_state(uint8_t state)
{
    const uint8_t data = state;
    ezpi_improv_serial_emit(EZPI_IMPROV_TYPE_CURRENT_STATE, &data, 1);
}

static void ezpi_improv_serial_publish_error(uint8_t error)
{
    const uint8_t data = error;
    ezpi_improv_serial_emit(EZPI_IMPROV_TYPE_ERROR_STATE, &data, 1);
}

static void ezpi_improv_serial_publish_result(const uint8_t *payload, uint16_t len)
{
    ezpi_improv_serial_emit(EZPI_IMPROV_TYPE_RPC_RESULT, payload, len);
}

/**
 * @brief Build one frame and put it on the wire.
 *
 * Deliberately a single uart_write_bytes() for the whole frame. The reference
 * implementation uses three -- header, payload, then checksum and newline --
 * which is fine when the port carries nothing else. Here the same port is the
 * IDF console and the EzloPi serial protocol, so another task can write
 * between those calls and split the frame around a log line. A client would
 * see a bad checksum and give up.
 */
static void ezpi_improv_serial_emit(e_ezpi_improv_type_t type,
                                    const uint8_t *data, uint16_t data_len)
{
    if (NULL == sg_tx_lock)
    {
        return;
    }

    if (pdTRUE != xSemaphoreTake(sg_tx_lock, portMAX_DELAY))
    {
        return;
    }

    const uint16_t frame_len = EZPI_improv_frame_build(sg_tx_frame, sizeof(sg_tx_frame),
                                                       type, data, data_len);

    if (0 != frame_len)
    {
        (void)uart_write_bytes(EZPI_SERV_UART_NUM_DEFAULT, (const char *)sg_tx_frame,
                              frame_len);
    }
    else
    {
        /* Only reachable if a payload is longer than the single length byte can
         * describe, which the core's buffer sizing is chosen to prevent. Logged
         * rather than escalated to an error frame, because building that frame
         * would re-enter this function while the lock is held. */
        TRACE_E("improv-serial: payload of %d bytes does not fit a frame",
                (int)data_len);
    }

    xSemaphoreGive(sg_tx_lock);
}

#endif // CONFIG_EZPI_ENABLE_IMPROV_SERIAL

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
