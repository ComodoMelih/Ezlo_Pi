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
 * @file    ezlopi_util_aledcfg.h
 * @brief   Reads an addressable-LED strip's shape out of a device config.
 * @author
 * @version 0.1
 * @date    October 19th, 2026
 */

/* WHAT PROBLEM THIS SOLVES
**
** EzloPi already onboards an addressable strip. device-0009-other-RMT-SK6812 is
** registered through all four files of the device contract and shows up in the
** onboarding list as "device-rgp_strip-RMT_SK6812". It reports the right cloud
** category (dimmable_light / dimmable_colored / dimmer.outlet), it accepts
** colour and dimmer commands, and it answers them.
**
** It also contains, verbatim:
**
**     dimmer_args->sk6812_strip.length = 1;
**
** and the only configuration keys it reads are gpio1, duty_cycle, freq_hz and
** dev_type. There is no pixel-count key anywhere in the config schema. Since
** led_strip_fill() is then called with that same length, a 60-pixel strip
** wired to a provisioned EzloPi lights its first LED and nothing else. The
** device works, the app shows it working, and 59 LEDs stay dark.
**
** Separately, led_strip's COLOR_SIZE(strip) is `3 + (strip->is_rgbw != 0)`, and
** `is_rgbw` is never assigned anywhere in the firmware -- the whole struct is
** memset to 0 -- so the white channel of an SK6812-RGBW is unreachable too.
**
** So this is a configuration gap rather than a driver gap, and that is why the
** fix is a parser rather than a rewrite.
**
** WHY IT IS A SEPARATE COMPONENT
**
** Following ezlopi-util-devcfgval, which exists for the same reason: the
** judgement about whether a stored configuration is usable is worth making
** somewhere a host test can put a few thousand shapes through it, rather than
** inside a driver that cannot be compiled without an RMT peripheral. What the
** driver then does is one struct assignment.
**
** It is NOT part of ezlopi-util-aled, which has a deliberately empty REQUIRES
** so the colour arithmetic can be swept exhaustively by a plain host compiler.
** This one needs cjext, so it goes next to it rather than in it.
**
** THE RULE THIS FILE FOLLOWS: NEVER RETURN SOMETHING THAT BRICKS THE DEVICE
**
** `led_strip_init()` starts with
**
**     CHECK_ARG(strip && strip->length > 0 && ...);
**
** so a length of 0 does not produce a dim strip, it produces a device that
** fails to initialise. And a length of 0 is easy to arrive at by accident,
** because EZPI_core_cjson_get_value_uint32() assigns `*item_val = 0` before it
** looks the key up, and leaves the 0 there if the key is absent *or* is not a
** JSON number. The obvious way to write the caller --
**
**     uint32_t n = 1;                              <-- looks like a default
**     CJSON_GET_VALUE_UINT32(cj_device, key, n);    <-- n is now 0
**
** -- silently discards the default. That trap is the reason this function
** exists at all, and the reason it is the one that owns the defaults.
**
** So every path through EZPI_aledcfg_from_json() leaves *out holding a config
** that works. The return value says whether the stored document was taken
** verbatim; it never says "I gave you nothing".
*/

#ifndef _EZLOPI_UTIL_ALEDCFG_H_
#define _EZLOPI_UTIL_ALEDCFG_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cjext.h"
#include "ezlopi_util_aled.h"

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

    /** @brief The shape a strip is configured to be, with every field usable.
    **
    ** Deliberately not led_strip_t: that type carries a GPIO, an RMT channel
    ** and a heap pointer, none of which are a question about the stored
    ** configuration, and including it would drag an IDF header in here.
    */
    typedef struct s_aledcfg
    {
        size_t pixel_count;      /**< always >= ALEDCFG_MIN_PIXELS */
        bool has_white;          /**< a 4th channel per pixel (SK6812-RGBW) */
        e_aled_order_t order;    /**< byte order on the wire */
    } s_aledcfg_t;

    /** @brief What a config that says nothing resolves to.
    **
    ** ONE pixel, no white channel, GRB. That is not a neutral-looking choice,
    ** it is the behaviour the shipped driver already has: length hardcoded to
    ** 1, `is_rgbw` left zero by the memset, and GRB because that is what
    ** led_strip does for SK6812 regardless.
    **
    ** Matching it exactly is the point. Every strip already provisioned in the
    ** field has a stored config with none of these keys in it, and after this
    ** change those installs must behave byte-for-byte as they did before --
    ** including the ones whose owners have already worked around the one-pixel
    ** limit. A "better" default would be a silent behaviour change pushed to
    ** working hardware by a firmware update.
    */
