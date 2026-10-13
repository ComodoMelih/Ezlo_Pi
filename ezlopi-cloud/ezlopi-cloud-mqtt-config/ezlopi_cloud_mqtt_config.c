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
 * @file    ezlopi_cloud_mqtt_config.c
 * @brief   API methods for pointing this controller at an MQTT broker.
 * @author
 * @version 1.0
 * @date    12th OCT 2025
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "ezlopi_cloud_mqtt_config.h"

#ifdef CONFIG_EZPI_ENABLE_MQTT

#include <string.h>

#include "ezlopi_cloud_constants.h"
#include "ezlopi_core_mqtt_config.h"
#include "ezlopi_util_mqttcfg.h"
#include "ezlopi_util_trace.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/**
 * @brief JSON field names.
 *
 * Local rather than added to ezlopi_cloud_keywords.h. That file is included by
 * most of the cloud layer, and five entries used by one method would be five
 * more symbols everything else links. "uri" is also a word general enough that
 * a shared keyword of that name would invite reuse by something unrelated.
 *
 * Macros rather than the `static const char *const` the rest of the cloud
 * layer uses for its keywords, and that is not a style preference. The field
 * table below has static storage duration, so its initialisers must be
 * constant expressions, and in C -- unlike C++ -- reading a `const char *const`
 * variable is not one. Written that way this file does not compile at all
 * ("initializer element is not constant"). Macros also let the same name be
 * used in the table and in the getter, so the two cannot drift.
 *
 * camelCase for the two-word names, which is what the rest of this API uses
 * ("interfaceId", "deviceId") rather than snake_case.
 */
#define MQTT_CFG_URI_STR "uri"
#define MQTT_CFG_USERNAME_STR "username"
#define MQTT_CFG_PASSWORD_STR "password"
#define MQTT_CFG_TPREFIX_STR "topicPrefix"
#define MQTT_CFG_DPREFIX_STR "discoveryPrefix"
#define MQTT_CFG_PASSWORD_SET_STR "passwordSet"
#define MQTT_CFG_REBOOT_REQUIRED_STR "rebootRequired"

/**
 * @brief One settable field: where to read it, how to check it, where to put it.
 *
 * Table-driven because the five fields differ only in these five ways, and
 * because the set method has to walk them twice -- once to validate
 * everything, once to store it. Two hand-written passes over five fields each
 * is ten places for the validate pass and the store pass to drift apart.
 */
typedef struct s_mqtt_cfg_field
{
    /** @brief Key in the request's params object. */
    const char *json_name;

    /** @brief Longest accepted value, excluding the terminator. */
    size_t max_len;

    /** @brief True if "" is a legitimate value that clears the setting. */
    bool empty_allowed;

    /** @brief Validator from ezlopi-util-mqttcfg. */
    bool (*is_valid)(const char *value);

    /** @brief Setter from ezlopi-core-mqtt-config. */
    ezlopi_error_t (*store)(const char *value);
} s_mqtt_cfg_field_t;

/** @brief Why a field was rejected, or that it was absent. */
typedef enum e_mqtt_cfg_take
{
    E_MQTT_CFG_ABSENT = 0, /**< not in params; leave the stored value alone */
    E_MQTT_CFG_PRESENT,    /**< present, well formed and valid */
    E_MQTT_CFG_NOT_STRING, /**< present but not a JSON string */
    E_MQTT_CFG_TOO_LONG,   /**< present but longer than this field allows */
    E_MQTT_CFG_INVALID,    /**< present, right length, failed validation */
} e_mqtt_cfg_take_t;

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static const s_mqtt_cfg_field_t *__fields(size_t *count);
static e_mqtt_cfg_take_t __take_field(cJSON *cj_params,
                                      const s_mqtt_cfg_field_t *field,
                                      char *buf, size_t buf_size);
static void __prepare_error(cJSON *cj_response, int error_code,
                            const char *error_message, const char *error_data);

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

