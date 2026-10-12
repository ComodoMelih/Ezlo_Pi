/* Host tests for the GY-GPS6MV2 GPGGA sentence parser.
 *
 * The parser fills each GPGGA field by writing single bytes at an index
 * derived from that field's own offset within the sentence. Nothing appends
 * and nothing re-terminates, so the thing worth asserting is that a field is
 * the value from the *current* sentence and not a blend of that value with
 * whatever was there before. The handle is long-lived -- one GPS6MV2_t per
 * device, reused for every sentence -- so "before" means the previous fix.
 *
 * The second thing worth asserting is that every field stays NUL-terminated.
 * Five of these fields are read with atoi(), and that is only safe because
 * GPS6MV2_t is memset at allocation and every fill guard stops at least one
 * byte short of its array. That is a property of the guards rather than
 * anything enforced, so it is checked explicitly here: the MB1013 driver had
 * exactly this shape of argument in a comment, and the argument was wrong.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "gps6mv2_parse.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (0 != strcmp(got, want))
    {
        failures++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

/* A complete, well-formed GPGGA sentence with every field populated. The
   trailing parameters are the ones the tests vary. */
static const char *SENTENCE_12_SATS =
    "$GPGGA,061732.00,2740.52768,N,08518.22110,E,1,12,1.05,1300.8,M,-41.3,M,,*48";

static GPS6MV2_t *fresh(void)
{
    /* calloc mirrors the driver, which memsets GPS6MV2_t immediately after
       ezlopi_malloc. Every claim below about termination depends on it. */
    return (GPS6MV2_t *)calloc(1, sizeof(GPS6MV2_t));
}

static void feed(GPS6MV2_t *h, const char *s)
{
    memset(h->GPGGA_data_structure.GPGGA_sentence, 0,
           sizeof(h->GPGGA_data_structure.GPGGA_sentence));
    strncpy(h->GPGGA_data_structure.GPGGA_sentence, s,
            sizeof(h->GPGGA_data_structure.GPGGA_sentence) - 1);
    GYGPS6MV2_parse_gpgga_mesg(h);
}

/* Exactly the computation in sensor_0053_UART_GYGPS6MV2.c's Fix_item branch. */
static bool gps_fix_of(const GPS6MV2_t *h)
{
    int total_sat = atoi(h->GPGGA_data_structure.Satellites_used);
    int gps_quality = ((int)(h->GPGGA_data_structure.Positon_fix_quality) - 48);
    gps_quality = (gps_quality < 0) ? 0 : ((gps_quality > 9) ? 9 : gps_quality);
    return ((total_sat > 2) && (gps_quality != 0)) ? true : false;
}

/* Build a GPGGA sentence with a chosen satellite count, altitude and HDOP. */
static void sentence_with(char *out, size_t n, const char *sats,
                          const char *hdop, const char *msl, const char *geoid)
{
    snprintf(out, n,
             "$GPGGA,061732.00,2740.52768,N,08518.22110,E,1,%s,%s,%s,M,%s,M,,*48",
             sats, hdop, msl, geoid);
}

/* ------------------------------------------------------------------ */

