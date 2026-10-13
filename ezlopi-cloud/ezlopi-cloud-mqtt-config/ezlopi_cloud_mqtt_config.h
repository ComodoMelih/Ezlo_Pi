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
 * @file    ezlopi_cloud_mqtt_config.h
 * @brief   API methods for pointing this controller at an MQTT broker.
 * @author
 * @version 1.0
 * @date    12th OCT 2025
 *
 * WHY THESE METHODS EXIST
 *
 * ezlopi-core-mqtt-config moved the MQTT settings out of Kconfig and into NVS,
 * but it only gave them a *reader*. Nothing in the tree called any of its
 * setters, so the linker dropped all five of them and the firmware could read
 * a broker URI out of NVS that there was no way to put there. "Configuration
 * without recompiling" is not delivered by a settings store; it is delivered
 * by something a user can actually reach. This is that something.
 *
 * THE METHODS
 *
 *   hub.mqtt.config.get     report the current settings
 *   hub.mqtt.config.set     change any subset of them
 *   hub.mqtt.config.reset   forget them all, reverting to the compiled-in ones
 *
 * One set method taking a subset, rather than one method per field. A
 * configuration screen fills in several fields and presses save once, and five
 * round trips to do that would make a half-applied configuration the normal
 * case rather than the exceptional one.
 *
 * WHAT GUARDS THESE
 *
 * On the local websocket server, the transport does it: __message_upcall() in
 * ezlopi_service_ws_server.c refuses every method except
 * "hub.offline.login.ui" until EZPI_core_offline_is_user_logged_in() is true.
 * So these methods are behind the local login added alongside this work, and
 * they do not need, and deliberately do not have, a second check of their own
 * -- a duplicate gate is a gate that can disagree with the real one.
 *
 * On the cloud websocket client the connection is authenticated by the cloud
 * before any method is dispatched.
 *
 * THE PASSWORD IS NOT READABLE
 *
 * hub.mqtt.config.get reports whether a password is set, as a boolean, and
 * never its value. There is no method that returns it. A getter that echoed it
 * back would put the broker password into every UI's logs, into the response
 * history of whatever tool was used, and onto the wire on every settings-screen
 * load rather than only when it is changed.
 *
 * CHANGES APPLY ON THE NEXT BOOT
 *
 * esp-mqtt fixes the broker URI and credentials when the client is created, so
 * the running client keeps whatever it started with. Both set and reset
 * therefore report "rebootRequired": true in their result. Returning a bare
 * success would let a UI tell the user their broker had changed while the
 * device was still publishing to the old one.
 */

#ifndef _EZLOPI_CLOUD_MQTT_CONFIG_H_
#define _EZLOPI_CLOUD_MQTT_CONFIG_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
/* sdkconfig.h rather than EZLOPI_USER_CONFIG.h, matching ezlopi_cloud_ota.h:
** this header is included unconditionally by the API method registry, so the
** guard below has to be evaluable without the including file having arranged
** for the CONFIG_* macros first. */
#include "sdkconfig.h"

#ifdef CONFIG_EZPI_ENABLE_MQTT

#include "cjext.h"

/*******************************************************************************
 *                          C++ Declaration Wrapper
 *******************************************************************************/
#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief "hub.mqtt.config.get" -- report the settings now in force.
     *
     * Result fields: uri, username, topicPrefix, discoveryPrefix and
     * passwordSet. The values are the *resolved* ones, so a device with
     * nothing stored reports its compiled-in defaults rather than blanks,
     * which is what a user needs to see to know what the device is doing.
     *
     * @param cj_request  the incoming request
     * @param cj_response the response being built
     */
    void EZPI_CLOUD_mqtt_config_get(cJSON *cj_request, cJSON *cj_response);

    /**
     * @brief "hub.mqtt.config.set" -- change any subset of the settings.
     *
     * Every field is optional; an absent field is left alone. An empty string
     * clears uri, username or password, which is how MQTT is switched off
     * again without reflashing. The two prefixes cannot be cleared this way
     * because there is no such thing as no prefix; use reset.
     *
     * Every supplied field is validated before any of them is written, so a
     * request with one bad field changes nothing at all. See the note in the
     * implementation on why that is "nothing is written", not "the write is
     * atomic".
     *
     * @param cj_request  the incoming request
     * @param cj_response the response being built
     */
    void EZPI_CLOUD_mqtt_config_set(cJSON *cj_request, cJSON *cj_response);

    /**
     * @brief "hub.mqtt.config.reset" -- discard the stored settings.
     *
     * After this the device behaves exactly as a freshly flashed one: the
     * compiled-in Kconfig values apply, which on a stock image means an empty
     * URI and therefore an idle MQTT service.
     *
     * @param cj_request  the incoming request
     * @param cj_response the response being built
     */
    void EZPI_CLOUD_mqtt_config_reset(cJSON *cj_request, cJSON *cj_response);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_EZPI_ENABLE_MQTT

#endif // _EZLOPI_CLOUD_MQTT_CONFIG_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
