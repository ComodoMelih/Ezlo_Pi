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
 * @file    ezlopi_cloud_devcfg.c
 * @brief   hub.ezlopi.config.get and hub.ezlopi.config.set
 * @version 1.0
 * @date    13th OCT 2025
 *
 * The reasoning for the whole method pair is in ezlopi_cloud_devcfg.h. What is
 * here is the ordering, and the ordering is the part that matters:
 *
 *   1. params, then params.config, then that it is an object
 *   2. serialise it once, and refuse if it does not fit
 *   3. EZPI_util_devcfgval_check() on the tree
 *   4. every id_item against this image's device table
 *   5. only then write
 *
 * Steps 3 and 4 are separate because they fail for different reasons and a
 * caller needs to tell them apart. A configuration refused at step 3 is
 * malformed and will be refused by every build. One refused at step 4 is
 * well-formed and would work on a build that included that driver -- the fix is
 * a different firmware image, not a different configuration, and a single
 * "invalid config" would send someone editing JSON that was already correct.
 *
 * Nothing is written until all four have passed, which is trivial here only
 * because the configuration is a single field. It is worth stating anyway: the
 * reason the MQTT settings method needed two explicit passes is that it writes
 * five fields, and this one does not have that problem rather than having
 * solved it.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "EZLOPI_USER_CONFIG.h"

#include "ezlopi_util_trace.h"

#include "cjext.h"
#include "ezlopi_cloud_keywords.h"

#include "ezlopi_util_devcfgval.h"

#include "ezlopi_cloud_devcfg.h"
#include "ezlopi_cloud_devcfg_port.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/* Result and params members this method names. Local for the reason given at
** length in ezlopi_cloud_mqtt_config.c: ezlopi_cloud_keywords.h is included by
** most of the cloud layer, and members used by one method do not belong in it.
**
** "config" is the one that could reasonably have been called something else.
** It is not "devices", because hub.devices.list already returns a different
** thing under that word -- the devices that were actually instantiated, which
** is precisely what a stored configuration does not guarantee. */
#define DEVCFG_CONFIG_STR "config"
#define DEVCFG_CONFIG_SIZE_STR "configSize"
#define DEVCFG_MAX_CONFIG_SIZE_STR "maxConfigSize"
#define DEVCFG_DEVICE_COUNT_STR "deviceCount"
#define DEVCFG_EXPECTED_CHIPSET_STR "expectedChipset"
#define DEVCFG_REBOOT_REQUIRED_STR "rebootRequired"

/* Lives in the error object, not the result, so it is next to the token it
** qualifies and is absent on success rather than being a meaningless 0 there. */
#define DEVCFG_BAD_INDEX_STR "badIndex"

/* The token for the one reason EZPI_util_devcfgval_token() does not have,
** because the check it names is the one that function deliberately does not
** make. Dotted and prefixed the same way as the rest so a client can treat
** them uniformly. */
#define DEVCFG_TOKEN_DRIVER_MISSING "devcfg.id_item.unsupported"

/* Upper bound on what this method will serialise while measuring, used only
** when the port declines to state a region size. Without some bound a caller
** could hand over a document large enough to exhaust the heap just by being
** measured. 64 KiB is sixteen times the real field and still small enough to
** allocate. */
#define DEVCFG_FALLBACK_LIMIT (64u * 1024u)

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static void __prepare_error(cJSON *cj_response, int error_code,
                            const char *error_message, const char *error_data,
                            int bad_index);
static int __device_count(const cJSON *root);
static bool __every_id_item_has_a_driver(const cJSON *root, int *bad_index);

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

