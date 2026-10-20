/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    ezlopi_util_aled.c
 * @brief   Colour arithmetic for addressable LED strips. See the header for
 *          why this is a dependency-free component and what "clean-room"
 *          means for it.
 */

#include <string.h>

#include "ezlopi_util_aled.h"

/* Which of r,g,b goes out first, second and third, indexed by e_aled_order_t.
** The values index a three-byte array in r,g,b order, so {1,0,2} means "green,
** then red, then blue" -- GRB.
**
** A table rather than a switch with six arms, because the table can be checked
** against a property the switch cannot: every row must be a permutation of
** {0,1,2}. tests/host/test_aled.c asserts exactly that over all six rows,
** which catches the transcription error this kind of table invites -- a
** duplicated index, say {1,0,1}, which drops a channel and doubles another.
** Six hand-written switch arms have no such single property to check.
**
** Row order must match the enum. The test pins that too, by checking that the
** GRB row really does emit green first rather than trusting the comment. */
static const uint8_t k_order_idx[ALED_ORDER_MAX][3] = {
    /* ALED_ORDER_GRB */ {1, 0, 2},
    /* ALED_ORDER_RGB */ {0, 1, 2},
    /* ALED_ORDER_BRG */ {2, 0, 1},
    /* ALED_ORDER_RBG */ {0, 2, 1},
    /* ALED_ORDER_GBR */ {1, 2, 0},
    /* ALED_ORDER_BGR */ {2, 1, 0},
};

uint8_t EZPI_aled_scale8(uint8_t value, uint8_t scale)
{
    /* The product is at most 255*255 = 65025, so uint16_t cannot overflow and
    ** the cast is what keeps the multiplication out of a possibly-16-bit int.
    ** Dividing by 255 rather than shifting by 8 is the entire point of the
    ** function: see the header. */
    return (uint8_t)(((uint16_t)value * (uint16_t)scale) / 255u);
}

s_aled_rgbw_t EZPI_aled_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v)
{
    /* Zero-initialised, even though the switch below assigns all three colour
    ** channels on every arm. Belt and braces, for a specific reason: a seventh
    ** sector arm added later that forgot one channel would otherwise return
    ** uninitialised stack, and an uninitialised read is the one defect class
    ** this suite cannot catch -- AddressSanitizer does not model it, and
    ** MemorySanitizer, which does, cannot be combined with ASan. A structural
    ** guarantee is worth more than a test here because no test is available.
    **
    ** A consequence worth stating: it makes deleting `out.w = 0` below a no-op.
    ** The test's "a hue never sets white" check is therefore satisfied twice
    ** over by construction, and tests/host cannot distinguish the two. That is
    ** recorded in test_aled.c rather than left for someone to discover by
    ** mutating this line and finding nothing fails. */
    s_aled_rgbw_t out = {0, 0, 0, 0};

    /* Six sectors across the full 0..255 hue range, computed as h*6 and then
    ** split into a sector index and a position within the sector.
    **
    ** h*6 >> 8 rather than h/43, which is the version this was first written
    ** as. 256/6 is 42.67, so /43 makes every sector 43 wide except the last,
    ** which is 41, and the position within a sector then spans 0..252 instead
    ** of 0..255. The hue wheel comes out very slightly compressed and the
    ** sector joins are discontinuous by up to three counts. h*6 has neither
    ** problem: the index is exactly h*6/256 and the remainder exactly spans a
    ** byte. */
    const uint16_t scaled_hue = (uint16_t)h * 6u;
    const uint8_t sector = (uint8_t)(scaled_hue >> 8);  /* 0..5 */
    const uint8_t offset = (uint8_t)(scaled_hue & 0xFFu); /* 0..255 */

    /* The three non-peak channel values. Each is v scaled by something no
    ** greater than 255, so each is no greater than v -- which is what makes
    ** max(r,g,b) == v true by construction, since every sector below assigns
    ** v to exactly one channel.
    **
    **   p  the channel that is fully desaturated (the sector's "low" channel)
    **   q  falling across the sector
    **   t  rising across the sector
    */
    const uint8_t p = EZPI_aled_scale8(v, (uint8_t)(255u - s));
    const uint8_t q = EZPI_aled_scale8(v, (uint8_t)(255u - EZPI_aled_scale8(s, offset)));
    const uint8_t t = EZPI_aled_scale8(v, (uint8_t)(255u - EZPI_aled_scale8(s, (uint8_t)(255u - offset))));

    /* No `if (0 == s)` fast path, deliberately. With s == 0 every scale8(s, x)
    ** above is 0, so p, q and t all collapse to scale8(v, 255) == v and every
    ** sector returns grey of exactly v on its own. A special case would be a
    ** second implementation of the greyscale answer, and the test asserts
    ** r == g == b == v for all 256*256 (h,v) pairs at s == 0 -- which proves
    ** the general path gets it right and would not notice a special case that
    ** disagreed with it. */
    switch (sector)
    {
    case 0:
        out.r = v;
        out.g = t;
        out.b = p;
        break;
    case 1:
        out.r = q;
        out.g = v;
        out.b = p;
        break;
    case 2:
        out.r = p;
        out.g = v;
        out.b = t;
        break;
    case 3:
        out.r = p;
        out.g = q;
        out.b = v;
        break;
    case 4:
        out.r = t;
        out.g = p;
        out.b = v;
        break;
    default: /* sector 5; scaled_hue >> 8 cannot exceed 5 for a uint8_t hue */
        out.r = v;
        out.g = p;
        out.b = q;
        break;
    }

    /* A hue carries no white. Zeroed rather than untouched -- see the header. */
    out.w = 0;

    return out;
}

