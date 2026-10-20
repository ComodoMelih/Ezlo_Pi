/* test_aledcfg.c -- the addressable-strip config resolver, on a host.
 *
 * WHAT THIS IS FOR
 *
 * EzloPi already onboards an addressable LED strip: device-0009-other-RMT-SK6812
 * is registered through all four files of the device contract and appears in
 * the onboarding list. It also sets `sk6812_strip.length = 1` as a literal, and
 * reads no pixel-count key, so a 60-pixel strip lights one LED and looks like a
 * working device while it does it.
 *
 * EZPI_aledcfg_from_json() is the fix for that, and it is a parser, so it is
 * worth testing here rather than on a board.
 *
 * THE DEFECT THIS FILE EXISTS TO PREVENT
 *
 * EZPI_core_cjson_get_value_uint32() assigns `*item_val = 0` BEFORE it looks
 * the key up, and leaves the 0 there when the key is absent or is not a JSON
 * number. So the obvious way to write a caller --
 *
 *     uint32_t n = 1;                              <-- looks like a default
 *     CJSON_GET_VALUE_UINT32(cj_device, key, n);    <-- n is now 0
 *
 * -- discards the default silently. And 0 is not a harmless value here:
 * led_strip_init() begins with CHECK_ARG(strip && strip->length > 0 && ...), so
 * a resolved count of 0 does not dim the strip, it fails initialisation and the
 * device never comes up.
 *
 * That is the whole reason this resolver owns the defaults instead of its
 * callers, and the first checks below are aimed squarely at it: a config with
 * no keys, and a config with a malformed count, must both yield a count of 1.
 *
 * BACKWARD COMPATIBILITY IS THE LOAD-BEARING PROPERTY
 *
 * Every strip already provisioned in the field has a stored config containing
 * none of these three keys. After this change those installs must behave
 * byte-for-byte as before -- one GRB pixel, no white channel -- or a firmware
 * update silently changes what working hardware does. The "says nothing"
 * checks are therefore not edge cases, they are the main case.
 *
 * WHAT IT WAS MEASURED TO CATCH
 *
 * Run against the subject broken on purpose before being trusted; the mutants
 * and their verdicts are recorded in the CHANGELOG entry for this component.
 */

#include <stdio.h>
#include <string.h>

#include "cjext.h"
#include "ezlopi_util_aledcfg.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond, what)                                         \
    do                                                            \
    {                                                             \
        checks++;                                                 \
        if (!(cond))                                              \
        {                                                         \
            failures++;                                           \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        }                                                         \
    } while (0)

/* Parse a JSON document and resolve it in one step. The cJSON object is freed
 * before returning, so a leak in the resolver shows up under ASan rather than
 * being masked by a document this test is still holding. */
static bool resolve_str(const char *json, s_aledcfg_t *out)
{
    cJSON *const root = cJSON_Parse(__FUNCTION__, json);
    bool verbatim;

    /* A document this test fails to parse would otherwise look exactly like a
     * document the resolver rejected. */
    if (NULL == root)
    {
        printf("FAIL  this test's own JSON did not parse: %s\n", json);
        failures++;
        checks++;
        out->pixel_count = 0;
        out->has_white = false;
        out->order = ALED_ORDER_GRB;
        return false;
    }

    verbatim = EZPI_aledcfg_from_json(root, out);
    cJSON_Delete(__FUNCTION__, root);
    return verbatim;
}

/* ------------------------------------------------------------------------- *
 * 1. A config that says nothing must reproduce the shipped behaviour exactly.
 * ------------------------------------------------------------------------- */

