/* Host tests for the addressable-LED colour arithmetic in
 * ezlopi-util/ezlopi-util-aled.
 *
 * WHY THESE ARE EXHAUSTIVE RATHER THAN A TABLE OF COLOURS
 *
 * A wrong colour is the hardest defect to see on this device. A strip with its
 * byte order permuted, or its brightness scaled by (v*s)>>8 instead of
 * (v*s)/255, lights up and responds to every command -- it just shows the
 * wrong colour, or is one count short of full white. Nothing logs and nothing
 * crashes. The only instrument that distinguishes "the driver is wrong" from
 * "this strip is a different chip than I assumed" is a stated expected output.
 *
 * And the usual way to get HSV wrong is at a sector boundary: hue is six arcs,
 * and an off-by-one in the arc arithmetic is wrong only within a few counts of
 * one sixth of the wheel. A table of the six primaries passes that defect
 * cheerfully. So the sweep asserts the two *definitional* invariants instead,
 * over every one of the 16,777,216 (h,s,v) triples:
 *
 *     max(r,g,b) == v          -- that is what "value" means
 *     s == 0  =>  r == g == b  -- that is what "saturation" means
 *
 * Those cannot be satisfied by an implementation with a broken sector.
 *
 * Run: make -C tests/host build/test_aled && tests/host/build/test_aled
 */

#include <stdio.h>
#include <string.h>

#include "ezlopi_util_aled.h"

static int checks = 0;
static int failures = 0;
#define CHECK(cond, what)                                        \
    do                                                           \
    {                                                            \
        checks++;                                                \
        if (!(cond))                                             \
        {                                                        \
            failures++;                                          \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        }                                                        \
    } while (0)

/* ------------------------------------------------------------------------- *
 * 1. EZPI_aled_scale8, swept over all 65,536 (value, scale) pairs.
 *
 * The identity at scale==255 is the load-bearing one: it is exactly what
 * `(value * scale) >> 8` fails, and it fails it for all 255 non-zero values at
 * once, so this check cannot be passed by an implementation using the shift.
 * ------------------------------------------------------------------------- */
static void test_scale8_over_its_whole_domain(void)
{
    int identity_violations = 0;
    int zero_violations = 0;
    int brighten_violations = 0;
    int monotonic_violations = 0;
    char what[160];

    for (int v = 0; v <= 255; v++)
    {
        uint8_t prev = 0;

        if (EZPI_aled_scale8((uint8_t)v, 255) != (uint8_t)v)
        {
            identity_violations++;
        }
        if (0 != EZPI_aled_scale8((uint8_t)v, 0))
        {
            zero_violations++;
        }
        if (0 != EZPI_aled_scale8(0, (uint8_t)v))
        {
            zero_violations++;
        }

        for (int s = 0; s <= 255; s++)
        {
            const uint8_t got = EZPI_aled_scale8((uint8_t)v, (uint8_t)s);

            /* Scaling must never brighten: the result is a dimmed channel, so
            ** it cannot exceed the channel it came from. This is what makes it
            ** safe to apply brightness to an already-full channel. */
            if (got > (uint8_t)v)
            {
                brighten_violations++;
            }
            /* Non-decreasing in scale, for fixed value. A rounding scheme that
            ** is not monotonic makes a brightness slider visibly jump
            ** backwards at some positions. */
            if (got < prev)
            {
                monotonic_violations++;
            }
            prev = got;
        }
    }

    snprintf(what, sizeof(what),
             "scale8(v, 255) == v for all 256 values; %d violation(s) -- this is "
             "the check that (v*s)>>8 cannot pass", identity_violations);
    CHECK(0 == identity_violations, what);

    snprintf(what, sizeof(what),
             "scale8 by 0, and of 0, is 0; %d violation(s)", zero_violations);
    CHECK(0 == zero_violations, what);

    snprintf(what, sizeof(what),
             "scale8 never brightens across all 65536 pairs; %d violation(s)",
             brighten_violations);
    CHECK(0 == brighten_violations, what);

    snprintf(what, sizeof(what),
             "scale8 is monotonic in scale across all 65536 pairs; %d violation(s)",
             monotonic_violations);
    CHECK(0 == monotonic_violations, what);

    /* Two spot values, so that a reader can see what the function does without
     * deriving it from the invariants above. */
    CHECK(128 == EZPI_aled_scale8(255, 128), "full channel at half brightness is 128");
    CHECK(255 == EZPI_aled_scale8(255, 255), "full channel at full brightness is 255");
}

/* ------------------------------------------------------------------------- *
 * 2. The HSV invariants, over the entire input space.
 * ------------------------------------------------------------------------- */
