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

    /** @brief Which LED family's bit timings to clock the data out with.
    **
    ** WHY THIS IS NOT led_strip_type_t
    **
    ** Same reason s_aledcfg_t is not led_strip_t: led_strip.h includes
    ** driver/rmt.h and driver/gpio.h, and this component's whole value is that
    ** a host compiler can put a few thousand configurations through it. The
    ** driver owns the one switch that maps this to its own enum.
    **
    ** SK6812 IS ZERO, AND THAT IS THE POINT
    **
    ** Not alphabetical and not the driver's order -- led_strip_type_t starts at
    ** LED_STRIP_WS2812. SK6812 is first here because
    ** device_0009_other_RMT_SK6812.c hardcoded `type = LED_STRIP_SK6812`, so
    ** SK6812 is what every strip already in the field is being driven as. A
    ** zero-initialised s_aledcfg_t, a memset, or a resolver that returns early
    ** therefore all mean "behave as the shipped firmware did" rather than
    ** "switch this install to WS2812 timings on the next update". e_aled_order_t
    ** puts ALED_ORDER_GRB at 0 for exactly the same reason.
    **
    ** WHAT CHANGING THIS ACTUALLY CHANGES, IN THIS FIRMWARE
    **
    ** Only the bit timings. The vendored driver's per-type table carries a
    ** colour order as well as timings, but `led_params[strip->type].order` is
    ** read at exactly one site -- inside led_strip_set_pixel()
    ** (led_strip.c:259) -- and this firmware never calls it: __fill_strip()
    ** packs the frame buffer itself through EZPI_aled_fill(), using the stored
    ** `color_order`. So selecting APA106, whose table row says ORDER_RGB, does
    ** NOT flip a strip from GRB to RGB here. That is worth stating because it
    ** is the opposite of what the vendored API's own documentation implies, and
    ** it is what makes `led_chip` and `color_order` genuinely independent keys.
    **
    ** The four rows are the four the driver has, with the timings it uses
    ** (led_strip.c:168-171). They are not interchangeable: SK6812 encodes a 1
    ** bit as 600ns high, WS2812 as 1000ns.
    */
    typedef enum e_aled_chip
    {
        ALED_CHIP_SK6812 = 0, /* t1h 600ns  -- the shipped default, see above */
        ALED_CHIP_WS2812,     /* t1h 1000ns -- also WS2812B, the common reel */
        ALED_CHIP_APA106,     /* t1h 1360ns                                  */
        ALED_CHIP_SM16703,    /* t1h 1360ns, t0l 900ns                       */
        ALED_CHIP_MAX         /* not a valid chip: bound for validation      */
    } e_aled_chip_t;

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
        e_aled_chip_t chip;      /**< bit timings; always < ALED_CHIP_MAX */
        e_aled_order_t order;    /**< byte order on the wire */
    } s_aledcfg_t;

    /** @brief What a config that says nothing resolves to.
    **
    ** ONE pixel, no white channel, GRB, SK6812 timings. That is not a
    ** neutral-looking choice, it is the behaviour the shipped driver already
    ** has: length hardcoded to 1, `is_rgbw` left zero by the memset, GRB
    ** because that is what led_strip does for SK6812 regardless, and SK6812
    ** because device_0009 assigned `type = LED_STRIP_SK6812` as a literal.
    **
    ** Matching it exactly is the point. Every strip already provisioned in the
    ** field has a stored config with none of these keys in it, and after this
    ** change those installs must behave byte-for-byte as they did before --
    ** including the ones whose owners have already worked around the one-pixel
    ** limit. A "better" default would be a silent behaviour change pushed to
    ** working hardware by a firmware update.
    **
    ** For the chip in particular, "better" would be tempting and wrong: WS2812B
    ** is the more common part, so defaulting to it would suit more NEW installs
    ** and would re-time every EXISTING one. Which strips are already out there
    ** is the only input that matters here.
    */
#define ALEDCFG_DEFAULT_PIXELS 1u
#define ALEDCFG_MIN_PIXELS 1u
#define ALEDCFG_DEFAULT_CHIP ALED_CHIP_SK6812

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
     * Reads the optional keys `pixel_count`, `rgbw`, `color_order` and
     * `led_chip` from @p cj_device. Anything missing, malformed, out of range
     * or not of the expected JSON type falls back to the documented default for
     * that field *individually* -- one bad key does not discard the others.
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
     * @brief   Parse an LED family name such as "ws2812" into an enum.
     *
     * Case-insensitive. The bound is read the way EZPI_aledcfg_order_from_strn()
     * reads it -- up to @p bound bytes, stopping early at a NUL -- and for the
     * same reason: a config that reached the driver through
     * cJSON_ParseWithRef() has a valuestring that ends at a quote, not a NUL.
     * There is no NUL-terminated variant of this function, deliberately. The
     * only caller is the resolver, which always has str_value_len, and the
     * colour-order pair exists only because EZPI_aledcfg_order_from_str()
     * predates the discovery that the bounded form was the one needed.
     *
     * ACCEPTED NAMES
     *
     *   "sk6812"   ALED_CHIP_SK6812
     *   "ws2812"   ALED_CHIP_WS2812
     *   "ws2812b"  ALED_CHIP_WS2812   <-- an alias, see below
     *   "apa106"   ALED_CHIP_APA106
     *   "sm16703"  ALED_CHIP_SM16703
     *
     * "ws2812b" is the one alias, and it is here because it is what is printed
     * on the reel. The driver has no separate WS2812B row -- the B is a
     * packaging revision, not a different protocol -- so refusing the name
     * would send the most common strip on the market back to the SK6812
     * default, which is a plausible-looking 600ns-vs-1000ns mis-time rather
     * than an obvious failure. Nothing else is aliased: "ws2813" and "ws2815"
     * are NOT accepted, because they are close enough in timing that guessing
     * would be defensible and far enough that being wrong would be invisible.
     *
     * @param   str    need not be NUL-terminated; may be NULL.
     * @param   bound  how many bytes of @p str may be read. Pass cJSON's
     *                 str_value_len for a parsed item.
     * @param   out    set only when the function returns true.
     * @return  true if @p str is one of the accepted names.
     */
    bool EZPI_aledcfg_chip_from_strn(const char *str, size_t bound, e_aled_chip_t *out);

    /**
     * @brief   The canonical name of a chip, for a log line.
     *
     * This exists because the driver's one warning about a config it could not
     * take verbatim is the only place a wrong `led_chip` becomes visible to
     * anyone, and a warning reading `chip=1` is a riddle rather than a
     * diagnosis. The chip is the field where that matters most: a wrong length
     * or a wrong order is obvious on sight, wrong timings are not.
     *
     * Canonical means the first name in the table for that chip, so
     * ALED_CHIP_WS2812 is "ws2812" and never the "ws2812b" alias that may have
     * selected it. Parsing a name and printing the result is therefore not
     * always byte-identical to the input, and deliberately so -- the log should
     * say which family is being driven, not echo what was typed.
     *
     * @param   chip  any value; need not be a valid enumerator.
     * @return  a NUL-terminated static string, never NULL. An out-of-range
     *          value gives "?" rather than indexing past the table -- a logging
     *          helper that can fault is worse than useless, because it fires
     *          only on the path that was already going wrong.
     */
    const char *EZPI_aledcfg_chip_name(e_aled_chip_t chip);

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