static void test_silence_means_what_the_driver_already_did(void)
{
    s_aledcfg_t cfg;
    bool verbatim;

    /* The empty object: a device entry from before these keys existed. */
    verbatim = resolve_str("{}", &cfg);
    CHECK(1 == cfg.pixel_count,
          "a config with no pixel_count resolves to 1 pixel -- the literal the "
          "shipped driver used, so an already-provisioned strip is unchanged");
    CHECK(false == cfg.has_white,
          "...and no white channel, matching is_rgbw left zero by the memset");
    CHECK(ALED_ORDER_GRB == cfg.order,
          "...and GRB, which is both the zeroed enum value and what a WS2812 "
          "actually wants");
    CHECK(true == verbatim,
          "an empty config is not a disagreement -- it is a valid configuration "
          "meaning one pixel, so it must not make the caller log about every "
          "strip provisioned before this key existed");

    /* A realistic device entry carrying the keys device-0009 does read. */
    verbatim = resolve_str("{\"gpio1\":18,\"duty_cycle\":0,\"freq_hz\":5000}", &cfg);
    CHECK(1 == cfg.pixel_count,
          "a real pre-existing device entry, with gpio1/duty_cycle/freq_hz and "
          "nothing else, still resolves to 1 pixel");
    CHECK(true == verbatim,
          "...and is reported as taken verbatim: unrelated keys are not the "
          "resolver's business");

    /* NULL, which is what a caller gets from a config with no dev_detail. */
    cfg.pixel_count = 999;
    cfg.has_white = true;
    cfg.order = ALED_ORDER_BGR;
    verbatim = EZPI_aledcfg_from_json(NULL, &cfg);
    CHECK(1 == cfg.pixel_count,
          "a NULL device object fills the defaults rather than leaving the "
          "caller's struct untouched -- it was pre-poisoned with 999/true/BGR "
          "above, so this check fails if the function returns early");
    CHECK(false == cfg.has_white, "...including has_white");
    CHECK(ALED_ORDER_GRB == cfg.order, "...including the order");
    CHECK(true == verbatim, "...and a missing device object is not a disagreement");

    /* A NULL out is the one case with nowhere to put an answer. */
    CHECK(false == EZPI_aledcfg_from_json(NULL, NULL),
          "a NULL destination returns false -- the only path that reports "
          "failure without leaving a usable config behind, because there is no "
          "config to leave");
}

/* ------------------------------------------------------------------------- *
 * 2. pixel_count: every integer across the bound, and the shapes that are not
 *    integers at all.
 * ------------------------------------------------------------------------- */

static void test_pixel_count_over_the_whole_range(void)
{
    char json[96];
    long accepted = 0;
    long wrong_value = 0;
    long wrong_verdict = 0;
    int first_bad = -1;
    char what[256];

    /* 0 through 4096: spans the whole legal range, both sides of
     * ALEDCFG_MAX_PIXELS, and 0 itself -- the value the zeroing getter
     * produces, and the one that fails led_strip_init(). */
    for (long n = 0; n <= 4096; n++)
    {
        s_aledcfg_t cfg;
        bool verbatim;
        const bool legal = ((n >= (long)ALEDCFG_MIN_PIXELS) && (n <= (long)ALEDCFG_MAX_PIXELS));

        snprintf(json, sizeof(json), "{\"pixel_count\":%ld}", n);
        verbatim = resolve_str(json, &cfg);

        if (legal)
        {
            accepted++;
            if ((size_t)n != cfg.pixel_count)
            {
                wrong_value++;
                if (first_bad < 0) { first_bad = (int)n; }
            }
            if (!verbatim) { wrong_verdict++; }
        }
        else
        {
            /* Out of range falls back to 1 rather than clamping to the maximum:
             * 100000 is far likelier a typo than a request for the longest
             * strip this can drive, and lighting 2048 pixels would hide it. */
            if (1u != cfg.pixel_count)
            {
                wrong_value++;
                if (first_bad < 0) { first_bad = (int)n; }
            }
            if (verbatim) { wrong_verdict++; }
        }

        /* Whatever happened, the result must be something led_strip_init()
         * accepts. This is the invariant that matters most: the function must
         * never hand back a count that bricks the device. */
        if (cfg.pixel_count < ALEDCFG_MIN_PIXELS)
        {
            wrong_value++;
        }
    }

    snprintf(what, sizeof(what),
             "pixel_count 0..4096: every value in 1..%u is used as given and "
             "everything outside it falls back to 1; %ld value error(s), first "
             "at n=%d",
             ALEDCFG_MAX_PIXELS, wrong_value, first_bad);
    CHECK(0 == wrong_value, what);

    snprintf(what, sizeof(what),
             "...and the verbatim/substituted verdict is right for all 4097 of "
             "them; %ld wrong verdict(s)",
             wrong_verdict);
    CHECK(0 == wrong_verdict, what);

    snprintf(what, sizeof(what),
             "the legal range really was exercised (%ld of 4097 values accepted, "
             "expected %u) -- a resolver that refused everything would pass the "
             "two checks above if this one did not exist",
             accepted, ALEDCFG_MAX_PIXELS);
    CHECK(accepted == (long)ALEDCFG_MAX_PIXELS, what);
}