void EZPI_CLOUD_devcfg_get(cJSON *cj_request, cJSON *cj_response)
{
    (void)cj_request;

    cJSON *cj_result =
        cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);

    if (cj_result)
    {
        const char *chipset = EZPI_CLOUD_devcfg_port_chipset();
        char *stored = EZPI_CLOUD_devcfg_port_read();

        /* Reported even when nothing is stored, and that is the useful case: a
        ** client with an unconfigured hub needs to know which chipset name to
        ** put in the configuration it is about to send, and the only other way
        ** to find out is to send a wrong one and be refused. */
        if (chipset)
        {
            cJSON_AddStringToObject(__FUNCTION__, cj_result,
                                    DEVCFG_EXPECTED_CHIPSET_STR, chipset);
        }
        else
        {
            cJSON_AddNullToObject(__FUNCTION__, cj_result,
                                  DEVCFG_EXPECTED_CHIPSET_STR);
        }

        cJSON_AddNumberToObject(__FUNCTION__, cj_result,
                                DEVCFG_MAX_CONFIG_SIZE_STR,
                                (double)EZPI_CLOUD_devcfg_port_region_size());

        if (stored)
        {
            /* cJSON_Parse, not cJSON_ParseWithRef. The parsed tree outlives
            ** `stored` -- it is attached to the response and released with it,
            ** while `stored` is freed a few lines below -- and a WithRef parse
            ** keeps pointers into the string it was given. The boot path uses
            ** WithRef correctly because its string lives as long as the tree;
            ** copying that call here would leave the response pointing into
            ** freed memory, which reads back as whatever the allocator did
            ** next. */
            cJSON *cj_stored = cJSON_Parse(__FUNCTION__, stored);

            cJSON_AddNumberToObject(__FUNCTION__, cj_result,
                                    DEVCFG_CONFIG_SIZE_STR,
                                    (double)strlen(stored));

            if (cj_stored)
            {
                cJSON_AddNumberToObject(__FUNCTION__, cj_result,
                                        DEVCFG_DEVICE_COUNT_STR,
                                        (double)__device_count(cj_stored));

                /* Ownership moves to the response on success. On failure the
                ** tree is still this function's, hence the delete -- without
                ** it a hub that is low enough on memory to fail here leaks a
                ** whole configuration on every get, which is the worst moment
                ** to start leaking. */
                if (false == cJSON_AddItemToObject(__FUNCTION__, cj_result,
                                                   DEVCFG_CONFIG_STR,
                                                   cj_stored))
                {
                    cJSON_Delete(__FUNCTION__, cj_stored);
                    cj_stored = NULL;
                }
            }

            if (NULL == cj_stored)
            {
                /* Something is stored and this firmware cannot parse it. Not
                ** reported as "no configuration", because the two need
                ** different responses from whoever is asking: an unconfigured
                ** hub wants a configuration sent, and this one has something
                ** in flash that the next boot will also fail to read. */
                __prepare_error(cj_response, -32500, "Internal error",
                                "devcfg.stored.unreadable", -1);
                TRACE_E("devcfg: stored configuration does not parse");
            }

            EZPI_CLOUD_devcfg_port_free(stored);
        }
        else
        {
            /* Not an error. A hub that has never been configured is in a
            ** perfectly ordinary state, and the whole point of this method is
            ** to be the thing a configuration UI calls first. */
            cJSON_AddNullToObject(__FUNCTION__, cj_result, DEVCFG_CONFIG_STR);
            cJSON_AddNumberToObject(__FUNCTION__, cj_result,
                                    DEVCFG_CONFIG_SIZE_STR, 0);
            cJSON_AddNumberToObject(__FUNCTION__, cj_result,
                                    DEVCFG_DEVICE_COUNT_STR, 0);
        }
    }
}

