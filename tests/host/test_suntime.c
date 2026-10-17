/* Host tests for the isSunState suntime helpers, ezlopi_core_scenes_suntime.c.
 *
 * WHY THIS FILE EXISTS
 *
 * __issunsate_update_sunstate_tm was a stub that looked like a working
 * function. It read the stored latitude and longitude, traced them, freed
 * them, and then built its URL from a hardcoded pair of Kathmandu
 * coordinates in both arms of the `if`. It called
 * EZPI_core_http_mbedtls_req for effect and never looked at the reply. Then it
 * assigned the sun time from three literals -- 05:48:42, plus twelve hours for
 * sunset -- under a comment reading "e.g. after valid extraction". So every
 * isSunState condition in the firmware fired at 05:48:42 or 17:48:42,
 * everywhere on earth, on every date, whether or not the request succeeded.
 *
 * The failure handling below those literals could not run either: it cleared
 * curr_tm_day when hour, minute and second were all zero, and the hour was
 * `5 + (... ? 12 : 0)` with the other two literals, so the condition was false
 * by construction.
 *
 * This file covers the three parts of the repair that are decidable without a
 * radio: parsing a clock reading, building the query from stored coordinates,
 * and pulling a time out of a reply body.
 *
 * THE ONE THING THAT CANNOT BE CHECKED HERE, AND WHY IT IS STILL SAFE
 *
 * Nothing in this environment can confirm the shape of a sunrisesunset.io
 * reply: there is no network and no hardware. The fixtures below are built
 * from the service's documented format, and that is an assumption, recorded as
 * one. It is survivable because of the direction the code fails in. A body
 * whose shape does not match is rejected, the caller clears curr_tm_day, and
 * the scene does not fire -- so a wrong guess about the field names costs a
 * scene that never triggers and says why, rather than one that triggers at a
 * fabricated time. test_reply_shape_mismatches_are_rejected is the test that
 * holds that property, and it matters more than the ones using the fixture.
 *
 * The 12-hour cases are the other reason this file is worth more than reading
 * the code. "12:00:00 AM" is hour 0 and "12:00:00 PM" is hour 12; adding
 * twelve to every PM reading puts noon at midnight the next day, and treating
 * 12 AM as hour 12 puts a sunrise at noon. Both are one-line mistakes that
 * look right.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ezlopi_core_scenes_suntime.h"

static int g_checks;
static int g_fails;

static void ok(int cond, const char *what)
{
    g_checks++;
    if (!cond)
    {
        g_fails++;
        printf("  FAIL: %s\n", what);
    }
}

/*******************************************************************************
 *                          the clock parser
 *******************************************************************************/

/* Assert a reading parses to an exact 24-hour time. The outputs are seeded
** with a value no clock can hold, so a function that returned success without
** writing them would fail rather than coincidentally match. */
static void clock_is(const char *in, int eh, int em, int es)
{
    int h = -1;
    int m = -1;
    int s = -1;
    char what[96];

    if (EZPI_SUCCESS != EZPI_core_suntime_parse_clock(in, &h, &m, &s))
    {
        snprintf(what, sizeof(what), "'%s' should parse", in);
        ok(0, what);
        return;
    }

    snprintf(what, sizeof(what), "'%s' -> %02d:%02d:%02d", in, eh, em, es);
    ok((eh == h) && (em == m) && (es == s), what);
}

/* Assert a reading is rejected AND that the outputs were left alone. The
** second half is the part worth testing: a caller that forgets to check the
** return value must not find a half-parsed time sitting in its variables. */
static void clock_rejected(const char *in)
{
    int h = -7;
    int m = -7;
    int s = -7;
    char what[96];

    snprintf(what, sizeof(what), "'%s' is rejected and writes nothing", in);
    ok((EZPI_SUCCESS != EZPI_core_suntime_parse_clock(in, &h, &m, &s)) &&
           (-7 == h) && (-7 == m) && (-7 == s),
       what);
}

static void test_clock_12_hour(void)
{
    printf("-- 12-hour readings --\n");

    /* The shape the service documents: no leading zero on the hour. */
    clock_is("5:59:11 AM", 5, 59, 11);
    clock_is("6:04:22 PM", 18, 4, 22);

    /* With a leading zero, in case the format ever gains one. */
    clock_is("05:59:11 AM", 5, 59, 11);

    /* The two that a naive +12 gets wrong. */
    clock_is("12:00:00 AM", 0, 0, 0);
    clock_is("12:00:00 PM", 12, 0, 0);
    clock_is("12:30:45 AM", 0, 30, 45);
    clock_is("12:30:45 PM", 12, 30, 45);

    /* Either side of each boundary, so an off-by-one in the 12 case shows. */
    clock_is("11:59:59 AM", 11, 59, 59);
    clock_is("1:00:00 PM", 13, 0, 0);
    clock_is("11:59:59 PM", 23, 59, 59);
    clock_is("1:00:00 AM", 1, 0, 0);

    /* Lower case and extra spacing, neither of which changes the meaning. */
    clock_is("7:15:00 pm", 19, 15, 0);
    clock_is("  7:15:00   PM  ", 19, 15, 0);
}

static void test_clock_24_hour(void)
{
    printf("-- 24-hour readings --\n");

    clock_is("17:59:11", 17, 59, 11);
    clock_is("00:00:00", 0, 0, 0);
    clock_is("23:59:59", 23, 59, 59);
    clock_is("5:48:42", 5, 48, 42);
}

static void test_clock_rejections(void)
{
    printf("-- readings that must be refused --\n");

    clock_rejected("");
    clock_rejected("   ");
    clock_rejected("AM");
    clock_rejected("5:59");           /* no seconds */
    clock_rejected("5:59:");          /* colon, then nothing */
    clock_rejected("5-59-11");        /* wrong separator */
    clock_rejected("5:59:11 XM");     /* not a meridiem */
    clock_rejected("5:59:11 A");      /* meridiem cut short */
    clock_rejected("5:59:11 AM junk");/* trailing text */
    clock_rejected("5:59:11AM extra");
    clock_rejected("245:00:00");      /* three-digit hour, must not read as 24 */
    clock_rejected("5:591:1");        /* three-digit minute */

    /* Three digits that would be in range once read. These are the cases that
    ** separate the two-digit cap from the range checks below it: "245:00:00"
    ** is refused either way, because 245 is not an hour, but "005:30:00" would
    ** pass every range check if a third digit were consumed. A mutation that
    ** widened the cap to three digits survived the whole suite until these
    ** were added, which is the only reason they exist. */
    clock_rejected("005:30:00");
    clock_rejected("5:030:00");
    clock_rejected("5:30:000");
    clock_rejected("012:30:00 PM");
    clock_rejected("24:00:00");       /* out of range for 24-hour */
    clock_rejected("5:60:00");
    clock_rejected("5:00:60");
    clock_rejected("0:30:00 AM");     /* no hour 0 on a 12-hour clock */
    clock_rejected("13:30:00 PM");    /* nor an hour 13 */
    clock_rejected("-5:30:00");
    clock_rejected("sunrise");

    /* NULL in any position. */
    {
        int h = 0;
        int m = 0;
        int s = 0;
        ok(EZPI_SUCCESS != EZPI_core_suntime_parse_clock(NULL, &h, &m, &s), "NULL string is refused");
        ok(EZPI_SUCCESS != EZPI_core_suntime_parse_clock("5:00:00", NULL, &m, &s), "NULL hour is refused");
        ok(EZPI_SUCCESS != EZPI_core_suntime_parse_clock("5:00:00", &h, NULL, &s), "NULL min is refused");
        ok(EZPI_SUCCESS != EZPI_core_suntime_parse_clock("5:00:00", &h, &m, NULL), "NULL sec is refused");
    }
}