static void test_well_formed_sentence(void)
{
    GPS6MV2_t *h = fresh();
    feed(h, SENTENCE_12_SATS);

    check_str(h->GPGGA_data_structure.UTC_time.hour, "06", "UTC hour");
    check_str(h->GPGGA_data_structure.UTC_time.min, "17", "UTC min");
    check_str(h->GPGGA_data_structure.UTC_time.sec, "32.00", "UTC sec");
    check_str(h->GPGGA_data_structure.Latitude.lat_degree, "27", "lat degree");
    check_str(h->GPGGA_data_structure.Latitude.lat_min, "40.52768", "lat min");
    check(h->GPGGA_data_structure.N_S_indicator == 'N', "N/S indicator");
    check_str(h->GPGGA_data_structure.Longitude.long_degree, "085", "long degree");
    check_str(h->GPGGA_data_structure.Longitude.long_min, "18.22110", "long min");
    check(h->GPGGA_data_structure.E_W_indicator == 'E', "E/W indicator");
    check(h->GPGGA_data_structure.Positon_fix_quality == '1', "fix quality");
    check_str(h->GPGGA_data_structure.Satellites_used, "12", "satellites used");
    check_str(h->GPGGA_data_structure.HDOP, "1.05", "HDOP");
    check_str(h->GPGGA_data_structure.Mean_sea_level, "1300.8", "mean sea level");
    check(h->GPGGA_data_structure.MSL_Unit == 'M', "MSL unit");
    check_str(h->GPGGA_data_structure.Geoid_Separation, "-41.3", "geoid separation");
    check(h->GPGGA_data_structure.GS_Unit == 'M', "GS unit");

    /* The checksum field is opened by '*' rather than by a comma, which is a
       separate branch from every field above -- the one place comma_count is
       assigned a literal instead of being incremented. Asserting its value is
       what separates "the '*' branch opens the right field" from "the '*'
       branch opens some field": a mutation changing that literal from 15 to 14
       was invisible to this suite until this check existed, because field 14 is
       an empty case and the only surviving claim about Checksum was that it
       stayed NUL-terminated -- which a never-written field trivially is.

       age_from_last and station_ID get no value check, for the opposite
       reason. The parser declares both in GPS6MV2_t and then leaves cases 13
       and 14 as a bare break;, so neither is ever filled, and nothing in the
       firmware reads them. Recorded here rather than asserted. */
    check_str(h->GPGGA_data_structure.Checksum, "48", "checksum");

    check(gps_fix_of(h), "12 satellites with quality 1 is a fix");

    free(h);
}

static void test_shorter_value_replaces_longer(void)
{
    /* The defect. The handle is reused for every sentence, and a field is
       written byte by byte at absolute indices, so a value shorter than the
       previous one used to leave the previous value's tail behind: a drop from
       12 satellites to 9 read back as "92". */
    GPS6MV2_t *h = fresh();
    char s[128];

    feed(h, SENTENCE_12_SATS);
    check_str(h->GPGGA_data_structure.Satellites_used, "12", "starts at 12");

    sentence_with(s, sizeof(s), "9", "1.0", "999.5", "-9.3");
    feed(h, s);
    check_str(h->GPGGA_data_structure.Satellites_used, "9",
              "9 satellites after 12 reads as 9, not 92");
    check_str(h->GPGGA_data_structure.HDOP, "1.0",
              "HDOP 1.0 after 1.05 reads as 1.0, not 1.05");
    check_str(h->GPGGA_data_structure.Mean_sea_level, "999.5",
              "altitude 999.5 after 1300.8 reads as 999.5, not 999.58");
    check_str(h->GPGGA_data_structure.Geoid_Separation, "-9.3",
              "geoid -9.3 after -41.3 reads as -9.3, not -9.33");

    /* And a longer value after a shorter one must still work. */
    sentence_with(s, sizeof(s), "11", "2.22", "1234.5", "-11.1");
    feed(h, s);
    check_str(h->GPGGA_data_structure.Satellites_used, "11", "back up to 11");
    check_str(h->GPGGA_data_structure.HDOP, "2.22", "back up to HDOP 2.22");
    check_str(h->GPGGA_data_structure.Mean_sea_level, "1234.5", "back up to 1234.5");
    check_str(h->GPGGA_data_structure.Geoid_Separation, "-11.1", "back up to -11.1");

    free(h);
}

static void test_no_false_fix_after_count_drops(void)
{
    /* Why the above matters. The driver computes
           GPS_FIX = ((atoi(Satellites_used) > 2) && (quality != 0))
       and a retained digit can only make the count larger, so the error is
       always in the direction of claiming a fix the device does not have.
       prev_GPS_FIX only broadcasts on change, so a false fix persists. */
    char s[128];
    for (int sats = 0; sats <= 2; sats++)
    {
        GPS6MV2_t *h = fresh();
        char what[80];

        feed(h, SENTENCE_12_SATS);
        check(gps_fix_of(h), "fix with 12 satellites");

        snprintf(s, sizeof(s), "%d", sats);
        {
            char full[128];
            sentence_with(full, sizeof(full), s, "1.0", "1300.8", "-41.3");
            feed(h, full);
        }

        snprintf(what, sizeof(what),
                 "%d satellites after 12 is not a fix", sats);
        check(!gps_fix_of(h), what);

        snprintf(what, sizeof(what), "%d satellites reads back as %d", sats, sats);
        check_str(h->GPGGA_data_structure.Satellites_used, s, what);

        free(h);
    }
}