static void test_hsv_invariants_over_the_whole_cube(void)
{
    long peak_violations = 0;
    long grey_violations = 0;
    long white_violations = 0;
    long black_violations = 0;
    char what[200];

    for (int h = 0; h <= 255; h++)
    {
        for (int s = 0; s <= 255; s++)
        {
            for (int v = 0; v <= 255; v++)
            {
                const s_aled_rgbw_t c = EZPI_aled_hsv_to_rgb((uint8_t)h, (uint8_t)s, (uint8_t)v);

                uint8_t max = c.r;
                if (c.g > max)
                {
                    max = c.g;
                }
                if (c.b > max)
                {
                    max = c.b;
                }

                if (max != (uint8_t)v)
                {
                    peak_violations++;
                }
                if (0 != c.w)
                {
                    white_violations++;
                }
                if ((0 == s) && !((c.r == c.g) && (c.g == c.b) && (c.r == (uint8_t)v)))
                {
                    grey_violations++;
                }
                if ((0 == v) && ((0 != c.r) || (0 != c.g) || (0 != c.b)))
                {
                    black_violations++;
                }
            }
        }
    }

    snprintf(what, sizeof(what),
             "max(r,g,b) == v for all 16777216 (h,s,v) triples -- the definition of "
             "value, and what a broken hue sector breaks; %ld violation(s)",
             peak_violations);
    CHECK(0 == peak_violations, what);

    snprintf(what, sizeof(what),
             "saturation 0 is grey of exactly v, for all 65536 (h,v) pairs, with no "
             "special case in the subject to produce it; %ld violation(s)",
             grey_violations);
    CHECK(0 == grey_violations, what);

    snprintf(what, sizeof(what),
             "value 0 is black for all 65536 (h,s) pairs; %ld violation(s)",
             black_violations);
    CHECK(0 == black_violations, what);

    /* Falsifiable, but only by breaking both of the subject's two defences at
     * once: it zero-initialises the return struct AND assigns out.w = 0, so
     * deleting either one alone leaves the other and nothing here fails. That
     * was measured both ways -- removing just `out.w = 0;` leaves all checks
     * green, and removing the initialiser as well (with w pre-set to 0xFF)
     * fails this check with all 16,777,216 triples violating it. So the check
     * is real, and the redundancy in the subject is deliberate rather than an
     * oversight. See the comment on the initialiser in ezlopi_util_aled.c. */
    snprintf(what, sizeof(what),
             "a hue never sets the white channel; %ld violation(s)", white_violations);
    CHECK(0 == white_violations, what);
}

/* ------------------------------------------------------------------------- *
 * 3. The hue wheel is continuous, and it turns the right way.
 *
 * These two checks exist because of a mutant that the whole of section 2
 * survived. Swapping the green and blue channels of sector 0 alone -- so that
 * the first sixth of the wheel runs red->magenta instead of red->yellow --
 * passes every invariant above, every named colour below, and all 58 checks
 * this file originally had. max(r,g,b) is still v, saturation 0 is still grey,
 * hue 0 is still pure red and hue 255 is still red, because at a sector's two
 * endpoints the swapped channels are both 0. The strip would simply show the
 * wrong colours in between: exactly the "lights up, responds to commands,
 * looks like a working product" defect the header says is the reason this
 * component is tested at all. The invariants were necessary and not
 * sufficient, and nothing revealed that until the mutant was run.
 *
 * CONTINUITY. Adjacent hues must differ by at most 6 counts summed across
 * r, g and b. 6 is measured, not chosen: it is the true maximum over all
 * 16,777,216 triples for the correct implementation, and it is what the
 * arithmetic predicts, since one step of hue advances the rising or falling
 * channel by 256*6/256 == 6. A permuted sector shows up as a step of ~500 at
 * the join. The wrap from hue 255 back to hue 0 is included, which is the join
 * a short last sector gets wrong.
 *
 * DIRECTION. Continuity alone does not pin which way the wheel turns -- a
 * wholly reversed wheel is perfectly continuous. So the six sector midpoints
 * are checked for which channel is brightest, which is middling and which is
 * dimmest. Ranking rather than intensity on purpose: the rank is forced by
 * which channel the sector assigns v to and which it assigns p, so it pins the
 * sector table exactly, while needing no hand-computed values that could carry
 * my own arithmetic error into the expected results.
 * ------------------------------------------------------------------------- */
static int abs_diff(int a, int b)
{
    return (a > b) ? (a - b) : (b - a);
}