/* Every minute of a day, through both renderings, must come back as itself.
** This is the check that would catch a 12-hour table that is right at the
** cases someone thought to list and wrong at one they did not. */
static void test_clock_round_trips_every_minute(void)
{
    int bad_12 = 0;
    int bad_24 = 0;
    int hh;
    int mm;

    printf("-- every minute of the day, both renderings --\n");

    for (hh = 0; hh < 24; hh++)
    {
        for (mm = 0; mm < 60; mm++)
        {
            char buf[32];
            int h = -1;
            int m = -1;
            int s = -1;
            int h12 = (0 == (hh % 12)) ? 12 : (hh % 12);

            snprintf(buf, sizeof(buf), "%d:%02d:30 %s", h12, mm, (hh < 12) ? "AM" : "PM");
            if ((EZPI_SUCCESS != EZPI_core_suntime_parse_clock(buf, &h, &m, &s)) ||
                (h != hh) || (m != mm) || (30 != s))
            {
                if (0 == bad_12)
                {
                    printf("    first 12-hour mismatch: '%s' -> %02d:%02d:%02d\n", buf, h, m, s);
                }
                bad_12++;
            }

            snprintf(buf, sizeof(buf), "%02d:%02d:30", hh, mm);
            h = -1;
            m = -1;
            s = -1;
            if ((EZPI_SUCCESS != EZPI_core_suntime_parse_clock(buf, &h, &m, &s)) ||
                (h != hh) || (m != mm) || (30 != s))
            {
                if (0 == bad_24)
                {
                    printf("    first 24-hour mismatch: '%s' -> %02d:%02d:%02d\n", buf, h, m, s);
                }
                bad_24++;
            }
        }
    }

    printf("    1440 minutes x 2 renderings: %d and %d mismatches\n", bad_12, bad_24);
    ok(0 == bad_12, "all 1440 minutes survive the 12-hour rendering");
    ok(0 == bad_24, "all 1440 minutes survive the 24-hour rendering");
}

/*******************************************************************************
 *                          the coordinate query
 *******************************************************************************/

static void test_query_from_coordinates(void)
{
    char q[64];

    printf("-- the query built from stored coordinates --\n");

    /* The exact string EZPI_hub_coordinates_set stores: the `params` object of
    ** the cloud call, printed. */
    ok(EZPI_SUCCESS == EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":27.700769,\"longitude\":85.300140}"),
       "a stored coordinate pair builds a query");
    ok(0 == strcmp(q, "json?lat=27.700769&lng=85.300140"), "and the query names those coordinates");

    /* No leading slash: __ezlopi_http_generate_request writes "GET /%s". */
    ok('/' != q[0], "the path carries no leading slash");

    /* Negative and southern/western values, where a format mistake shows. */
    ok(EZPI_SUCCESS == EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":-33.868820,\"longitude\":151.209290}"),
       "a southern latitude builds a query");
    ok(0 == strcmp(q, "json?lat=-33.868820&lng=151.209290"), "and keeps the sign");

    ok(EZPI_SUCCESS == EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":0,\"longitude\":0}"),
       "the null island is a real place as far as this is concerned");
    ok(0 == strcmp(q, "json?lat=0.000000&lng=0.000000"), "and formats as zero");

    /* Extra members are the normal case -- the stored object is whatever the
    ** cloud sent as `params`. */
    ok(EZPI_SUCCESS == EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":10.5,\"longitude\":20.25,\"accuracy\":3}"),
       "unrelated members are ignored");
    ok(0 == strcmp(q, "json?lat=10.500000&lng=20.250000"), "and do not disturb the query");
}

static void test_query_refusals(void)
{
    char q[64];

    printf("-- coordinates that must not produce a query --\n");

    /* Each of these used to be the Kathmandu fallback. A query must not be
    ** produced at all, because a query for somewhere the user never chose
    ** returns a sunrise that looks entirely plausible. */
    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), "{}"),
       "an empty object is refused");
    ok('\0' == q[0], "and leaves no query behind");

    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), "not json at all"),
       "a non-JSON blob is refused");
    ok('\0' == q[0], "and leaves no query behind");

    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":27.7}"),
       "latitude without longitude is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), "{\"longitude\":85.3}"),
       "longitude without latitude is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":\"27.7\",\"longitude\":\"85.3\"}"),
       "coordinates sent as strings are refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":null,\"longitude\":null}"),
       "null coordinates are refused");

    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":91,\"longitude\":0}"),
       "a latitude past the pole is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":-91,\"longitude\":0}"),
       "and past the other pole");
    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":0,\"longitude\":181}"),
       "a longitude past the date line is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":0,\"longitude\":-181}"),
       "and past it the other way");

    /* The boundaries themselves are legal. */
    ok(EZPI_SUCCESS == EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":90,\"longitude\":180}"),
       "the poles and the date line are accepted");
    ok(EZPI_SUCCESS == EZPI_core_suntime_build_query(q, sizeof(q), "{\"latitude\":-90,\"longitude\":-180}"),
       "as are their opposites");

    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(NULL, sizeof(q), "{\"latitude\":0,\"longitude\":0}"),
       "a NULL destination is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, 0, "{\"latitude\":0,\"longitude\":0}"),
       "a zero-length destination is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(q, sizeof(q), NULL),
       "a NULL coordinate blob is refused");
}

/* A buffer too small must fail and clear, not succeed with a prefix.
** snprintf terminates its truncated output, so the dangerous state is a
** well-formed query for the wrong place -- "json?lat=27.700769&lng=8". */
static void test_query_truncation(void)
{
    char small[20];
    int n;

    printf("-- a destination too small --\n");

    memset(small, 'Z', sizeof(small));
    ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(small, sizeof(small), "{\"latitude\":27.700769,\"longitude\":85.300140}"),
       "a short buffer is a failure, not a truncation");
    ok('\0' == small[0], "and the partial query is cleared rather than left to be sent");

    /* One byte short of exact, then exactly enough: the boundary itself. */
    n = (int)strlen("json?lat=27.700769&lng=85.300140");
    {
        char *tight = malloc((size_t)n + 1);
        char *short1 = malloc((size_t)n);
        ok(NULL != tight, "allocated the exact-fit buffer");
        ok(NULL != short1, "allocated the one-byte-short buffer");
        if (tight && short1)
        {
            ok(EZPI_SUCCESS == EZPI_core_suntime_build_query(tight, (uint32_t)n + 1, "{\"latitude\":27.700769,\"longitude\":85.300140}"),
               "a buffer of exactly strlen+1 succeeds");
            ok(0 == strcmp(tight, "json?lat=27.700769&lng=85.300140"), "with the whole query in it");
            ok(EZPI_SUCCESS != EZPI_core_suntime_build_query(short1, (uint32_t)n, "{\"latitude\":27.700769,\"longitude\":85.300140}"),
               "one byte less fails");
            ok('\0' == short1[0], "and clears");
        }
        free(tight);
        free(short1);
    }
}

/*******************************************************************************
 *                          the reply
 *******************************************************************************/