#define ALEDCFG_DEFAULT_PIXELS 1u
#define ALEDCFG_MIN_PIXELS 1u

    /** @brief The largest strip this accepts, and where the number comes from.
    **
    ** 2048, derived from the wire protocol rather than picked:
    **
    **   A WS2812/SK6812 bit is 1.25us. An RGBW pixel is 32 bits, so 40us per
    **   pixel, so 2048 pixels is 81.9ms of unbroken transmission -- about 12
    **   frames per second, and that is before the >=50us reset gap. RGB is 24
    **   bits, 30us, 61.4ms, 16fps.
    **
    ** So 2048 is already past the point where the strip stops looking animated,
    ** on a single RMT channel. A cap further up would not buy a usable strip,
    ** it would buy a 32KB allocation that fails on a loaded heap.
    **
    ** For scale, 2048 RGBW pixels is an 8KB buffer from ezlopi_calloc().
    */
#define ALEDCFG_MAX_PIXELS 2048u

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

    /**
     * @brief   Resolve an addressable strip's shape from a device config object.
     *
     * Reads the optional keys `pixel_count`, `rgbw` and `color_order` from
     * @p cj_device. Anything missing, malformed, out of range or not of the
     * expected JSON type falls back to the documented default for that field
     * *individually* -- one bad key does not discard the others.
     *
     * @param   cj_device  one entry of the config's `dev_detail` array. May be
     *                     NULL, in which case @p out gets the full defaults.
     * @param   out        filled on every call, including every failure path
     *                     and including when @p cj_device is NULL. Only ignored
     *                     if it is itself NULL.
     *
     * @return  true  if every key present was understood and used as given
     *                (including the case where none were present at all, which
     *                is a valid configuration meaning "one pixel").
     * @return  false if any value had to be substituted -- a count of 0, a
     *                count above ALEDCFG_MAX_PIXELS, a `color_order` that is
     *                not a permutation of r/g/b, or a key of the wrong JSON
     *                type. @p out is still usable; the caller should log.
     *
     * @note    false is not an error code to propagate. It means "the stored
     *          document disagrees with what I am going to do", which is worth a
     *          log line precisely because the strip will then light up and look
     *          fine at the wrong length.
     */
    bool EZPI_aledcfg_from_json(cJSON *cj_device, s_aledcfg_t *out);

    /**
     * @brief   Parse a colour-order string such as "grb" into an enum.
     *
     * Case-insensitive, and accepts only the six strings that are genuine
     * permutations of r, g and b. "rrg", "rg", "rgba" and "" are all refused
     * rather than partially matched, because a near-miss here is exactly the
     * defect ezlopi-util-aled exists to make visible: the strip lights up and
     * shows the wrong colours.
     *
     * @param   str    NUL-terminated; may be NULL.
     * @param   out    set only when the function returns true.
     * @return  true if @p str is one of the six permutations.
     *
     * @warning Do NOT pass a cJSON valuestring to this function. A string
     *          parsed by cJSON_ParseWithRef() points into the JSON payload and
     *          is terminated by a quote, not a NUL, so this function sees
     *          "grb\"}]}" and refuses it. Use
     *          EZPI_aledcfg_order_from_strn() with str_value_len instead.
     *          That is not hypothetical: it is the defect this note was added
     *          for, and it refused every colour order on a real board.
     */
    bool EZPI_aledcfg_order_from_str(const char *str, e_aled_order_t *out);

    /**
     * @brief   As EZPI_aledcfg_order_from_str(), for a string that is not
     *          necessarily NUL-terminated.
     *
     * The length is taken the way "%.*s" takes it: up to @p bound bytes,
     * stopping early at a NUL. That single rule is correct for all three ways
     * cjext builds a string, which is why the bound is not required to be the
     * exact length:
     *
     *   - cJSON_ParseWithRef()  str_value_len is exact, and valuestring points
     *                           into the payload with no NUL to stop at.
     *   - cJSON_Parse()         str_value_len is an *allocation* size, strlen
     *                           + 1 for a string with no escapes, and the NUL
     *                           is what stops the scan.
     *   - cJSON_CreateString()  str_value_len is strlen(), so exact.
     *
     * The same reasoning, with the measurements behind it, is recorded at
     * length in ezlopi_cloud_mqtt_config.c's __take_field().
     *
     * @param   str    need not be NUL-terminated; may be NULL.
     * @param   bound  how many bytes of @p str may be read. Pass
     *                 cJSON's str_value_len for a parsed item.
     * @param   out    set only when the function returns true.
     * @return  true if @p str is one of the six permutations.
     */
    bool EZPI_aledcfg_order_from_strn(const char *str, size_t bound, e_aled_order_t *out);

    /**
     * @brief   The byte count a resolved config needs on the wire.
     *
     * Thin wrapper over EZPI_aled_wire_len(), which carries the overflow check.
     * Kept as a named function so the driver's allocation and this component's
     * bound are provably the same arithmetic rather than two copies of it.
     *
     * @return  0 only if the product would wrap, which ALEDCFG_MAX_PIXELS
     *          already makes unreachable -- the check stays so that raising
     *          that cap cannot quietly introduce a wrap.
     */
    size_t EZPI_aledcfg_wire_len(const s_aledcfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_UTIL_ALEDCFG_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