static void test_pixel_count_that_is_not_a_number(void)
{
    s_aledcfg_t cfg;

    CHECK((false == resolve_str("{\"pixel_count\":\"60\"}", &cfg)) && (1 == cfg.pixel_count),
          "a quoted count is refused, not coerced -- \"60\" is what a form "
          "field sends when nobody converted it, and silently accepting it "
          "would make the string/number distinction untestable later");

    CHECK((false == resolve_str("{\"pixel_count\":null}", &cfg)) && (1 == cfg.pixel_count),
          "a null count falls back to 1");

    CHECK((false == resolve_str("{\"pixel_count\":true}", &cfg)) && (1 == cfg.pixel_count),
          "a boolean count falls back to 1");

    CHECK((false == resolve_str("{\"pixel_count\":{}}", &cfg)) && (1 == cfg.pixel_count),
          "an object count falls back to 1");

    CHECK((false == resolve_str("{\"pixel_count\":[60]}", &cfg)) && (1 == cfg.pixel_count),
          "an array count falls back to 1");

    CHECK((false == resolve_str("{\"pixel_count\":-1}", &cfg)) && (1 == cfg.pixel_count),
          "a negative count falls back to 1 -- compared as a double before any "
          "cast, because casting -1.0 to size_t is undefined and a cap applied "
          "afterwards would be testing SIZE_MAX rather than -1");

    CHECK((false == resolve_str("{\"pixel_count\":1e9}", &cfg)) && (1 == cfg.pixel_count),
          "1e9 falls back to 1 -- a range check, nothing subtler: cJSON would "
          "clamp valueint to INT_MAX, which is also out of range, so this case "
          "does NOT discriminate between reading valueint and valuedouble");

    CHECK((false == resolve_str("{\"pixel_count\":2.5}", &cfg)) && (1 == cfg.pixel_count),
          "2.5 is refused rather than truncated to 2 -- a fractional pixel "
          "count means the writer is confused about something. This is the "
          "check that actually pins valuedouble over valueint: swapping them "
          "was measured to fail here and nowhere else, because every value "
          "big enough for the INT_MAX clamp to matter is refused by the range "
          "bound anyway");

    CHECK((true == resolve_str("{\"pixel_count\":60}", &cfg)) && (60 == cfg.pixel_count),
          "and the ordinary case still works: 60 pixels, reported verbatim");
}

/* ------------------------------------------------------------------------- *
 * 3. color_order: all 17,576 three-letter strings, so that exactly six are
 *    accepted and the other 17,570 are not.
 * ------------------------------------------------------------------------- */