/* Built from the service's documented format. NOT verified against a live
** reply -- see the note at the top of this file. The tests that matter most
** are the rejections below, which hold regardless of whether this shape is
** right. */
static const char *k_reply =
    "{\"results\":{\"date\":\"2026-10-16\",\"sunrise\":\"5:59:11 AM\","
    "\"sunset\":\"6:04:22 PM\",\"dawn\":\"5:35:02 AM\",\"dusk\":\"6:28:31 PM\","
    "\"solar_noon\":\"12:01:46 PM\",\"day_length\":\"12:05:11\","
    "\"timezone\":\"Asia/Kathmandu\",\"utc_offset\":345},\"status\":\"OK\"}";

static void reply_gives(const char *body, uint8_t mode, int eh, int em, int es, const char *what)
{
    int h = -1;
    int m = -1;
    int s = -1;

    if (EZPI_SUCCESS != EZPI_core_suntime_from_reply(body, mode, &h, &m, &s))
    {
        ok(0, what);
        return;
    }
    ok((eh == h) && (em == m) && (es == s), what);
}

static void reply_refused(const char *body, uint8_t mode, const char *what)
{
    int h = -7;
    int m = -7;
    int s = -7;

    ok((EZPI_SUCCESS != EZPI_core_suntime_from_reply(body, mode, &h, &m, &s)) &&
           (-7 == h) && (-7 == m) && (-7 == s),
       what);
}

static void test_reply_extraction(void)
{
    printf("-- pulling a time out of a reply --\n");

    reply_gives(k_reply, EZPI_SUNTIME_MODE_SUNRISE, 5, 59, 11, "sunrise comes out as 05:59:11");
    reply_gives(k_reply, EZPI_SUNTIME_MODE_SUNSET, 18, 4, 22, "sunset comes out as 18:04:22");

    /* The mode selects the field, so asking for the other one must not return
    ** the first. This is the check that would catch a key lookup wired to a
    ** constant. */
    {
        int h1 = -1, m1 = -1, s1 = -1;
        int h2 = -1, m2 = -1, s2 = -1;
        EZPI_core_suntime_from_reply(k_reply, EZPI_SUNTIME_MODE_SUNRISE, &h1, &m1, &s1);
        EZPI_core_suntime_from_reply(k_reply, EZPI_SUNTIME_MODE_SUNSET, &h2, &m2, &s2);
        ok((h1 != h2) || (m1 != m2) || (s1 != s2), "the two modes do not return the same time");
    }

    /* Without the results wrapper. */
    reply_gives("{\"sunrise\":\"6:30:00 AM\",\"status\":\"OK\"}",
                EZPI_SUNTIME_MODE_SUNRISE, 6, 30, 0, "a reply with no results wrapper still parses");

    /* 24-hour times in the reply, which the service can be asked for. */
    reply_gives("{\"results\":{\"sunrise\":\"05:59:11\"},\"status\":\"OK\"}",
                EZPI_SUNTIME_MODE_SUNRISE, 5, 59, 11, "a 24-hour reply parses");
}

static void test_reply_shape_mismatches_are_rejected(void)
{
    printf("-- replies that must be refused --\n");

    /* THE tests of this file. If the assumed shape is wrong, these are what
    ** keep the consequence to "the scene does not fire". */
    reply_refused("{\"results\":{\"sunrise\":\"5:59:11 AM\"},\"status\":\"ERROR\"}",
                  EZPI_SUNTIME_MODE_SUNRISE, "a non-OK status is refused even with a time present");
    reply_refused("{\"results\":{\"sunrise\":\"5:59:11 AM\"}}",
                  EZPI_SUNTIME_MODE_SUNRISE, "a missing status is refused");
    reply_refused("{\"results\":{\"sunset\":\"6:04:22 PM\"},\"status\":\"OK\"}",
                  EZPI_SUNTIME_MODE_SUNRISE, "asking for sunrise when only sunset is present is refused");
    reply_refused("{\"results\":{},\"status\":\"OK\"}",
                  EZPI_SUNTIME_MODE_SUNRISE, "an empty results object is refused");
    reply_refused("{\"results\":{\"sunrise\":12345},\"status\":\"OK\"}",
                  EZPI_SUNTIME_MODE_SUNRISE, "a numeric sunrise is refused");
    reply_refused("{\"results\":{\"sunrise\":null},\"status\":\"OK\"}",
                  EZPI_SUNTIME_MODE_SUNRISE, "a null sunrise is refused");
    reply_refused("{\"results\":{\"sunrise\":\"not a time\"},\"status\":\"OK\"}",
                  EZPI_SUNTIME_MODE_SUNRISE, "an unparseable sunrise is refused");
    reply_refused("{\"status\":\"OK\"}",
                  EZPI_SUNTIME_MODE_SUNRISE, "a reply with no times at all is refused");

    /* Not JSON, and the kinds of not-JSON an HTTP path actually produces. */
    reply_refused("", EZPI_SUNTIME_MODE_SUNRISE, "an empty body is refused");
    reply_refused("<html><body>502 Bad Gateway</body></html>",
                  EZPI_SUNTIME_MODE_SUNRISE, "an HTML error page is refused");
    reply_refused("{\"results\":{\"sunrise\":\"5:59:11 AM\"},\"status\":\"OK\"",
                  EZPI_SUNTIME_MODE_SUNRISE, "a truncated body is refused");

    /* The exact shape every reply used to arrive in. Until the response
    ** accumulator was fixed, the mbedTLS path dropped the last byte of each
    ** 255-byte read, so a reply that fit in one read lost its closing brace.
    ** This is that body -- the fixture minus its final byte, sliced here
    ** rather than retyped so it cannot drift from the fixture -- and it must
    ** be refused rather than half-read. */
    {
        size_t len = strlen(k_reply);
        char *clipped = malloc(len); /* len-1 bytes plus a terminator */
        ok(NULL != clipped, "allocated the clipped reply");
        if (clipped)
        {
            memcpy(clipped, k_reply, len - 1);
            clipped[len - 1] = '\0';
            ok('}' == k_reply[len - 1], "the fixture really does end in the brace being removed");
            reply_refused(clipped, EZPI_SUNTIME_MODE_SUNRISE, "a reply missing its last byte is refused");
            free(clipped);
        }
    }

    reply_refused(NULL, EZPI_SUNTIME_MODE_SUNRISE, "a NULL body is refused");
    reply_refused(k_reply, 0, "sunstate mode 0 is refused");
    reply_refused(k_reply, 3, "an unknown sunstate mode is refused");

    {
        int h = 0;
        int m = 0;
        int s = 0;
        ok(EZPI_SUCCESS != EZPI_core_suntime_from_reply(k_reply, EZPI_SUNTIME_MODE_SUNRISE, NULL, &m, &s),
           "a NULL hour output is refused");
        ok(EZPI_SUCCESS != EZPI_core_suntime_from_reply(k_reply, EZPI_SUNTIME_MODE_SUNRISE, &h, NULL, &s),
           "a NULL min output is refused");
        ok(EZPI_SUCCESS != EZPI_core_suntime_from_reply(k_reply, EZPI_SUNTIME_MODE_SUNRISE, &h, &m, NULL),
           "a NULL sec output is refused");
    }
}