static void test_the_hue_wheel_is_continuous(void)
{
    long discontinuities = 0;
    int worst = 0;
    int worst_h = -1, worst_s = -1, worst_v = -1;
    char what[220];

    /* s and v outer, h inner, so that consecutive iterations differ only in
     * hue -- which is what makes `prev` the right comparison. */
    for (int s = 0; s <= 255; s++)
    {
        for (int v = 0; v <= 255; v++)
        {
            /* Start from hue 255 so the first comparison is the 255->0 wrap. */
            s_aled_rgbw_t prev = EZPI_aled_hsv_to_rgb(255, (uint8_t)s, (uint8_t)v);

            for (int h = 0; h <= 255; h++)
            {
                const s_aled_rgbw_t c = EZPI_aled_hsv_to_rgb((uint8_t)h, (uint8_t)s, (uint8_t)v);
                const int step = abs_diff(c.r, prev.r) + abs_diff(c.g, prev.g) + abs_diff(c.b, prev.b);

                if (step > 6)
                {
                    discontinuities++;
                    if (step > worst)
                    {
                        worst = step;
                        worst_h = h;
                        worst_s = s;
                        worst_v = v;
                    }
                }
                prev = c;
            }
        }
    }

    snprintf(what, sizeof(what),
             "adjacent hues differ by at most 6 counts summed over r,g,b, for all "
             "16777216 triples and across the 255->0 wrap; %ld discontinuit(ies), "
             "worst %d at h=%d s=%d v=%d -- a permuted sector steps by ~500 here",
             discontinuities, worst, worst_h, worst_s, worst_v);
    CHECK(0 == discontinuities, what);
}

static void test_the_wheel_turns_the_right_way(void)
{
    /* Brightest, middling and dimmest channel at each sector's midpoint. The
     * six rows are the six permutations of "rgb", which is itself a property
     * of a correct wheel: each sector has a different channel ordering, and
     * between them they use every ordering exactly once. */
    static const char *const expect[6] = {"rgb", "grb", "gbr", "bgr", "brg", "rbg"};
    int distinct = 1;
    char what[200];

    for (int n = 0; n < 6; n++)
    {
        /* Sector n spans hue n*256/6 .. (n+1)*256/6, so its midpoint is
         * (2n+1)*256/12. Computed rather than tabulated, so that the hues
         * follow from the sector count instead of being six more magic
         * numbers to get wrong. */
        const int h = ((2 * n + 1) * 256) / 12;
        const s_aled_rgbw_t c = EZPI_aled_hsv_to_rgb((uint8_t)h, 255, 255);

        const int val[3] = {c.r, c.g, c.b};
        const char let[3] = {'r', 'g', 'b'};
        int used[3] = {0, 0, 0};
        char rank[4];

        for (int i = 0; i < 3; i++)
        {
            int best = -1, bi = 0;
            for (int j = 0; j < 3; j++)
            {
                if (!used[j] && (val[j] > best))
                {
                    best = val[j];
                    bi = j;
                }
            }
            used[bi] = 1;
            rank[i] = let[bi];
        }
        rank[3] = '\0';

        snprintf(what, sizeof(what),
                 "sector %d (h=%d) ranks its channels %s, brightest first "
                 "(got %s: %d %d %d)", n, h, expect[n], rank, c.r, c.g, c.b);
        CHECK(0 == strcmp(rank, expect[n]), what);

        for (int m = 0; m < n; m++)
        {
            if (0 == strcmp(expect[n], expect[m]))
            {
                distinct = 0;
            }
        }
    }

    CHECK(1 == distinct,
          "the six expected rankings above are six different orderings -- if two "
          "sectors expected the same one, a sector table with a repeated row "
          "would pass the checks above");
}

/* 4. Named colours, so the sweeps above are anchored to something a reader can
 * check by eye. Pure red at hue 0 is the one fixed point every hue convention
 * agrees on. */
