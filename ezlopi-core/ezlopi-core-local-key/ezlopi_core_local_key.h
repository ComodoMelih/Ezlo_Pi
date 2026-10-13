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
 * @file    ezlopi_core_local_key.h
 * @brief   Where the local API key comes from on a device nobody provisioned
 * @author
 * @version 1.0
 * @date    12th OCT 2025
 *
 * THE PROBLEM THIS SOLVES
 *
 * The offline websocket API accepts exactly one credential: the local key. An
 * unauthenticated client may call `hub.offline.login.ui` and nothing else
 * (ezlopi_service_ws_server.c:292), and that method compares the supplied
 * token against the key.
 *
 * Before this component, the key had exactly one source --
 * EZPI_core_factory_info_v3_get_local_key(), which reads the factory-info
 * partition, written only by Ezlo cloud provisioning over BLE
 * (ezlopi_core_ble_provisioning.c:418 -> ezlopi_core_factory_info.c:665).
 *
 * On a device somebody built and flashed themselves, that region has never
 * been written. It reads back as erased flash, 0xFF, which is not printable,
 * so get_local_key() returns NULL and the login fails. The consequence is the
 * one that matters for an open-source product: *a self-flashed EzloPi cannot
 * log in to its own local API*. Every one of the 117 local methods is
 * implemented, reachable and working, and none of them can be called.
 *
 * WHAT THIS DOES ABOUT IT
 *
 * Three sources, in a fixed order of precedence:
 *
 *   1. The factory-info partition. A cloud-provisioned device behaves exactly
 *      as it did before this component existed -- same key, same bytes, same
 *      login. This ordering is the compatibility guarantee, and it is first
 *      for that reason and no other.
 *
 *   2. An NVS entry, if an earlier boot wrote one.
 *
 *   3. Otherwise: generate 32 characters from esp_fill_random(), store them in
 *      NVS, and use them from then on.
 *
 * So a provisioned device is untouched and a self-flashed one works out of the
 * box. Nothing has to be compiled in, typed at build time, or configured
 * before first boot.
 *
 * ON PRINTING IT TO THE SERIAL LOG
 *
 * A generated key is useless if nobody can find out what it is, so
 * EZPI_core_local_key_init() prints it, once per boot, behind
 * CONFIG_EZPI_LOCAL_KEY_ANNOUNCE.
 *
 * That is a deliberate exposure and it replaces an accidental one. The code
 * this changes already printed the key on every login attempt, via
 * `TRACE_D("password: %s", password_saved)` -- and contrary to how it looks,
 * that line was *not* silenced by the log level: the trace system is EzloPi's
 * own, not ESP_LOG, so CONFIG_LOG_DEFAULT_LEVEL does not gate it, and
 * serial_log_severity defaults to ENUM_EZLOPI_TRACE_SEVERITY_MAX
 * (ezlopi_core_log.c:86), which passes severity 4. It printed in the shipped
 * build.
 *
 * The difference between the two exposures:
 *
 *   - Only a *locally generated* key is ever announced. A cloud-provisioned
 *     key is never printed, because the Ezlo app already has it, so printing
 *     it would be a leak with no usability benefit whatsoever.
 *   - It happens once at startup, not on every login attempt, so it is not in
 *     the log that gets pasted into a bug report after a failure.
 *   - It can be turned off in a build, which is what anyone shipping a product
 *     on this firmware should do.
 *
 * The residual risk is that anybody with the serial port can read the key. On
 * this hardware that threshold is already total: serial access means the
 * ability to reflash the device. It is not a new exposure, and it is not the
 * same as the key reaching a cloud log.
 */

#ifndef _EZLOPI_CORE_LOCAL_KEY_H_
#define _EZLOPI_CORE_LOCAL_KEY_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>

#include "ezlopi_core_errors.h"

/*******************************************************************************
 *                          C++ Declaration Wrapper
 *******************************************************************************/
#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Declarations
     *******************************************************************************/

    /**
     * @brief Where the key that is currently in use came from.
     *
     * Reported rather than inferred, because the three cases need different
     * handling: a factory key must never be printed, a generated one must be,
     * and "none" is a device whose local API cannot be used at all.
     */
    typedef enum e_ezpi_local_key_source
    {
        E_EZPI_LOCAL_KEY_SRC_NONE = 0,  /*!< No usable key from any source */
        E_EZPI_LOCAL_KEY_SRC_FACTORY,   /*!< Cloud-provisioned, from factory-info */
        E_EZPI_LOCAL_KEY_SRC_NVS,       /*!< Generated on an earlier boot */
        E_EZPI_LOCAL_KEY_SRC_GENERATED, /*!< Generated on this boot and stored */
    } e_ezpi_local_key_source_t;

    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief Make sure the device has a local key, and say what it is.
     *
     * Call once at startup, before the local websocket server begins
     * accepting connections. Resolves the key through the three sources in
     * order, generating and persisting one if there is none, and announces it
     * if it was locally generated and CONFIG_EZPI_LOCAL_KEY_ANNOUNCE is set.
     *
     * Safe to call more than once; subsequent calls re-resolve but do not
     * announce again.
     *
     * @return EZPI_SUCCESS if the device ends up with a usable key
     * @return EZPI_FAILED if there was none and one could not be made
     */
    ezlopi_error_t EZPI_core_local_key_init(void);

    /**
     * @brief Fetch the local key in use.
     *
     * @param source where the returned key came from, may be NULL if the
     *               caller does not care. Set to E_EZPI_LOCAL_KEY_SRC_NONE
     *               when the return value is NULL.
     * @return a NUL-terminated key the caller must release with
     *         EZPI_core_local_key_free(), or NULL if the device has none
     */
    char *EZPI_core_local_key_get(e_ezpi_local_key_source_t *source);

    /**
     * @brief Release a key returned by EZPI_core_local_key_get().
     *
     * Overwrites the characters before freeing, so a heap dump taken later
     * does not hand out the credential for nothing. This is hygiene rather
     * than a defence -- the key is also in NVS and in flash -- but it costs
     * one memset.
     *
     * @param key pointer from EZPI_core_local_key_get(), NULL is accepted
     */
    void EZPI_core_local_key_free(char *key);

    /**
     * @brief Set the local key explicitly, overriding any generated one.
     *
     * For a user who wants to choose the credential rather than read a
     * generated one off the serial log. Writes to NVS, so it does not and
     * cannot overwrite a cloud-provisioned factory key -- on a provisioned
     * device the factory key keeps winning and this has no effect on login.
     *
     * @param key NUL-terminated key, must satisfy EZPI_util_localkey_is_valid()
     * @return EZPI_SUCCESS on success
     * @return EZPI_ERR_WRONG_PARAM if the key is unusable
     * @return EZPI_ERR_NVS_WRITE_FAILED if it could not be stored
     */
    ezlopi_error_t EZPI_core_local_key_set(const char *key);

    /**
     * @brief Forget the NVS-stored key.
     *
     * The next EZPI_core_local_key_init() will generate a fresh one, unless
     * the device has a factory key, in which case this changes nothing that
     * was in use anyway.
     *
     * @return EZPI_SUCCESS once no NVS key remains
     */
    ezlopi_error_t EZPI_core_local_key_clear(void);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_CORE_LOCAL_KEY_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