/* The firmware's old behaviour, asserted in the direction of the bug so that
** reintroducing it fails here. 05:48:42 is not sunrise anywhere the fixture
** describes, and the point is that the old code returned it regardless. */
static void test_the_old_hardcoded_time_is_gone(void)
{
    int h = -1;
    int m = -1;
    int s = -1;

    printf("-- the stub's hardcoded time --\n");

    ok(EZPI_SUCCESS == EZPI_core_suntime_from_reply(k_reply, EZPI_SUNTIME_MODE_SUNRISE, &h, &m, &s),
       "the fixture's sunrise is readable");
    ok(!((5 == h) && (48 == m) && (42 == s)), "and it is not the stub's 05:48:42");

    ok(EZPI_SUCCESS == EZPI_core_suntime_from_reply(k_reply, EZPI_SUNTIME_MODE_SUNSET, &h, &m, &s),
       "the fixture's sunset is readable");
    ok(!((17 == h) && (48 == m) && (42 == s)), "and it is not the stub's 17:48:42");
}

/*******************************************************************************
 *                          the offset parser
 *******************************************************************************/

/* Assert an offset string parses to an exact number of seconds. Seeded with a
** value the function cannot legitimately produce, so a success return that
** wrote nothing fails here rather than matching by luck. */
static void offs_is(const char *in, int expect_sec)
{
    int got = -12345;
    char what[96];

    if (EZPI_SUCCESS != EZPI_core_suntime_parse_offset(in, &got))
    {
        snprintf(what, sizeof(what), "offset '%s' should parse", in);
        ok(0, what);
        return;
    }

    snprintf(what, sizeof(what), "offset '%s' -> %ds", in, expect_sec);
    ok(expect_sec == got, what);
    if (expect_sec != got)
    {
        printf("    got %ds\n", got);
    }
}

/* Assert an offset is rejected AND that the output was left alone. The second
** half carries the weight: the shipped parser had no failure path at all, so
** every caller was a caller that ignored a return value it never got. */
static void offs_rejected(const char *in)
{
    int got = -12345;
    char what[96];

    snprintf(what, sizeof(what), "offset '%s' is rejected and writes nothing", in);
    ok((EZPI_SUCCESS != EZPI_core_suntime_parse_offset(in, &got)) && (-12345 == got), what);
}

static void test_offset_field_count_sets_the_units(void)
{
    printf("-- how many fields an offset has decides what they mean --\n");

    /* One field is seconds. This is the case the shipped code could not
    ** reach: its "only has seconds" branch sat behind `if (NULL != ptr1)`,
    ** and strtoul always stores an end pointer, so "30" was thirty hours. */
    offs_is("0", 0);
    offs_is("1", 1);
    offs_is("30", 30);
    offs_is("59", 59);

    /* Two fields are minutes and seconds -- the other dead branch. */
    offs_is("0:00", 0);
    offs_is("5:30", (5 * 60) + 30);
    offs_is("20:00", 20 * 60);
    offs_is("59:59", (59 * 60) + 59);

    /* Three fields are hours, minutes and seconds: the one form that worked
    ** before, so these are the checks that guard against the repair changing
    ** an answer that was already right. */
    offs_is("0:00:00", 0);
    offs_is("0:20:00", 20 * 60);
    offs_is("1:05:30", 3600 + (5 * 60) + 30);
    offs_is("23:59:59", (23 * 3600) + (59 * 60) + 59);

    /* Leading zeros are the same number, in every position. */
    offs_is("05", 5);
    offs_is("05:30", (5 * 60) + 30);
    offs_is("00:00:30", 30);
    offs_is("01:05:03", 3600 + (5 * 60) + 3);
}

static void test_offset_rejections(void)
{
    printf("-- offsets that are refused --\n");

    /* Nothing to parse. */
    offs_rejected("");
    offs_rejected(":");
    offs_rejected("::");

    /* A field missing on either side of a colon. */
    offs_rejected("5:");
    offs_rejected(":5");
    offs_rejected("1:2:");
    offs_rejected("1::2");

    /* More fields than there are units. "20:00:00:00" parsing as twenty
    ** hours and ignoring the rest is the failure mode worth naming. */
    offs_rejected("1:2:3:4");
    offs_rejected("20:00:00:00");

    /* Signs. strtoul accepts a leading '-' and negates the unsigned result,
    ** so the shipped code read "-1" as 4294967295, truncated it to -1 in an
    ** int, and quietly shifted the trigger an hour the other way. */
    offs_rejected("-1");
    offs_rejected("+1");
    offs_rejected("-0:20:00");

    /* Padding and units. An offset comes from the scene the user configured,
    ** not from a wire format with its own spacing rules. */
    offs_rejected(" 30");
    offs_rejected("30 ");
    offs_rejected("30s");
    offs_rejected("0:20:00 ");
    offs_rejected("20 min");

    /* Three digits in a field. "123" is not two minutes three seconds and it
    ** is not one hour twenty-three; it is not an offset. */
    offs_rejected("123");
    offs_rejected("1:234");
    offs_rejected("100:00:00");

    /* Out of range per field. */
    offs_rejected("60");
    offs_rejected("5:60");
    offs_rejected("60:00");
    offs_rejected("24:00:00");
    offs_rejected("0:60:00");
    offs_rejected("0:00:60");

    /* The one from the probe. The shipped code turned this into a trigger at
    ** 81:39:39 -- a scene that sits in the list looking configured and cannot
    ** fire, because the moment is matched against the wall clock. */
    offs_rejected("99:99:99");

    /* Not numbers. */
    offs_rejected("abc");
    offs_rejected("1:2:3a");
    offs_rejected("0x10");
    offs_rejected("1e3");

    /* Nine digits: the input that made the shipped code read
    ** time_diff[10] out of a char[10]. ASan confirms that read is real when
    ** the same byte is loaded directly, and says nothing when strtoul does
    ** it -- libc is not instrumented. So this check is not a formality; it is
    ** the only instrument that sees this input at all. */
    offs_rejected("123456789");
    offs_rejected("999999999");

    {
        int sink = 0;
        ok(EZPI_SUCCESS != EZPI_core_suntime_parse_offset(NULL, &sink), "a NULL offset string is refused");
        ok(EZPI_SUCCESS != EZPI_core_suntime_parse_offset("30", NULL), "a NULL output is refused");
    }
}

/*******************************************************************************
 *                          moving a suntime by an offset
 *******************************************************************************/

static void shift_is(int sh, int sm, int ss, int offs, int eh, int em, int es)
{
    int h = -1;
    int m = -1;
    int s = -1;
    char what[128];

    if (EZPI_SUCCESS != EZPI_core_suntime_shift(sh, sm, ss, offs, &h, &m, &s))
    {
        snprintf(what, sizeof(what), "%02d:%02d:%02d %+ds should shift", sh, sm, ss, offs);
        ok(0, what);
        return;
    }

    snprintf(what, sizeof(what), "%02d:%02d:%02d %+ds -> %02d:%02d:%02d", sh, sm, ss, offs, eh, em, es);
    ok((eh == h) && (em == m) && (es == s), what);
    if ((eh != h) || (em != m) || (es != s))
    {
        printf("    got %02d:%02d:%02d\n", h, m, s);
    }
}

/* The defect, stated as the cases that produced it.
 *
 * The shipped code reduced hours, minutes and seconds independently, each with
 * its own one-period wrap, and never carried between them. So the minute
 * borrowed and the hour did not. Every one of these came out exactly one hour
 * away from the right answer, in the wrong direction -- and "twenty minutes
 * before sunrise" is close to the canonical reason to write an isSunState
 * scene at all. */