static void test_hsv_named_colours(void)
{
    s_aled_rgbw_t c = EZPI_aled_hsv_to_rgb(0, 255, 255);
    CHECK((255 == c.r) && (0 == c.g) && (0 == c.b), "hue 0, fully saturated, full value is pure red");

    c = EZPI_aled_hsv_to_rgb(0, 0, 255);
    CHECK((255 == c.r) && (255 == c.g) && (255 == c.b), "saturation 0 at full value is white");

    c = EZPI_aled_hsv_to_rgb(0, 0, 0);
    CHECK((0 == c.r) && (0 == c.g) && (0 == c.b), "value 0 is black");

    /* Hue 255 is one count short of wrapping to 0, so it must be red with only
     * a trace of blue -- the check that the top of the wheel joins the bottom
     * rather than jumping. An implementation splitting hue by /43 lands
     * elsewhere here, because its last sector is two counts narrow. */
    c = EZPI_aled_hsv_to_rgb(255, 255, 255);
    CHECK((255 == c.r) && (0 == c.g) && (c.b <= 8),
          "hue 255 is red with at most a trace of blue -- the wheel closes");

    /* Hue 128 is the exact midpoint of the wheel and lands exactly on a sector
     * boundary (128*6 == 768 == 3*256), so cyan comes out exact rather than
     * within a count or two. An exact expected value is worth having: it is
     * the one named colour here that pins the sector arithmetic to the count
     * rather than to a tolerance. */
    c = EZPI_aled_hsv_to_rgb(128, 255, 255);
    CHECK((0 == c.r) && (255 == c.g) && (255 == c.b),
          "hue 128 is exactly cyan -- it falls on a sector boundary, so this one "
          "is exact and not within a tolerance");

    /* Inside sector 0, where red and green are both high. This is the readable
     * form of what the continuity and ranking checks above cover: the mutant
     * that swapped sector 0's green and blue made this magenta, and every
     * check in this file passed. */
    c = EZPI_aled_hsv_to_rgb(42, 255, 255);
    CHECK((255 == c.r) && (c.g >= 240) && (0 == c.b),
          "hue 42 is yellow: red and green both high, no blue -- the first sixth "
          "of the wheel runs red->yellow, not red->magenta");
}

/* ------------------------------------------------------------------------- *
 * 4. The byte-order table. Every row must be a permutation of {r,g,b}: a
 * duplicated index would drop one channel and double another, which on a board
 * looks like a strip with a dead colour.
 * ------------------------------------------------------------------------- */
static void test_every_order_is_a_permutation(void)
{
    /* Three distinguishable values, so a permutation is identifiable from the
     * output alone. */
    const s_aled_rgbw_t px = {10, 20, 30, 40};
    const e_aled_order_t orders[] = {
        ALED_ORDER_GRB, ALED_ORDER_RGB, ALED_ORDER_BRG,
        ALED_ORDER_RBG, ALED_ORDER_GBR, ALED_ORDER_BGR};
    const int n_orders = (int)(sizeof(orders) / sizeof(orders[0]));
    char what[160];

    CHECK(6 == n_orders, "there are six byte orders, one per non-sentinel enumerator");
    CHECK(6 == (int)ALED_ORDER_MAX, "ALED_ORDER_MAX agrees with that count");

    for (int i = 0; i < n_orders; i++)
    {
        uint8_t out[3] = {0, 0, 0};
        int seen_10 = 0, seen_20 = 0, seen_30 = 0;

        const size_t n = EZPI_aled_pack(&px, 1, orders[i], 255, false, out, sizeof(out));

        snprintf(what, sizeof(what), "order %d packs one RGB pixel into 3 bytes", i);
        CHECK(3 == n, what);

        for (int b = 0; b < 3; b++)
        {
            seen_10 += (10 == out[b]);
            seen_20 += (20 == out[b]);
            seen_30 += (30 == out[b]);
        }

        snprintf(what, sizeof(what),
                 "order %d emits each of the three channels exactly once "
                 "(got %02x %02x %02x)", i, out[0], out[1], out[2]);
        CHECK((1 == seen_10) && (1 == seen_20) && (1 == seen_30), what);
    }

    /* And the three orders a reader is most likely to need, spelled out. If the
     * table's rows were shuffled relative to the enum, the permutation check
     * above would still pass and these would not. */
    {
        uint8_t out[3];
        EZPI_aled_pack(&px, 1, ALED_ORDER_GRB, 255, false, out, sizeof(out));
        CHECK((20 == out[0]) && (10 == out[1]) && (30 == out[2]),
              "GRB really does put green first -- the WS2812 default");

        EZPI_aled_pack(&px, 1, ALED_ORDER_RGB, 255, false, out, sizeof(out));
        CHECK((10 == out[0]) && (20 == out[1]) && (30 == out[2]), "RGB is identity");

        EZPI_aled_pack(&px, 1, ALED_ORDER_BGR, 255, false, out, sizeof(out));
        CHECK((30 == out[0]) && (20 == out[1]) && (10 == out[2]), "BGR is reversed");
    }
}

/* 5. Refusal rather than a partial frame, and refusal rather than an overrun.
 * Every rejection path is checked to have written *nothing*, by filling the
 * destination with a sentinel first. Under ASan a one-byte overrun of a heap
 * buffer would abort here; the sentinel catches the subtler case of a write
 * that stays in bounds but should not have happened at all. */