void EZPI_CLOUD_mqtt_config_get(cJSON *cj_request, cJSON *cj_response)
{
    (void)cj_request;

    cJSON *cj_result =
        cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);

    if (cj_result)
    {
        s_ezpi_mqtt_config_t cfg;

        if (EZPI_SUCCESS == EZPI_core_mqtt_config_get(&cfg))
        {
            cJSON_AddStringToObject(__FUNCTION__, cj_result, MQTT_CFG_URI_STR,
                                    cfg.uri);
            cJSON_AddStringToObject(__FUNCTION__, cj_result,
                                    MQTT_CFG_USERNAME_STR, cfg.username);
            cJSON_AddStringToObject(__FUNCTION__, cj_result,
                                    MQTT_CFG_TPREFIX_STR, cfg.topic_prefix);
            cJSON_AddStringToObject(__FUNCTION__, cj_result,
                                    MQTT_CFG_DPREFIX_STR,
                                    cfg.discovery_prefix);

            /* Whether a password exists, never the password. See the header.
            ** A UI needs to show "a password is set" so that leaving the field
            ** blank on a settings screen is distinguishable from clearing it;
            ** it does not need the value to do that. */
            cJSON_AddBoolToObject(__FUNCTION__, cj_result,
                                  MQTT_CFG_PASSWORD_SET_STR,
                                  ('\0' != cfg.password[0]));

            /* cfg held the password until this point. Wiping it is not much of
            ** a defence -- it was a copy of something sitting in NVS in plain
            ** text either way -- but it costs one call and keeps the value off
            ** a task stack that later requests will reuse. */
            memset(&cfg, 0, sizeof(cfg));
        }
        else
        {
            /* EZPI_core_mqtt_config_get() only fails on a NULL argument, and
            ** the argument is a local. Unreachable, and reported rather than
            ** ignored so that it cannot become silently reachable. */
            __prepare_error(cj_response, -32500, "Internal error",
                            "hub.mqtt.config.unavailable");
        }
    }
}

