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
 * @file    ezlopi_util_devcfgval.c
 * @brief   Whether a device configuration is worth storing
 * @version 1.0
 * @date    13th OCT 2025
 *
 * The reasoning for each rejection is in ezlopi_util_devcfgval.h, against what
 * ezlopi_device_parse_json_v3() in ezlopi_core_devices.c actually does with the
 * shape. This file is only the mechanics.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <string.h>

#include "ezlopi_util_devcfgval.h"

#include "ezlopi_cloud_keywords.h"

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * @brief   Judge one entry of "dev_detail".
 *
 * Split out so the loop below reads as "find the first bad entry" rather than
 * as three nested conditions.
 */
static e_ezpi_devcfgval_t __check_device(const cJSON *cj_device)
{
    if (false == cJSON_IsObject(cj_device))
    {
        return EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT;
    }

    /* The boot parser's test is `NULL == cj_device_id || NULL == cj_device_id
    ** ->valuestring`, which it treats as a parse failure. cJSON_IsString is
    ** used here instead of a valuestring null check because valuestring is
    ** also set on cJSON_Raw nodes, and a Raw node is not something a
    ** configuration arriving as text can contain -- cJSON_Parse never produces
    ** one. For anything that can reach either function the two tests agree. */
    const cJSON *cj_device_id = cJSON_GetObjectItem(__FUNCTION__, cj_device,
                                                    ezlopi_device_id_str);

    if ((false == cJSON_IsString(cj_device_id)) ||
        (NULL == cj_device_id->valuestring))
    {
        return EZPI_DEVCFGVAL_DEVICE_ID_BAD;
    }

    /* The quiet one. The boot parser reads id_item through
    ** CJSON_GET_VALUE_INT, which starts by writing 0 and only overwrites it
    ** `if (o_item && (o_item->type & cJSON_Number))`; it then looks for a
    ** driver only `if (0 != id_item)`. So an absent id_item, a string one, and
    ** a numeric 0 are all the same thing to it: the entry is skipped with no
    ** error and no log line naming it.
    **
    ** The cast is the parser's own `(int)o_item->valuedouble`, kept rather than
    ** comparing valuedouble against 0.0, because that cast is what decides
    ** whether the entry is skipped. 0.4 is a non-zero number that truncates to
    ** zero and so builds nothing, and this has to refuse what the parser would
    ** drop, not what looks non-zero here. */
    const cJSON *cj_id_item = cJSON_GetObjectItem(__FUNCTION__, cj_device,
                                                  ezlopi_id_item_str);

    if ((false == cJSON_IsNumber(cj_id_item)) ||
        (0 == (int)cj_id_item->valuedouble))
    {
        return EZPI_DEVCFGVAL_ITEM_ID_BAD;
    }

    return EZPI_DEVCFGVAL_OK;
}

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
e_ezpi_devcfgval_t EZPI_util_devcfgval_check(const cJSON *root,
                                             const char *expected_chipset,
                                             uint32_t serialised_len,
                                             uint32_t region_size,
                                             int *bad_index)
{
    if (bad_index)
    {
        /* Set once, up front. Every return below that is not per-entry then
        ** leaves it at -1 without having to remember to. */
        *bad_index = -1;
    }

    /* cJSON_IsObject(NULL) is false, so a null tree lands here rather than
    ** needing its own reason: there is nothing useful to say about it beyond
    ** "that is not a configuration". */
    if (false == cJSON_IsObject(root))
    {
        return EZPI_DEVCFGVAL_NOT_OBJECT;
    }

    /* Written as >= rather than serialised_len + 1 > region_size, which is the
    ** same comparison for integers and cannot wrap when serialised_len is
    ** UINT32_MAX. The + 1 is the terminator: the field has to hold it, and the
    ** writer counts it even though its callers do not pass it. */
    if ((0u != serialised_len) && (0u != region_size) &&
        (serialised_len >= region_size))
    {
        return EZPI_DEVCFGVAL_TOO_LARGE;
    }

    const cJSON *cj_chipset = cJSON_GetObjectItem(__FUNCTION__, root,
                                                  ezlopi_chipset_str);

    if (NULL == cj_chipset)
    {
        return EZPI_DEVCFGVAL_CHIPSET_MISSING;
    }

    if ((false == cJSON_IsString(cj_chipset)) ||
        (NULL == cj_chipset->valuestring))
    {
        return EZPI_DEVCFGVAL_CHIPSET_NOT_STRING;
    }

    /* NULL means this build has no name for its target -- see the header. The
    ** chipset is then left unchecked rather than assumed wrong.
    **
    ** strcmp, where the boot parser uses strncmp over
    ** max(strlen(expected), str_value_len) bytes of a 16 byte clamped copy.
    ** The two agree on everything that can arrive: a name longer than the
    ** clamp is truncated there and so fails its strncmp, and strcmp fails it
    ** too; a name carrying an embedded NUL compares equal up to that NUL in
    ** both. Matching the parser is the whole point of this check, so where the
    ** two could differ the parser wins. */
    if ((NULL != expected_chipset) &&
        (0 != strcmp(cj_chipset->valuestring, expected_chipset)))
    {
        return EZPI_DEVCFGVAL_CHIPSET_MISMATCH;
    }

    const cJSON *cj_device_list = cJSON_GetObjectItem(__FUNCTION__, root,
                                                      ezlopi_dev_detail_str);

    if (NULL == cj_device_list)
    {
        return EZPI_DEVCFGVAL_DEVLIST_MISSING;
    }

    if (false == cJSON_IsArray(cj_device_list))
    {
        return EZPI_DEVCFGVAL_DEVLIST_NOT_ARRAY;
    }

    /* An empty array is accepted. It is the only way to say "no devices", which
    ** is what a user who wants to start over sends, and it is not the failure
    ** this check exists for: an empty device list the user asked for is not the
    ** same as an empty device list produced by a configuration they thought
    ** named five sensors. */
    int index = 0;
    const cJSON *cj_device = NULL;

    for (cj_device = cj_device_list->child; NULL != cj_device;
         cj_device = cj_device->next)
    {
        e_ezpi_devcfgval_t reason = __check_device(cj_device);

        if (EZPI_DEVCFGVAL_OK != reason)
        {
            if (bad_index)
            {
                *bad_index = index;
            }

            return reason;
        }

        index++;
    }

    return EZPI_DEVCFGVAL_OK;
}