static void test_pack_refuses_instead_of_overrunning(void)
{
    const s_aled_rgbw_t px[2] = {{1, 2, 3, 4}, {5, 6, 7, 8}};
    uint8_t out[16];

    /* One byte short for two RGB pixels. */
    memset(out, 0xAA, sizeof(out));
    CHECK(0 == EZPI_aled_pack(px, 2, ALED_ORDER_GRB, 255, false, out, 5),
          "a destination one byte too small is refused, not partly filled");
    CHECK((0xAA == out[0]) && (0xAA == out[1]) && (0xAA == out[2]),
          "...and nothing at all was written -- a half-packed frame is still a "
          "frame, and the strip would display it");

    /* Exactly big enough must succeed, so the bound is not off by one in the
     * safe direction -- a refusal that is too eager leaves a strip dark. */
    memset(out, 0xAA, sizeof(out));
    CHECK(6 == EZPI_aled_pack(px, 2, ALED_ORDER_GRB, 255, false, out, 6),
          "a destination of exactly the needed size is accepted");

    /* RGBW needs four bytes per pixel, so the same six are now too few. */
    memset(out, 0xAA, sizeof(out));
    CHECK(0 == EZPI_aled_pack(px, 2, ALED_ORDER_GRB, 255, true, out, 6),
          "the same buffer is too small once a white channel is being emitted");
    CHECK(8 == EZPI_aled_pack(px, 2, ALED_ORDER_GRB, 255, true, out, 8),
          "...and eight bytes is what RGBW needs for two pixels");

    /* An order outside the enum. This is the out-of-bounds table read: it
     * arrives as an int from a cloud field, so the enum's type guarantees
     * nothing about the value. */
    memset(out, 0xAA, sizeof(out));
    CHECK(0 == EZPI_aled_pack(px, 1, ALED_ORDER_MAX, 255, false, out, sizeof(out)),
          "the sentinel ALED_ORDER_MAX is not a usable order");
    CHECK(0 == EZPI_aled_pack(px, 1, (e_aled_order_t)99, 255, false, out, sizeof(out)),
          "an order of 99 is refused rather than indexing the order table off its end");
    CHECK(0 == EZPI_aled_pack(px, 1, (e_aled_order_t)-1, 255, false, out, sizeof(out)),
          "a negative order arriving through an int-shaped field is refused too");
    CHECK(0xAA == out[0], "none of the three invalid orders wrote anything");

    /* Null pointers and an empty strip. */
    CHECK(0 == EZPI_aled_pack(NULL, 1, ALED_ORDER_GRB, 255, false, out, sizeof(out)),
          "a NULL pixel buffer is refused");
    CHECK(0 == EZPI_aled_pack(px, 1, ALED_ORDER_GRB, 255, false, NULL, 16),
          "a NULL destination is refused");
    CHECK(0 == EZPI_aled_pack(px, 0, ALED_ORDER_GRB, 255, false, out, sizeof(out)),
          "a strip of no pixels writes nothing");
}

/* 6. Brightness is applied by the packer, to every channel including white,
 * and the pixel buffer is left alone -- so raising the brightness again
 * recovers the full colour rather than a colour already rounded down once. */
static void test_brightness_is_applied_at_pack_time(void)
{
    s_aled_rgbw_t px = {200, 100, 50, 25};
    uint8_t out[4];

    const size_t n = EZPI_aled_pack(&px, 1, ALED_ORDER_RGB, 128, true, out, sizeof(out));
    CHECK(4 == n, "one RGBW pixel packs to four bytes");
    CHECK(EZPI_aled_scale8(200, 128) == out[0], "red is scaled by brightness");
    CHECK(EZPI_aled_scale8(100, 128) == out[1], "green is scaled by brightness");
    CHECK(EZPI_aled_scale8(50, 128) == out[2], "blue is scaled by brightness");
    CHECK(EZPI_aled_scale8(25, 128) == out[3],
          "and so is white -- an unscaled white channel makes an RGBW strip "
          "refuse to dim below the white level");

    CHECK((200 == px.r) && (100 == px.g) && (50 == px.b) && (25 == px.w),
          "the pixel buffer is unchanged, so brightness is not applied twice "
          "and raising it recovers the full colour");

    /* Brightness 0 is off, not "nearly off". */
    CHECK(4 == EZPI_aled_pack(&px, 1, ALED_ORDER_RGB, 0, true, out, sizeof(out)),
          "brightness 0 still emits a full frame");
    CHECK((0 == out[0]) && (0 == out[1]) && (0 == out[2]) && (0 == out[3]),
          "...and every channel of it is 0");
}

/* 7. The wire-length arithmetic, including the overflow a caller would
 * otherwise do by hand in a malloc argument. */