void EZPI_CLOUD_mqtt_config_set(cJSON *cj_request, cJSON *cj_response)
{
    cJSON *cj_result =
        cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);

    if (cj_result)
    {
        cJSON *cj_params =
            cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_params_str);

        if (cj_params)
        {
            size_t count = 0u;
            const s_mqtt_cfg_field_t *fields = __fields(&count);

            /* One buffer, sized for the longest field, reused by both passes.
            ** Five buffers would be a little over 600 bytes of stack on a task
            ** this component does not size. */
            char value[EZPI_MQTTCFG_URI_MAX + 1u];

            bool rejected = false;
            size_t supplied = 0u;
            size_t i;

            /* Pass one: validate everything that was supplied, store nothing.
            **
            ** This is not a transaction. A power cut between two writes in
            ** pass two still leaves some fields updated and some not. What the
            ** two passes buy is that a *rejected* request changes nothing,
            ** which is the case that happens to people -- a typo in the URI
            ** should not also wipe the username. The power-cut case is
            ** survivable for a different reason: every field is revalidated
            ** when it is read, so a partially written setting falls back to
            ** the compiled-in default rather than being used. */
            for (i = 0u; (i < count) && (false == rejected); i++)
            {
                const e_mqtt_cfg_take_t took =
                    __take_field(cj_params, &fields[i], value, sizeof(value));

                switch (took)
                {
                case E_MQTT_CFG_ABSENT:
                    break;

                case E_MQTT_CFG_PRESENT:
                    supplied++;
                    break;

                case E_MQTT_CFG_NOT_STRING:
                    __prepare_error(cj_response, -32602, "Wrong params",
                                    "rpc.params.invalid");
                    TRACE_W("mqtt-config: '%s' is not a string",
                            fields[i].json_name);
                    rejected = true;
                    break;

                case E_MQTT_CFG_TOO_LONG:
                    __prepare_error(cj_response, -32602, "Wrong params",
                                    "rpc.params.invalid");
                    TRACE_W("mqtt-config: '%s' is too long",
                            fields[i].json_name);
                    rejected = true;
                    break;

                case E_MQTT_CFG_INVALID:
                default:
                    __prepare_error(cj_response, -32602, "Wrong params",
                                    "rpc.params.invalid");
                    /* The field is named, the value is not echoed. For
                    ** password that is the point; doing it uniformly means the
                    ** one line that mattered is not the one that got
                    ** overlooked. */
                    TRACE_W("mqtt-config: '%s' was refused",
                            fields[i].json_name);
                    rejected = true;
                    break;
                }
            }

            if (false == rejected)
            {
                if (0u == supplied)
                {
                    /* Nothing recognised in params. Reported rather than
                    ** answered with a cheerful success, because the likeliest
                    ** cause is a misspelled field name and a success reply
                    ** would send the caller looking anywhere but there. */
                    __prepare_error(cj_response, -32602, "Wrong params",
                                    "rpc.params.invalid");
                }
                else
                {
                    bool write_failed = false;

                    /* Pass two: store. Every value here has already passed the
                    ** same validator the setter applies, so a failure now is a
                    ** storage failure and not a bad value. */
                    for (i = 0u; (i < count) && (false == write_failed); i++)
                    {
                        if (E_MQTT_CFG_PRESENT ==
                            __take_field(cj_params, &fields[i], value,
                                         sizeof(value)))
                        {
                            if (EZPI_SUCCESS != fields[i].store(value))
                            {
                                write_failed = true;
                                TRACE_E("mqtt-config: storing '%s' failed",
                                        fields[i].json_name);
                            }
                        }
                    }

                    /* The buffer last held whichever field came last, which
                    ** may have been the password. */
                    memset(value, 0, sizeof(value));

                    if (write_failed)
                    {
                        __prepare_error(cj_response, -32500,
                                        "Internal error",
                                        "hub.mqtt.config.storefailed");
                    }
                    else
                    {
                        /* The settings are stored; the running client is not
                        ** moved. Saying so in the result is the difference
                        ** between a UI that tells the user to reboot and one
                        ** that implies the broker already changed. */
                        cJSON_AddBoolToObject(__FUNCTION__, cj_result,
                                              MQTT_CFG_REBOOT_REQUIRED_STR,
                                              true);
                    }
                }
            }
        }
        else
        {
            __prepare_error(cj_response, -32600, "Bad request",
                            "rpc.params.notfound");
        }
    }
}

void EZPI_CLOUD_mqtt_config_reset(cJSON *cj_request, cJSON *cj_response)
{
    (void)cj_request;

    cJSON *cj_result =
        cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);

    if (cj_result)
    {
        if (EZPI_SUCCESS == EZPI_core_mqtt_config_reset())
        {
            cJSON_AddBoolToObject(__FUNCTION__, cj_result,
                                  MQTT_CFG_REBOOT_REQUIRED_STR, true);
        }
        else
        {
            __prepare_error(cj_response, -32500, "Internal error",
                            "hub.mqtt.config.storefailed");
        }
    }
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

/**
 * @brief The field table and its length.
 *
 * The table is built here rather than as a file-scope initialiser because its
 * function-pointer members come from two other components, and an initialiser
 * at file scope would be fine today but is the kind of thing that stops being
 * a constant expression the moment one of those turns into a macro. Building
 * it in a function also keeps the row count and the rows in one place.
 *
 * @param count out: number of rows
 * @return the table
 */