static void test_only_six_strings_are_an_order(void)
{
    long accepted = 0;
    long bad_accept = 0;
    long bad_reject = 0;
    char first_bad[8] = "";
    char what[256];
    char s[4] = {0, 0, 0, 0};

    for (int a = 'a'; a <= 'z'; a++)
    {
        for (int b = 'a'; b <= 'z'; b++)
        {
            for (int c = 'a'; c <= 'z'; c++)
            {
                e_aled_order_t order = ALED_ORDER_MAX;
                bool ok;
                /* A genuine permutation: three distinct letters, all from
                 * {r,g,b}. Computed rather than listed, so the expectation is
                 * independent of the subject's own table. */
                const bool is_perm =
                    (a != b) && (b != c) && (a != c) &&
                    ((a == 'r') || (a == 'g') || (a == 'b')) &&
                    ((b == 'r') || (b == 'g') || (b == 'b')) &&
                    ((c == 'r') || (c == 'g') || (c == 'b'));

                s[0] = (char)a;
                s[1] = (char)b;
                s[2] = (char)c;

                ok = EZPI_aledcfg_order_from_str(s, &order);

                if (ok)
                {
                    accepted++;
                    if (!is_perm)
                    {
                        bad_accept++;
                        if ('\0' == first_bad[0]) { snprintf(first_bad, sizeof(first_bad), "%s", s); }
                    }
                    else if ((unsigned)order >= (unsigned)ALED_ORDER_MAX)
                    {
                        /* Accepted but produced an out-of-range enum, which
                         * would index past k_order_idx in EZPI_aled_pack. */
                        bad_accept++;
                    }
                }
                else if (is_perm)
                {
                    bad_reject++;
                    if ('\0' == first_bad[0]) { snprintf(first_bad, sizeof(first_bad), "%s", s); }
                }
            }
        }
    }

    snprintf(what, sizeof(what),
             "of all 17576 three-letter lowercase strings, exactly the 6 "
             "permutations of r/g/b are accepted (got %ld accepted, %ld wrongly "
             "accepted, %ld wrongly refused, first offender '%s')",
             accepted, bad_accept, bad_reject, first_bad);
    CHECK((6 == accepted) && (0 == bad_accept) && (0 == bad_reject), what);
}

static void test_order_strings_that_are_near_misses(void)
{
    e_aled_order_t order = ALED_ORDER_MAX;

    CHECK(false == EZPI_aledcfg_order_from_str("rg", &order),
          "\"rg\" is refused -- a prefix compare would have matched the first "
          "two bytes of \"rgb\"; measured against exactly that mutant");

    CHECK(false == EZPI_aledcfg_order_from_str("rgbx", &order),
          "\"rgbx\" is refused -- a bounded compare of 3 would have accepted it, "
          "which is why the length is checked before the compare");

    CHECK(false == EZPI_aledcfg_order_from_str("", &order),
          "the empty string is refused");

    CHECK(false == EZPI_aledcfg_order_from_str("rrg", &order),
          "\"rrg\" is refused: three letters from the right alphabet, but a "
          "repeat, so one channel would be dropped and another doubled");

    CHECK(false == EZPI_aledcfg_order_from_str(NULL, &order),
          "NULL is refused rather than dereferenced");

    CHECK(true == EZPI_aledcfg_order_from_str("GRB", &order) && (ALED_ORDER_GRB == order),
          "uppercase \"GRB\" is accepted and maps to GRB -- the app has no "
          "reason to know this field is case-sensitive, so it is not");

    CHECK(true == EZPI_aledcfg_order_from_str("rGb", &order) && (ALED_ORDER_RGB == order),
          "...and mixed case too");

    /* `out` must not be written when the answer is "no". A caller that checks
     * the return value is unaffected, but one that does not would otherwise
     * get a plausible-looking order out of a refused string. */
    order = ALED_ORDER_BGR;
    (void)EZPI_aledcfg_order_from_str("zzz", &order);
    CHECK(ALED_ORDER_BGR == order,
          "a refused string leaves *out untouched rather than zeroing it to "
          "GRB, so a caller that ignores the return value keeps its own value "
          "instead of silently getting a different strip order");
}