static void test_wire_len_refuses_to_wrap(void)
{
    CHECK(30 == EZPI_aled_wire_len(10, false), "ten RGB pixels need 30 bytes");
    CHECK(40 == EZPI_aled_wire_len(10, true), "ten RGBW pixels need 40 bytes");
    CHECK(0 == EZPI_aled_wire_len(0, false), "no pixels need no bytes");

    CHECK(0 == EZPI_aled_wire_len(SIZE_MAX, false),
          "a pixel count of SIZE_MAX reports 0 rather than a wrapped product");
    CHECK(0 == EZPI_aled_wire_len((SIZE_MAX / 3u) + 1u, false),
          "the first RGB count that would wrap reports 0");
    CHECK(0 == EZPI_aled_wire_len((SIZE_MAX / 4u) + 1u, true),
          "the first RGBW count that would wrap reports 0");

    /* The largest count that does not wrap must still work, so the bound is
     * not one too strict. */
    CHECK(0 != EZPI_aled_wire_len(SIZE_MAX / 4u, true),
          "the largest non-wrapping RGBW count is still accepted");
}

/* ------------------------------------------------------------------------- *
 * 8. A solid fill, and reading the first pixel back.
 *
 * These two are the pair that lets a strip be driven in any of the six orders
 * instead of only the one the vendored component's type table names. So the
 * thing worth checking hardest is not that they agree with each other -- they
 * would agree just as well if the order table were shuffled, and a round-trip
 * test is exactly what cannot see that. The expectations below are derived from
 * the three-letter *names* of the orders, written out here, so a table whose
 * rows were permuted against the enum fails them.
 *
 * The existing section 4 does this for three orders using hand-written
 * expectations. This does it for all six, computed from the names, at every
 * pixel of a multi-pixel strip and in both channel widths.
 * ------------------------------------------------------------------------- */
static const struct
{
    const char *name;
    e_aled_order_t order;
} k_named_orders[] = {
    {"grb", ALED_ORDER_GRB},
    {"rgb", ALED_ORDER_RGB},
    {"brg", ALED_ORDER_BRG},
    {"rbg", ALED_ORDER_RBG},
    {"gbr", ALED_ORDER_GBR},
    {"bgr", ALED_ORDER_BGR},
};

/* The channel value a name's letter stands for, for one chosen pixel. */
static uint8_t channel_named(char letter, s_aled_rgbw_t px)
{
    switch (letter)
    {
    case 'r':
        return px.r;
    case 'g':
        return px.g;
    case 'b':
        return px.b;
    default:
        return 0xFF; /* not a channel letter; guarantees a mismatch */
    }
}

static void test_fill_writes_every_pixel_in_the_named_order(void)
{
    /* Four distinguishable values, none of them equal, so a dropped or
     * duplicated channel cannot hide behind a coincidence. */
    const s_aled_rgbw_t px = {10, 20, 30, 40};
    const int n_orders = (int)(sizeof(k_named_orders) / sizeof(k_named_orders[0]));
    char what[220];

    CHECK(6 == n_orders, "all six orders are named here, not a subset");

    for (int i = 0; i < n_orders; i++)
    {
        const char *name = k_named_orders[i].name;
        const e_aled_order_t order = k_named_orders[i].order;

        for (int white = 0; white <= 1; white++)
        {
            const bool has_white = (1 == white);
            const size_t stride = has_white ? 4u : 3u;
            const size_t n_pixels = 7u; /* not a power of two, not 1 */
            uint8_t buf[7u * 4u];
            size_t wrong_bytes = 0u;
            size_t n;

            memset(buf, 0xAA, sizeof(buf));
            n = EZPI_aled_fill(px, n_pixels, order, 255, has_white,
                               buf, n_pixels * stride);

            snprintf(what, sizeof(what),
                     "fill(%s, rgbw=%d) writes %u bytes for seven pixels",
                     name, white, (unsigned)(n_pixels * stride));
            CHECK((n_pixels * stride) == n, what);

            /* Every pixel, not just the first: the copy loop is the part that
             * could stop one pixel short or overlap by a byte, and a strip
             * whose last pixel is a stale 0xAA is a strip with one wrong LED
             * at the far end -- the least likely thing to be noticed. */
            for (size_t p = 0u; p < n_pixels; p++)
            {
                for (size_t j = 0u; j < 3u; j++)
                {
                    if (buf[(p * stride) + j] != channel_named(name[j], px))
                    {
                        wrong_bytes++;
                    }
                }
                if (has_white && (buf[(p * stride) + 3u] != px.w))
                {
                    wrong_bytes++;
                }
            }

            snprintf(what, sizeof(what),
                     "fill(%s, rgbw=%d) puts the right channel in every byte of "
                     "all seven pixels; %u byte(s) wrong",
                     name, white, (unsigned)wrong_bytes);
            CHECK(0u == wrong_bytes, what);

            /* One pixel is the pre-existing shipped configuration and the
             * default the resolver hands back, so it gets its own check rather
             * than riding on the seven-pixel loop. */
            memset(buf, 0xAA, sizeof(buf));
            CHECK(stride == EZPI_aled_fill(px, 1u, order, 255, has_white, buf, stride),
                  "a one-pixel strip is filled too -- the pre-existing default");
        }
    }
}

