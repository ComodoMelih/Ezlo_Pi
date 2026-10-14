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
 * @file    ezlopi_util_sanitize.c
 * @brief   Helpers for data that arrived from an untrusted client.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ezlopi_util_sanitize.h"

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/**
 * @brief JSON member prefixes whose values are credentials in this firmware.
 *
 * Matched against the serialised text, so each entry includes the quotes and
 * the colon. That makes the match exact: a member merely *containing* one of
 * these names, such as "passkey_hint", does not match "pass".
 */
static const char *const sanitize_secret_members[] = {
    "\"key\":\"",
    "\"pass\":\"",
    "\"password\":\"",
    "\"wifi_password\":\"",
};

/*******************************************************************************
 *                          Public Function Definitions
 *******************************************************************************/

const char *EZPI_util_json_value_end(const char *start)
{
    int depth = 0;
    bool in_string = false;

    if (NULL == start)
    {
        return NULL;
    }

    for (const char *p = start; '\0' != *p; p++)
    {
        if (in_string)
        {
            if ('\\' == *p)
            {
                /* Step over the escape pair as a unit, otherwise an escaped
                 * quote reads as the end of the string and every delimiter
                 * after it is counted at the wrong depth. */
                if ('\0' == p[1])
                {
                    return NULL;
                }

                p++;
            }
            else if ('"' == *p)
            {
                in_string = false;
            }

            continue;
        }

        switch (*p)
        {
        case '"':
        {
            in_string = true;
            break;
        }
        case '{':
        case '[':
        {
            depth++;
            break;
        }
        case '}':
        case ']':
        {
            depth--;

            if (0 == depth)
            {
                return p + 1;
            }

            /* A closing delimiter with nothing open means the text did not
             * start a container, so there is no value here to delimit. */
            if (depth < 0)
            {
                return NULL;
            }

            break;
        }
        default:
        {
            break;
        }
        }
    }

    /* Ran out of input with the container still open. */
    return NULL;
}

void EZPI_util_redact_secrets_in_place(char *json)
{
    if (NULL == json)
    {
        return;
    }

    for (uint32_t i = 0; i < (sizeof(sanitize_secret_members) / sizeof(sanitize_secret_members[0])); i++)
    {
        uint32_t prefix_len = (uint32_t)strlen(sanitize_secret_members[i]);
        char *at = json;

        while (NULL != (at = strstr(at, sanitize_secret_members[i])))
        {
            char *value = at + prefix_len;

            while (('\0' != *value) && ('"' != *value))
            {
                /* Step over an escape pair as a unit, otherwise an embedded \"
                 * looks like the closing quote and the rest of the secret is
                 * left in the clear. */
                if (('\\' == *value) && ('\0' != value[1]))
                {
                    *value++ = '*';
                }

                *value++ = '*';
            }

            at = value;
        }
    }
}

uint32_t EZPI_util_clamp_copy_len(uint32_t requested, uint32_t src_len, uint32_t dst_remaining)
{
    uint32_t len = requested;

    if (len > src_len)
    {
        len = src_len;
    }

    if (len > dst_remaining)
    {
        len = dst_remaining;
    }

    return len;
}

uint32_t EZPI_util_copy_str_bounded(char *dst, uint32_t dst_size, const char *src, uint32_t src_len)
{
    uint32_t len = 0;

    if ((NULL == dst) || (0 == dst_size))
    {
        return 0;
    }

    if (NULL != src)
    {
        len = EZPI_util_clamp_copy_len(src_len, src_len, dst_size - 1);
        memcpy(dst, src, len);
    }

    /* Always terminate: the callers hand the result to strcmp/strncmp, and an
     * exactly-buffer-sized source used to leave it unterminated. */
    dst[len] = '\0';

    return len;
}

uint32_t EZPI_util_pem_store_len(const char *value, const char *end_marker, uint32_t region_size)
{
    if ((NULL == value) || (NULL == end_marker) || (0 == region_size))
    {
        return 0;
    }

    const char *end_pos = strstr(value, end_marker);

    if (NULL == end_pos)
    {
        /* Refuse. The caller must not write: see the header for why erasing
         * the field on a malformed value was the old behaviour and is wrong. */
        return 0;
    }

    /* Both operands are within one NUL-terminated string, so this cannot
     * overflow: end_pos is a pointer into value, and the marker was found
     * entirely inside it. */
    const size_t length = (size_t)(end_pos - value) + strlen(end_marker);

    /* The writer appends a terminator, so the field must hold length + 1.
     * Compared this way round rather than as (region_size - 1) because
     * region_size is unsigned and a zero would wrap -- it is rejected above,
     * but the comparison should not depend on that happening first. */
    if ((length + 1u) > (size_t)region_size)
    {
        return 0;
    }

    return (uint32_t)length;
}
