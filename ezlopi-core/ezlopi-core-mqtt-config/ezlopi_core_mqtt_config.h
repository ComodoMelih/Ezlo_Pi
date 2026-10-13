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
 * @file    ezlopi_core_mqtt_config.h
 * @brief   Where the MQTT service's settings come from.
 * @author
 * @version 1.0
 * @date    12th OCT 2025
 *
 * THE PROBLEM THIS SOLVES
 *
 * The MQTT service read CONFIG_EZPI_MQTT_BROKER_URI and friends directly --
 * Kconfig macros, baked into the image. Pointing an EzloPi at a broker meant
 * installing ESP-IDF, running menuconfig, rebuilding and reflashing. For a
 * feature whose entire purpose is to let someone use their devices in Home
 * Assistant without a cloud account, requiring them to become a firmware
 * developer first is not a configuration story.
 *
 * So the settings now live in NVS and are written at runtime. Kconfig remains,
 * as the fallback: a build can still ship with a broker pre-set, which is what
 * someone flashing a fleet wants, and the committed defaults leave the URI
 * empty so a stock image simply has MQTT idle until somebody configures it.
 *
 * PRECEDENCE, AND WHY THIS WAY ROUND
 *
 *   1. A valid value in NVS.
 *   2. Otherwise the CONFIG_EZPI_MQTT_* value compiled in.
 *
 * NVS first, because that is the one the user set most recently and by hand.
 * The alternative -- Kconfig wins -- would mean a device could not be
 * reconfigured without a rebuild, which is the problem being fixed.
 *
 * Note the word *valid*. Every field read out of NVS is put back through
 * ezlopi-util-mqttcfg before it is used, not merely when it is written.
 * Validating only on write assumes the only way a value gets into NVS is
 * through this component's setter, and that assumption has a short shelf life:
 * a partial write, a truncated entry, an older firmware's key, or a future
 * provisioning path would all produce a stored value nobody checked. An
 * invalid stored value is treated as absent and the Kconfig value is used,
 * which degrades to the old behaviour instead of to a broken client.
 *
 * ABOUT THE PASSWORD
 *
 * It is stored in NVS as plain text. NVS is not encrypted unless
 * CONFIG_NVS_ENCRYPTION is on, which these builds do not turn on, so anyone
 * who can read the flash can read the password.
 *
 * This is not a regression -- it is strictly better than where it was. As a
 * Kconfig string the password was compiled into the firmware image, so it was
 * readable by anyone who had the *image*, including anyone the image was
 * distributed to. In NVS it is at least confined to the one device the person
 * configuring it owns. Neither is good enough to put an administrative broker
 * account into; the Kconfig help says so and so does the service README.
 *
 * No function in this component logs a password, and none returns one through
 * a getter meant for display. That is the same rule applied to the local key
 * and the Wi-Fi password elsewhere in this tree.
 *
 * APPLYING A CHANGE
 *
 * Writing a setting does not reconnect the client. The MQTT service reads this
 * once, at startup, because esp-mqtt's client is configured when it is
 * created. A change therefore takes effect on the next boot. That is a real
 * limitation and is tracked separately; it is called out here so that nobody
 * reads a successful set() as meaning the broker connection moved.
 */

#ifndef _EZLOPI_CORE_MQTT_CONFIG_H_
#define _EZLOPI_CORE_MQTT_CONFIG_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>

#include "ezlopi_core_errors.h"
#include "ezlopi_util_mqttcfg.h"

/*******************************************************************************
 *                          C++ Declaration Wrapper
 *******************************************************************************/