static const s_mqtt_cfg_field_t *__fields(size_t *count)
{
    static const s_mqtt_cfg_field_t fields[] = {
        {
            .json_name = MQTT_CFG_URI_STR,
            .max_len = EZPI_MQTTCFG_URI_MAX,
            /* "" clears it, and an empty URI is the state in which the MQTT
            ** service does not start. That is how a user turns this off again
            ** without building firmware. */
            .empty_allowed = true,
            .is_valid = EZPI_util_mqttcfg_uri_is_valid,
            .store = EZPI_core_mqtt_config_set_uri,
        },
        {
            .json_name = MQTT_CFG_USERNAME_STR,
            .max_len = EZPI_MQTTCFG_CRED_MAX,
            .empty_allowed = true,
            .is_valid = EZPI_util_mqttcfg_cred_is_valid,
            .store = EZPI_core_mqtt_config_set_username,
        },
        {
            .json_name = MQTT_CFG_PASSWORD_STR,
            .max_len = EZPI_MQTTCFG_CRED_MAX,
            .empty_allowed = true,
            .is_valid = EZPI_util_mqttcfg_cred_is_valid,
            .store = EZPI_core_mqtt_config_set_password,
        },
        {
            .json_name = MQTT_CFG_TPREFIX_STR,
            .max_len = EZPI_MQTTCFG_PREFIX_MAX,
            /* A prefix cannot be empty -- every topic is built from it, and an
            ** empty one produces topics beginning with '/'. Clearing it is
            ** hub.mqtt.config.reset. */
            .empty_allowed = false,
            .is_valid = EZPI_util_mqttcfg_prefix_is_valid,
            .store = EZPI_core_mqtt_config_set_topic_prefix,
        },
        {
            .json_name = MQTT_CFG_DPREFIX_STR,
            .max_len = EZPI_MQTTCFG_PREFIX_MAX,
            .empty_allowed = false,
            .is_valid = EZPI_util_mqttcfg_prefix_is_valid,
            .store = EZPI_core_mqtt_config_set_discovery_prefix,
        },
    };

    if (count)
    {
        *count = sizeof(fields) / sizeof(fields[0]);
    }

    return fields;
}

/**
 * @brief Copy one field out of params and say whether it is usable.
 *
 * The value is copied using cJSON's recorded length and terminated here, not
 * read as a NUL-terminated string from valuestring. Every existing reader in
 * this tree uses the "%.*s" precision form with str_value_len for the same
 * reason, and it matters more here than usual: the value is about to be handed
 * to a validator that takes a const char *, so if valuestring were not
 * terminated the validator would read past the end of it.
 *
 * STR_VALUE_LEN IS NOT THE LENGTH OF THE STRING
 *
 * It depends on how the item was built, which is not a thing a caller should
 * have to know, so this function does not rely on it:
 *
 *   - cJSON_Parse/cJSON_ParseWithLength, which is what the websocket server
 *     and ezlopi_core_api.c use, goes through parse_string(), where
 *     str_value_len is set to an *allocation* size: the span from the opening
 *     quote to the closing quote less any escape backslashes. For a string
 *     with no escapes that is strlen + 1, and for one with escapes it is more.
 *     Measured, not deduced: a parsed 128-character URI reports 129.
 *   - cJSON_ParseWithRef goes through parse_string_with_ref(), which subtracts
 *     the extra one, so there it is the exact length -- and valuestring points
 *     into the payload and is terminated by a quote, not a NUL.
 *   - cJSON_CreateString sets it from strlen(), so exact again.
 *
 * The rest of the tree never notices because it only ever feeds the value to
 * "%.*s" or to a bounded compare, and both stop at the first NUL regardless of
 * the precision they were given. Code that uses it as a length arithmetically
 * does notice: with str_value_len, a URI of exactly the maximum length was
 * rejected as one byte too long, and the memcpy below would have copied the
 * terminator as a byte and then written a second one past the end of a buffer
 * sized for the maximum.
 *
 * So the length is determined the way "%.*s" determines it -- up to
 * str_value_len bytes, stopping at a NUL -- which is correct for all three
 * construction paths and does not read past valuestring in the reference case.
 *
 * Over-long values are rejected, not truncated. The shared helper
 * EZPI_core_cjson_get_value_string_by_copy() truncates with snprintf, which is
 * wrong for these fields twice over: a truncated URI can still *pass*
 * validation and would then be stored as a plausible-looking address nobody
 * typed, and a truncated topic prefix silently relocates every topic. That
 * helper also cannot tell an absent field from an empty one, because it leaves
 * the buffer untouched in both cases -- and "" is a meaningful value for three
 * of these five fields. Hence the direct cJSON access.
 *
 * @param cj_params the request's params object
 * @param field which field to take
 * @param buf destination
 * @param buf_size its size, including room for the terminator
 * @return what happened; buf is only written for E_MQTT_CFG_PRESENT
 */