static void test_the_order_table_matches_the_enum(void)
{
    /* The table in the subject is indexed by the enum. If it were merely
     * shuffled relative to the enum, every string would still round-trip
     * through order_from_str and the strip would light up in the wrong
     * colours -- so the pairing is checked explicitly against the names
     * written here, independently of the subject's own array. */
    static const struct
    {
        const char *str;
        e_aled_order_t order;
    } k_expect[] = {
        {"grb", ALED_ORDER_GRB},
        {"rgb", ALED_ORDER_RGB},
        {"brg", ALED_ORDER_BRG},
        {"rbg", ALED_ORDER_RBG},
        {"gbr", ALED_ORDER_GBR},
        {"bgr", ALED_ORDER_BGR},
    };
    const size_t n = sizeof(k_expect) / sizeof(k_expect[0]);
    size_t seen[ALED_ORDER_MAX];
    size_t i;
    int all_distinct = 1;
    char what[200];

    for (i = 0; i < (size_t)ALED_ORDER_MAX; i++) { seen[i] = 0; }

    for (i = 0; i < n; i++)
    {
        e_aled_order_t got = ALED_ORDER_MAX;

        snprintf(what, sizeof(what),
                 "\"%s\" maps to enum value %d and not to any other -- a table "
                 "shuffled against the enum is invisible to a round-trip test",
                 k_expect[i].str, (int)k_expect[i].order);
        CHECK(EZPI_aledcfg_order_from_str(k_expect[i].str, &got) && (k_expect[i].order == got),
              what);

        if ((unsigned)k_expect[i].order < (unsigned)ALED_ORDER_MAX)
        {
            seen[(unsigned)k_expect[i].order]++;
        }
    }

    for (i = 0; i < (size_t)ALED_ORDER_MAX; i++)
    {
        if (1 != seen[i]) { all_distinct = 0; }
    }
    CHECK(1 == all_distinct,
          "the six expectations above cover all six enum values exactly once, "
          "so this test cannot pass by mapping several strings to one order");

    CHECK(6 == (int)ALED_ORDER_MAX,
          "there are still exactly six orders -- if a seventh is added, the "
          "table above is incomplete and this check is the reminder");
}

/* ------------------------------------------------------------------------- *
 * 4. rgbw, and the wire length the resolved config implies.
 * ------------------------------------------------------------------------- */

static void test_the_white_channel_flag(void)
{
    s_aledcfg_t cfg;

    CHECK((true == resolve_str("{\"rgbw\":true}", &cfg)) && (true == cfg.has_white),
          "rgbw:true gives a four-channel pixel");

    CHECK((true == resolve_str("{\"rgbw\":false}", &cfg)) && (false == cfg.has_white),
          "rgbw:false gives three");

    CHECK((true == resolve_str("{\"rgbw\":1}", &cfg)) && (true == cfg.has_white),
          "rgbw:1 is accepted as true -- unlike pixel_count, a two-valued field "
          "has no range to get wrong, and the app has historically sent 0/1");

    CHECK((true == resolve_str("{\"rgbw\":0}", &cfg)) && (false == cfg.has_white),
          "rgbw:0 is accepted as false");

    CHECK((false == resolve_str("{\"rgbw\":\"yes\"}", &cfg)) && (false == cfg.has_white),
          "a string is refused and leaves the three-channel default");
}