static void test_empty_fields_are_cleared(void)
{
    /* The real "no fix yet" sentence the part emits on power-up. Every field
       between the commas is empty, and those fields must come back empty even
       when a populated sentence preceded them -- this is what the parser's
       original empty-field memset was for, and it must not regress. */
    GPS6MV2_t *h = fresh();
    feed(h, SENTENCE_12_SATS);
    check_str(h->GPGGA_data_structure.Satellites_used, "12", "populated first");

    feed(h, "$GPGGA,,,,,,0,00,99.99,,,,,,*48");

    check_str(h->GPGGA_data_structure.UTC_time.hour, "", "empty UTC hour");
    check_str(h->GPGGA_data_structure.Latitude.lat_degree, "", "empty lat degree");
    check_str(h->GPGGA_data_structure.Latitude.lat_min, "", "empty lat min");
    check_str(h->GPGGA_data_structure.Longitude.long_degree, "", "empty long degree");
    check_str(h->GPGGA_data_structure.Longitude.long_min, "", "empty long min");
    check_str(h->GPGGA_data_structure.Mean_sea_level, "", "empty mean sea level");
    check_str(h->GPGGA_data_structure.Geoid_Separation, "", "empty geoid");
    check(h->GPGGA_data_structure.Positon_fix_quality == '0', "quality 0");
    check_str(h->GPGGA_data_structure.Satellites_used, "00", "00 satellites");
    check(!gps_fix_of(h), "the power-up sentence is not a fix");

    free(h);
}