static e_mqtt_cfg_take_t __take_field(cJSON *cj_params,
                                      const s_mqtt_cfg_field_t *field,
                                      char *buf, size_t buf_size)
{
    e_mqtt_cfg_take_t result = E_MQTT_CFG_ABSENT;

    cJSON *cj_value =
        cJSON_GetObjectItem(__FUNCTION__, cj_params, field->json_name);

    if (cj_value)
    {
        if (false == cJSON_IsString(cj_value))
        {
            result = E_MQTT_CFG_NOT_STRING;
        }
        else if (NULL == cj_value->valuestring)
        {
            /* A string item with no storage. Treated as malformed rather than
            ** as an empty string: an empty string arrives with a non-NULL
            ** pointer and a length of zero. */
            result = E_MQTT_CFG_NOT_STRING;
        }
        else
        {
            /* See the function comment: str_value_len is an upper bound on the
            ** length, not the length. Stopping at the first NUL as well is
            ** what makes this exact for every way the item can have been
            ** built, and what keeps it inside valuestring when the item is a
            ** reference into the payload rather than a copy. */
            size_t len = 0u;

            while ((len < (size_t)cj_value->str_value_len) &&
                   ('\0' != cj_value->valuestring[len]))
            {
                len++;
            }

            if ((len > field->max_len) || (len >= buf_size))
            {
                /* Both bounds are checked. field->max_len is what the setting
                ** allows and is what the caller gets told about; buf_size is
                ** what this function can physically hold. They are equal for
                ** the longest field today, which is exactly why relying on
                ** just one of them would survive until someone added a sixth
                ** field. */
                result = E_MQTT_CFG_TOO_LONG;
            }
            else
            {
                memcpy(buf, cj_value->valuestring, len);
                buf[len] = '\0';

                if ('\0' == buf[0])
                {
                    result = field->empty_allowed ? E_MQTT_CFG_PRESENT
                                                  : E_MQTT_CFG_INVALID;
                }
                else
                {
                    result = field->is_valid(buf) ? E_MQTT_CFG_PRESENT
                                                  : E_MQTT_CFG_INVALID;
                }
            }
        }
    }

    return result;
}

/**
 * @brief Add the JSON-RPC error object to a response.
 *
 * A private copy of the same shape ezlopi_cloud_offline_login.c uses, which is
 * itself private. Sharing one would be better and is a wider cleanup than this
 * change: every cloud component that reports errors has its own. Diverging
 * from the established shape here would be worse than repeating it.
 */
static void __prepare_error(cJSON *cj_response, int error_code,
                            const char *error_message, const char *error_data)
{
    cJSON *cj_error =
        cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_error_str);

    if (cj_error)
    {
        cJSON_AddNumberToObject(__FUNCTION__, cj_error, ezlopi_code_str,
                                error_code);
        cJSON_AddStringToObject(__FUNCTION__, cj_error, ezlopi_message_str,
                                error_message);
        cJSON_AddStringToObject(__FUNCTION__, cj_error, ezlopi_data_str,
                                error_data);
    }
}

#endif // CONFIG_EZPI_ENABLE_MQTT

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