static void test_wire_len_tracks_the_resolved_shape(void)
{
    long mismatches = 0;
    long zeros = 0;
    char what[320];

    /* The driver allocates calloc(length, COLOR_SIZE(strip)) where COLOR_SIZE
     * is 3 + is_rgbw. This must be the same number, for every legal count and
     * both widths, because the whole point of sharing EZPI_aled_wire_len is
     * that the bound and the allocation cannot drift apart. */
    for (long n = 1; n <= (long)ALEDCFG_MAX_PIXELS; n++)
    {
        int w;

        for (w = 0; w <= 1; w++)
        {
            s_aledcfg_t cfg;
            size_t got;
            const size_t expect = (size_t)n * (size_t)(3 + w);

            cfg.pixel_count = (size_t)n;
            cfg.has_white = (0 != w);
            cfg.order = ALED_ORDER_GRB;

            got = EZPI_aledcfg_wire_len(&cfg);
            if (got != expect) { mismatches++; }
            if (0u == got) { zeros++; }
        }
    }

    snprintf(what, sizeof(what),
             "for every legal count 1..%u at both 3 and 4 bytes per pixel, the "
             "wire length is exactly count*bpp (%ld mismatch(es)) -- the same "
             "arithmetic the driver's calloc uses",
             ALEDCFG_MAX_PIXELS, mismatches);
    CHECK(0 == mismatches, what);

    snprintf(what, sizeof(what),
             "...and none of them is 0 (%ld were), so the overflow guard inside "
             "EZPI_aled_wire_len never fires inside the accepted range -- "
             "ALEDCFG_MAX_PIXELS makes a wrap unreachable, and this is what "
             "would notice if that cap were raised carelessly",
             zeros);
    CHECK(0 == zeros, what);

    CHECK(0u == EZPI_aledcfg_wire_len(NULL),
          "a NULL config has no length rather than being dereferenced");
}

/* ------------------------------------------------------------------------- *
 * 5. All three keys at once, and one bad key among good ones.
 * ------------------------------------------------------------------------- */

static void test_the_keys_are_independent(void)
{
    s_aledcfg_t cfg;
    bool verbatim;

    verbatim = resolve_str("{\"pixel_count\":144,\"rgbw\":true,\"color_order\":\"rgb\"}", &cfg);
    CHECK(true == verbatim, "a fully specified strip is taken verbatim");
    CHECK(144 == cfg.pixel_count, "...144 pixels");
    CHECK(true == cfg.has_white, "...with a white channel");
    CHECK(ALED_ORDER_RGB == cfg.order, "...in RGB order");
    CHECK(576 == EZPI_aledcfg_wire_len(&cfg),
          "...needing 144*4 = 576 bytes on the wire");

    /* One malformed key must not discard the other two. This is the check that
     * makes "falls back per-field" a property rather than a comment: a
     * resolver that bailed out on the first bad key would reset all three. */
    verbatim = resolve_str("{\"pixel_count\":60,\"rgbw\":\"maybe\",\"color_order\":\"bgr\"}", &cfg);
    CHECK(false == verbatim, "a single malformed key makes the whole config non-verbatim");
    CHECK(60 == cfg.pixel_count,
          "...but the valid pixel_count beside it survives -- one bad key does "
          "not discard the others");
    CHECK(ALED_ORDER_BGR == cfg.order, "...and so does the valid color_order");
    CHECK(false == cfg.has_white, "...while the bad key itself takes its default");

    /* The reverse ordering, so the result does not depend on which key is read
     * first. */
    verbatim = resolve_str("{\"color_order\":\"zzz\",\"pixel_count\":30,\"rgbw\":true}", &cfg);
    CHECK(false == verbatim, "a bad color_order also makes the config non-verbatim");
    CHECK(30 == cfg.pixel_count, "...with the good count kept");
    CHECK(true == cfg.has_white, "...and the good rgbw kept");
    CHECK(ALED_ORDER_GRB == cfg.order, "...and the bad order defaulted to GRB");
}

int main(void)
{
    test_silence_means_what_the_driver_already_did();
    test_pixel_count_over_the_whole_range();
    test_pixel_count_that_is_not_a_number();
    test_only_six_strings_are_an_order();
    test_order_strings_that_are_near_misses();
    test_the_order_table_matches_the_enum();
    test_the_white_channel_flag();
    test_wire_len_tracks_the_resolved_shape();
    test_the_keys_are_independent();

    /* Bare, with no "test_aledcfg: " prefix: "N check(s), M failure(s)" is the
     * shape the suite's aggregate tally reads, and a prefixed line matches
     * nothing. See the note in test_scenefields.c, which got this wrong. */
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return (0 == failures) ? 0 : 1;
}