void EZPI_CLOUD_devcfg_set(cJSON *cj_request, cJSON *cj_response)
{
    cJSON *cj_result =
        cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);

    if (cj_result)
    {
        cJSON *cj_params =
            cJSON_GetObjectItem(__FUNCTION__, cj_request, ezlopi_params_str);

        if (NULL == cj_params)
        {
            __prepare_error(cj_response, -32600, "Bad request",
                            "rpc.params.notfound", -1);
        }
        else
        {
            cJSON *cj_config = cJSON_GetObjectItem(__FUNCTION__, cj_params,
                                                   DEVCFG_CONFIG_STR);

            if (false == cJSON_IsObject(cj_config))
            {
                /* One message for absent and for present-but-wrong-type. The
                ** distinction is not worth two tokens: in both cases the
                ** caller has to go and put a configuration object in
                ** params.config. */
                __prepare_error(cj_response, -32602, "Wrong params",
                                "rpc.params.invalid", -1);
                TRACE_W("devcfg: params.config is missing or not an object");
            }
            else
            {
                const uint32_t region = EZPI_CLOUD_devcfg_port_region_size();
                const uint32_t limit =
                    (0u != region) ? region : DEVCFG_FALLBACK_LIMIT;

                /* Measured by serialising, because the length that matters is
                ** the length of the bytes that will be written, and nothing
                ** cheaper gives that. cJSON_PrintUnformatted produces the same
                ** bytes as the cJSON_PrintPreallocated(..., format=false) call
                ** inside the storer -- same printer, same flag -- so a document
                ** this accepts is a document that fits there.
                **
                ** One transient allocation of up to the serialised size. That
                ** is the same order as the request it came in, which the
                ** websocket layer has already allocated and parsed by the time
                ** this runs. */
                char *serialised =
                    cJSON_PrintUnformatted(__FUNCTION__, cj_config);

                if (NULL == serialised)
                {
                    __prepare_error(cj_response, -32500, "Internal error",
                                    "devcfg.serialise.failed", -1);
                    TRACE_E("devcfg: could not serialise the configuration");
                }
                else
                {
                    const size_t serialised_len = strlen(serialised);
                    int bad_index = -1;

                    /* Clamped before the cast so that a document longer than
                    ** 4 GiB -- which cannot happen on this hardware, and is
                    ** exactly the sort of "cannot happen" that makes a
                    ** truncating cast look safe -- cannot wrap to a small
                    ** number and be judged as fitting. */
                    const uint32_t measured =
                        (serialised_len > (size_t)limit)
                            ? limit
                            : (uint32_t)serialised_len;

                    const e_ezpi_devcfgval_t reason =
                        EZPI_util_devcfgval_check(
                            cj_config, EZPI_CLOUD_devcfg_port_chipset(),
                            measured, limit, &bad_index);

                    ezlopi_free(__FUNCTION__, serialised);

                    if (EZPI_DEVCFGVAL_OK != reason)
                    {
                        __prepare_error(cj_response, -32602, "Wrong params",
                                        EZPI_util_devcfgval_token(reason),
                                        bad_index);
                        TRACE_W("devcfg: refused (%s), entry %d",
                                EZPI_util_devcfgval_token(reason), bad_index);
                    }
                    else if (false ==
                             __every_id_item_has_a_driver(cj_config,
                                                          &bad_index))
                    {
                        /* Well formed, and names something this image cannot
                        ** build. Reported separately from the shape failures
                        ** above because the remedy is different -- see the
                        ** comment at the top of this file. */
                        __prepare_error(cj_response, -32602, "Wrong params",
                                        DEVCFG_TOKEN_DRIVER_MISSING,
                                        bad_index);
                        TRACE_W("devcfg: entry %d names a driver this build "
                                "does not have",
                                bad_index);
                    }
                    else if (false == EZPI_CLOUD_devcfg_port_write(cj_config))
                    {
                        __prepare_error(cj_response, -32500, "Internal error",
                                        "devcfg.store.failed", -1);
                        TRACE_E("devcfg: storing the configuration failed");
                    }
                    else
                    {
                        cJSON_AddNumberToObject(
                            __FUNCTION__, cj_result, DEVCFG_DEVICE_COUNT_STR,
                            (double)__device_count(cj_config));

                        /* The configuration is stored; no device has moved.
                        ** ezlopi_device_parse_json_v3() runs once, during
                        ** EZPI_init(), so the device list in memory is still
                        ** the one from the last boot. Saying so is the
                        ** difference between a UI that tells the user to
                        ** restart and one that shows a device list which
                        ** silently disagrees with the hub. */
                        cJSON_AddBoolToObject(__FUNCTION__, cj_result,
                                              DEVCFG_REBOOT_REQUIRED_STR,
                                              true);
                    }
                }
            }
        }
    }
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

/**
 * @brief How many device entries a configuration describes.
 *
 * Reported rather than computed by the client so that the number in the reply
 * is this firmware's count of the list it is holding. A client counting the
 * array it sent would agree today and would stop agreeing the moment the two
 * disagree about anything -- which is the only interesting case.
 *
 * @param   root    A configuration object, already known to be one.
 * @return  Entries in "dev_detail", or 0 when there is no usable list. Never
 *          negative: cJSON_GetArraySize returns 0 for a non-array.
 */
static int __device_count(const cJSON *root)
{
    const cJSON *cj_list =
        cJSON_GetObjectItem(__FUNCTION__, root, ezlopi_dev_detail_str);

    return cJSON_IsArray(cj_list) ? cJSON_GetArraySize(cj_list) : 0;
}