static void test_shift_carries_between_fields(void)
{
    printf("-- the borrow and the carry --\n");

    /* 20 minutes before a 06:10:00 sunrise. The shipped code said 06:50:00:
    ** forty minutes *after* sunrise, for a scene configured to fire before
    ** it. */
    shift_is(6, 10, 0, -(20 * 60), 5, 50, 0);

    /* 20 minutes after a 06:50:00 sunrise. The shipped code said 06:10:00. */
    shift_is(6, 50, 0, 20 * 60, 7, 10, 0);

    /* 30 seconds before 06:10:10, where only the second field borrows. */
    shift_is(6, 10, 10, -30, 6, 9, 40);

    /* 90 minutes before 06:10:00, written as 1:30:00, where the hour field is
    ** nonzero *and* the minutes still borrow -- the shipped code said
    ** 05:40:00 instead of 04:40:00. */
    shift_is(6, 10, 0, -((1 * 3600) + (30 * 60)), 4, 40, 0);

    /* Both fields borrowing at once. */
    shift_is(6, 0, 0, -((0 * 3600) + (20 * 60) + 30), 5, 39, 30);

    /* And the carries, in the other direction. */
    shift_is(6, 50, 30, (0 * 3600) + (20 * 60) + 45, 7, 11, 15);

    /* Where no borrow is needed the old code was already right, so these are
    ** the ones that must not have moved. */
    shift_is(6, 10, 0, -(1 * 3600), 5, 10, 0);
    shift_is(6, 10, 0, 1 * 3600, 7, 10, 0);
    shift_is(6, 50, 30, 0, 6, 50, 30);
}

static void test_shift_wraps_at_midnight(void)
{
    printf("-- midnight, from both sides --\n");

    shift_is(0, 10, 0, -(20 * 60), 23, 50, 0);
    shift_is(23, 50, 0, 20 * 60, 0, 10, 0);
    shift_is(0, 0, 0, -1, 23, 59, 59);
    shift_is(23, 59, 59, 1, 0, 0, 0);
    shift_is(0, 0, 0, 0, 0, 0, 0);

    /* The extremes the parser can hand over: one second short of a full day,
    ** either way. */
    shift_is(0, 0, 0, 86399, 23, 59, 59);
    shift_is(0, 0, 0, -86399, 0, 0, 1);
    shift_is(12, 0, 0, 86399, 11, 59, 59);
    shift_is(12, 0, 0, -86399, 12, 0, 1);
}

/* An explicit carry chain -- the shape the shipped code was reaching for,
** written so that it actually carries. Used as an independent reference for
** the seconds-since-midnight arithmetic, so the sweep below is a cross-check
** between two different methods rather than a restatement of one. */
static void ref_step(int *h, int *m, int *s, int dir)
{
    *s += dir;
    if (*s < 0)
    {
        *s = 59;
        (*m)--;
    }
    else if (*s > 59)
    {
        *s = 0;
        (*m)++;
    }

    if (*m < 0)
    {
        *m = 59;
        (*h)--;
    }
    else if (*m > 59)
    {
        *m = 0;
        (*h)++;
    }

    if (*h < 0)
    {
        *h = 23;
    }
    else if (*h > 23)
    {
        *h = 0;
    }
}

static void test_shift_every_second_of_the_day(void)
{
    int bad_fwd = 0;
    int bad_back = 0;
    int bad_trip = 0;
    int bad_zero = 0;
    int t;

    /* One each side of every minute, hour and day boundary there is. */
    static const int k_offs[] = {1, -1, 59, -59, 60, -60, 61, -61,
                                 3599, -3599, 3600, -3600, 3601, -3601,
                                 20 * 60, -(20 * 60), 86399, -86399};

    printf("-- all 86400 seconds of the day, stepped and round-tripped --\n");

    for (t = 0; t < 86400; t++)
    {
        const int sh = t / 3600;
        const int sm = (t / 60) % 60;
        const int ss = t % 60;

        int h = -1;
        int m = -1;
        int s = -1;
        size_t i;

        /* Forward one second, against the explicit carry chain. */
        {
            int rh = sh;
            int rm = sm;
            int rs = ss;
            ref_step(&rh, &rm, &rs, +1);

            if ((EZPI_SUCCESS != EZPI_core_suntime_shift(sh, sm, ss, 1, &h, &m, &s)) ||
                (h != rh) || (m != rm) || (s != rs))
            {
                if (0 == bad_fwd)
                {
                    printf("    first +1s mismatch at %02d:%02d:%02d: got %02d:%02d:%02d, carry chain says %02d:%02d:%02d\n",
                           sh, sm, ss, h, m, s, rh, rm, rs);
                }
                bad_fwd++;
            }
        }

        /* And back one second. */
        {
            int rh = sh;
            int rm = sm;
            int rs = ss;
            ref_step(&rh, &rm, &rs, -1);

            if ((EZPI_SUCCESS != EZPI_core_suntime_shift(sh, sm, ss, -1, &h, &m, &s)) ||
                (h != rh) || (m != rm) || (s != rs))
            {
                if (0 == bad_back)
                {
                    printf("    first -1s mismatch at %02d:%02d:%02d: got %02d:%02d:%02d, carry chain says %02d:%02d:%02d\n",
                           sh, sm, ss, h, m, s, rh, rm, rs);
                }
                bad_back++;
            }
        }

        /* A zero offset must be the identity, everywhere. */
        if ((EZPI_SUCCESS != EZPI_core_suntime_shift(sh, sm, ss, 0, &h, &m, &s)) ||
            (h != sh) || (m != sm) || (s != ss))
        {
            bad_zero++;
        }

        /* Shift by an offset and back by the same, which has to land where it
        ** started -- including across midnight, where it is the wrap that has
        ** to be symmetric. This is a property rather than a recomputation, so
        ** it holds the arithmetic without assuming it. */
        for (i = 0; i < (sizeof(k_offs) / sizeof(k_offs[0])); i++)
        {
            int bh = -1;
            int bm = -1;
            int bs = -1;

            if ((EZPI_SUCCESS != EZPI_core_suntime_shift(sh, sm, ss, k_offs[i], &h, &m, &s)) ||
                (EZPI_SUCCESS != EZPI_core_suntime_shift(h, m, s, -k_offs[i], &bh, &bm, &bs)) ||
                (bh != sh) || (bm != sm) || (bs != ss))
            {
                if (0 == bad_trip)
                {
                    printf("    first round-trip mismatch at %02d:%02d:%02d offset %+d: came back %02d:%02d:%02d\n",
                           sh, sm, ss, k_offs[i], bh, bm, bs);
                }
                bad_trip++;
            }
        }
    }

    ok(0 == bad_fwd, "+1s agrees with an explicit carry chain at all 86400 seconds");
    ok(0 == bad_back, "-1s agrees with an explicit carry chain at all 86400 seconds");
    ok(0 == bad_zero, "a zero offset is the identity at all 86400 seconds");
    ok(0 == bad_trip, "every offset round-trips at all 86400 seconds");
}

