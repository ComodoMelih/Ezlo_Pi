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
 * @file    ezlopi_core_mqtt_config.c
 * @brief   NVS-backed MQTT settings, falling back to Kconfig
 * @author
 * @version 1.0
 * @date    12th OCT 2025
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <string.h>

#include "EZLOPI_USER_CONFIG.h"
#include "ezlopi_core_mqtt_config.h"
#include "ezlopi_core_nvs.h"
#include "ezlopi_util_mqttcfg.h"
#include "ezlopi_util_trace.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/**
 * @brief NVS entry names.
 *
 * NVS caps a key name at 15 characters (NVS_KEY_NAME_MAX_SIZE is 16 including
 * the terminator). The longest here is 10, which leaves room should these ever
 * need a suffix.
 *
 * They are prefixed "mq_" rather than named "uri" or "password" because the
 * namespace is shared with every other setting this firmware stores -- Wi-Fi
 * credentials, the local key, scene scripts. A bare "password" would be an
 * invitation to collide with something.
 */
static const char *const mqtt_nvs_uri = "mq_uri";
static const char *const mqtt_nvs_user = "mq_user";
static const char *const mqtt_nvs_pass = "mq_pass";
static const char *const mqtt_nvs_tprefix = "mq_tprefix";
static const char *const mqtt_nvs_dprefix = "mq_dprefix";

/**
 * @brief The compiled-in fallbacks.
 *
 * Defined here rather than used inline so that this file compiles whether or
 * not CONFIG_EZPI_ENABLE_MQTT is set. The MQTT Kconfig strings all "depends on
 * EZPI_ENABLE_MQTT", so with the option off none of the CONFIG_EZPI_MQTT_*
 * macros exist at all, and a direct reference would break the build for every
 * configuration that has MQTT disabled -- which is all four committed
 * defaults. That failure would not show up in the MQTT CI job, because that
 * job is the one build where the macros do exist.
 */
#ifdef CONFIG_EZPI_MQTT_BROKER_URI
#define MQTT_DEFAULT_URI CONFIG_EZPI_MQTT_BROKER_URI
#else
#define MQTT_DEFAULT_URI ""
#endif

#ifdef CONFIG_EZPI_MQTT_USERNAME
#define MQTT_DEFAULT_USER CONFIG_EZPI_MQTT_USERNAME
#else
#define MQTT_DEFAULT_USER ""
#endif

#ifdef CONFIG_EZPI_MQTT_PASSWORD
#define MQTT_DEFAULT_PASS CONFIG_EZPI_MQTT_PASSWORD
#else
#define MQTT_DEFAULT_PASS ""
#endif

/* These two have non-empty Kconfig defaults ("ezlopi", "homeassistant") and
** must never resolve to empty, so the fallback-of-the-fallback repeats those
** literals rather than being "". A device with MQTT compiled out does not use
** them, but EZPI_core_mqtt_config_get() still promises a non-empty prefix and
** it should not be the one place that promise has an exception. */
#ifdef CONFIG_EZPI_MQTT_TOPIC_PREFIX
#define MQTT_DEFAULT_TPREFIX CONFIG_EZPI_MQTT_TOPIC_PREFIX
#else
#define MQTT_DEFAULT_TPREFIX "ezlopi"
#endif

#ifdef CONFIG_EZPI_MQTT_DISCOVERY_PREFIX
#define MQTT_DEFAULT_DPREFIX CONFIG_EZPI_MQTT_DISCOVERY_PREFIX
#else
#define MQTT_DEFAULT_DPREFIX "homeassistant"
#endif

/** @brief Which validator a field is checked with. */
typedef enum e_mqtt_field_kind
{
    E_MQTT_FIELD_URI = 0,
    E_MQTT_FIELD_PREFIX,
    E_MQTT_FIELD_CRED,
} e_mqtt_field_kind_t;

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static bool __field_is_valid(const char *value, e_mqtt_field_kind_t kind);
static void __resolve_field(char *dst, size_t dst_size, const char *nvs_name,
                            const char *fallback, e_mqtt_field_kind_t kind,
                            bool empty_is_allowed);
static ezlopi_error_t __store_field(const char *nvs_name, const char *value,
                                    e_mqtt_field_kind_t kind,
                                    bool empty_is_allowed);

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

