/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/
/**
 * @file    test_coords.c
 * @brief   The hub's stored location: whether it survives a reboot, and
 *          whether the two things that report it agree
 *
 * WHAT IS WORTH TESTING HERE
 *
 * ezlopi_cloud_coordinates.c keeps the hub's latitude and longitude in two
 * file-static doubles and also writes them to NVS. So the same datum has two
 * readers and they take different routes to it:
 *
 *     hub.info.get        -> EZPI_cloud_get_latitude()  -> the statics
 *     hub.coordinates.get -> EZPI_core_nvs_read_...()   -> flash
 *
 * Nothing loaded the statics from flash at boot. They were zero from reset
 * until a hub.coordinates.set arrived, which means that after every reboot
 * hub.info.get reported a location in the Gulf of Guinea while
 * hub.coordinates.get reported the real one. The property this file is built
 * around is therefore not "can it store a coordinate" -- that is one call --
 * but:
 *
 *     the two readers must agree, including across a reboot.
 *
 * That is a property no single-call test can see and no manual test is likely
 * to: setting the coordinates and immediately reading them back works fine. It
 * takes a power cycle to break, and then it is a wrong number rather than an
 * error, in a field most clients display rather than validate.
 *
 * The second property is the one the tree already tests for in
 * test_devcfg_api.c and test_mqtt_api.c, and it is here for the same reason:
 *
 *     a request that is refused must leave the stored value alone.
 *
 * Every refusal case below asserts the write count, not just the error, since
 * that is the only way "refused but stored anyway" can fail a test rather than
 * fail a user.
 *
 * WHY A HOST TEST AND NOT A BOARD TEST
 *
 * The reboot is the whole point, and a host process gives it for free: the
 * file-statics of a freshly started process are zero exactly as they are after
 * a reset, so test_the_cache_starts_empty() below observes the real pre-init
 * state rather than a simulation of it. On hardware the same check needs a
 * power cycle between two API calls and a way to read the statics, which is to
 * say it needs this file.
 *
 * WHAT IS FAKED, AND WHY THAT IS HONEST
 *
 * The two NVS functions. The real ones reach a flash partition that does not
 * exist here, and they are not where any decision is -- they are a string in
 * and a string out. The fakes keep that string in a buffer and count the
 * writes, which is what makes the "refused must not write" assertions
 * possible. Everything that decides anything is the real subject file.
 *
 * cjext is linked real rather than faked, for the reason it is everywhere else
 * in this suite: the subject's job is reading numbers out of a cJSON tree, and
 * a fake tree would let this file agree with itself about what cJSON did with
 * "1e999". ezlopi_cloud_keywords.c is linked real too -- "latitude" and
 * "longitude" are the keys the firmware actually stores under, and a stubbed
 * copy of those two strings would let every assertion here pass against key
 * names no hub uses.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cjext.h"

#include "ezlopi_core_errors.h"
#include "ezlopi_cloud_constants.h"
#include "ezlopi_cloud_coordinates.h"

/*******************************************************************************
 *                          Test harness
 *******************************************************************************/
static int checks = 0;
static int failures = 0;

