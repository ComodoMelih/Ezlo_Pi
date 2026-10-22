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
 * THE SECOND DEFECT, WHICH THIS FILE ORIGINALLY LET THROUGH
 *
 * Sections 1 to 5 parse with cJSON_Parse(), which copies each string into its
 * own NUL-terminated allocation. The device does not: EZPI_device_prepare()
 * uses cJSON_ParseWithRef() (ezlopi_core_devices.c:630), whose valuestring
 * points into the stored payload and ends at a quote, with the length in
 * str_value_len.
 *
 * So the resolver read "grb\"}]}", measured 7 bytes, and refused all six
 * colour orders on a real board -- while section 3 classified all 17,576
 * three-letter strings correctly on the host. A test file can be thorough
 * about the wrong input.
 *
 * Sections 6 and 7 are the fix for that, and they use the parser the device
 * actually uses.
 *
 * WHAT IT WAS MEASURED TO CATCH
 *
 * Run against the subject broken on purpose before being trusted; the mutants
 * and their verdicts are recorded in the CHANGELOG entry for this component.
 * For sections 6 and 7 specifically, probe/aledmutants.sh restores the shipped
 * single-argument call and requires these checks to fail: they do, 7 of them,
 * and "zzz" is still refused, so the catch is selective rather than blanket.
 * A second mutant that ignores the bound and compares 3 bytes regardless is
 * caught as well, by ASan rather than by an assertion -- it reads past the
 * one-byte "" literal in section 3.
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
        /* ALED_CHIP_MAX, not the default: this is the path where the test's own
         * JSON was broken, and a field left at its default here could make a
         * later CHECK pass for the wrong reason. MAX cannot satisfy any of
         * them. */
        out->chip = ALED_CHIP_MAX;
        return false;
    }

    verbatim = EZPI_aledcfg_from_json(root, out);
    cJSON_Delete(__FUNCTION__, root);
    return verbatim;
}

/* Parse a document the way the DEVICE does, and resolve the device entry out of
 * it. cJSON_ParseWithRef() is the parser EZPI_device_prepare() uses
 * (ezlopi_core_devices.c:630), and it does not copy strings: each valuestring
 * points into `json` and is terminated by the closing quote, not a NUL, with
 * the length in str_value_len.
 *
 * `json` must therefore outlive the resolve, which is why every caller below
 * passes a string literal. A stack buffer freed first would make the resolver
 * read freed memory, and the test would be reporting on that instead.
 *
 * The document is the full UART `cmd 3` shape rather than a bare device entry,
 * because the whole point is that bytes follow the value. A single-key object
 * would end in `"}` and still mislead by looking shorter. */
