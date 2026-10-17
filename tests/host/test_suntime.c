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

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);
    if (g_fails)
    {
        printf("SUNTIME TESTS FAILED\n");
        return 1;
    }
    printf("SUNTIME TESTS PASSED\n");
    return 0;
}