static void ok(bool condition, const char *what)
{
    checks++;

    if (false == condition)
    {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* A parse failure in this file's own JSON is this file being wrong, not the
** subject being wrong, so it exits 2 rather than counting as a failure -- the
** same split test_devcfg_api.c uses. Without it, a typo in a test fixture
** would read as a defect in the subject. */
static cJSON *must_parse(const char *json)
{
    cJSON *cj = cJSON_Parse("test", json);

    if (NULL == cj)
    {
        printf("  TEST BUG: fixture does not parse: %s\n", json);
        exit(2);
    }

    return cj;
}

/* Coordinates are compared with a tolerance rather than with ==, because they
** make a round trip through snprintf("%.6f") and back through strtod. Six
** decimal places of a degree is about 11 cm, so 1e-6 is the resolution the
** stored form actually has and asserting anything tighter would be asserting
** about the printer. */
static bool close_enough(double a, double b)
{
    return (fabs(a - b) < 1e-6);
}

/*******************************************************************************
 *                          The NVS fake
 *******************************************************************************/
#define FAKE_NVS_MAX 512

static char fake_nvs[FAKE_NVS_MAX];
static bool fake_nvs_has_value = false;
static int  fake_nvs_writes = 0;
static bool fake_nvs_write_fails = false;

static void fake_nvs_reset(void)
{
    memset(fake_nvs, 0, sizeof(fake_nvs));
    fake_nvs_has_value = false;
    fake_nvs_writes = 0;
    fake_nvs_write_fails = false;
}

/* Puts a value in the store without going through the subject, which is how a
** "previous boot wrote this" fixture is set up. */
static void fake_nvs_preload(const char *json)
{
    snprintf(fake_nvs, sizeof(fake_nvs), "%s", json);
    fake_nvs_has_value = true;
}

/* The real one returns a freshly allocated copy that the caller frees, so the
** fake does too -- a fake that returned a pointer into its own buffer would
** make a double free or a missing free in the subject invisible here, and this
** suite runs under ASan precisely to see those. */
char *EZPI_core_nvs_read_latidtude_longitude(void)
{
    if (false == fake_nvs_has_value)
    {
        return NULL;
    }

    return strdup(fake_nvs);
}

ezlopi_error_t EZPI_core_nvs_write_latitude_longitude(char *data)
{
    if (NULL == data)
    {
        return EZPI_ERR_NVS_WRITE_FAILED;
    }

    fake_nvs_writes++;

    if (fake_nvs_write_fails)
    {
        return EZPI_ERR_NVS_WRITE_FAILED;
    }

    snprintf(fake_nvs, sizeof(fake_nvs), "%s", data);
    fake_nvs_has_value = true;

    return EZPI_SUCCESS;
}

/*******************************************************************************
 *                          Helpers over the subject
 *******************************************************************************/

/* Builds {"params":{...}} around a params body and runs the setter, returning
** the response for the caller to inspect and delete. */
static cJSON *do_set(const char *params_json)
{
    char request[512];
    snprintf(request, sizeof(request), "{\"%s\":%s}", ezlopi_params_str, params_json);

    cJSON *cj_request = must_parse(request);
    cJSON *cj_response = cJSON_CreateObject("test");

    if (NULL == cj_response)
    {
        printf("  TEST BUG: could not create a response object\n");
        exit(2);
    }

    EZPI_hub_coordinates_set(cj_request, cj_response);
    cJSON_Delete("test", cj_request);

    return cj_response;
}

static bool response_has_error(cJSON *cj_response)
{
    return (NULL != cJSON_GetObjectItem("test", cj_response, ezlopi_error_str));
}

/* Simulating a reboot is the whole difficulty of this file, and the honest
** version of it is this one.
**
** A reset loses RAM and keeps flash. The flash half is easy -- fake_nvs_preload
** puts a value there without going through the subject. The RAM half is the
** problem: sg_latitude and sg_longitude are file statics with no setter, so a
** test cannot zero them, and calling init with nothing stored leaves them where
** they were rather than clearing them.
**
** So instead of clearing the cache, poison it: drive a different pair through
** the real setter, which is the only thing that writes those statics. After
** this the cache holds the sentinel. Preload flash with the pair under test and
** call init, and the cache must move off the sentinel to match flash. An init
** that loads nothing now leaves the sentinel visible, and the assertion fails.
**
** The first draft of this file skipped the poisoning and just called init a
** second time after a successful set. That passes, and it is worthless: the
** cache already held the right answer, so the re-init had nothing to prove.
** The mutation sweep is what exposed it -- an init with an empty body scored 2
** failures where it should have scored sixteen, and the gap between those two
** numbers was the bug in this test. A check that cannot fail is not a check. */
#define POISON_LAT 12.345678
#define POISON_LNG (-98.765432)

static void poison_the_cache(void)
{
    char params[128];
    snprintf(params, sizeof(params), "{\"%s\":%.6f,\"%s\":%.6f}",
             ezlopi_latitude_str, POISON_LAT, ezlopi_longitude_str, POISON_LNG);

    cJSON *cj_response = do_set(params);
    cJSON_Delete("test", cj_response);

    if ((false == close_enough(POISON_LAT, EZPI_cloud_get_latitude())) ||
        (false == close_enough(POISON_LNG, EZPI_cloud_get_longitude())))
    {
        /* The poison not taking means the setter is broken, which the setter's
        ** own tests report. Failing here too would blame the reboot tests for
        ** it, so this exits instead of counting a failure. */
        printf("  TEST BUG: could not poison the cache -- the setter did not take\n");
        exit(2);
    }
}

/*******************************************************************************
 *                          The reboot property
 *******************************************************************************/

/* MUST RUN FIRST, and is the one order-dependent check in this file.
**
** It observes the state the statics are in before anything has initialised
** them, which only exists once per process -- the same way it only exists once
** per boot. Calling it later would assert against whatever the preceding test
** left behind, so main() calls it before everything else and this comment is
** the reason not to move it. */
static void test_the_cache_starts_empty(void)
{
    ok(0.0 == EZPI_cloud_get_latitude(), "latitude reads 0 before init");
    ok(0.0 == EZPI_cloud_get_longitude(), "longitude reads 0 before init");
}

/* The defect, stated as a test. A pair written during an earlier boot is in
** flash; nothing has called set this time round; the getters must report it.
**
** Against the tree before this change there was no init function to call at
** all. Measured rather than asserted: this file was compiled against HEAD's
** subject, and it stops at four compile errors -- the three new functions
** undeclared and COORDINATES_JSON_MAX undefined. Compile, not link, which is
** what an earlier draft of this comment said; the symbols are missing from the
** header, so it never reaches the linker.
**
** That is a loud failure but a shallow one: it proves the functions are new,
** not that this test can see the behaviour going wrong. The mutant that proves
** the latter is an init with an empty body, which compiles, links, and
** reproduces exactly the old defect -- statics at zero while NVS holds the real
** pair. See mutant 1 in the sweep recorded in the commit message. */
static void test_a_stored_pair_survives_a_reboot(void)
{
    /* Poisoned rather than relying on the cache still being zero from program
    ** start. It would be, at the point main() calls this, and that is exactly
    ** the kind of dependency that holds until someone reorders two lines. */
    fake_nvs_reset();
    poison_the_cache();

    fake_nvs_reset();
    fake_nvs_preload("{\"latitude\":27.700769,\"longitude\":85.300140}");

    EZPI_cloud_coordinates_init();

    ok(close_enough(27.700769, EZPI_cloud_get_latitude()), "latitude restored from nvs at boot");
    ok(close_enough(85.300140, EZPI_cloud_get_longitude()), "longitude restored from nvs at boot");
    ok(0 == fake_nvs_writes, "init does not write back to nvs");
}

/* The property the defect violated, asserted directly rather than through the
** symptom: whatever hub.info.get reports must be what is in flash, which is
** what hub.coordinates.get reports. Checked over a spread of coordinates
** including both negative quadrants and the equator/meridian zero. */
static void test_both_readers_agree_after_a_reboot(void)
{
    static const double pairs[][2] = {
        { 27.700769, 85.300140 },   /* Kathmandu, the one the old code hardcoded */
        { -33.868820, 151.209290 }, /* southern and eastern */
        { 51.507351, -0.127758 },   /* northern and western */
        { -22.906847, -43.172896 }, /* southern and western */
        { 0.0, 0.0 },               /* the honest zero, not the absent one */
        { 90.0, 180.0 },            /* the bounds themselves */
        { -90.0, -180.0 },
    };

    for (size_t i = 0; i < (sizeof(pairs) / sizeof(pairs[0])); i++)
    {
        char params[128];
        snprintf(params, sizeof(params), "{\"%s\":%.6f,\"%s\":%.6f}",
                 ezlopi_latitude_str, pairs[i][0], ezlopi_longitude_str, pairs[i][1]);

        fake_nvs_reset();

        cJSON *cj_response = do_set(params);
        ok(false == response_has_error(cj_response), "an in-range pair is accepted");
        cJSON_Delete("test", cj_response);

        ok(1 == fake_nvs_writes, "an accepted pair is written once");

        /* What flash now holds, read the way hub.coordinates.get reads it. */
        double stored_lat = 0.0;
        double stored_lng = 0.0;
        ok(EZPI_SUCCESS == EZPI_cloud_coordinates_parse(fake_nvs, &stored_lat, &stored_lng),
           "what was stored parses back");

        /* And what hub.info.get would report. */
        ok(close_enough(stored_lat, EZPI_cloud_get_latitude()), "flash and cache agree on latitude");
        ok(close_enough(stored_lng, EZPI_cloud_get_longitude()), "flash and cache agree on longitude");

        /* Now the reboot. Keep what flash holds, lose what RAM holds -- see
        ** poison_the_cache() for why losing RAM has to be faked this way. */
        char flash_held[FAKE_NVS_MAX];
        snprintf(flash_held, sizeof(flash_held), "%s", fake_nvs);

        poison_the_cache();
        fake_nvs_reset();
        fake_nvs_preload(flash_held);

        EZPI_cloud_coordinates_init();
        ok(close_enough(stored_lat, EZPI_cloud_get_latitude()), "latitude unchanged across a reboot");
        ok(close_enough(stored_lng, EZPI_cloud_get_longitude()), "longitude unchanged across a reboot");
    }
}

static void test_init_with_nothing_stored(void)
{
    fake_nvs_reset();

    /* Put something in the cache first, so "left at zero" is distinguishable
    ** from "never touched". */
    cJSON *cj_response = do_set("{\"latitude\":10.0,\"longitude\":20.0}");
    cJSON_Delete("test", cj_response);
    ok(close_enough(10.0, EZPI_cloud_get_latitude()), "precondition: cache holds 10");

    fake_nvs_reset(); /* as if flash were erased under it */
    EZPI_cloud_coordinates_init();

    /* The cache is not cleared by a failed load. That is deliberate and worth
    ** pinning: init's contract is "load if there is something to load", and
    ** zeroing a good in-RAM value because flash went missing would turn a
    ** read problem into a data loss. */
    ok(close_enough(10.0, EZPI_cloud_get_latitude()), "nothing stored leaves the cache alone");
    ok(0 == fake_nvs_writes, "init writes nothing when there is nothing stored");
}

static void test_init_with_an_unusable_stored_value(void)
{
    static const char *bad[] = {
        "",                                              /* empty */
        "not json at all",                               /* not JSON */
        "{}",                                            /* no keys */
        "{\"latitude\":1.0}",                            /* longitude missing */
        "{\"longitude\":1.0}",                           /* latitude missing */
        "{\"latitude\":\"1.0\",\"longitude\":2.0}",      /* latitude a string */
        "{\"latitude\":1.0,\"longitude\":null}",         /* longitude null */
        "{\"latitude\":91.0,\"longitude\":0.0}",         /* out of range */
        "{\"latitude\":0.0,\"longitude\":181.0}",        /* out of range */
        "[27.7,85.3]",                                   /* an array */
        "{\"latitude\":{\"v\":1},\"longitude\":2.0}",    /* nested */
    };

    for (size_t i = 0; i < (sizeof(bad) / sizeof(bad[0])); i++)
    {
        fake_nvs_reset();

        cJSON *cj_response = do_set("{\"latitude\":5.0,\"longitude\":6.0}");
        cJSON_Delete("test", cj_response);

        fake_nvs_preload(bad[i]);
        EZPI_cloud_coordinates_init();

        /* Same reasoning as above: an unreadable stored value is ignored, not
        ** substituted for. No default location, because a default is a
        ** plausible wrong answer. */
        ok(close_enough(5.0, EZPI_cloud_get_latitude()), "an unusable stored value does not move latitude");
        ok(close_enough(6.0, EZPI_cloud_get_longitude()), "an unusable stored value does not move longitude");
    }
}

/*******************************************************************************
 *                          The parser
 *******************************************************************************/
static void test_parse_accepts_a_good_pair(void)
{
    double lat = 0.0;
    double lng = 0.0;

    ok(EZPI_SUCCESS == EZPI_cloud_coordinates_parse("{\"latitude\":27.700769,\"longitude\":85.300140}", &lat, &lng),
       "a well-formed pair parses");
    ok(close_enough(27.700769, lat), "latitude value");
    ok(close_enough(85.300140, lng), "longitude value");

    /* Key order is not significant in JSON and must not be here either. */
    lat = lng = 0.0;
    ok(EZPI_SUCCESS == EZPI_cloud_coordinates_parse("{\"longitude\":85.3,\"latitude\":27.7}", &lat, &lng),
       "reversed key order parses");
    ok(close_enough(27.7, lat), "latitude found with keys reversed");
    ok(close_enough(85.3, lng), "longitude found with keys reversed");

    /* Extra members are ignored rather than rejected: the stored form is
    ** written by this component, but a pair arriving from a client may carry
    ** anything, and the two go through the same function. */
    lat = lng = 0.0;
    ok(EZPI_SUCCESS == EZPI_cloud_coordinates_parse("{\"latitude\":1.5,\"longitude\":2.5,\"altitude\":100}", &lat, &lng),
       "extra members are ignored");
    ok(close_enough(1.5, lat), "latitude with extra members present");
    ok(close_enough(2.5, lng), "longitude with extra members present");

    /* Integers are numbers. A client sending 27 rather than 27.0 is sending a
    ** latitude, and cJSON gives both the same type bit. */
    lat = lng = 0.0;
    ok(EZPI_SUCCESS == EZPI_cloud_coordinates_parse("{\"latitude\":27,\"longitude\":85}", &lat, &lng),
       "integer-valued coordinates parse");
    ok(close_enough(27.0, lat), "integer latitude");
    ok(close_enough(85.0, lng), "integer longitude");
}

static void test_parse_bounds(void)
{
    struct
    {
        const char *json;
        bool accepted;
        const char *what;
    } cases[] = {
        { "{\"latitude\":90,\"longitude\":0}", true, "latitude 90 accepted" },
        { "{\"latitude\":-90,\"longitude\":0}", true, "latitude -90 accepted" },
        { "{\"latitude\":0,\"longitude\":180}", true, "longitude 180 accepted" },
        { "{\"latitude\":0,\"longitude\":-180}", true, "longitude -180 accepted" },
        { "{\"latitude\":90.000001,\"longitude\":0}", false, "latitude just over 90 refused" },
        { "{\"latitude\":-90.000001,\"longitude\":0}", false, "latitude just under -90 refused" },
        { "{\"latitude\":0,\"longitude\":180.000001}", false, "longitude just over 180 refused" },
        { "{\"latitude\":0,\"longitude\":-180.000001}", false, "longitude just under -180 refused" },
        { "{\"latitude\":91,\"longitude\":0}", false, "latitude 91 refused" },
        { "{\"latitude\":0,\"longitude\":360}", false, "longitude 360 refused" },

        /* A latitude and longitude swapped by the caller: 85 is a valid
        ** latitude, so only the longitude catches it, and only sometimes. This
        ** records what the bound can and cannot do rather than claiming it
        ** detects transposition. */
        { "{\"latitude\":85.3,\"longitude\":27.7}", true, "a transposed pair is NOT detectable and is accepted" },

        /* 1e999 overflows to infinity through strtod. The range test rejects
        ** it by arithmetic, with no explicit isinf anywhere in the subject. */
        { "{\"latitude\":1e999,\"longitude\":0}", false, "an infinite latitude is refused" },
        { "{\"latitude\":0,\"longitude\":-1e999}", false, "a negative infinite longitude is refused" },

        /* Member lookup is case insensitive, so "Latitude" finds the same
        ** member "latitude" does. That is cjext's cJSON_GetObjectItem, not
        ** anything this subject chose -- it calls get_object_item() with
        ** case_sensitive false, which is what all ~200 method handlers in this
        ** firmware get. Asserted as accepted rather than listed as malformed
        ** for two reasons: making this one handler stricter than every other
        ** method would be an inconsistency dressed up as a fix, and the value
        ** is range-checked and then stored under canonical lowercase keys
        ** either way, so the leniency does not reach flash.
        **
        ** This started as a fixture in the malformed list below, asserting
        ** refusal because "JSON keys are case sensitive" -- true of JSON, not
        ** true of this reader. The test was wrong and the subject was right.
        ** Kept as a live assertion rather than deleted so that a future
        ** switch to cJSON_GetObjectItemCaseSensitive here or a change to
        ** cjext's default fails this line instead of passing quietly. */
        { "{\"Latitude\":27.7,\"Longitude\":85.3}", true, "key lookup is case insensitive, so mixed case is accepted" },
    };

    for (size_t i = 0; i < (sizeof(cases) / sizeof(cases[0])); i++)
    {
        double lat = 999.0;
        double lng = 999.0;
        ezlopi_error_t error = EZPI_cloud_coordinates_parse(cases[i].json, &lat, &lng);

        ok(cases[i].accepted == (EZPI_SUCCESS == error), cases[i].what);

        if (false == cases[i].accepted)
        {
            /* The contract: on failure the outputs are untouched. A caller
            ** that ignores the return value then reads what it had before
            ** rather than a half-updated pair -- which for this datum is the
            ** difference between an unchanged location and a wrong one. */
            ok(999.0 == lat, "refused: latitude output untouched");
            ok(999.0 == lng, "refused: longitude output untouched");
        }
    }
}

static void test_parse_rejects_malformed_input(void)
{
    static const char *bad[] = {
        "",
        " ",
        "null",
        "true",
        "27.7",
        "\"a string\"",
        "[]",
        "[27.7,85.3]",
        "{",
        "{\"latitude\":}",
        "{\"latitude\":1.0,}",
        "{}",
        "{\"lat\":27.7,\"lng\":85.3}", /* the short key names */
        "{\"latitude\":true,\"longitude\":1.0}",
        "{\"latitude\":[1],\"longitude\":1.0}",
    };

    for (size_t i = 0; i < (sizeof(bad) / sizeof(bad[0])); i++)
    {
        double lat = 42.0;
        double lng = 43.0;

        /* The input goes into the message rather than a bare "malformed input
        ** refused" repeated sixteen times. With one shared string a failure
        ** names the property but not the case, and the first run of this sweep
        ** printed three identical lines that could have come from any of the
        ** sixteen -- a check that cannot say what broke costs a bisect to
        ** read. */
        char what[128];

        snprintf(what, sizeof(what), "refused: %s", bad[i]);
        ok(EZPI_SUCCESS != EZPI_cloud_coordinates_parse(bad[i], &lat, &lng), what);

        snprintf(what, sizeof(what), "latitude untouched by: %s", bad[i]);
        ok(42.0 == lat, what);

        snprintf(what, sizeof(what), "longitude untouched by: %s", bad[i]);
        ok(43.0 == lng, what);
    }
}

static void test_parse_null_arguments(void)
{
    double lat = 1.0;
    double lng = 2.0;

    ok(EZPI_SUCCESS != EZPI_cloud_coordinates_parse(NULL, &lat, &lng), "a NULL string is refused");
    ok(EZPI_SUCCESS != EZPI_cloud_coordinates_parse("{\"latitude\":1,\"longitude\":2}", NULL, &lng),
       "a NULL latitude output is refused");
    ok(EZPI_SUCCESS != EZPI_cloud_coordinates_parse("{\"latitude\":1,\"longitude\":2}", &lat, NULL),
       "a NULL longitude output is refused");
    ok(EZPI_SUCCESS != EZPI_cloud_coordinates_from_cjson(NULL, &lat, &lng), "a NULL object is refused");

    /* And none of the above wrote through the pointers it was given. */
    ok(1.0 == lat, "latitude untouched by the NULL cases");
    ok(2.0 == lng, "longitude untouched by the NULL cases");
}

static void test_from_cjson_on_a_live_tree(void)
{
    /* The cJSON entry point, exercised on a tree built rather than parsed,
    ** because that is the shape the setter sees: a node hanging off a request
    ** someone else parsed. */
    cJSON *cj = must_parse("{\"latitude\":-12.5,\"longitude\":130.25}");
    double lat = 0.0;
    double lng = 0.0;

    ok(EZPI_SUCCESS == EZPI_cloud_coordinates_from_cjson(cj, &lat, &lng), "from_cjson accepts a good object");
    ok(close_enough(-12.5, lat), "from_cjson latitude");
    ok(close_enough(130.25, lng), "from_cjson longitude");

    /* And it does not consume or modify the tree it was given. */
    cJSON *cj_lat = cJSON_GetObjectItem("test", cj, ezlopi_latitude_str);
    ok(NULL != cj_lat, "from_cjson leaves the object intact");
    ok((NULL != cj_lat) && close_enough(-12.5, cj_lat->valuedouble), "from_cjson does not alter the value");

    cJSON_Delete("test", cj);
}

/*******************************************************************************
 *                          The setter
 *******************************************************************************/
static void test_set_refuses_without_storing(void)
{
    static const char *bad_params[] = {
        "{}",
        "{\"latitude\":1.0}",
        "{\"longitude\":1.0}",
        "{\"latitude\":\"27.7\",\"longitude\":\"85.3\"}",
        "{\"latitude\":null,\"longitude\":null}",
        "{\"latitude\":91.0,\"longitude\":0.0}",
        "{\"latitude\":0.0,\"longitude\":181.0}",
        "{\"latitude\":1e999,\"longitude\":0.0}",
        "{\"lat\":27.7,\"lng\":85.3}",
    };

    for (size_t i = 0; i < (sizeof(bad_params) / sizeof(bad_params[0])); i++)
    {
        fake_nvs_reset();

        /* Establish a known good stored state first, so "did not store" is
        ** checked against something that could have been overwritten. */
        cJSON *cj_good = do_set("{\"latitude\":7.5,\"longitude\":8.5}");
        cJSON_Delete("test", cj_good);
        int writes_after_good = fake_nvs_writes;

        cJSON *cj_response = do_set(bad_params[i]);

        ok(true == response_has_error(cj_response), "a refused set reports an error");
        ok(writes_after_good == fake_nvs_writes, "a refused set does not write to nvs");
        ok(close_enough(7.5, EZPI_cloud_get_latitude()), "a refused set does not move the cached latitude");
        ok(close_enough(8.5, EZPI_cloud_get_longitude()), "a refused set does not move the cached longitude");

        /* And the stored value is still the good one, so the two readers have
        ** not been pushed out of agreement by the refusal either. */
        double stored_lat = 0.0;
        double stored_lng = 0.0;
        ok(EZPI_SUCCESS == EZPI_cloud_coordinates_parse(fake_nvs, &stored_lat, &stored_lng),
           "the previously stored pair is still in nvs");
        ok(close_enough(7.5, stored_lat), "nvs still holds the good latitude");

        cJSON_Delete("test", cj_response);
    }
}

static void test_set_with_no_params_or_request(void)
{
    fake_nvs_reset();

    cJSON *cj_response = cJSON_CreateObject("test");
    EZPI_hub_coordinates_set(NULL, cj_response);
    ok(0 == fake_nvs_writes, "a NULL request writes nothing");
    ok(NULL != cJSON_GetObjectItem("test", cj_response, ezlopi_result_str),
       "a NULL request still gets a result member");
    cJSON_Delete("test", cj_response);

    /* A request with no params at all is not a refusal -- there is nothing to
    ** refuse -- so it gets no error object, which is what it did before this
    ** change too. Recorded so a future edit that starts erroring here is a
    ** deliberate decision rather than a surprise. */
    cJSON *cj_request = must_parse("{\"method\":\"hub.coordinates.set\"}");
    cj_response = cJSON_CreateObject("test");
    EZPI_hub_coordinates_set(cj_request, cj_response);
    ok(0 == fake_nvs_writes, "a request with no params writes nothing");
    ok(false == response_has_error(cj_response), "a request with no params reports no error");
    cJSON_Delete("test", cj_request);
    cJSON_Delete("test", cj_response);
}

static void test_set_when_the_write_fails(void)
{
    fake_nvs_reset();

    cJSON *cj_good = do_set("{\"latitude\":3.5,\"longitude\":4.5}");
    cJSON_Delete("test", cj_good);
    ok(close_enough(3.5, EZPI_cloud_get_latitude()), "precondition: cache holds the first pair");

    fake_nvs_write_fails = true;
    cJSON *cj_response = do_set("{\"latitude\":60.0,\"longitude\":70.0}");

    ok(true == response_has_error(cj_response), "a failed write is reported as an error");

    /* The point of the test. A write that did not land must not be reflected
    ** in the cache, because the cache is what hub.info.get reports and flash
    ** is what survives the reboot. Updating the cache here would recreate the
    ** exact disagreement this change removes, just with a shorter fuse. */
    ok(close_enough(3.5, EZPI_cloud_get_latitude()), "a failed write leaves the cached latitude alone");
    ok(close_enough(4.5, EZPI_cloud_get_longitude()), "a failed write leaves the cached longitude alone");

    cJSON_Delete("test", cj_response);
    fake_nvs_write_fails = false;
}

/* The stored form is written by this component and read back by it, so its
** shape is a contract with itself across a reboot. These assertions are what
** make the canonical rendering a decision rather than an accident. */
static void test_what_gets_stored(void)
{
    fake_nvs_reset();

    /* Extra members in params are NOT stored. The old code printed the whole
    ** params object into a 256-byte buffer and wrote that, so whatever else a
    ** caller put in the object was persisted under the coordinates key. This
    ** is the check that pins the change. */
    cJSON *cj_response = do_set("{\"latitude\":1.25,\"longitude\":2.75,\"junk\":\"xxxxxxxxxx\",\"more\":[1,2,3]}");
    cJSON_Delete("test", cj_response);

    ok(NULL == strstr(fake_nvs, "junk"), "an extra member is not persisted");
    ok(NULL == strstr(fake_nvs, "more"), "an extra array member is not persisted");
    ok(NULL != strstr(fake_nvs, ezlopi_latitude_str), "the stored form names latitude");
    ok(NULL != strstr(fake_nvs, ezlopi_longitude_str), "the stored form names longitude");

    cJSON *cj_stored = must_parse(fake_nvs);
    ok(2 == cJSON_GetArraySize(cj_stored), "the stored object has exactly two members");
    cJSON_Delete("test", cj_stored);

    /* And it fits the buffer the header sizes, with the widest values the
    ** bounds allow. */
    fake_nvs_reset();
    cj_response = do_set("{\"latitude\":-90.0,\"longitude\":-180.0}");
    cJSON_Delete("test", cj_response);
    ok(1 == fake_nvs_writes, "the widest in-range pair is stored");
    ok(strlen(fake_nvs) < COORDINATES_JSON_MAX, "the widest stored form fits COORDINATES_JSON_MAX");

    double lat = 0.0;
    double lng = 0.0;
    ok(EZPI_SUCCESS == EZPI_cloud_coordinates_parse(fake_nvs, &lat, &lng), "the widest stored form parses back");
    ok(close_enough(-90.0, lat), "the widest latitude round-trips");
    ok(close_enough(-180.0, lng), "the widest longitude round-trips");
}

/* hub.coordinates.get hands back whatever string is in flash, so the check
** worth having is that it is the canonical one the setter wrote -- i.e. that
** the two API methods are consistent with each other and not just each
** internally consistent. */
static void test_get_returns_what_was_set(void)
{
    fake_nvs_reset();

    cJSON *cj_set = do_set("{\"latitude\":48.858093,\"longitude\":2.294694}");
    cJSON_Delete("test", cj_set);

    cJSON *cj_response = cJSON_CreateObject("test");
    EZPI_hub_coordinates_get(NULL, cj_response);

    cJSON *cj_result = cJSON_GetObjectItem("test", cj_response, ezlopi_result_str);
    ok(NULL != cj_result, "get produces a result");

    cJSON_Delete("test", cj_response);

    /* Read the stored string the way the getter does and confirm it carries
    ** the pair the setter accepted. */
    double lat = 0.0;
    double lng = 0.0;
    ok(EZPI_SUCCESS == EZPI_cloud_coordinates_parse(fake_nvs, &lat, &lng), "the stored string parses");
    ok(close_enough(48.858093, lat), "get and set agree on latitude");
    ok(close_enough(2.294694, lng), "get and set agree on longitude");
    ok(close_enough(lat, EZPI_cloud_get_latitude()), "get, set and info all agree on latitude");
    ok(close_enough(lng, EZPI_cloud_get_longitude()), "get, set and info all agree on longitude");
}

static void test_get_with_nothing_stored(void)
{
    fake_nvs_reset();

    cJSON *cj_response = cJSON_CreateObject("test");
    EZPI_hub_coordinates_get(NULL, cj_response);

    /* No stored value means no result member at all, which is what this method
    ** did before and is left alone deliberately: changing it would change a
    ** reply shape clients already handle. Asserted so the behaviour is
    ** recorded rather than assumed. */
    ok(NULL == cJSON_GetObjectItem("test", cj_response, ezlopi_result_str),
       "get adds no result when nothing is stored");

    cJSON_Delete("test", cj_response);
}

/* A sweep over the whole coordinate space at a coarse step. The round trip is
** set -> snprintf -> flash -> strtod -> parse -> compare, and the thing being
** checked is that no value inside the bounds is mangled by it. A single
** fixture cannot see a defect that only affects, say, negative values near a
** boundary, or one that loses a digit at a particular magnitude. */
static void test_the_whole_coordinate_space_round_trips(void)
{
    int tested = 0;
    int mismatches = 0;

    for (int lat_deg = -90; lat_deg <= 90; lat_deg += 5)
    {
        for (int lng_deg = -180; lng_deg <= 180; lng_deg += 5)
        {
            /* An offset that is not representable exactly in binary, so the
            ** test is not accidentally only checking whole degrees. */
            double want_lat = (double)lat_deg + ((lat_deg < 90) ? 0.123456 : 0.0);
            double want_lng = (double)lng_deg + ((lng_deg < 180) ? 0.654321 : 0.0);

            char params[128];
            snprintf(params, sizeof(params), "{\"%s\":%.6f,\"%s\":%.6f}",
                     ezlopi_latitude_str, want_lat, ezlopi_longitude_str, want_lng);

            fake_nvs_reset();
            cJSON *cj_response = do_set(params);
            bool errored = response_has_error(cj_response);
            cJSON_Delete("test", cj_response);

            double got_lat = 0.0;
            double got_lng = 0.0;
            bool parsed = (EZPI_SUCCESS == EZPI_cloud_coordinates_parse(fake_nvs, &got_lat, &got_lng));

            tested++;

            if (errored || (false == parsed) ||
                (false == close_enough(want_lat, got_lat)) ||
                (false == close_enough(want_lng, got_lng)) ||
                (false == close_enough(want_lat, EZPI_cloud_get_latitude())) ||
                (false == close_enough(want_lng, EZPI_cloud_get_longitude())))
            {
                if (mismatches < 5)
                {
                    printf("  round trip mismatch: wanted %.6f,%.6f got %.6f,%.6f (errored=%d parsed=%d)\n",
                           want_lat, want_lng, got_lat, got_lng, (int)errored, (int)parsed);
                }
                mismatches++;
            }
        }
    }

    printf("  (%d coordinates round-tripped, %d mismatches)\n", tested, mismatches);
    ok(tested == (37 * 73), "the sweep covered the grid it claims to");
    ok(0 == mismatches, "every in-range coordinate round-trips through storage");
}

/*******************************************************************************
 *                          Main
 *******************************************************************************/
int main(void)
{
    /* First, and only once: see the note on the function. */
    test_the_cache_starts_empty();

    test_a_stored_pair_survives_a_reboot();
    test_both_readers_agree_after_a_reboot();
    test_init_with_nothing_stored();
    test_init_with_an_unusable_stored_value();

    test_parse_accepts_a_good_pair();
    test_parse_bounds();
    test_parse_rejects_malformed_input();
    test_parse_null_arguments();
    test_from_cjson_on_a_live_tree();

    test_set_refuses_without_storing();
    test_set_with_no_params_or_request();
    test_set_when_the_write_fails();
    test_what_gets_stored();

    test_get_returns_what_was_set();
    test_get_with_nothing_stored();

    test_the_whole_coordinate_space_round_trips();

    printf("%d check(s), %d failure(s)\n", checks, failures);

    return (0 == failures) ? 0 : 1;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