#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Definitions
     *******************************************************************************/

    /**
     * @brief A complete, already-validated set of MQTT settings.
     *
     * Fixed buffers rather than pointers, and therefore no ownership and
     * nothing to free. The whole struct is about 390 bytes, which is cheap
     * next to getting a free() wrong on a path that runs at boot -- and the
     * MQTT service keeps exactly one of these at file scope for the lifetime
     * of the process, so it is allocated once either way.
     *
     * A struct returned by EZPI_core_mqtt_config_get() always holds valid
     * values or, for uri/username/password, an empty string. topic_prefix and
     * discovery_prefix are never empty.
     */
    typedef struct s_ezpi_mqtt_config
    {
        /** @brief Broker URI, or empty if none is configured. Empty means the
         **        service does not start; it is how MQTT stays idle on a
         **        stock image. */
        char uri[EZPI_MQTTCFG_URI_MAX + 1u];

        /** @brief Broker username, or empty for an anonymous broker. */
        char username[EZPI_MQTTCFG_CRED_MAX + 1u];

        /** @brief Broker password. Only meaningful when username is set.
         **        Never logged, never printed. */
        char password[EZPI_MQTTCFG_CRED_MAX + 1u];

        /** @brief Root of this controller's own topics. Never empty. */
        char topic_prefix[EZPI_MQTTCFG_PREFIX_MAX + 1u];

        /** @brief Home Assistant's discovery prefix. Never empty. */
        char discovery_prefix[EZPI_MQTTCFG_PREFIX_MAX + 1u];
    } s_ezpi_mqtt_config_t;

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief Resolve the MQTT settings to use.
     *
     * Fills every field of *out, taking each one from NVS if a valid value is
     * stored there and from the compiled-in Kconfig value otherwise. Never
     * leaves a field uninitialised, so the caller does not have to
     * pre-populate or check individual fields.
     *
     * Reading is deliberately cheap enough to do at startup and nowhere else;
     * see the note on applying changes in the file comment.
     *
     * @param out destination, must be non-NULL
     * @return EZPI_SUCCESS when out has been filled
     * @return EZPI_ERR_WRONG_PARAM if out is NULL
     */
    ezlopi_error_t EZPI_core_mqtt_config_get(s_ezpi_mqtt_config_t *out);

    /**
     * @brief Store a broker URI, replacing whatever is there.
     *
     * An empty string is accepted and clears the stored URI, which is how
     * MQTT is turned off again without reflashing: with no URI the service
     * logs and does not start.
     *
     * @param uri NUL-terminated URI, or "" to clear
     * @return EZPI_SUCCESS on a successful write
     * @return EZPI_ERR_WRONG_PARAM if the URI is NULL or malformed
     * @return EZPI_ERR_NVS_WRITE_FAILED if the write did not stick
     */
    ezlopi_error_t EZPI_core_mqtt_config_set_uri(const char *uri);

    /**
     * @brief Store the broker username, or "" for an anonymous broker.
     *
     * @param username NUL-terminated username, or "" to clear
     * @return EZPI_SUCCESS, EZPI_ERR_WRONG_PARAM or EZPI_ERR_NVS_WRITE_FAILED
     */
    ezlopi_error_t EZPI_core_mqtt_config_set_username(const char *username);

    /**
     * @brief Store the broker password, or "" to clear it.
     *
     * The value is not logged, not echoed and not returned by any function
     * other than EZPI_core_mqtt_config_get(), which exists for the MQTT
     * client and not for display.
     *
     * @param password NUL-terminated password, or "" to clear
     * @return EZPI_SUCCESS, EZPI_ERR_WRONG_PARAM or EZPI_ERR_NVS_WRITE_FAILED
     */
    ezlopi_error_t EZPI_core_mqtt_config_set_password(const char *password);

    /**
     * @brief Store this controller's topic prefix.
     *
     * Refuses a prefix containing an MQTT wildcard; see
     * EZPI_util_mqttcfg_prefix_is_valid(), which explains why that
     * particular refusal is the important one. Unlike the URI, "" is not
     * accepted: there is no such thing as no prefix, and clearing it is done
     * with EZPI_core_mqtt_config_reset() instead.
     *
     * @param prefix NUL-terminated prefix
     * @return EZPI_SUCCESS, EZPI_ERR_WRONG_PARAM or EZPI_ERR_NVS_WRITE_FAILED
     */
    ezlopi_error_t EZPI_core_mqtt_config_set_topic_prefix(const char *prefix);

    /**
     * @brief Store the Home Assistant discovery prefix.
     *
     * Must match Home Assistant's own discovery_prefix setting. If it does
     * not, every discovery message goes to a topic nothing is subscribed to
     * and no entity ever appears, with nothing logged on either side.
     *
     * @param prefix NUL-terminated prefix
     * @return EZPI_SUCCESS, EZPI_ERR_WRONG_PARAM or EZPI_ERR_NVS_WRITE_FAILED
     */
    ezlopi_error_t EZPI_core_mqtt_config_set_discovery_prefix(const char *prefix);

    /**
     * @brief Forget every stored setting and fall back to the compiled-in ones.
     *
     * Used by the factory-reset path and by anyone who has configured a
     * broker they now want gone. After this, EZPI_core_mqtt_config_get()
     * returns exactly what a freshly flashed image would have returned.
     *
     * @return EZPI_SUCCESS once the entries have been removed
     */
    ezlopi_error_t EZPI_core_mqtt_config_reset(void);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_CORE_MQTT_CONFIG_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
