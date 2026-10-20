/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    ezlopi_util_aled.h
 * @brief   The colour arithmetic behind an addressable LED strip: brightness
 *          scaling, HSV to RGB, and packing pixels into the byte order a
 *          particular chip expects on the wire.
 *
 * WHY THIS IS A SEPARATE, DEPENDENCY-FREE COMPONENT
 *
 * Everything here is arithmetic on a buffer. No GPIO, no RMT, no clock, no
 * allocation. That is deliberate and it is the same split that
 * ezlopi-core-scenes-suntime was carved out for: the part that can be got
 * wrong is separated from the part that needs a board, so the first part can
 * be swept exhaustively by tests/host under ASan and UBSan while the second
 * stays as small as possible.
 *
 * It matters more here than usual, because a wrong colour is the hardest class
 * of defect to notice on this kind of device. A strip with its byte order
 * swapped lights up, responds to commands and looks like a working product --
 * it just shows green where red was asked for. Nothing fails, nothing logs,
 * and no amount of staring at a board distinguishes "the driver is wrong" from
 * "the strip is a different chip than I thought". A table of inputs and
 * expected outputs does distinguish them.
 *
 * CLEAN-ROOM, AND WHAT THAT MEANS CONCRETELY
 *
 * No code, no tables and no effect implementations were taken from WLED
 * (EUPL-1.2, Christian Schwinne), from WS2812FX (MIT, Harm Aldick), or from
 * hoene/esp32-led-controller (AGPL-3.0, Symonics). This file is written from
 * the published descriptions of the WS2812 wire format and from the standard
 * definition of the HSV colour space. That keeps the whole component under
 * EASL with the rest of the tree, with no licence to reconcile -- which is the
 * reason it is written rather than vendored.
 *
 * The one thing deliberately shared with WLED is its *JSON API shape*, and
 * that is a wire contract rather than code: a format can be implemented
 * independently, an implementation cannot be copied. That belongs to the HTTP
 * layer and not to this file.
 */