const char *EZPI_util_devcfgval_token(e_ezpi_devcfgval_t reason)
{
    /* No default label on purpose: -Wswitch then makes adding an enumerator
    ** without a token a build warning rather than a silent "unknown" at
    ** runtime. The return below the switch is what covers a value that is not
    ** an enumerator at all, which is how this stays non-NULL for every input. */
    switch (reason)
    {
    case EZPI_DEVCFGVAL_OK:
        return "devcfg.ok";
    case EZPI_DEVCFGVAL_NOT_OBJECT:
        return "devcfg.not_object";
    case EZPI_DEVCFGVAL_TOO_LARGE:
        return "devcfg.too_large";
    case EZPI_DEVCFGVAL_CHIPSET_MISSING:
        return "devcfg.chipset.missing";
    case EZPI_DEVCFGVAL_CHIPSET_NOT_STRING:
        return "devcfg.chipset.not_string";
    case EZPI_DEVCFGVAL_CHIPSET_MISMATCH:
        return "devcfg.chipset.mismatch";
    case EZPI_DEVCFGVAL_DEVLIST_MISSING:
        return "devcfg.dev_detail.missing";
    case EZPI_DEVCFGVAL_DEVLIST_NOT_ARRAY:
        return "devcfg.dev_detail.not_array";
    case EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT:
        return "devcfg.device.not_object";
    case EZPI_DEVCFGVAL_DEVICE_ID_BAD:
        return "devcfg.device_id.invalid";
    case EZPI_DEVCFGVAL_ITEM_ID_BAD:
        return "devcfg.id_item.invalid";
    }

    return "devcfg.unknown";
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