static void test_fill_refuses_instead_of_overrunning(void)
{
    const s_aled_rgbw_t px = {1, 2, 3, 4};
    uint8_t out[16];

    memset(out, 0xAA, sizeof(out));
    CHECK(0 == EZPI_aled_fill(px, 2, ALED_ORDER_GRB, 255, false, out, 5),
          "a destination one byte too small is refused, not partly filled");
    CHECK((0xAA == out[0]) && (0xAA == out[1]) && (0xAA == out[2]),
          "...and nothing at all was written");

    CHECK(6 == EZPI_aled_fill(px, 2, ALED_ORDER_GRB, 255, false, out, 6),
          "a destination of exactly the needed size is accepted");

    memset(out, 0xAA, sizeof(out));
    CHECK(0 == EZPI_aled_fill(px, 2, ALED_ORDER_GRB, 255, true, out, 6),
          "six bytes is too few for two RGBW pixels");
    CHECK(8 == EZPI_aled_fill(px, 2, ALED_ORDER_GRB, 255, true, out, 8),
          "...and eight is what they need");

    memset(out, 0xAA, sizeof(out));
    CHECK(0 == EZPI_aled_fill(px, 1, ALED_ORDER_MAX, 255, false, out, sizeof(out)),
          "the sentinel order is refused by fill as well as by pack");
    CHECK(0 == EZPI_aled_fill(px, 1, (e_aled_order_t)99, 255, false, out, sizeof(out)),
          "an order of 99 is refused rather than indexing the table off its end");
    CHECK(0 == EZPI_aled_fill(px, 1, (e_aled_order_t)-1, 255, false, out, sizeof(out)),
          "a negative order arriving through an int-shaped field is refused");
    CHECK(0xAA == out[0], "none of the three invalid orders wrote anything");

    CHECK(0 == EZPI_aled_fill(px, 1, ALED_ORDER_GRB, 255, false, NULL, 16),
          "a NULL destination is refused");
    CHECK(0 == EZPI_aled_fill(px, 0, ALED_ORDER_GRB, 255, false, out, sizeof(out)),
          "a strip of no pixels writes nothing");
    CHECK(0 == EZPI_aled_fill(px, SIZE_MAX, ALED_ORDER_GRB, 255, false, out, sizeof(out)),
          "a pixel count that would wrap the byte count is refused, not wrapped");

    /* Brightness reaches fill, which is not obvious: it is applied inside the
     * one-pixel pack and then copied, so a fill that scaled nothing would still
     * produce a uniform strip of the wrong brightness. */
    memset(out, 0xAA, sizeof(out));
    CHECK(8 == EZPI_aled_fill((s_aled_rgbw_t){200, 100, 50, 25}, 2,
                              ALED_ORDER_RGB, 128, true, out, 8),
          "fill with a brightness writes a full frame");
    CHECK((EZPI_aled_scale8(200, 128) == out[0]) &&
              (EZPI_aled_scale8(100, 128) == out[1]) &&
              (EZPI_aled_scale8(50, 128) == out[2]) &&
              (EZPI_aled_scale8(25, 128) == out[3]),
          "...with brightness applied to all four channels of the first pixel");
    CHECK((out[4] == out[0]) && (out[5] == out[1]) &&
              (out[6] == out[2]) && (out[7] == out[3]),
          "...and the second pixel is the same dimmed colour, not the full one");
}