ezlopi_error_t EZPI_core_mqtt_config_get(s_ezpi_mqtt_config_t *out)
{
    ezlopi_error_t error = EZPI_ERR_WRONG_PARAM;

    if (NULL != out)
    {
        memset(out, 0, sizeof(*out));

        /* An empty URI is allowed and meaningful: it is the "MQTT configured
        ** but idle" state, and the state of every stock image. */
        __resolve_field(out->uri, sizeof(out->uri), mqtt_nvs_uri,
                        MQTT_DEFAULT_URI, E_MQTT_FIELD_URI, true);

        __resolve_field(out->username, sizeof(out->username), mqtt_nvs_user,
                        MQTT_DEFAULT_USER, E_MQTT_FIELD_CRED, true);

        __resolve_field(out->password, sizeof(out->password), mqtt_nvs_pass,
                        MQTT_DEFAULT_PASS, E_MQTT_FIELD_CRED, true);

        /* The two prefixes may not be empty, so empty_is_allowed is false and
        ** an empty or invalid stored value falls through to the compiled-in
        ** default rather than producing a topic that starts with '/'. */
        __resolve_field(out->topic_prefix, sizeof(out->topic_prefix),
                        mqtt_nvs_tprefix, MQTT_DEFAULT_TPREFIX,
                        E_MQTT_FIELD_PREFIX, false);

        __resolve_field(out->discovery_prefix, sizeof(out->discovery_prefix),
                        mqtt_nvs_dprefix, MQTT_DEFAULT_DPREFIX,
                        E_MQTT_FIELD_PREFIX, false);

        error = EZPI_SUCCESS;
    }

    return error;
}

ezlopi_error_t EZPI_core_mqtt_config_set_uri(const char *uri)
{
    return __store_field(mqtt_nvs_uri, uri, E_MQTT_FIELD_URI, true);
}

ezlopi_error_t EZPI_core_mqtt_config_set_username(const char *username)
{
    return __store_field(mqtt_nvs_user, username, E_MQTT_FIELD_CRED, true);
}

ezlopi_error_t EZPI_core_mqtt_config_set_password(const char *password)
{
    /* No TRACE_* of the value, here or anywhere below. */
    return __store_field(mqtt_nvs_pass, password, E_MQTT_FIELD_CRED, true);
}

ezlopi_error_t EZPI_core_mqtt_config_set_topic_prefix(const char *prefix)
{
    return __store_field(mqtt_nvs_tprefix, prefix, E_MQTT_FIELD_PREFIX, false);
}

ezlopi_error_t EZPI_core_mqtt_config_set_discovery_prefix(const char *prefix)
{
    return __store_field(mqtt_nvs_dprefix, prefix, E_MQTT_FIELD_PREFIX, false);
}

