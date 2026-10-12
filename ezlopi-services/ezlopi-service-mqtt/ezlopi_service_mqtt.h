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
 * @file    ezlopi_service_mqtt.h
 * @brief   Publish this hub's items to an MQTT broker for Home Assistant.
 *
 * This is an addition, not a replacement. It changes nothing about how the
 * hub talks to the Ezlo cloud: it attaches to the same item-update broadcast
 * the cloud path already produces, and writes arrive through the same
 * hub.item.value.set handler the cloud uses. With CONFIG_EZPI_ENABLE_MQTT
 * off, none of this is compiled: an esp32 image built with the option off was
 * compared byte by byte against one built from the commit before this service
 * existed, and the only differences were the app descriptor's build metadata,
 * the image's own trailing checksum, and eleven bytes holding the __LINE__
 * values that TRACE_* embeds in ezlopi_main.c -- those moved purely because
 * the init call added sixteen lines above __blinky. No generated code differs.
 *
 * Three things are worth knowing before changing anything here.
 *
 * First, state updates are observed through
 * EZPI_core_broadcast_method_add(), which already exists for the websocket
 * transports. Nothing in the core had to be modified to add a third observer.
 * A registered method runs on the loop-service task while that task holds the
 * single global serialization buffer, so anything slow in this file delays the
 * Ezlo cloud broadcast behind it. Everything on that path uses
 * esp_mqtt_client_enqueue(), which hands the message to the MQTT client's own
 * task and returns without touching the network.
 *
 * Second, the client is started unconditionally and left to retry. It is
 * deliberately not gated on EZPI_core_wait_for_wifi_to_connect(): that waits
 * on a bit only the Wi-Fi driver ever sets, so on an Ethernet-only board it
 * never returns. esp-mqtt's own reconnect loop is indifferent to which
 * interface came up, which makes this the one gate that cannot be wrong.
 *
 * Third, the item -> entity mapping is not here. It lives in
 * ezlopi-util-ha, which has no ESP-IDF dependencies so that every item name,
 * value type and scale the drivers produce can be swept on a desktop. A wrong
 * mapping is invisible on the device -- Home Assistant simply drops the
 * discovery message -- so that separation is the only way it gets tested.
 */

#ifndef _EZLOPI_SERVICE_MQTT_H_
#define _EZLOPI_SERVICE_MQTT_H_

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
     * @brief Start the MQTT client and begin publishing this hub's items.
     *
     * Registers an observer on the item-update broadcast, then starts an
     * esp-mqtt client that retries until the broker is reachable. Safe to
     * call before any network interface is up, and safe to call when no
     * broker URI is configured -- in that case it logs and does nothing,
     * rather than retrying a URI that cannot work.
     *
     * Returns nothing because there is no useful failure for the caller to
     * act on: a broker that is not yet reachable is the normal state at boot
     * and is indistinguishable here from one that never will be.
     */
    void EZPI_service_mqtt_init(void);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_SERVICE_MQTT_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