static void test_shift_rejections(void)
{
    int h = -7;
    int m = -7;
    int s = -7;

    printf("-- shifts that are refused --\n");

    /* A suntime that is not a time of day. The caller's choosen_suntime comes
    ** from a parsed reply so it should always be in range -- which is exactly
    ** why this is worth refusing rather than assuming: if it ever is not, the
    ** alternative is a trigger computed from it. */
    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(24, 0, 0, 0, &h, &m, &s), "hour 24 is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(-1, 0, 0, 0, &h, &m, &s), "hour -1 is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(0, 60, 0, 0, &h, &m, &s), "minute 60 is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(0, -1, 0, 0, &h, &m, &s), "minute -1 is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(0, 0, 60, 0, &h, &m, &s), "second 60 is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(0, 0, -1, 0, &h, &m, &s), "second -1 is refused");

    /* An offset of a whole day or more. */
    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(6, 0, 0, 86400, &h, &m, &s), "an offset of a full day is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(6, 0, 0, -86400, &h, &m, &s), "a negative full day is refused");

    ok((-7 == h) && (-7 == m) && (-7 == s), "and none of those wrote an output");

    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(6, 0, 0, 0, NULL, &m, &s), "a NULL hour output is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(6, 0, 0, 0, &h, NULL, &s), "a NULL min output is refused");
    ok(EZPI_SUCCESS != EZPI_core_suntime_shift(6, 0, 0, 0, &h, &m, NULL), "a NULL sec output is refused");

    /* The widest offset the parser can produce has to be accepted, or the two
    ** functions disagree about their shared range. */
    ok(EZPI_SUCCESS == EZPI_core_suntime_shift(6, 0, 0, 86399, &h, &m, &s), "86399s is accepted");
    ok(EZPI_SUCCESS == EZPI_core_suntime_shift(6, 0, 0, -86399, &h, &m, &s), "-86399s is accepted");
}

/* The shipped answers, asserted in the direction of the bug, so that
** reintroducing any of them fails here rather than somewhere on a roof at
** sunrise. Each number below is one this code actually produced -- measured,
** not reasoned about, by compiling the function verbatim and running it. */
static void test_the_shipped_offset_answers_are_gone(void)
{
    int sec = -12345;
    int h = -1;
    int m = -1;
    int s = -1;

    printf("-- the answers the shipped parser gave --\n");

    /* "30" meant thirty hours, which the one-period wrap then turned into an
    ** offset of six. */
    ok((EZPI_SUCCESS == EZPI_core_suntime_parse_offset("30", &sec)) && (30 == sec),
       "'30' is thirty seconds, not thirty hours");

    /* "5:30" meant five hours thirty minutes. */
    ok((EZPI_SUCCESS == EZPI_core_suntime_parse_offset("5:30", &sec)) && (330 == sec),
       "'5:30' is five minutes thirty, not five hours thirty");

    /* Twenty minutes before sunrise landed forty minutes after it. */
    ok(EZPI_SUCCESS == EZPI_core_suntime_shift(6, 10, 0, -(20 * 60), &h, &m, &s), "the canonical case shifts");
    ok(!((6 == h) && (50 == m) && (0 == s)), "and 20m before 06:10 is not 06:50");
    ok((5 == h) && (50 == m) && (0 == s), "it is 05:50");

    /* "99:99:99" became a trigger at 81:39:39 in 'after' mode. There is no
    ** such time, so the scene could never fire; it is refused outright now. */
    sec = -12345;
    ok(EZPI_SUCCESS != EZPI_core_suntime_parse_offset("99:99:99", &sec), "'99:99:99' is refused outright");
    ok(-12345 == sec, "and leaves nothing behind to be used by mistake");

    /* "-1" became a one-hour shift in the opposite direction. */
    sec = -12345;
    ok(EZPI_SUCCESS != EZPI_core_suntime_parse_offset("-1", &sec), "'-1' is refused, not read as 4294967295");

    /* '500:00:00' produced an hour of -470. */
    sec = -12345;
    ok(EZPI_SUCCESS != EZPI_core_suntime_parse_offset("500:00:00", &sec), "'500:00:00' is refused");

    /* And the input whose minute field came out of never-written stack: the
    ** probe measured 7777717 for "5" with the stack primed. There is no
    ** assertion that can see that from here, which is the point -- the only
    ** defence is that the string is now parsed in full or not at all. */
    sec = -12345;
    ok((EZPI_SUCCESS == EZPI_core_suntime_parse_offset("5", &sec)) && (5 == sec),
       "'5' is five seconds and reads no further than the string");
}

/*******************************************************************************
 *                          the retry backoff
 *******************************************************************************/

/* WHAT THESE CHECK, AND WHY THE SIMULATION IS THE ONE THAT MATTERS
**
** The backoff replaces nothing -- before it there was no state at all, so
** "does the new code differ from the old code" cannot be asked of a function
** that did not exist. It can still be asked mechanically, because the old
** behaviour has an exact representative here: a NULL `bo`. The header
** documents a NULL as "no state to throttle on, so always attempt", which is
** precisely what the shipped code did by deleting the user_arg block on every
** failure. test_backoff_a_full_day_of_outage runs one loop over one day twice,
** once with state and once with NULL, and asserts 50 against 86400. That pair
** of numbers is the change; everything else below pins the mechanism that
** produces it.
**
** 86400 is not a figure of speech. __scenes_loop calls
** ezpi_scene_proto_thread(scene_node, 1000) and the protothread releases on
** (xTaskGetTickCount() - ctx->curr_ticks) > routine_delay_ms, so a
** when-condition is evaluated about once a second, and each evaluation that
** reached __issunsate_update_sunstate_tm opened a TLS connection. */

static void test_backoff_the_ladder(void)
{
    printf("-- the backoff ladder --\n");

    /* Step 0 is "no failure yet", not "retry immediately". A nonzero delay
    ** here would be harmless, but a reader checking the trace against the
    ** header has to be able to tell the two apart. */
    ok(0 == EZPI_core_suntime_backoff_delay(0), "step 0 is no delay");

    ok(60 == EZPI_core_suntime_backoff_delay(1), "step 1 is 60s");
    ok(120 == EZPI_core_suntime_backoff_delay(2), "step 2 is 120s");
    ok(240 == EZPI_core_suntime_backoff_delay(3), "step 3 is 240s");
    ok(480 == EZPI_core_suntime_backoff_delay(4), "step 4 is 480s");
    ok(960 == EZPI_core_suntime_backoff_delay(5), "step 5 is 960s");
    ok(1920 == EZPI_core_suntime_backoff_delay(6), "step 6 is 1920s (32 min)");

    /* The saturation is applied to the step before the shift, not to the
    ** result after it. A step of 31 or more shifted into an int is undefined
    ** behaviour, and the usual shape of this mistake -- compute then clamp --
    ** has already executed it by the time the clamp runs. These two are the
    ** checks that distinguish the two orders. */
    ok(1920 == EZPI_core_suntime_backoff_delay(7), "step 7 is held at 1920s");
    ok(1920 == EZPI_core_suntime_backoff_delay(64), "so is step 64");
    ok(1920 == EZPI_core_suntime_backoff_delay(1000000), "and step 1000000");

    /* Negative cannot arrive from the caller -- backoff_step only ever
    ** increments from 0 -- but the function is reachable from the trace
    ** statement and a shift by a negative count is undefined too. */
    ok(0 == EZPI_core_suntime_backoff_delay(-1), "a negative step is no delay");
}

