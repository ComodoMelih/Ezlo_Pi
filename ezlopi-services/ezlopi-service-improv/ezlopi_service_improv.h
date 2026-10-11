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
 * @file    ezlopi_service_improv.h
 * @brief   Transport-independent half of Improv Wi-Fi provisioning.
 *
 * Improv runs over two transports in this firmware -- BLE (a sixth GATT
 * service) and Serial (framed bytes on UART0) -- and almost everything about it
 * is the same on both: whether the controller may be provisioned at all, the
 * state machine, applying credentials, deciding when an attempt has failed, and
 * what the RPC answers contain. Only *how* a state byte reaches the client
 * differs.
 *
 * So that logic lives here once and the transports register themselves against
 * it. The alternative was a second copy of it in the serial path, and
 * tests/host/Makefile already explains why that is the wrong answer: "a copy
 * drifts". Two copies of a provisioning gate is a worse kind of drift than
 * most -- the whole security argument for shipping Improv at all is that it
 * refuses to re-provision a claimed controller, and that argument is only as
 * good as its least careful copy.
 *
 * Two properties of this design are deliberate:
 *
 *  1. **State is per device, not per transport.** There is one Wi-Fi radio and
 *     one provisioning attempt, so a state change is broadcast to every
 *     registered transport. A client watching over BLE while someone
 *     provisions over the cable sees the truth rather than a stale
 *     AUTHORIZED.
 *
 *  2. **An RPC answer goes only to whoever asked.** State and error are
 *     broadcast; an RPC result is addressed, because it answers a question that
 *     one specific client asked.
 *
 * The wire format itself is not here -- it is in ezlopi-util-improv, which
 * depends on nothing and is therefore tested on the host.
 */

#ifndef __EZLOPI_SERVICE_IMPROV_H__
#define __EZLOPI_SERVICE_IMPROV_H__

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdint.h>
#include <stdbool.h>

#include "ezlopi_util_improv.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Definitions
     *******************************************************************************/

    /**
     * @brief How long to wait for an IP before reporting the attempt failed
     *
     * The window has to cover association, the four-way handshake and DHCP, and
     * it has to outlast the retry loop in ezlopi-core-wifi, which calls
     * esp_wifi_connect() again on every disconnect up to
     * EXAMPLE_ESP_MAXIMUM_RETRY (5). Thirty seconds is comfortably past that on
     * a network that works and short enough that a client is not left guessing
     * on one that does not.
     */
#define EZPI_IMPROV_PROVISION_TIMEOUT_US (30 * 1000 * 1000)

    /**
     * @brief One Improv transport.
     *
     * Every function pointer may be NULL; a transport that cannot deliver a
     * particular thing simply does not. BLE, for instance, delivers nothing
     * until the client has subscribed to the relevant descriptor.
     *
     * @note The callbacks are invoked from whichever task produced the change,
     *       which is the Wi-Fi event task or the esp_timer task for state and
     *       error, and the transport's own task for an RPC result. A transport
     *       must therefore not assume it is on its own task inside
     *       publish_state() or publish_error().
     */
    typedef struct s_ezpi_improv_transport
    {
        /** @brief For the log, so a message says which transport it came from. */
        const char *name;

        /** @brief Deliver a new provisioning state. */
        void (*publish_state)(uint8_t state);

        /** @brief Deliver a new error code. */
        void (*publish_error)(uint8_t error);

        /**
         * @brief Deliver an RPC result payload to the client that asked.
         *
         * @param payload Result bytes, in the form produced by
         *                EZPI_improv_result_finish().
         * @param len     How many.
         */
        void (*publish_result)(const uint8_t *payload, uint16_t len);
    } s_ezpi_improv_transport_t;

    /*******************************************************************************
     *                          Function Prototypes
     *******************************************************************************/

    /**
     * @brief Register a transport and start the core if it is not already up.
     *
     * Safe to call from more than one transport, and safe to call twice with
     * the same pointer -- the second call is ignored rather than consuming a
     * slot.
     *
     * @param transport Must remain valid for the lifetime of the firmware;
     *                  the pointer is stored, not the contents.
     * @return true if the transport is registered.
     */
    bool EZPI_SERV_improv_transport_register(const s_ezpi_improv_transport_t *transport);

    /**
     * @brief Current provisioning state, one of e_ezpi_improv_state_t.
     */
    uint8_t EZPI_SERV_improv_state(void);

    /**
     * @brief Most recent error code, one of e_ezpi_improv_error_t.
     */
    uint8_t EZPI_SERV_improv_error(void);

    /**
     * @brief Capability bits for the capabilities characteristic.
     *
     * Always EZPI_IMPROV_CAP_NONE. Kept as a call rather than a constant so
     * that adding a capability later is a change in one place -- but note that
     * "identify" is not a capability this firmware can ever gain over Serial,
     * because the protocol has no identify command. See ezlopi_util_improv.h.
     */
    uint8_t EZPI_SERV_improv_capabilities(void);

    /**
     * @brief Handle one decoded RPC payload.
     *
     * Dispatches the command, applies credentials if that is what it is, and
     * answers through @p origin. Errors are reported through every registered
     * transport, because an error is a change to the device's reported error
     * state and not only a reply.
     *
     * @param origin       Transport the RPC arrived on; results go back here.
     * @param payload      RPC payload: [command][data length][data ...] and,
     *                     if @p has_checksum, a trailing checksum byte.
     * @param len          Length of @p payload.
     * @param has_checksum Whether the payload carries its own checksum. True
     *                     for BLE, where there is no enclosing frame to carry
     *                     one; false for Serial, where the frame checksum
     *                     already covers the payload.
     */
    void EZPI_SERV_improv_handle_rpc(const s_ezpi_improv_transport_t *origin,
                                     const uint8_t *payload, uint32_t len,
                                     bool has_checksum);

    /**
     * @brief Report an error that the transport itself detected.
     *
     * Used for a frame that failed its checksum, which the core never sees.
     *
     * @param error One of e_ezpi_improv_error_t.
     */
    void EZPI_SERV_improv_report_error(uint8_t error);

    /**
     * @brief Whether an Ezlo account already owns this controller.
     *
     * Exposed because a transport may want to decline to start at all. The
     * answer is read fresh from NVS on every call rather than cached: the
     * controller can be claimed through the Ezlo app while an Improv client is
     * connected, and the decision has to reflect the state at the moment the
     * credentials arrive.
     */
    bool EZPI_SERV_improv_is_claimed(void);

#ifdef __cplusplus
}
#endif

#endif // __EZLOPI_SERVICE_IMPROV_H__

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