/**
 * @brief Whether every entry names a driver this image was built with.
 *
 * Only reached once EZPI_util_devcfgval_check() has accepted the document, so
 * every entry here is an object with a usable id_item and the walk needs no
 * defending against the shapes that function already refused. The cast and the
 * zero test are still the parser's own, because the value that has to match a
 * device-table row is the value the parser will look up -- (int)valuedouble.
 *
 * @param   root        The configuration.
 * @param   bad_index   out: index of the first entry whose id_item no driver
 *                      claims. Left alone when every entry is fine.
 * @return  true if every entry is buildable on this image.
 */
static bool __every_id_item_has_a_driver(const cJSON *root, int *bad_index)
{
    const cJSON *cj_list =
        cJSON_GetObjectItem(__FUNCTION__, root, ezlopi_dev_detail_str);
    const cJSON *cj_device = NULL;
    int index = 0;

    for (cj_device = cJSON_IsArray(cj_list) ? cj_list->child : NULL;
         NULL != cj_device; cj_device = cj_device->next)
    {
        const cJSON *cj_id_item =
            cJSON_GetObjectItem(__FUNCTION__, cj_device, ezlopi_id_item_str);

        /* An id_item of exactly zero never reaches here -- the validator
        ** refuses it -- and a negative one cannot match any row, since the
        ** table's ids are uint32_t. Casting a negative int to uint32_t is
        ** defined and simply produces a large value no row holds, so the
        ** comparison answers correctly without a special case.
        **
        ** The cJSON_IsNumber() guard is unreachable today, and deliberate
        ** anyway. The precondition above is real -- the validator refuses an
        ** entry whose id_item is absent or not a number before this runs --
        ** but the cost of trusting it is a NULL dereference, and this sits on
        ** an unauthenticated websocket method. Mutating the validator call
        ** away turns the line below into a SEGV here, which is how this was
        ** noticed. The guard stops a loosening of the validator, in another
        ** component, from becoming a remotely triggerable crash in this one:
        ** such an entry degrades to "no driver claims it" and is refused with
        ** its index, which is the right answer for an entry with no usable
        ** id_item.
        **
        ** No test covers it, because nothing this file can send reaches it
        ** while the validator is correct. Said plainly rather than left to
        ** look like tested code. */
        if ((false == cJSON_IsNumber(cj_id_item)) ||
            (false == EZPI_CLOUD_devcfg_port_driver_exists(
                          (uint32_t)(int)cj_id_item->valuedouble)))
        {
            if (bad_index)
            {
                *bad_index = index;
            }

            return false;
        }

        index++;
    }

    return true;
}

/**
 * @brief Add the JSON-RPC error object to a response.
 *
 * A private copy of the shape used by ezlopi_cloud_offline_login.c and
 * ezlopi_cloud_mqtt_config.c, both of which keep their own. Three copies is not
 * good, and the fix is one shared helper for the whole cloud layer rather than
 * this file being the one that invents a fourth shape.
 *
 * The one addition to that shape is "badIndex", and it is the difference
 * between an error a user can act on and one they cannot. A thirty-device
 * configuration refused for "devcfg.id_item.invalid" with no index leaves
 * someone to find the bad entry by bisection; the check already knows which
 * entry it was, and throwing that away at the last moment would be perverse.
 * Omitted rather than sent as -1 when the reason is not about a particular
 * entry, so a client can test for the member instead of knowing the sentinel.
 *
 * Note that "result" has already been added by the time this is called, so a
 * refused request carries both an empty result and an error. That is not what
 * JSON-RPC describes, and it is what every existing method here does; a client
 * that reads "result" unconditionally works against all of them, and this
 * method is not the place to find out which clients those are.
 *
 * @param   bad_index   Index of the offending device entry, or negative for a
 *                      reason that is not about one entry.
 */
static void __prepare_error(cJSON *cj_response, int error_code,
                            const char *error_message, const char *error_data,
                            int bad_index)
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

        if (bad_index >= 0)
        {
            cJSON_AddNumberToObject(__FUNCTION__, cj_error,
                                    DEVCFG_BAD_INDEX_STR, (double)bad_index);
        }
    }
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