static void test_backoff_first_attempt_is_never_withheld(void)
{
    s_suntime_backoff_t bo;
    memset(&bo, 0, sizeof(bo));

    printf("-- the first attempt of a day --\n");

    /* An all-zero struct is what ezlopi_malloc + memset leaves in
    ** s_sunstate_data_t, so this is the state every scene starts in. If it
    ** withheld the first attempt an isSunState scene would never work at all. */
    ok(EZPI_core_suntime_should_attempt(&bo, 14, 0), "a zeroed state attempts at midnight");

    memset(&bo, 0, sizeof(bo));
    ok(EZPI_core_suntime_should_attempt(&bo, 14, 43200), "and at noon");

    /* A NULL state attempts. This is the old behaviour's representative and
    ** also the real failure mode it guards: a hub that could not allocate the
    ** block must not get a scene that silently never fires. */
    ok(EZPI_core_suntime_should_attempt(NULL, 14, 0), "a NULL state always attempts");
    ok(EZPI_core_suntime_should_attempt(NULL, 14, 86399), "at any time of day");

    /* And the no-op calls do not fault. The caller passes &user_data->backoff,
    ** which cannot be NULL, so this is defence for the tests and for a future
    ** caller rather than for today's. */
    EZPI_core_suntime_note_failure(NULL, 14, 0);
    EZPI_core_suntime_note_success(NULL);
    ok(1, "note_failure and note_success tolerate a NULL state");
}

static void test_backoff_one_failure_withholds_exactly_60s(void)
{
    s_suntime_backoff_t bo;
    memset(&bo, 0, sizeof(bo));

    printf("-- the first failure --\n");

    ok(EZPI_core_suntime_should_attempt(&bo, 14, 1000), "the attempt runs");
    EZPI_core_suntime_note_failure(&bo, 14, 1000);

    ok(1 == bo.backoff_step, "one failure is step 1");
    ok(1060 == bo.next_attempt_sod, "the next attempt is 60s later");

    ok(!EZPI_core_suntime_should_attempt(&bo, 14, 1000), "the same second is refused");
    ok(!EZPI_core_suntime_should_attempt(&bo, 14, 1001), "a second later is refused");
    ok(!EZPI_core_suntime_should_attempt(&bo, 14, 1059), "59 seconds later is refused");

    /* The boundary is inclusive. The caller's resolution is one second, so a
    ** strict > would skip the evaluation that lands exactly on the deadline and
    ** every delay in the ladder would silently be one second longer -- a
    ** difference nothing else here would reveal. */
    ok(EZPI_core_suntime_should_attempt(&bo, 14, 1060), "60 seconds later is allowed");
    ok(EZPI_core_suntime_should_attempt(&bo, 14, 1061), "and so is any time after");
}

static void test_backoff_consecutive_failures_walk_the_ladder(void)
{
    s_suntime_backoff_t bo;
    const int expect[] = {60, 120, 240, 480, 960, 1920, 1920, 1920};
    int t = 0;
    int i = 0;
    char what[96];

    memset(&bo, 0, sizeof(bo));

    printf("-- consecutive failures --\n");

    for (i = 0; i < (int)(sizeof(expect) / sizeof(expect[0])); i++)
    {
        ok(EZPI_core_suntime_should_attempt(&bo, 14, t), "the due attempt runs");
        EZPI_core_suntime_note_failure(&bo, 14, t);

        snprintf(what, sizeof(what), "failure %d waits %ds", i + 1, expect[i]);
        ok((t + expect[i]) == bo.next_attempt_sod, what);

        /* Walk to the deadline, which is where the next iteration's
        ** should_attempt is asked. */
        t = bo.next_attempt_sod;
    }

    ok(EZPI_SUNTIME_BACKOFF_MAX_STEP == bo.backoff_step, "the step saturates rather than growing");
}

static void test_backoff_success_clears_it(void)
{
    s_suntime_backoff_t bo;

    memset(&bo, 0, sizeof(bo));

    printf("-- a success after failures --\n");

    EZPI_core_suntime_note_failure(&bo, 14, 0);
    EZPI_core_suntime_note_failure(&bo, 14, 60);
    EZPI_core_suntime_note_failure(&bo, 14, 180);
    ok(3 == bo.backoff_step, "three failures is step 3");

    EZPI_core_suntime_note_success(&bo);
    ok(0 == bo.backoff_step, "a success clears the step");

    /* The day is deliberately left alone by note_success -- should_attempt owns
    ** it. Asserting that here is what stops a later "tidy up" from clearing it
    ** and reopening the day-change reset to a double reset.
    **
    ** ASSERTED BEFORE THE NEXT should_attempt CALL, AND THAT ORDER IS THE
    ** CHECK. It was written after one, and the mutant that clears the day in
    ** note_success survived the sweep because of it: should_attempt(14, ...)
    ** sees the cleared day as a day change and writes 14 straight back, so by
    ** the time the assertion ran the field had been repaired by the call it was
    ** supposed to be independent of. The check was there and it could not fail.
    ** See probe/bomut.py, mutant C3. */
    ok(14 == bo.attempt_tm_day, "note_success does not touch the day");

    /* The deadline too, and this one is asserted on the field rather than
    ** through behaviour because behaviour cannot see it: with the step back at
    ** 0, should_attempt returns true from its early-out without ever reading
    ** next_attempt_sod, so a stale deadline left here is invisible. It is still
    ** worth clearing -- the two fields are the state, and one of them holding a
    ** time from a backoff that has been cancelled is the kind of thing the next
    ** person to touch should_attempt's control flow will step on. */
    ok(0 == bo.next_attempt_sod, "and clears the deadline with it");

    /* The point of clearing the step: an intermittent service must not be
    ** treated as a down one. Without this a hub that failed four times in the
    ** morning would be on 32-minute retries for the rest of the day even though
    ** every lookup since had worked. */
    ok(EZPI_core_suntime_should_attempt(&bo, 14, 181), "and the next attempt is immediate");

    /* A failure after the success starts at the bottom of the ladder again,
    ** not where it left off. */
    EZPI_core_suntime_note_failure(&bo, 14, 200);
    ok(1 == bo.backoff_step, "a failure after a success is step 1 again");
    ok(260 == bo.next_attempt_sod, "waiting 60s, not 480s");
}

static void test_backoff_a_new_day_clears_it(void)
{
    s_suntime_backoff_t bo;

    memset(&bo, 0, sizeof(bo));

    printf("-- the day rollover --\n");

    /* Deep into the backoff, late in the day. */
    EZPI_core_suntime_note_failure(&bo, 14, 0);
    EZPI_core_suntime_note_failure(&bo, 14, 60);
    EZPI_core_suntime_note_failure(&bo, 14, 180);
    EZPI_core_suntime_note_failure(&bo, 14, 420);
    EZPI_core_suntime_note_failure(&bo, 14, 900);
    EZPI_core_suntime_note_failure(&bo, 14, 86000);
    ok(6 == bo.backoff_step, "six failures is the top of the ladder");
    ok(!EZPI_core_suntime_should_attempt(&bo, 14, 86399), "nothing more happens that day");

    /* Midnight. A new day needs a new suntime whatever yesterday did, so there
    ** is nothing to carry over and the ladder restarts. */
    ok(EZPI_core_suntime_should_attempt(&bo, 15, 0), "the new day attempts immediately");
    ok(0 == bo.backoff_step, "and the step was reset by that call");
    ok(15 == bo.attempt_tm_day, "which also recorded the new day");
    ok(0 == bo.next_attempt_sod, "and cleared the deadline");

    /* The reset is in should_attempt and not in a function the caller has to
    ** remember to call, because should_attempt is the only one of the three
    ** that is reached on every evaluation. Asking a second time must not
    ** re-reset anything -- it is idempotent, so the state after a day change
    ** behaves like a fresh struct rather than like one being cleared
    ** repeatedly. */
    EZPI_core_suntime_note_failure(&bo, 15, 10);
    ok(1 == bo.backoff_step, "a failure on the new day is step 1");
    ok(!EZPI_core_suntime_should_attempt(&bo, 15, 11), "and is honoured");

    /* A day number going backwards is a reset too, not an error. The hub's
    ** clock can be corrected downward by SNTP, and tm_mday wraps from 31 to 1
    ** every month, so "different" is the only test that works. */
    ok(EZPI_core_suntime_should_attempt(&bo, 1, 11), "day 31 -> day 1 resets");
    ok(1 == bo.attempt_tm_day, "recording day 1");

    /* note_failure applies the same rule, so the two cannot disagree about
    ** which day the state describes even if the caller ever reaches one
    ** without the other. */
    EZPI_core_suntime_note_failure(&bo, 1, 100);
    EZPI_core_suntime_note_failure(&bo, 1, 160);
    ok(2 == bo.backoff_step, "two failures on day 1");
    EZPI_core_suntime_note_failure(&bo, 2, 0);
    ok(1 == bo.backoff_step, "a failure on day 2 restarts the ladder");
    ok(2 == bo.attempt_tm_day, "and moves the day");
    ok(60 == bo.next_attempt_sod, "waiting 60s, not 240s");
}