size_t EZPI_aled_wire_len(size_t n_pixels, bool has_white)
{
    const size_t bytes_per_pixel = has_white ? 4u : 3u;

    if (0u == n_pixels)
    {
        return 0u;
    }

    /* Checked before multiplying, not after. Testing the product for
    ** plausibility afterwards cannot work: unsigned multiplication wraps
    ** silently, and a wrapped product is a small number that looks entirely
    ** reasonable. A pixel count of SIZE_MAX/3 + 1 would yield 1 on a 32-bit
    ** target. */
    if (n_pixels > (SIZE_MAX / bytes_per_pixel))
    {
        return 0u;
    }

    return n_pixels * bytes_per_pixel;
}

size_t EZPI_aled_pack(const s_aled_rgbw_t *pixels,
                      size_t n_pixels,
                      e_aled_order_t order,
                      uint8_t brightness,
                      bool has_white,
                      uint8_t *out,
                      size_t out_cap)
{
    size_t needed = 0u;
    size_t written = 0u;
    size_t i = 0u;

    if ((NULL == pixels) || (NULL == out))
    {
        return 0u;
    }

    /* `order` arrives from a configuration JSON by way of an int, so the
    ** enum's type says nothing about its value -- C does not constrain an
    ** enum-typed variable to the enumerated values. Unchecked, an order of 9
    ** indexes k_order_idx out of bounds and reads whatever follows it in
    ** .rodata as channel indices, which then index a 3-byte array with
    ** arbitrary values. Both reads are off the end; neither would fault.
    **
    ** Comparing against ALED_ORDER_MAX rather than listing the six, so that
    ** adding a seventh order to the enum and a seventh row to the table is
    ** sufficient. A `>= 0` test is deliberately absent: e_aled_order_t is an
    ** unsigned-compatible enum here and GCC warns that such a test is always
    ** true. The cast to unsigned makes a negative value arriving through an
    ** int-shaped cloud field fail the upper bound instead. */
    if ((unsigned)order >= (unsigned)ALED_ORDER_MAX)
    {
        return 0u;
    }

    needed = EZPI_aled_wire_len(n_pixels, has_white);
    if ((0u == needed) || (needed > out_cap))
    {
        /* Nothing written, and the caller keeps whatever frame it last sent.
        ** See the header for why this is not a short write. */
        return 0u;
    }

    for (i = 0u; i < n_pixels; i++)
    {
        /* Indexed r,g,b so the order table can permute them. The white
        ** channel is not in this array: it is never permuted, and on every
        ** RGBW chip this component targets it follows the three colours. */
        const uint8_t channel[3] = {
            EZPI_aled_scale8(pixels[i].r, brightness),
            EZPI_aled_scale8(pixels[i].g, brightness),
            EZPI_aled_scale8(pixels[i].b, brightness),
        };

        out[written++] = channel[k_order_idx[order][0]];
        out[written++] = channel[k_order_idx[order][1]];
        out[written++] = channel[k_order_idx[order][2]];

        if (has_white)
        {
            out[written++] = EZPI_aled_scale8(pixels[i].w, brightness);
        }
    }

    return written;
}