ezlopi_error_t EZPI_core_mqtt_config_reset(void)
{
    /* Deleting rather than writing "" so that get() takes the Kconfig branch
    ** for the right reason -- "nothing stored" -- instead of relying on an
    ** empty stored string being treated as absent. Those are the same thing
    ** today; they are not the same thing to read. */
    EZPI_core_nvs_delete_stored_data_by_name((char *)mqtt_nvs_uri);
    EZPI_core_nvs_delete_stored_data_by_name((char *)mqtt_nvs_user);
    EZPI_core_nvs_delete_stored_data_by_name((char *)mqtt_nvs_pass);
    EZPI_core_nvs_delete_stored_data_by_name((char *)mqtt_nvs_tprefix);
    EZPI_core_nvs_delete_stored_data_by_name((char *)mqtt_nvs_dprefix);

    TRACE_I("mqtt-config: stored settings cleared, compiled-in values apply");

    return EZPI_SUCCESS;
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

/**
 * @brief Apply the validator that belongs to this kind of field.
 */
static bool __field_is_valid(const char *value, e_mqtt_field_kind_t kind)
{
    bool valid = false;

    switch (kind)
    {
    case E_MQTT_FIELD_URI:
        valid = EZPI_util_mqttcfg_uri_is_valid(value);
        break;

    case E_MQTT_FIELD_PREFIX:
        valid = EZPI_util_mqttcfg_prefix_is_valid(value);
        break;

    case E_MQTT_FIELD_CRED:
        valid = EZPI_util_mqttcfg_cred_is_valid(value);
        break;

    default:
        /* Unreachable unless a new kind is added without a case. Refusing is
        ** the safe direction: a field that cannot be validated is a field
        ** that does not get used. */
        valid = false;
        break;
    }

    return valid;
}

/**
 * @brief Fill dst from NVS if a valid value is stored, otherwise from fallback.
 *
 * The validation on the *read* path is the point of this function, and it is
 * not redundant with the validation on the write path. Validating only on
 * write assumes every value in NVS got there through this component's setters.
 * Reasons that can be false: a write interrupted by a power cut, an entry
 * written by an older or newer firmware, a hand-edited NVS partition, or a
 * future provisioning path that writes the key directly. In each case the
 * stored value is treated as absent and the compiled-in default is used, so
 * the failure mode is "behaves like a stock image" rather than "hands a
 * malformed topic prefix to the MQTT client".
 *
 * @param dst destination buffer
 * @param dst_size its size, including room for the terminator
 * @param nvs_name NVS entry to read
 * @param fallback compiled-in value, used when NVS has nothing valid
 * @param kind which validator applies
 * @param empty_is_allowed true if "" is a legitimate value for this field
 */
static void __resolve_field(char *dst, size_t dst_size, const char *nvs_name,
                            const char *fallback, e_mqtt_field_kind_t kind,
                            bool empty_is_allowed)
{
    char *stored = EZPI_core_nvs_read_str(nvs_name);
    const char *chosen = NULL;

    if (NULL != stored)
    {
        const size_t stored_len = strlen(stored);

        if ((0u == stored_len) && empty_is_allowed)
        {
            /* Explicitly cleared. This is how a user turns MQTT off again
            ** without reflashing, so it must win over the Kconfig value --
            ** otherwise clearing the URI on a build that has one compiled in
            ** would silently restore it. */
            chosen = "";
        }
        else if (stored_len < dst_size)
        {
            /* The length check is before validation and not merged into it:
            ** the validators bound a value by its own maximum, which is the
            ** same number as this buffer today. Checking the buffer
            ** separately means a future field whose validator is looser than
            ** its buffer still cannot overflow it. */
            if (__field_is_valid(stored, kind))
            {
                chosen = stored;
            }
            else
            {
                /* The value itself is not printed. For the password that
                ** matters; for the others it is simply consistent. */
                TRACE_W("mqtt-config: stored '%s' is not valid, using built-in",
                        nvs_name);
            }
        }
        else
        {
            TRACE_W("mqtt-config: stored '%s' is too long, using built-in",
                    nvs_name);
        }
    }

    if (NULL == chosen)
    {
        chosen = fallback;
    }

    /* The fallback can itself be invalid -- CONFIG_EZPI_MQTT_BROKER_URI
    ** defaults to "", and someone can type anything into menuconfig -- so it
    ** is copied without being revalidated, and the caller's contract is only
    ** that the two prefixes are non-empty. That is guaranteed by the
    ** MQTT_DEFAULT_*PREFIX macros being non-empty literals, not by a check
    ** here. The service validates the URI it is about to use regardless of
    ** where it came from. */
    if (dst_size > 0u)
    {
        const size_t n = strlen(chosen);

        if (n < dst_size)
        {
            memcpy(dst, chosen, n + 1u);
        }
        else
        {
            /* Only reachable from an over-long Kconfig string. Truncating
            ** would produce a plausible-looking wrong value, so this stores
            ** nothing and lets the empty-URI path refuse to start. */
            dst[0] = '\0';
            TRACE_E("mqtt-config: built-in '%s' does not fit, left empty",
                    nvs_name);
        }
    }

    if (NULL != stored)
    {
        ezlopi_free(__FUNCTION__, stored);
    }
}

/**
 * @brief Validate, then write, then read back and compare.
 *
 * The read-back is the same discipline as in ezlopi_core_local_key.c: an NVS
 * write that silently does not stick would otherwise surface much later as
 * "my broker setting did not take effect after a reboot", with no way to tell
 * whether the write or the read was at fault.
 */
static ezlopi_error_t __store_field(const char *nvs_name, const char *value,
                                    e_mqtt_field_kind_t kind,
                                    bool empty_is_allowed)
{
    ezlopi_error_t error = EZPI_ERR_WRONG_PARAM;

    if (NULL != value)
    {
        const bool is_empty = ('\0' == value[0]);

        if (is_empty && (false == empty_is_allowed))
        {
            /* A prefix cannot be empty. Falls through to the error return. */
        }
        else if (is_empty || __field_is_valid(value, kind))
        {
            /* strlen + 1 so the terminator is stored. Reading back with
            ** EZPI_core_nvs_read_str() then yields a NUL-terminated string
            ** without the reader having to know the length. */
            const uint32_t len = (uint32_t)(strlen(value) + 1u);

            if (EZPI_SUCCESS == EZPI_core_nvs_write_str(value, len, nvs_name))
            {
                char *check = EZPI_core_nvs_read_str(nvs_name);

                if ((NULL != check) && (0 == strcmp(check, value)))
                {
                    error = EZPI_SUCCESS;
                }
                else
                {
                    error = EZPI_ERR_NVS_WRITE_FAILED;
                    TRACE_E("mqtt-config: '%s' did not read back as written",
                            nvs_name);
                }

                if (NULL != check)
                {
                    ezlopi_free(__FUNCTION__, check);
                }
            }
            else
            {
                error = EZPI_ERR_NVS_WRITE_FAILED;
            }
        }
        else
        {
            /* Rejected. The offending value is not logged: for the password
            ** that is the whole point, and treating the fields differently
            ** here would mean the one log line that mattered was the one
            ** nobody remembered to leave out. */
            TRACE_W("mqtt-config: refused an invalid value for '%s'", nvs_name);
        }
    }

    return error;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