#ifndef _EZLOPI_UTIL_ALED_H_
#define _EZLOPI_UTIL_ALED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief One pixel, as the caller thinks of it: always in R,G,B,W order
     *        regardless of what the chip wants on the wire.
     *
     * Keeping the caller-facing order fixed is the point. Effects write r/g/b
     * and never need to know the strip's byte order; exactly one function
     * (EZPI_aled_pack) knows it, so there is exactly one place for that
     * knowledge to be wrong, and one place to test.
     *
     * `w` is the separate white channel on RGBW strips (SK6812-RGBW and
     * similar). It is carried for every pixel even on RGB strips, costing one
     * byte each in RAM, because the alternative -- two pixel types, or a
     * packed buffer whose stride depends on configuration -- puts a
     * stride calculation into every effect. That is the kind of arithmetic
     * this component exists to keep in one place.
     */
    typedef struct s_aled_rgbw
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t w;
    } s_aled_rgbw_t;

    /**
     * @brief The byte order a chip expects, named by the order itself rather
     *        than by chip.
     *
     * Named by order and not by part number on purpose. "WS2812" is not one
     * answer -- the same part number has shipped in more than one colour order
     * over the years, and a strip bought as WS2812B may be any of these. A
     * configuration that says GRB is checkable against a board by eye; one
     * that says WS2812 is a guess about the factory.
     *
     * GRB is first because it is the common case, and so is the value a
     * zeroed configuration struct gets. That is a deliberate choice of
     * default: of the orders below it is the one most strips use, so a
     * configuration that forgets to name an order is most likely right rather
     * than most likely wrong.
     */
    typedef enum e_aled_order
    {
        ALED_ORDER_GRB = 0, /* WS2812, WS2812B, SK6812 -- the usual case */
        ALED_ORDER_RGB,     /* WS2811, some WS2813 batches              */
        ALED_ORDER_BRG,
        ALED_ORDER_RBG,
        ALED_ORDER_GBR,
        ALED_ORDER_BGR,
        ALED_ORDER_MAX /* not a valid order: bound for validation */
    } e_aled_order_t;

    /**
     * @brief Scale an 8-bit channel by an 8-bit factor, 255 meaning "unchanged".
     *
     * @param   value   the channel
     * @param   scale   0 for off, 255 for full
     * @return  value * scale / 255, rounded down
     *
     * Exactly `(value * scale) / 255`, and the division is the whole reason
     * this is a named function rather than an expression at each call site.
     * The obvious fast version is `(value * scale) >> 8`, which is wrong at
     * the top of the range: 255 scaled by 255 gives 254, so a strip asked for
     * full white never reaches it and every channel is short by one count at
     * full brightness. That is invisible on a board and obvious in a test, so
     * the test asserts the identity `scale8(v, 255) == v` for all 256 values
     * rather than sampling it.
     *
     * Rounding down rather than to nearest, which costs at most one count and
     * buys a guarantee worth more: the result is never above `value`, so
     * applying brightness can never make a channel overflow or brighten.
     */
    uint8_t EZPI_aled_scale8(uint8_t value, uint8_t scale);

    /**
     * @brief Convert HSV to RGB, all three channels 8-bit.
     *
     * @param   h   hue, 0..255 wrapping (not 0..359)
     * @param   s   saturation, 0 for grey, 255 for fully saturated
     * @param   v   value/brightness, 0 for black, 255 for full
     * @return  the colour, with `w` always 0
     *
     * Hue is 0..255 and not 0..359 so that it wraps for free on a uint8_t.
     * Every effect that sweeps a hue does so by adding to it, and a 0..359
     * range needs a conditional subtract at every such site -- which is a
     * branch each effect author can forget, in return for an angular
     * resolution no LED resolves anyway.
     *
     * Two invariants hold by construction and are asserted exhaustively over
     * all 16,777,216 (h,s,v) triples in tests/host/test_aled.c:
     *
     *   max(r,g,b) == v            -- that is the definition of "value"
     *   s == 0  implies  r == g == b == v
     *
     * They are worth stating as invariants rather than as a table of expected
     * colours because the usual way to get this function wrong is at a sector
     * boundary -- hue is split into six arcs, and an off-by-one in the arc
     * arithmetic produces a colour that is wrong only within a few counts of
     * one sixth of the wheel. A table of the six primaries passes that.
     * `max(r,g,b) == v` does not.
     *
     * `w` is zeroed rather than left alone: a hue has no white component, and
     * on an RGBW strip the alternative is that an effect's colour silently
     * inherits whatever white the previous frame left in the buffer.
     */
    s_aled_rgbw_t EZPI_aled_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v);

    /**
     * @brief Number of wire bytes a strip of `n_pixels` needs.
     *
     * @param   n_pixels    pixel count
     * @param   has_white   true for RGBW strips (4 bytes/pixel), false for RGB (3)
     * @return  the byte count, or 0 if it would overflow a size_t
     *
     * Returns 0 on overflow rather than a wrapped product. A caller that
     * multiplies pixel count by bytes-per-pixel itself, with a count that came
     * from a configuration JSON, is one `malloc(n * 3)` away from a buffer
     * smaller than the strip it is about to describe -- and the write that
     * follows is off the end of the heap. 0 is unambiguous: no strip needs
     * zero bytes, so a caller that checks for it cannot mistake the error for
     * a valid answer.
     */
    size_t EZPI_aled_wire_len(size_t n_pixels, bool has_white);

    /**
     * @brief Pack a pixel buffer into wire order, applying brightness.
     *
     * @param   pixels      the pixels, in caller order (r,g,b,w)
     * @param   n_pixels    how many
     * @param   order       the chip's byte order
     * @param   brightness  applied to every channel via EZPI_aled_scale8
     * @param   has_white   true to emit the white byte after the three colours
     * @param   out         destination
     * @param   out_cap     how many bytes `out` can hold
     * @return  bytes written, or 0 if nothing was written
     *
     * Returns 0 -- having written nothing at all -- when `out` is too small,
     * when either pointer is NULL, or when `order` is not one of the six. It
     * does not write what fits and report a short count, and that is the
     * deliberate part: a partially packed frame is a frame, and the strip would
     * display it. A caller that ignores the return value sees the previous
     * frame held rather than a corrupted one, which is both safer and far more
     * noticeable.
     *
     * Brightness is applied here rather than by the effects, so that the
     * pixel buffer always holds the colour that was asked for. An effect that
     * dimmed its own output would lose the low bits of a dim colour
     * permanently, and raising the brightness afterwards would not bring them
     * back -- the strip would stay muddy until the effect next recomputed from
     * scratch.
     */
    size_t EZPI_aled_pack(const s_aled_rgbw_t *pixels,
                          size_t n_pixels,
                          e_aled_order_t order,
                          uint8_t brightness,
                          bool has_white,
                          uint8_t *out,
                          size_t out_cap);

    /**
     * @brief Fill a whole strip with one colour, in wire order.
     *
     * @param   color       the colour, in caller order (r,g,b,w)
     * @param   n_pixels    how many pixels to write
     * @param   order       the chip's byte order
     * @param   brightness  applied to every channel via EZPI_aled_scale8
     * @param   has_white   true to emit the white byte after the three colours
     * @param   out         destination
     * @param   out_cap     how many bytes `out` can hold
     * @return  bytes written, or 0 if nothing was written
     *
     * Same contract as EZPI_aled_pack: all or nothing, and 0 for a NULL
     * destination, an order outside the six, no pixels, or a destination too
     * small.
     *
     * This exists rather than leaving callers to call EZPI_aled_pack with an
     * n-pixel array because a solid fill is what every caller in this firmware
     * actually does, and the array it would need is the problem: at the
     * 2048-pixel cap it is an 8 KB temporary, allocated to hold 2048 copies of
     * four bytes, on a heap where the strip's own frame buffer is the thing
     * most likely to fail to allocate.
     *
     * The order permutation is not reimplemented here. One pixel goes through
     * EZPI_aled_pack and the remaining pixels are byte copies of the result,
     * so the knowledge of which channel goes first exists in exactly one
     * function in this component. A second loop that permuted channels itself
     * would be a second place for the order to be wrong, and the two would
     * disagree only for the orders nobody tested.
     */
    size_t EZPI_aled_fill(s_aled_rgbw_t color,
                          size_t n_pixels,
                          e_aled_order_t order,
                          uint8_t brightness,
                          bool has_white,
                          uint8_t *out,
                          size_t out_cap);

    /**
     * @brief Read the first pixel back out of a wire buffer.
     *
     * @param   wire        the packed bytes
     * @param   wire_len    how many bytes `wire` holds
     * @param   order       the chip's byte order
     * @param   has_white   true if the buffer carries a white byte per pixel
     * @param   out         the colour, in caller order (r,g,b,w)
     * @return  true if `out` was written
     *
     * The inverse of the permutation EZPI_aled_pack applies, and it exists for
     * one caller: reporting a strip's current colour to the cloud. The colour
     * is not stored anywhere else -- the wire buffer *is* the state -- so
     * answering "what colour is this strip" means reading it back, and reading
     * it back means knowing the byte order. Before this existed the only
     * read-back in the firmware indexed the buffer as green-red-blue with
     * three integer literals, which is correct for the one order it was
     * written for and silently reports the wrong colour for the other five.
     *
     * **Brightness is not undone, and cannot be.** EZPI_aled_scale8 is lossy,
     * so this returns the channel values as they sit on the wire, already
     * scaled. For the caller that matters this is the right answer rather than
     * a limitation: a hub reports colour and brightness as separate items, and
     * the brightness is read from its own field. Stated here because
     * `unpack(fill(c)) == c` is a property that holds at brightness 255 and
     * only there, and a reader who assumed otherwise would be building a
     * round-trip on sand.
     *
     * `w` is set to 0 rather than left alone when `has_white` is false, so a
     * caller cannot read a white channel that was never on the wire.
     */
    bool EZPI_aled_unpack_first(const uint8_t *wire,
                                size_t wire_len,
                                e_aled_order_t order,
                                bool has_white,
                                s_aled_rgbw_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _EZLOPI_UTIL_ALED_H_ */