static void test_unpack_inverts_the_named_order(void)
{
    const s_aled_rgbw_t px = {10, 20, 30, 40};
    const int n_orders = (int)(sizeof(k_named_orders) / sizeof(k_named_orders[0]));
    char what[220];

    for (int i = 0; i < n_orders; i++)
    {
        const char *name = k_named_orders[i].name;
        const e_aled_order_t order = k_named_orders[i].order;

        for (int white = 0; white <= 1; white++)
        {
            const bool has_white = (1 == white);
            const size_t stride = has_white ? 4u : 3u;
            uint8_t buf[4];
            s_aled_rgbw_t got = {0, 0, 0, 0};

            /* The wire bytes are written here from the order's *name*, not by
             * calling fill. So this is not a round-trip: it is unpack checked
             * against an independently constructed buffer, which is what makes
             * it able to catch a table that is wrong in both directions at
             * once. */
            for (size_t j = 0u; j < 3u; j++)
            {
                buf[j] = channel_named(name[j], px);
            }
            buf[3] = px.w;

            CHECK(EZPI_aled_unpack_first(buf, stride, order, has_white, &got),
                  "unpack accepts a buffer of exactly one pixel");

            snprintf(what, sizeof(what),
                     "unpack(%s, rgbw=%d) recovers r,g,b from the wire bytes "
                     "(got %d,%d,%d)",
                     name, white, got.r, got.g, got.b);
            CHECK((10 == got.r) && (20 == got.g) && (30 == got.b), what);

            snprintf(what, sizeof(what),
                     "unpack(%s, rgbw=%d) reports white as %d",
                     name, white, got.w);
            CHECK((has_white ? 40 : 0) == got.w, what);

            /* And the round trip, which adds the claim that fill and unpack
             * agree -- weaker than the above, but it is the property the
             * driver depends on. */
            {
                uint8_t wire[4];
                s_aled_rgbw_t back = {0, 0, 0, 0};
                CHECK(stride == EZPI_aled_fill(px, 1, order, 255, has_white, wire, stride),
                      "fill writes one pixel for the round trip");
                CHECK(EZPI_aled_unpack_first(wire, stride, order, has_white, &back),
                      "...which unpack reads back");
                CHECK((px.r == back.r) && (px.g == back.g) && (px.b == back.b) &&
                          ((has_white ? px.w : 0) == back.w),
                      "...to the colour that went in, at brightness 255");
            }
        }
    }

    /* Brightness is not undone, and the header says so. Pinned, because a
     * caller reading this back to report a colour to the cloud will see the
     * dimmed value and the alternative reading -- that it reverses the scaling
     * -- is impossible rather than merely unimplemented. */
    {
        uint8_t wire[3];
        s_aled_rgbw_t back = {0, 0, 0, 0};
        EZPI_aled_fill(px, 1, ALED_ORDER_RGB, 128, false, wire, sizeof(wire));
        CHECK(EZPI_aled_unpack_first(wire, sizeof(wire), ALED_ORDER_RGB, false, &back),
              "a dimmed frame unpacks");
        CHECK(EZPI_aled_scale8(10, 128) == back.r,
              "...to the dimmed channel value, not the original -- scale8 is "
              "lossy and this does not pretend to invert it");
    }

    /* Refusals. */
    {
        uint8_t wire[4] = {1, 2, 3, 4};
        s_aled_rgbw_t back;

        CHECK(false == EZPI_aled_unpack_first(NULL, 4, ALED_ORDER_GRB, false, &back),
              "a NULL wire buffer is refused");
        CHECK(false == EZPI_aled_unpack_first(wire, 4, ALED_ORDER_GRB, false, NULL),
              "a NULL destination is refused");
        CHECK(false == EZPI_aled_unpack_first(wire, 2, ALED_ORDER_GRB, false, &back),
              "two bytes is too few for an RGB pixel -- refused, not read past");
        CHECK(false == EZPI_aled_unpack_first(wire, 3, ALED_ORDER_GRB, true, &back),
              "three bytes is too few for an RGBW pixel, which is the read past "
              "the end that an is_rgbw mismatch would cause");
        CHECK(true == EZPI_aled_unpack_first(wire, 3, ALED_ORDER_GRB, false, &back),
              "...while exactly three is enough for RGB, so the bound is not "
              "one too strict");
        CHECK(false == EZPI_aled_unpack_first(wire, 4, (e_aled_order_t)99, false, &back),
              "an order of 99 is refused");
        CHECK(false == EZPI_aled_unpack_first(wire, 4, ALED_ORDER_MAX, false, &back),
              "the sentinel order is refused");
    }
}

int main(void)
{
    test_scale8_over_its_whole_domain();
    test_hsv_invariants_over_the_whole_cube();
    test_the_hue_wheel_is_continuous();
    test_the_wheel_turns_the_right_way();
    test_hsv_named_colours();
    test_every_order_is_a_permutation();
    test_pack_refuses_instead_of_overrunning();
    test_brightness_is_applied_at_pack_time();
    test_wire_len_refuses_to_wrap();
    test_fill_writes_every_pixel_in_the_named_order();
    test_fill_refuses_instead_of_overrunning();
    test_unpack_inverts_the_named_order();

    /* Bare, with no "test_aled: " prefix: "N check(s), M failure(s)" is the
     * shape the suite's aggregate tally reads, and a prefixed line matches
     * nothing. See the note in test_issun.c, which got this wrong first. */
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return (0 == failures) ? 0 : 1;
}