/* Every char array in GPGGA_t, for the termination sweep below. */
static void check_all_terminated(const GPS6MV2_t *h, const char *context)
{
    const GPGGA_t *g = &h->GPGGA_data_structure;
    struct
    {
        const char *arr;
        size_t len;
        const char *name;
    } fields[] = {
        {g->UTC_time.hour, sizeof(g->UTC_time.hour), "hour"},
        {g->UTC_time.min, sizeof(g->UTC_time.min), "min"},
        {g->UTC_time.sec, sizeof(g->UTC_time.sec), "sec"},
        {g->Latitude.lat_degree, sizeof(g->Latitude.lat_degree), "lat_degree"},
        {g->Latitude.lat_min, sizeof(g->Latitude.lat_min), "lat_min"},
        {g->Longitude.long_degree, sizeof(g->Longitude.long_degree), "long_degree"},
        {g->Longitude.long_min, sizeof(g->Longitude.long_min), "long_min"},
        {g->Satellites_used, sizeof(g->Satellites_used), "Satellites_used"},
        {g->HDOP, sizeof(g->HDOP), "HDOP"},
        {g->Mean_sea_level, sizeof(g->Mean_sea_level), "Mean_sea_level"},
        {g->Geoid_Separation, sizeof(g->Geoid_Separation), "Geoid_Separation"},
        {g->age_from_last, sizeof(g->age_from_last), "age_from_last"},
        {g->station_ID, sizeof(g->station_ID), "station_ID"},
        {g->Checksum, sizeof(g->Checksum), "Checksum"},
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
    {
        if (fields[i].arr[fields[i].len - 1] != '\0')
        {
            char what[160];
            snprintf(what, sizeof(what), "%s stays NUL-terminated (%s)",
                     fields[i].name, context);
            check(false, what);
            return;
        }
    }
    checks++; /* all terminated */
}

static void test_fields_stay_terminated(void)
{
    /* The property the five atoi() calls in the driver depend on. Swept over
       inputs chosen to push each field to its maximum width and past it. */
    const char *nasty[] = {
        "$GPGGA,061732.00,2740.52768,N,08518.22110,E,1,12,1.05,1300.8,M,-41.3,M,,*48",
        /* over-long fields: more digits than any array can hold */
        "$GPGGA,9999999999.99999,99999999.99999,N,999999999.99999,E,9,999999,99999999,99999999,M,99999999,M,999999,999999999,*99",
        /* every field empty */
        "$GPGGA,,,,,,,,,,,,,,,*",
        /* no commas at all */
        "$GPGGA",
        /* nothing recognisable */
        "xxxxxxxxxxxxxxxxxxxxxxxx",
        /* commas only */
        ",,,,,,,,,,,,,,,,,,",
        /* stars only */
        "****************",
        /* a second sentence glued on */
        "$GPGGA,061732.00,2740.52768,N,08518.22110,E,1,12,1.05,1300.8,M,-41.3,M,,*48$GPGGA,1,2",
        "",
    };

    for (size_t i = 0; i < sizeof(nasty) / sizeof(nasty[0]); i++)
    {
        GPS6MV2_t *h = fresh();
        feed(h, nasty[i]);
        check_all_terminated(h, nasty[i]);
        free(h);
    }

    /* And on a reused handle, where the arrays already hold data. */
    GPS6MV2_t *h = fresh();
    for (size_t i = 0; i < sizeof(nasty) / sizeof(nasty[0]); i++)
    {
        feed(h, nasty[i]);
        check_all_terminated(h, "reused handle");
    }
    free(h);
}

static void test_truncated_sentences(void)
{
    /* Every prefix of a valid sentence. A sentence can be cut short by the
       circular buffer, and the parser indexes dummy_container[start_index + 1]
       -- one past the delimiter -- so a sentence ending in ',' or '*' is the
       interesting case rather than an unlikely one. Under ASan, reading
       outside the copy aborts. */
    const char *full = SENTENCE_12_SATS;
    size_t n = strlen(full);

    for (size_t keep = 0; keep <= n; keep++)
    {
        GPS6MV2_t *h = fresh();
        char buf[128];
        memcpy(buf, full, keep);
        buf[keep] = '\0';
        feed(h, buf);
        check_all_terminated(h, "truncated prefix");
        free(h);
    }
    check(true, "every prefix of a sentence parses without an invalid access");
}

static void test_repeated_identical_sentences(void)
{
    /* The ordinary case: the same fix arriving once a second must not drift. */
    GPS6MV2_t *h = fresh();
    for (int i = 0; i < 50; i++)
    {
        feed(h, SENTENCE_12_SATS);
    }
    check_str(h->GPGGA_data_structure.Satellites_used, "12", "stable over 50 sentences");
    check_str(h->GPGGA_data_structure.Mean_sea_level, "1300.8", "altitude stable");
    check_str(h->GPGGA_data_structure.Latitude.lat_min, "40.52768", "lat min stable");
    free(h);
}

static void test_satellite_count_sweep(void)
{
    /* Every count 0..99, each arriving after a 2-digit count, so each one has
       a stale tail available to pick up. */
    int wrong = 0;
    for (int v = 0; v <= 99; v++)
    {
        GPS6MV2_t *h = fresh();
        char want[16]; /* 16 not 3: GCC cannot narrow v's range through the
                          inlining here, so a tight buffer trips
                          -Werror=format-truncation. */
        char s[128];

        feed(h, SENTENCE_12_SATS);

        snprintf(want, sizeof(want), "%d", v);
        sentence_with(s, sizeof(s), want, "1.0", "1300.8", "-41.3");
        feed(h, s);

        if (0 != strcmp(h->GPGGA_data_structure.Satellites_used, want))
        {
            wrong++;
        }
        free(h);
    }
    check(wrong == 0, "every satellite count 0..99 survives a preceding 2-digit count");
    printf("  swept 100 satellite counts after a 2-digit count: %d wrong\n", wrong);
}

int main(void)
{
    test_well_formed_sentence();
    test_shorter_value_replaces_longer();
    test_no_false_fix_after_count_drops();
    test_empty_fields_are_cleared();
    test_fields_stay_terminated();
    test_truncated_sentences();
    test_repeated_identical_sentences();
    test_satellite_count_sweep();

    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