static void test_backoff_does_not_wrap_past_midnight(void)
{
    s_suntime_backoff_t bo;

    memset(&bo, 0, sizeof(bo));

    printf("-- a deadline past the end of the day --\n");

    /* next_attempt_sod is not clamped to 86399, on purpose. A deadline of
    ** 87920 means "not again today", which is a correct answer: the day
    ** rollover is itself a reason to retry, and it resets the state. Clamping
    ** instead would put the deadline at 86399 and then every evaluation in that
    ** last second would attempt -- a small storm in exactly the place the
    ** ladder was supposed to have gone quiet. */
    EZPI_core_suntime_note_failure(&bo, 14, 86000);
    ok(86060 == bo.next_attempt_sod, "a 60s deadline from 86000 is 86060");

    bo.backoff_step = EZPI_SUNTIME_BACKOFF_MAX_STEP;
    EZPI_core_suntime_note_failure(&bo, 14, 86399);
    ok(88319 == bo.next_attempt_sod, "a 1920s deadline from 86399 is 88319, not 86399");
    ok(!EZPI_core_suntime_should_attempt(&bo, 14, 86399), "which is never reached today");
    ok(EZPI_core_suntime_should_attempt(&bo, 15, 0), "and ends at the day change");
}

/* Count the attempts a whole day of total outage costs, against the number the
** shipped code cost. This is the check the change exists for. */
static void test_backoff_a_full_day_of_outage(void)
{
    s_suntime_backoff_t bo;
    int sod = 0;
    int with_state = 0;
    int without_state = 0;
    char what[96];

    printf("-- a full day of outage, once per second --\n");

    memset(&bo, 0, sizeof(bo));
    for (sod = 0; sod < 86400; sod++)
    {
        if (EZPI_core_suntime_should_attempt(&bo, 14, sod))
        {
            with_state++;
            EZPI_core_suntime_note_failure(&bo, 14, sod);
        }
    }

    /* The same loop with no state to throttle on, which is what the shipped
    ** code amounted to: every failure deleted the user_arg block, so the next
    ** evaluation started from a fresh zeroed one. */
    for (sod = 0; sod < 86400; sod++)
    {
        if (EZPI_core_suntime_should_attempt(NULL, 14, sod))
        {
            without_state++;
            EZPI_core_suntime_note_failure(NULL, 14, sod);
        }
    }

    ok(86400 == without_state, "with no state: one TLS attempt per second, all day");

    /* 50, counted from the ladder rather than approximated: 0, 60, 180, 420
    ** and 900 while the delay is still doubling, then every 1920s from 1860 to
    ** 86340, which is 45 more. If a delay in the ladder changes this number
    ** changes with it, which is the intended way to notice. */
    snprintf(what, sizeof(what), "with state: 50 attempts, measured %d", with_state);
    ok(50 == with_state, what);

    /* Stated as a ratio as well, because the exact count is the kind of number
    ** that gets "fixed" to match a changed ladder without anyone asking whether
    ** the ladder is still doing its job. */
    ok(with_state < (without_state / 1000), "which is more than a thousandfold fewer");
}

/* The case that decided the design. Recorded as a test because it is the
** argument for a timestamp over an attempt count, and the prose version of it
** has already been wrong once in this tree. */
static void test_backoff_survives_a_late_network(void)
{
    s_suntime_backoff_t bo;
    int sod = 0;
    int got_it_at = -1;
    int counter_got_it_at = -1;
    int counter_budget = 5;

    printf("-- a hub whose network arrives after the scene starts --\n");

    /* The scene begins evaluating at boot; DHCP and DNS complete 120 seconds
    ** later. Before then every lookup fails, after it every lookup works. */
    memset(&bo, 0, sizeof(bo));
    for (sod = 0; (sod < 86400) && (got_it_at < 0); sod++)
    {
        if (EZPI_core_suntime_should_attempt(&bo, 14, sod))
        {
            if (sod >= 120)
            {
                EZPI_core_suntime_note_success(&bo);
                got_it_at = sod;
            }
            else
            {
                EZPI_core_suntime_note_failure(&bo, 14, sod);
            }
        }
    }

    /* Attempts at 0 and 60 fail; the one at 180 succeeds. Three minutes, for a
    ** scene that fires at sunrise. */
    ok(180 == got_it_at, "the suntime is obtained at 00:03:00");

    /* And the design that was rejected, modelled in the same loop so the
    ** comparison is mechanical rather than asserted in a comment. "Stop after
    ** N attempts today" spends its whole budget in N seconds while the radio
    ** is still coming up, and the scene is then finished for the day -- worse
    ** than having no backoff at all, which at least eventually succeeds. */
    for (sod = 0; (sod < 86400) && (counter_got_it_at < 0); sod++)
    {
        if (counter_budget > 0)
        {
            counter_budget--;
            if (sod >= 120)
            {
                counter_got_it_at = sod;
            }
        }
    }
    ok(-1 == counter_got_it_at, "a 5-attempt daily budget never gets it at all");
}

int main(void)
{
    printf("ezlopi-core-scenes suntime host tests\n\n");

    test_clock_12_hour();
    test_clock_24_hour();
    test_clock_rejections();
    test_clock_round_trips_every_minute();
    test_query_from_coordinates();
    test_query_refusals();
    test_query_truncation();
    test_reply_extraction();
    test_reply_shape_mismatches_are_rejected();
    test_the_old_hardcoded_time_is_gone();
    test_offset_field_count_sets_the_units();
    test_offset_rejections();
    test_shift_carries_between_fields();
    test_shift_wraps_at_midnight();
    test_shift_every_second_of_the_day();
    test_shift_rejections();
    test_the_shipped_offset_answers_are_gone();
    test_backoff_the_ladder();
    test_backoff_first_attempt_is_never_withheld();
    test_backoff_one_failure_withholds_exactly_60s();
    test_backoff_consecutive_failures_walk_the_ladder();
    test_backoff_success_clears_it();
    test_backoff_a_new_day_clears_it();
    test_backoff_does_not_wrap_past_midnight();
    test_backoff_a_full_day_of_outage();
    test_backoff_survives_a_late_network();

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);
    if (g_fails)
    {
        printf("SUNTIME TESTS FAILED\n");
        return 1;
    }
    printf("SUNTIME TESTS PASSED\n");
    return 0;
}
