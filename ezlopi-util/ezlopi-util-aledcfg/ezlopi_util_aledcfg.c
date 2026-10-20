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
 * @file    ezlopi_util_aledcfg.c
 * @brief   Reads an addressable-LED strip's shape out of a device config.
 * @author
 * @version 0.1
 * @date    October 19th, 2026
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "ezlopi_util_aledcfg.h"

#include <stddef.h>

/* The key names themselves live in ezlopi-cloud-constants with every other
** config key, rather than as literals here. That is the house convention, and
** it matters more than usual for these three: a key whose spelling differs
** between the writer and the reader is not a visible failure, it is a strip
** that silently keeps the default length. */
#include "ezlopi_cloud_keywords.h"

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/* The six permutations of r, g, b, indexed by e_aled_order_t.
**
** Order matters and is not alphabetical: it follows the enum in
** ezlopi_util_aled.h, where GRB is 0 because that is what a zeroed config
** means and also what a WS2812 actually wants. The test checks this table
** against the enum rather than taking the pairing on trust, because a table
** that is merely shuffled relative to the enum is the defect that lights the
** strip up in the wrong colours while every other check passes.
*/
static const char *const k_order_names[ALED_ORDER_MAX] = {
    "grb", /* ALED_ORDER_GRB */
    "rgb", /* ALED_ORDER_RGB */
    "brg", /* ALED_ORDER_BRG */
    "rbg", /* ALED_ORDER_RBG */
    "gbr", /* ALED_ORDER_GBR */
    "bgr", /* ALED_ORDER_BGR */
};

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/* ASCII lowercase, written out rather than using tolower().
**
** tolower() is locale-dependent and its argument must be representable as
** unsigned char or the behaviour is undefined -- passing a plain `char` that
** happens to be negative is the classic way to get that wrong. The keys here
** are ASCII by definition, so the narrow version is both correct and has no
** argument precondition to get wrong.
*/
static char __ascii_lower(char c)
{
    return ((c >= 'A') && (c <= 'Z')) ? (char)(c - 'A' + 'a') : c;
}

/*******************************************************************************
 *                          Public Function Definitions
 *******************************************************************************/

bool EZPI_aledcfg_order_from_str(const char *str, e_aled_order_t *out)
{
    bool ret = false;

    if ((NULL != str) && (NULL != out))
    {
        /* Length checked before the compare, so that "rg" cannot match the
        ** first two bytes of "rgb" and "rgbx" cannot match all three of them.
        ** Measured against both: a prefix compare accepts the first and a
        ** bounded compare of 3 accepts the second. */
        if (('\0' != str[0]) && ('\0' != str[1]) && ('\0' != str[2]) && ('\0' == str[3]))
        {
            const char want[3] = {
                __ascii_lower(str[0]),
                __ascii_lower(str[1]),
                __ascii_lower(str[2]),
            };

            for (unsigned i = 0u; i < (unsigned)ALED_ORDER_MAX; i++)
            {
                const char *const name = k_order_names[i];

                if ((want[0] == name[0]) && (want[1] == name[1]) && (want[2] == name[2]))
                {
                    *out = (e_aled_order_t)i;
                    ret = true;
                    break;
                }
            }
        }
    }

    return ret;
}

size_t EZPI_aledcfg_wire_len(const s_aledcfg_t *cfg)
{
    size_t ret = 0u;

    if (NULL != cfg)
    {
        ret = EZPI_aled_wire_len(cfg->pixel_count, cfg->has_white);
    }

    return ret;
}