size_t EZPI_aled_fill(s_aled_rgbw_t color,
                      size_t n_pixels,
                      e_aled_order_t order,
                      uint8_t brightness,
                      bool has_white,
                      uint8_t *out,
                      size_t out_cap)
{
    const size_t stride = has_white ? 4u : 3u;
    size_t needed = 0u;
    size_t at = 0u;

    if (NULL == out)
    {
        return 0u;
    }

    needed = EZPI_aled_wire_len(n_pixels, has_white);
    if ((0u == needed) || (needed > out_cap))
    {
        return 0u;
    }

    /* The first pixel goes through the real packer, which is what validates
    ** `order` and applies both the permutation and the brightness. Capacity is
    ** given as `stride` and not as `out_cap`, so a packer that ever wrote more
    ** than one pixel's worth here would be refused by its own bound rather
    ** than scribble over the rest of the frame. */
    if (stride != EZPI_aled_pack(&color, 1u, order, brightness, has_white, out, stride))
    {
        return 0u;
    }

    /* Every later pixel is a byte-for-byte copy of the first. `needed` is an
    ** exact multiple of `stride` by construction -- EZPI_aled_wire_len
    ** multiplies by it -- so the last copy lands exactly on the end and this
    ** cannot write a partial pixel. */
    for (at = stride; at < needed; at += stride)
    {
        memcpy(out + at, out, stride);
    }

    return needed;
}

bool EZPI_aled_unpack_first(const uint8_t *wire,
                            size_t wire_len,
                            e_aled_order_t order,
                            bool has_white,
                            s_aled_rgbw_t *out)
{
    const size_t stride = has_white ? 4u : 3u;
    uint8_t channel[3] = {0, 0, 0};

    if ((NULL == wire) || (NULL == out))
    {
        return false;
    }

    /* Same bound, same reason as in EZPI_aled_pack: an order that came from a
    ** configuration JSON is an int wearing an enum's type, and an unchecked
    ** one indexes k_order_idx off its end. */
    if ((unsigned)order >= (unsigned)ALED_ORDER_MAX)
    {
        return false;
    }

    if (wire_len < stride)
    {
        return false;
    }

    /* The inverse permutation, written as the same table read backwards rather
    ** than as a second table. k_order_idx[order][j] says which channel the
    ** byte at wire position j holds, so assigning *into* that channel index
    ** inverts it. A separate inverse table would be six more rows to get
    ** wrong, and wrong in a way that pack/unpack round-trip tests cannot see
    ** if both are wrong consistently. */
    channel[k_order_idx[order][0]] = wire[0];
    channel[k_order_idx[order][1]] = wire[1];
    channel[k_order_idx[order][2]] = wire[2];

    out->r = channel[0];
    out->g = channel[1];
    out->b = channel[2];
    out->w = has_white ? wire[3] : 0u;

    return true;
}
