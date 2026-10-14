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
 * @file    ezlopi_cloud_devcfg.h
 * @brief   Reading and writing the device configuration over the API
 * @version 1.0
 * @date    13th OCT 2025
 *
 * WHY THIS EXISTS
 *
 * Before this, the only ways to tell this firmware which sensors exist and
 * which GPIOs they are on were the BLE dynamic-config characteristic and the
 * UART command channel. Both require physical proximity -- a phone within
 * Bluetooth range running the Ezlo app, or a cable -- and neither is reachable
 * from the websocket API that every other hub setting is changed through.
 *
 * So a hub on a shelf could be asked its firmware version, its rooms, its
 * scenes and its MQTT broker over the network, but not what devices it has been
 * configured with, and could not be reconfigured at all. "No-compile
 * configuration" is the feature this completes: the device list becomes a thing
 * you can read and replace over the same connection as everything else, which
 * is what a local configuration UI needs in order to exist.
 *
 * WHAT THESE TWO METHODS ARE CAREFUL ABOUT
 *
 * The write path is the dangerous one, and not for the obvious reason. Storing
 * a configuration that is wrong does not fail loudly; it fails on the next
 * boot, silently, as devices that simply are not there. The parser the stored
 * configuration will eventually meet, ezlopi_device_parse_json_v3(), skips an
 * entry whose id_item it does not recognise without logging anything, and
 * treats an absent device list as success. A user who sends a slightly wrong
 * configuration therefore gets a hub that comes back up, works, and has
 * forgotten a sensor -- with nothing linking that to the request they made
 * minutes earlier.
 *
 * hub.ezlopi.config.set therefore refuses before it writes. It applies
 * EZPI_util_devcfgval_check(), which is host-tested in its own right, and then
 * the one check that function deliberately leaves to its caller: whether each
 * id_item names a driver this particular image was built with. A configuration
 * that names a driver compiled out of this build is a configuration that will
 * come up short a device, and that is worth an error rather than a surprise.
 *
 * It is all-or-nothing by construction. The configuration is one field, so
 * there is no half-written state to reason about: either the whole document is
 * acceptable and replaces what was there, or nothing is touched.
 *
 * WHAT IT DOES NOT DO
 *
 * It does not reboot. The parser runs once, from EZPI_device_prepare() during
 * EZPI_init(), so a stored configuration takes effect on the next boot and not
 * before. The result says "rebootRequired": true rather than restarting the hub
 * out from under the caller, which is also what the BLE path effectively does
 * -- it sets a flag and lets the service decide.
 *
 * There is no .reset to match hub.mqtt.config.reset. Resetting this field means
 * a hub with no devices, which is a destructive operation already reachable
 * through the existing factory-reset paths, and adding a second unauthenticated
 * route to "forget everything" is not an improvement. Sending a configuration
 * with an empty dev_detail does the same thing deliberately.
 *
 * It does not validate GPIOs -- that they exist on this target, are not already
 * in use by the flash chip, or are not claimed twice. That needs the HAL and is
 * recorded under Known gaps in the changelog rather than approximated here.
 */

#ifndef _EZLOPI_CLOUD_DEVCFG_H_
#define _EZLOPI_CLOUD_DEVCFG_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "cjext.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Function Prototypes
     *******************************************************************************/

    /**
     * @brief   hub.ezlopi.config.get -- the stored device configuration.
     *
     * Adds to the response, under "result":
     *
     *   "config"           the stored configuration as an object, or null when
     *                      nothing is stored. An object rather than the raw
     *                      string, so a client does not have to parse twice.
     *   "configSize"       serialised length of what is stored, in bytes.
     *   "maxConfigSize"    the field size, so a UI can show the headroom rather
     *                      than discovering the limit by being refused.
     *   "deviceCount"      entries in "dev_detail", or 0.
     *   "expectedChipset"  the chipset name this build accepts, or null. A
     *                      client writing a configuration needs this, and
     *                      guessing it from the hub model is how a config for
     *                      the wrong chip gets sent.
     *
     * Reports no error for an unconfigured hub; "config": null is the answer.
     *
     * @param   cj_request  the request; params are ignored.
     * @param   cj_response the response to add to.
     */
    void EZPI_CLOUD_devcfg_get(cJSON *cj_request, cJSON *cj_response);

    /**
     * @brief   hub.ezlopi.config.set -- replace the device configuration.
     *
     * Expects params.config to be the complete configuration object. This
     * replaces the stored one; it is not a merge, because a merge of two device
     * lists has no meaning that a caller could predict.
     *
     * On success, "result" carries "rebootRequired": true and "deviceCount".
     *
     * On refusal, a JSON-RPC error whose "data" is the machine-readable token
     * for the reason, and -- for the reasons that are about one device entry --
     * a "badIndex" giving its position in "dev_detail", so a client can point
     * at the entry instead of making the user bisect a thirty-device list. The
     * tokens come from EZPI_util_devcfgval_token(), plus
     * "devcfg.id_item.unsupported" for an id_item this image has no driver for.
     *
     * "badIndex" is absent, not -1, when the reason is not about an entry.
     *
     * @param   cj_request  the request.
     * @param   cj_response the response to add to.
     */
    void EZPI_CLOUD_devcfg_set(cJSON *cj_request, cJSON *cj_response);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_CLOUD_DEVCFG_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