bool EZPI_aledcfg_from_json(cJSON *cj_device, s_aledcfg_t *out)
{
    bool verbatim = true;

    if (NULL == out)
    {
        /* Nothing can be reported and nothing can be written. Distinct from
        ** every other path, which always leaves a usable config behind. */
        return false;
    }

    /* The defaults go in FIRST, unconditionally, so that every early return
    ** below leaves a usable struct. This is the opposite of the pattern the
    ** header warns about: here the default is written and then only replaced
    ** by a value that has been understood, rather than being written and then
    ** clobbered by a getter that zeroes before it looks. */
    out->pixel_count = ALEDCFG_DEFAULT_PIXELS;
    out->has_white = false;
    out->order = ALED_ORDER_GRB;

    if (NULL == cj_device)
    {
        /* A device entry that is not there is not a disagreement -- it is the
        ** same case as one with none of these keys in it. Returning true keeps
        ** the caller from logging about every strip provisioned before these
        ** keys existed, which is all of them. */
        return true;
    }

    /* ---- pixel_count ---------------------------------------------------- */
    {
        cJSON *const cj_n = cJSON_GetObjectItem(__FUNCTION__, cj_device, ezlopi_pixel_count_str);

        if (NULL != cj_n)
        {
            if (0 != (cj_n->type & cJSON_Number))
            {
                /* valuedouble, not valueint: cJSON's valueint is clamped to
                ** INT_MAX on parse, so a config saying 1e9 would arrive as a
                ** plausible-looking integer rather than something to refuse.
                ** The double is what the document actually said.
                **
                ** Compared as a double before any cast to size_t, because
                ** casting a negative or huge double to an unsigned integer is
                ** undefined -- -1 does not reliably become SIZE_MAX, and a cap
                ** applied after the cast would be testing the wrong value. */
                const double n = cj_n->valuedouble;

                if ((n >= (double)ALEDCFG_MIN_PIXELS) && (n <= (double)ALEDCFG_MAX_PIXELS))
                {
                    /* In range, so the cast is now defined, and only now. */
                    const size_t as_int = (size_t)n;

                    if ((double)as_int == n)
                    {
                        out->pixel_count = as_int;
                    }
                    else
                    {
                        /* 2.5 pixels. Refused rather than truncated to 2: the
                        ** writer is confused about something, and a count is
                        ** the one field here where quietly rounding would
                        ** produce a strip that works at almost the right
                        ** length -- the hardest kind of wrong to notice. */
                        verbatim = false;
                    }
                }
                else
                {
                    /* Out of range, including 0 and anything negative. Left at
                    ** the default of 1 rather than clamped to MAX: a config
                    ** saying 100000 is far more likely to be a typo or a unit
                    ** confusion than a genuine request for the longest strip
                    ** this can drive, and quietly lighting 2048 pixels would
                    ** hide that. 1 is conspicuous, recoverable, and is what
                    ** the device did before this key existed. */
                    verbatim = false;
                }
            }
            else
            {
                /* Present but not a number -- a string "60", or null, or an
                ** object. Refused rather than coerced. */
                verbatim = false;
            }
        }
    }

    /* ---- rgbw ----------------------------------------------------------- */
    {
        cJSON *const cj_w = cJSON_GetObjectItem(__FUNCTION__, cj_device, ezlopi_rgbw_str);

        if (NULL != cj_w)
        {
            if (0 != (cj_w->type & (cJSON_True | cJSON_False)))
            {
                out->has_white = (0 != (cj_w->type & cJSON_True));
            }
            else if (0 != (cj_w->type & cJSON_Number))
            {
                /* A number is accepted here, unlike for pixel_count, because
                ** the app has historically sent flags as 0/1 and this is a
                ** two-valued field where any non-zero can only mean true.
                ** There is no range to get wrong. */
                out->has_white = (0.0 != cj_w->valuedouble);
            }
            else
            {
                verbatim = false;
            }
        }
    }

    /* ---- color_order ---------------------------------------------------- */
    {
        cJSON *const cj_o = cJSON_GetObjectItem(__FUNCTION__, cj_device, ezlopi_color_order_str);

        if (NULL != cj_o)
        {
            e_aled_order_t parsed = ALED_ORDER_GRB;

            if ((0 != (cj_o->type & cJSON_String)) &&
                EZPI_aledcfg_order_from_str(cj_o->valuestring, &parsed))
            {
                out->order = parsed;
            }
            else
            {
                /* Not a string, or not a permutation of r/g/b. Deliberately
                ** NOT falling back to a numeric index: "2" and "brg" would
                ** then both be accepted for the same field, and the enum's
                ** numbering is an implementation detail that a stored config
                ** must not depend on. */
                verbatim = false;
            }
        }
    }

    return verbatim;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