static bool resolve_ref_dev0(const char *json, s_aledcfg_t *out)
{
    cJSON *const root = cJSON_ParseWithRef(__FUNCTION__, json);
    cJSON *list;
    cJSON *dev;
    bool verbatim;

    list = (NULL != root) ? cJSON_GetObjectItem(__FUNCTION__, root, "dev_detail") : NULL;
    dev = (NULL != list) ? cJSON_GetArrayItem(list, 0) : NULL;

    /* A document this test failed to navigate would otherwise look exactly
     * like a device entry the resolver rejected. */
    if (NULL == dev)
    {
        printf("FAIL  this test's own JSON did not parse by reference: %s\n", json);
        failures++;
        checks++;
        out->pixel_count = 0;
        out->has_white = false;
        out->order = ALED_ORDER_GRB;
        /* ALED_CHIP_MAX, not the default: this is the path where the test's own
         * JSON was broken, and a field left at its default here could make a
         * later CHECK pass for the wrong reason. MAX cannot satisfy any of
         * them. */
        out->chip = ALED_CHIP_MAX;
        cJSON_Delete(__FUNCTION__, root);
        return false;
    }

    verbatim = EZPI_aledcfg_from_json(dev, out);
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
    CHECK(ALED_CHIP_SK6812 == cfg.chip,
          "...and SK6812 timings, which is what device_0009 hardcoded, so the "
          "strips already in the field keep the 600ns t1h they are working on "
          "today rather than being re-timed by a firmware update");
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
    cfg.chip = ALED_CHIP_APA106;
    verbatim = EZPI_aledcfg_from_json(NULL, &cfg);
    CHECK(1 == cfg.pixel_count,
          "a NULL device object fills the defaults rather than leaving the "
          "caller's struct untouched -- it was pre-poisoned with "
          "999/true/BGR/APA106 above, so this check fails if the function "
          "returns early");
    CHECK(false == cfg.has_white, "...including has_white");
    CHECK(ALED_ORDER_GRB == cfg.order, "...including the order");
    CHECK(ALED_CHIP_SK6812 == cfg.chip, "...including the chip");
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

/* ------------------------------------------------------------------------- *
 * 6. The parser the device actually uses.
 *
 * THIS IS THE SECTION THE REST OF THE FILE MISSED.
 *
 * Every check above this point goes through cJSON_Parse(), which COPIES each
 * string into its own NUL-terminated allocation. The device never does that.
 * EZPI_device_prepare() parses the stored configuration with
 * cJSON_ParseWithRef() (ezlopi_core_devices.c:630), whose valuestring points
 * into the payload and ends at a quote.
 *
 * So the resolver read "grb\"}]}" where it expected "grb", found a length of 7
 * rather than 3, and refused all six colour orders on a real board while all
 * 17,576 strings in section 3 were classified correctly on the host. Measured
 * from the board itself, printed by temporary instrumentation in device-0009:
 *
 *     ALEDDBG: type=0x310 Strmask=0x10 and=0x10 ptr=0x3ffd8564
 *              str='grb"}]}' from_str=0 order=0
 *
 * The type test passed; the string was not a C string. These checks fail
 * against the resolver as it was before str_value_len was threaded through --
 * confirmed by reverting that one branch and re-running, which is the only
 * reason to believe they would catch it again.
 * ------------------------------------------------------------------------- */

/* The exact 179-byte document the board had stored at flash 0x19000 while it
 * was refusing the order. color_order is deliberately the LAST key, so the
 * bytes the resolver over-reads are `"}]}` -- the real failing case, not a
 * contrived one. */
#define REF_DOC_HEAD                                 \
    "{\"cmd\":3,\"chipset\":\"ESP32\",\"dev_detail\":" \
    "[{\"device_id\":\"1002e001\",\"id_item\":9,"      \
    "\"dev_name\":\"SP803E Strip\",\"dev_type\":5,"   \
    "\"gpio1\":16,\"pixel_count\":30,\"rgbw\":false,\"color_order\":\""

static void test_the_parser_the_device_actually_uses(void)
{
    s_aledcfg_t cfg;
    bool verbatim;

    verbatim = resolve_ref_dev0(REF_DOC_HEAD "grb\"}]}", &cfg);
    CHECK(true == verbatim,
          "the exact document the board had stored is taken verbatim when "
          "parsed by reference -- this is the check that was failing on "
          "hardware while the whole rest of this file passed");
    CHECK(30 == cfg.pixel_count, "...30 pixels");
    CHECK(false == cfg.has_white, "...no white channel");
    CHECK(ALED_ORDER_GRB == cfg.order, "...and GRB, read from a string that is "
                                       "terminated by a quote rather than a NUL");

    /* All six, because GRB is also the default: accepting it proves nothing on
     * its own. RGB is the discriminating one -- under the defect it came back
     * GRB, which is a strip lit in the wrong colours, not a strip that fails. */
    verbatim = resolve_ref_dev0(REF_DOC_HEAD "rgb\"}]}", &cfg);
    CHECK((true == verbatim) && (ALED_ORDER_RGB == cfg.order),
          "\"rgb\" by reference resolves to RGB -- under the defect this "
          "returned GRB and verbatim=false, i.e. the wrong colours rather than "
          "a visible failure");

    verbatim = resolve_ref_dev0(REF_DOC_HEAD "brg\"}]}", &cfg);
    CHECK((true == verbatim) && (ALED_ORDER_BRG == cfg.order), "\"brg\" by reference");

    verbatim = resolve_ref_dev0(REF_DOC_HEAD "rbg\"}]}", &cfg);
    CHECK((true == verbatim) && (ALED_ORDER_RBG == cfg.order), "\"rbg\" by reference");

    verbatim = resolve_ref_dev0(REF_DOC_HEAD "gbr\"}]}", &cfg);
    CHECK((true == verbatim) && (ALED_ORDER_GBR == cfg.order), "\"gbr\" by reference");

    verbatim = resolve_ref_dev0(REF_DOC_HEAD "bgr\"}]}", &cfg);
    CHECK((true == verbatim) && (ALED_ORDER_BGR == cfg.order), "\"bgr\" by reference");

    /* Selective, not blanket: a resolver that accepted everything by reference
     * would pass all six above. */
    verbatim = resolve_ref_dev0(REF_DOC_HEAD "zzz\"}]}", &cfg);
    CHECK((false == verbatim) && (ALED_ORDER_GRB == cfg.order),
          "\"zzz\" by reference is still refused -- the six checks above are "
          "only worth anything if a bad order can still be told apart");

    /* A value with another key after it, so the over-read bytes are `","` and
     * a letter rather than punctuation. The defect refused this one too, but
     * for a 3-byte value followed by `",` a prefix-matching mutant would not. */
    verbatim = resolve_ref_dev0(
        "{\"cmd\":3,\"dev_detail\":[{\"id_item\":9,\"color_order\":\"rbg\","
        "\"gpio1\":16,\"pixel_count\":12}]}", &cfg);
    CHECK((true == verbatim) && (ALED_ORDER_RBG == cfg.order) && (12 == cfg.pixel_count),
          "color_order in the middle of the document, followed by more keys, "
          "resolves too -- the over-read bytes being punctuation is not what "
          "makes this work");
}

/* ------------------------------------------------------------------------- *
 * 7. The bounded entry point, against buffers no parser was involved in.
 *
 * The three ways cjext builds a string give three different relationships
 * between str_value_len and the real length, documented in the header and
 * measured in ezlopi_cloud_mqtt_config.c:428-455. One rule has to be right for
 * all of them: take up to `bound` bytes, stopping early at a NUL -- what
 * "%.*s" does. These checks are that rule stated as behaviour, built by hand so
 * they do not depend on any parser agreeing with them.
 * ------------------------------------------------------------------------- */

static void test_the_bound_is_honoured_both_ways(void)
{
    /* No NUL anywhere. Deliberately not a string literal: a literal would
     * carry a terminator and quietly make this test the same as the old one. */
    static const char not_terminated[] = {'g', 'r', 'b', '"', '}', ']', '}'};
    /* An exact length from cJSON_ParseWithRef and cJSON_CreateString. */
    static const char exact_three[] = {'b', 'g', 'r'};
    /* An upper bound, which is what cJSON_Parse's allocation_length is: the
     * value is shorter than the bound and a NUL arrives first. */
    static const char padded[] = {'r', 'g', 'b', '\0', 'x', 'y'};

    e_aled_order_t order = ALED_ORDER_MAX;

    CHECK(EZPI_aledcfg_order_from_strn(not_terminated, 3u, &order) &&
              (ALED_ORDER_GRB == order),
          "a buffer with no NUL in it at all resolves when the bound says 3 -- "
          "the reference-parser case, reduced to the one fact that matters");

    order = ALED_ORDER_MAX;
    CHECK(EZPI_aledcfg_order_from_strn(exact_three, 3u, &order) &&
              (ALED_ORDER_BGR == order),
          "a bound exactly equal to the length is the ordinary case");

    order = ALED_ORDER_MAX;
    CHECK(EZPI_aledcfg_order_from_strn(padded, sizeof(padded), &order) &&
              (ALED_ORDER_RGB == order),
          "a bound LARGER than the value still resolves, stopping at the NUL -- "
          "required because cJSON_Parse reports allocation_length, an upper "
          "bound, so a parsed 3-letter order arrives with str_value_len 4");

    order = ALED_ORDER_MAX;
    CHECK(false == EZPI_aledcfg_order_from_strn(not_terminated,
                                                sizeof(not_terminated), &order),
          "...but a bound that over-states the length of a buffer with no NUL "
          "is refused rather than guessed at: 'grb\"}]}' is 7 bytes and is not "
          "an order. This is the defect's own symptom, pinned as behaviour");

    order = ALED_ORDER_MAX;
    CHECK(false == EZPI_aledcfg_order_from_strn(not_terminated, 2u, &order),
          "a bound of 2 over \"grb\" is refused -- a prefix compare would have "
          "matched, and a bound is not permission to read less either");

    order = ALED_ORDER_MAX;
    CHECK(false == EZPI_aledcfg_order_from_strn(not_terminated, 0u, &order),
          "a bound of 0 is refused and reads nothing");

    CHECK(false == EZPI_aledcfg_order_from_strn(NULL, 3u, &order),
          "a NULL string is refused rather than dereferenced");

    CHECK(false == EZPI_aledcfg_order_from_strn(exact_three, 3u, NULL),
          "a NULL out pointer is refused rather than written through");

    /* The NUL-terminated wrapper must still agree with the bounded form, or
     * there are two implementations of the table walk to keep in step. */
    order = ALED_ORDER_MAX;
    CHECK(EZPI_aledcfg_order_from_str("GBR", &order) && (ALED_ORDER_GBR == order),
          "the NUL-terminated wrapper still case-folds and still agrees with "
          "the bounded form it delegates to");
}

/* ------------------------------------------------------------------------- *
 * 8. led_chip -- which timing family the data is clocked out at.
 *
 * WHAT THIS KEY IS FOR
 *
 * device_0009 assigned `type = LED_STRIP_SK6812` as a literal while the driver
 * carried four timing families (led_strip.c:168-171). A WS2812B, which is the
 * commonest addressable reel sold, was therefore clocked with SK6812's 600ns
 * t1h instead of its own 1000ns, whatever the stored configuration said --
 * there was no key to say it with.
 *
 * WHY THE FAILURE MODE JUSTIFIES MORE CARE THAN THE OTHER KEYS, NOT LESS
 *
 * A wrong pixel_count lights a prefix of the strip. A wrong color_order lights
 * it in the wrong colours. Both are obvious the moment anyone looks. Wrong bit
 * timings are not: they are within the parts' tolerances often enough that the
 * strip mostly works and occasionally shows one wrong pixel, which is the
 * hardest class of fault to attribute -- and this project has already spent a
 * day attributing a dark strip to the wrong layer.
 *
 * So the checks below are weighted towards the quiet wrongs: that a name which
 * is nearly right is refused rather than guessed at, that the default is the
 * shipped timing rather than the more popular part, and that every chip the
 * enum offers can actually be named.
 * ------------------------------------------------------------------------- */

static void test_only_the_named_chips_are_a_chip(void)
{
    static const struct
    {
        const char *str;
        e_aled_chip_t want;
    } k_ok[] = {
        {"sk6812", ALED_CHIP_SK6812},
        {"ws2812", ALED_CHIP_WS2812},
        {"ws2812b", ALED_CHIP_WS2812},
        {"apa106", ALED_CHIP_APA106},
        {"sm16703", ALED_CHIP_SM16703},
        /* Case folding, on each of the two name lengths. */
        {"SK6812", ALED_CHIP_SK6812},
        {"Ws2812B", ALED_CHIP_WS2812},
        {"SM16703", ALED_CHIP_SM16703},
    };

    /* Every one of these is a plausible thing for a person to type off a reel
     * or a datasheet, which is exactly why each must be refused rather than
     * accepted approximately. A substituted chip leaves SK6812 in place and is
     * reported, so the user gets a working-looking strip and a log line --
     * whereas a guess would get them marginal timings and silence. */
    static const char *const k_refused[] = {
        "ws2813",   /* a real part, close timings. Guessing would be the trap */
        "ws2815",   /* ditto, and 12V -- not something to infer from a name   */
        "ws2812c",  /* not a part this driver has a row for                   */
        "ws281",    /* truncated                                             */
        "ws2812bb", /* over-long: a bounded compare of 7 would accept this    */
        "sk6812rgbw", /* the white channel is the `rgbw` key's job, not this  */
        "sk",
        "",
        " ws2812",  /* leading space: not trimmed, because trimming one field
                     * and not the others is worse than trimming none         */
        "ws2812 ",
        "neopixel", /* a brand, not a timing family                           */
        "apa102",   /* a real part, but SPI -- this driver cannot drive it at
                     * all, so mapping it to anything would be a lie          */
    };

    for (size_t i = 0u; i < (sizeof(k_ok) / sizeof(k_ok[0])); i++)
    {
        e_aled_chip_t got = ALED_CHIP_MAX;
        const bool ok = EZPI_aledcfg_chip_from_strn(k_ok[i].str,
                                                    strlen(k_ok[i].str), &got);

        CHECK(ok && (k_ok[i].want == got), k_ok[i].str);
    }

    for (size_t i = 0u; i < (sizeof(k_refused) / sizeof(k_refused[0])); i++)
    {
        e_aled_chip_t got = ALED_CHIP_MAX;
        const bool ok = EZPI_aledcfg_chip_from_strn(k_refused[i],
                                                    strlen(k_refused[i]), &got);

        /* Refused AND untouched. A parser that returned false after writing
         * the out parameter would pass a return-value-only check while
         * corrupting a caller that kept its default in that variable. */
        CHECK((false == ok) && (ALED_CHIP_MAX == got), k_refused[i]);
    }

    e_aled_chip_t spare = ALED_CHIP_MAX;

    CHECK(false == EZPI_aledcfg_chip_from_strn(NULL, 6u, &spare),
          "a NULL string is refused rather than dereferenced");
    CHECK(false == EZPI_aledcfg_chip_from_strn("ws2812", 6u, NULL),
          "a NULL out pointer is refused rather than written through");
}

static void test_the_chip_bound_is_honoured(void)
{
    /* No NUL anywhere in the value, which is what cJSON_ParseWithRef() hands
     * over: the bytes after the name are the rest of the payload. Written as a
     * sized array with no terminator so that reading past it is a real
     * overrun for ASan to find, not a read of a convenient zero. */
    static const char not_terminated[] = {'w', 's', '2', '8', '1', '2', '"', '}', ']', '}'};
    e_aled_chip_t chip = ALED_CHIP_MAX;

    CHECK(EZPI_aledcfg_chip_from_strn(not_terminated, 6u, &chip) &&
              (ALED_CHIP_WS2812 == chip),
          "a 6-byte bound over an unterminated buffer reads exactly the name "
          "-- the colour-order parser's field defect, pinned for this key "
          "before it can happen rather than after");

    chip = ALED_CHIP_MAX;
    CHECK(false == EZPI_aledcfg_chip_from_strn(not_terminated,
                                               sizeof(not_terminated), &chip),
          "...and a bound that over-states the length is refused rather than "
          "guessed at: 'ws2812\"}]}' is not a chip");

    chip = ALED_CHIP_MAX;
    CHECK(false == EZPI_aledcfg_chip_from_strn(not_terminated, 5u, &chip),
          "a bound of 5 over \"ws2812\" is refused -- a prefix compare would "
          "have matched 'ws281' against nothing, but strncmp(str, name, len) "
          "would have matched it against \"ws2812\"");

    chip = ALED_CHIP_MAX;
    CHECK(false == EZPI_aledcfg_chip_from_strn(not_terminated, 0u, &chip),
          "a bound of 0 is refused and reads nothing");

    /* A NUL inside the bound stops the measurement, so a short name in a
     * generously-bounded buffer still works -- the rule "%.*s" uses, and the
     * one the order parser uses, stated for this one too. */
    chip = ALED_CHIP_MAX;
    CHECK(EZPI_aledcfg_chip_from_strn("apa106", 64u, &chip) &&
              (ALED_CHIP_APA106 == chip),
          "a bound well past a NUL-terminated name stops at the NUL, which is "
          "what cJSON_Parse()'s allocation-sized str_value_len needs");
}

static void test_every_chip_can_be_named_and_printed(void)
{
    /* The name table is a list of pairs rather than an array indexed by the
     * enum, so nothing in the code guarantees the two are in step. A chip the
     * enum offers that no string can select would be a configuration that
     * silently cannot be requested; a chip with no name would make the
     * driver's warning print "?" on the one path where it matters. */
    for (unsigned c = 0u; c < (unsigned)ALED_CHIP_MAX; c++)
    {
        const char *const name = EZPI_aledcfg_chip_name((e_aled_chip_t)c);
        e_aled_chip_t back = ALED_CHIP_MAX;

        CHECK((NULL != name) && ('?' != name[0]),
              "every chip the enum offers has a printable name");

        /* Round trip. This is what actually ties the two directions of the
         * table together: a shuffled table would still print *a* name for
         * every chip and still parse *a* chip for every name, and only the
         * round trip notices that they are not the same pairing. */
        CHECK(EZPI_aledcfg_chip_from_strn(name, strlen(name), &back) &&
                  ((e_aled_chip_t)c == back),
              name);
    }

    /* Canonical, not an echo: the alias must never be what gets printed. */
    CHECK(0 == strcmp("ws2812", EZPI_aledcfg_chip_name(ALED_CHIP_WS2812)),
          "ALED_CHIP_WS2812 prints as \"ws2812\", never as the \"ws2812b\" "
          "alias that may have selected it");

    /* The logging helper is called on a path that is already going wrong, so
     * it is the worst possible place to fault. */
    CHECK(NULL != EZPI_aledcfg_chip_name(ALED_CHIP_MAX),
          "ALED_CHIP_MAX gives a string rather than indexing past the table");
    CHECK(0 == strcmp("?", EZPI_aledcfg_chip_name((e_aled_chip_t)99)),
          "and so does a value that is not an enumerator at all");
}

static void test_the_chip_key_through_both_parsers(void)
{
    s_aledcfg_t cfg;
    bool verbatim;

    verbatim = resolve_str("{\"led_chip\":\"ws2812b\"}", &cfg);
    CHECK(true == verbatim, "a recognised led_chip is taken verbatim");
    CHECK(ALED_CHIP_WS2812 == cfg.chip, "...and selects WS2812 timings");
    CHECK(1 == cfg.pixel_count, "...without disturbing the other defaults");
    CHECK(ALED_ORDER_GRB == cfg.order, "...including the order");

    /* The device's own parser, which is the one that broke for color_order. */
    verbatim = resolve_ref_dev0(
        "{\"dev_detail\":[{\"gpio1\":16,\"pixel_count\":30,"
        "\"color_order\":\"grb\",\"led_chip\":\"ws2812\"}]}",
        &cfg);
    CHECK(true == verbatim,
          "a full device entry read by reference -- the parser the device "
          "actually uses -- is taken verbatim with led_chip last, so the bytes "
          "the parser could over-read are the real `\"}]}`");
    CHECK(ALED_CHIP_WS2812 == cfg.chip, "...and the chip survives it");
    CHECK(30 == cfg.pixel_count, "...alongside the count");
    CHECK(ALED_ORDER_GRB == cfg.order, "...and the order");

    /* An unrecognised name must not take the other keys down with it. */
    verbatim = resolve_str("{\"led_chip\":\"ws2813\",\"pixel_count\":30,"
                           "\"rgbw\":true,\"color_order\":\"rgb\"}",
                           &cfg);
    CHECK(false == verbatim, "an unknown led_chip makes the config non-verbatim");
    CHECK(ALED_CHIP_SK6812 == cfg.chip, "...and falls back to the shipped chip");
    CHECK(30 == cfg.pixel_count, "...with the good count kept");
    CHECK(true == cfg.has_white, "...the good rgbw kept");
    CHECK(ALED_ORDER_RGB == cfg.order, "...and the good order kept");

    /* Wrong JSON types. A number is the tempting one to accept, and accepting
     * it would make the enum's numbering part of the stored format -- the same
     * argument color_order makes, and the reason SK6812 could be renumbered
     * without touching a single provisioned device. */
    CHECK((false == resolve_str("{\"led_chip\":1}", &cfg)) &&
              (ALED_CHIP_SK6812 == cfg.chip),
          "a numeric led_chip is refused rather than used as an enum index");
    CHECK((false == resolve_str("{\"led_chip\":null}", &cfg)) &&
              (ALED_CHIP_SK6812 == cfg.chip),
          "a null led_chip is refused");
    CHECK((false == resolve_str("{\"led_chip\":true}", &cfg)) &&
              (ALED_CHIP_SK6812 == cfg.chip),
          "a boolean led_chip is refused");
    CHECK((false == resolve_str("{\"led_chip\":{\"name\":\"ws2812\"}}", &cfg)) &&
              (ALED_CHIP_SK6812 == cfg.chip),
          "an object led_chip is refused rather than searched");

    /* The key name itself. A reader looking for the wrong spelling finds
     * nothing and reports verbatim, which is the silent failure the key
     * constants in ezlopi-cloud-constants exist to prevent. */
    verbatim = resolve_str("{\"chip\":\"ws2812\"}", &cfg);
    CHECK(true == verbatim,
          "a key named \"chip\" is not this key and is not the resolver's "
          "business, so it is not a disagreement");
    CHECK(ALED_CHIP_SK6812 == cfg.chip,
          "...and it does not select anything either -- the spelling is "
          "\"led_chip\"");
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
    test_the_parser_the_device_actually_uses();
    test_the_bound_is_honoured_both_ways();
    test_only_the_named_chips_are_a_chip();
    test_the_chip_bound_is_honoured();
    test_every_chip_can_be_named_and_printed();
    test_the_chip_key_through_both_parsers();

    /* Bare, with no "test_aledcfg: " prefix: "N check(s), M failure(s)" is the
     * shape the suite's aggregate tally reads, and a prefixed line matches
     * nothing. See the note in test_scenefields.c, which got this wrong. */
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return (0 == failures) ? 0 : 1;
}
