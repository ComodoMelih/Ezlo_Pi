/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    test_tz.c
 * @brief   Host tests for the hub's timezone: ezlopi_core_sntp.c.
 *
 * WHAT WENT WRONG
 *
 * TZ is process-global. Every localtime() caller in the firmware shares one
 * setting, and libc only picks up a change when something calls tzset(). The
 * only place in the tree that ever called setenv("TZ")+tzset() was
 * EZPI_core_sntp_get_local_time, and its only caller is hub.info.get
 * (ezlopi_cloud_info.c:146).
 *
 * So nothing installed a timezone at boot. Until a client happened to ask for
 * hub.info.get, localtime() meant UTC -- including in the four time-of-day
 * scene evaluators in ezlopi_core_scenes_when_methods.c, which call
 * localtime() directly at lines 561, 616, 672 and 727. A scene set for 7am
 * fired at 7am UTC. And because hub.location.set never applied what it stored,
 * changing the timezone did not move those scenes either; the new zone took
 * effect only when some unrelated hub.info.get came along and re-installed it
 * as a side effect.
 *
 * Separately, hub.location.set stored the name before checking it was a zone
 * the table knows. An unrecognised name was persisted, after which
 * hub.location.get reported it while every localtime() caller silently ran on
 * the GMT0 fallback -- one datum, two readers, two answers.
 *
 * WHY A HOST PROCESS IS THE RIGHT INSTRUMENT
 *
 * Because setenv, tzset and localtime here are the host's own libc, this test
 * can assert the thing that actually matters rather than a proxy for it: it
 * converts a fixed epoch and checks the wall-clock answer. That is exactly
 * what a scene evaluator does. Asserting "setenv was called" would pass
 * against a setenv that installed the wrong string.
 *
 * It also makes a reboot cheap. The real question is whether a stored zone
 * survives a restart, and on hardware each case costs a flash write and a
 * reset; here it is a fake-NVS preload and a call. The TZ is poisoned to a
 * different zone before each simulated boot, so an init that did nothing
 * cannot pass by leaving a correct value already in place -- the mistake that
 * the first draft of tests/host/test_coords.c made and that its mutation
 * sweep caught.
 *
 * WHAT IS NOT TESTED
 *
 * SNTP itself. Whether a time server answers and whether the clock gets set is
 * ESP-IDF plus a UDP round trip; stubs-sntp/esp_sntp.h supplies names and a
 * sync status that never completes. Nothing here asserts that the hub's clock
 * is correct, only that a correct clock would be interpreted in the right
 * zone.
 *
 * WHAT THIS FILE HAS BEEN SEEN TO CATCH
 *
 * 151 checks passing proves nothing on its own, so the subject was mutated
 * nineteen ways, one edit at a time, with the unmutated control confirmed at 0
 * failures first. Eighteen of the nineteen were caught: seventeen by this file,
 * the eighteenth by the compiler. The results worth carrying forward are these.
 *
 * Both halves of the original defect are caught loudly: a boot that resolves
 * the zone but never installs it fails 19 checks, and a set that resolves but
 * never installs fails 18. Dropping tzset() while keeping setenv() fails 37 --
 * the largest of any mutant, which is the right shape, since that is the
 * mistake most likely to be made by someone who thinks setting the variable is
 * enough. Reverting to the original store-before-validate ordering does not
 * merely fail, it faults: the unknown name reaches setenv() as a NULL and ASan
 * reports a SEGV at address zero.
 *
 * Two mutants were not caught by an assertion, and both are worth stating
 * plainly rather than being quietly counted as passes:
 *
 *   Freeing the location string twice never reaches the test -- GCC rejects it
 *   at compile time under -Werror=use-after-free. A detection, but the
 *   compiler's, not this file's.
 *
 *   Removing the NULL guard from EZPI_core_sntp_set_location changes nothing
 *   observable here, so it survived. That is an equivalent mutant rather than a
 *   gap, and only because of how this harness is built: with the name NULL, the
 *   table lookup is still safe -- tz_name_cmp guards both arguments explicitly
 *   -- so posix_str comes back NULL and the function returns the same error the
 *   guard would have produced. The guard is not redundant on a real build,
 *   though, and the reason it survives here is precisely why: stubs-sntp's
 *   sdkconfig.h leaves CONFIG_EZPI_UTIL_TRACE_EN undefined, which compiles
 *   TRACE_W away. All four sdkconfig.defaults.* pin it to y, so on hardware
 *   that line passes a NULL to a %s conversion. Keep the guard.
 *
 * One mutant is caught by the sanitizers alone and could not be caught any
 * other way: dropping the NULL/length guard from EZPI_core_sntp_get_local_time.
 * glibc declares strftime nonnull and, handed a NULL destination, returns the
 * length it would have written without writing anything -- so no assertion
 * about the buffer can see it, and UBSan's "null pointer passed as argument 1"
 * is the whole of the evidence. Newlib makes no such promise.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ezlopi_core_errors.h"
#include "ezlopi_core_sntp.h"
#include "zones.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/
#define FAKE_NVS_MAX 256

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static int checks = 0;
static int failures = 0;

/* Referenced by stubs-sntp/esp_sntp.h; defined here because that stub is all
** static inline functions and has no translation unit of its own. */
unsigned fake_sntp_init_calls = 0;
unsigned fake_sntp_status_polls = 0;
void (*fake_sntp_notify_cb)(struct timeval *tv) = NULL;

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/
static void ok(bool condition, const char *what)
{
    checks++;

    if (false == condition)
    {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/*******************************************************************************
 *                      The fake NVS the subject writes through
 *******************************************************************************/
/* Two functions, which is the whole surface ezlopi_core_sntp.c uses:
** EZPI_core_nvs_write_time_location and EZPI_core_nvs_read_time_location.
** Real signatures, taken from ezlopi_core_nvs.h, so a change to either breaks
** this file rather than silently diverging from it. */
static char fake_nvs[FAKE_NVS_MAX];
static bool fake_nvs_has_value = false;
static unsigned fake_nvs_writes = 0;
static bool fake_nvs_write_fails = false;

static void fake_nvs_reset(void)
{
    memset(fake_nvs, 0, sizeof(fake_nvs));
    fake_nvs_has_value = false;
    fake_nvs_writes = 0;
    fake_nvs_write_fails = false;
}

static void fake_nvs_preload(const char *value)
{
    snprintf(fake_nvs, sizeof(fake_nvs), "%s", value);
    fake_nvs_has_value = true;
}

ezlopi_error_t EZPI_core_nvs_write_time_location(const char *time_loc, uint32_t len)
{
    fake_nvs_writes++;

    if (fake_nvs_write_fails)
    {
        return EZPI_ERR_NVS_WRITE_FAILED;
    }

    if ((NULL == time_loc) || (len >= sizeof(fake_nvs)))
    {
        return EZPI_ERR_NVS_WRITE_FAILED;
    }

    memset(fake_nvs, 0, sizeof(fake_nvs));
    memcpy(fake_nvs, time_loc, len);
    fake_nvs_has_value = true;

    return EZPI_SUCCESS;
}

/* strdup, not a pointer into fake_nvs, so that a missing or doubled free in
** the subject is a real leak or a real double free for the sanitizers to
** catch, the same way the on-target allocator would see it. */
char *EZPI_core_nvs_read_time_location(void)
{
    if (false == fake_nvs_has_value)
    {
        return NULL;
    }

    return strdup(fake_nvs);
}

/*******************************************************************************
 *                          Helpers
 *******************************************************************************/
/* What a scene evaluator sees: a fixed epoch converted through whatever TZ is
** currently installed. 2026-07-01T12:00:00Z, chosen in July so that northern
** summer-time zones are in their DST half and the offset is the interesting
** one rather than the standard one. */
#define FIXED_EPOCH ((time_t)1782907200) /* 2026-07-01T12:00:00Z */

static void wall_clock_at_fixed_epoch(char *out, size_t out_len)
{
    struct tm tm_local;
    time_t when = FIXED_EPOCH;

    localtime_r(&when, &tm_local);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%S%z", &tm_local);
}

static bool wall_clock_is(const char *expected)
{
    char got[64];

    wall_clock_at_fixed_epoch(got, sizeof(got));

    return (0 == strcmp(expected, got));
}

/* Put the process in a zone that is not the one about to be tested, so that a
** subject which does nothing at all cannot be mistaken for one that installed
** the right answer. Asia/Kathmandu's POSIX string is used because its +05:45
** offset is not a whole number of hours and matches nothing else in this file.
**
** The exit(2) on a failed self-check is not decoration. It earned its place on
** the first run of this file: FIXED_EPOCH had been written out by hand as
** 1782993600 and labelled 2026-07-01T12:00:00Z, which is a day later than that
** -- and this is what said so, reporting "poison did not take, wall clock
** reads 2026-07-02T17:45:00+0545". Without it the mislabelled constant would
** have turned into twenty-odd expected strings all one day out, and the file
** would have failed as if the subject were wrong. A fixture that checks itself
** fails in its own name rather than in the subject's. */
static void poison_the_timezone(void)
{
    if (0 != setenv("TZ", "NPT-5:45", 1))
    {
        printf("  TEST BUG: could not poison TZ\n");
        exit(2);
    }
    tzset();

    if (false == wall_clock_is("2026-07-01T17:45:00+0545"))
    {
        char got[64];
        wall_clock_at_fixed_epoch(got, sizeof(got));
        printf("  TEST BUG: poison did not take, wall clock reads %s\n", got);
        exit(2);
    }
}

/*******************************************************************************
 *                          The tests
 *******************************************************************************/

/* The headline defect. A zone is stored, the hub reboots, and every
** localtime() caller must already be in that zone before anything calls
** hub.info.get. */
static void test_a_stored_zone_is_installed_at_boot(void)
{
    /* name, POSIX string the table maps it to, wall clock at FIXED_EPOCH */
    const struct
    {
        const char *name;
        const char *expect;
    } cases[] = {
        {"Europe/Istanbul", "2026-07-01T15:00:00+0300"},
        {"America/New_York", "2026-07-01T08:00:00-0400"},
        {"Asia/Tokyo", "2026-07-01T21:00:00+0900"},
        {"Australia/Eucla", "2026-07-01T20:45:00+0845"},
        {"Africa/Abidjan", "2026-07-01T12:00:00+0000"},
        {"America/St_Johns", "2026-07-01T09:30:00-0230"},
        {"Pacific/Chatham", "2026-07-02T00:45:00+1245"},
        {NULL, NULL},
    };

    for (int i = 0; cases[i].name; i++)
    {
        char what[160];

        fake_nvs_reset();
        fake_nvs_preload(cases[i].name);

        /* The reboot: a different zone is live, then the boot-time init runs
        ** and has to replace it from what is in flash. */
        poison_the_timezone();

        ezlopi_error_t error = EZPI_core_sntp_apply_stored_timezone();

        snprintf(what, sizeof(what), "boot with %s stored reports success", cases[i].name);
        ok(EZPI_SUCCESS == error, what);

        snprintf(what, sizeof(what), "boot with %s stored puts localtime in that zone (want %s)",
                 cases[i].name, cases[i].expect);
        ok(wall_clock_is(cases[i].expect), what);

        if (false == wall_clock_is(cases[i].expect))
        {
            char got[64];
            wall_clock_at_fixed_epoch(got, sizeof(got));
            printf("    got %s\n", got);
        }

        snprintf(what, sizeof(what), "boot with %s stored writes nothing back", cases[i].name);
        ok(0 == fake_nvs_writes, what);
    }
}

/* Nothing stored: the fallback has to be applied and announced, not left to
** whatever TZ happened to be in the environment. */
static void test_boot_with_nothing_stored_falls_back_to_utc(void)
{
    fake_nvs_reset();
    poison_the_timezone();

    ezlopi_error_t error = EZPI_core_sntp_apply_stored_timezone();

    ok(EZPI_SUCCESS != error, "boot with nothing stored does not report success");
    ok(wall_clock_is("2026-07-01T12:00:00+0000"), "boot with nothing stored leaves localtime on UTC");
    ok(0 == fake_nvs_writes, "boot with nothing stored writes nothing");
}

/* A stored name the table does not know. Same fallback, and still no write --
** the bad value is not "repaired" in flash behind the user's back. */
static void test_boot_with_an_unknown_stored_zone_falls_back(void)
{
    const char *bad[] = {"Mars/Olympus_Mons", "", "Europe/Istanbul\xff", "America/New_Yor",
                         "zzzzzzzzzzzz", "UTC+3", NULL};

    for (int i = 0; bad[i]; i++)
    {
        char what[160];

        fake_nvs_reset();
        fake_nvs_preload(bad[i]);
        poison_the_timezone();

        ezlopi_error_t error = EZPI_core_sntp_apply_stored_timezone();

        snprintf(what, sizeof(what), "boot with unknown stored zone '%s' does not report success", bad[i]);
        ok(EZPI_SUCCESS != error, what);

        snprintf(what, sizeof(what), "boot with unknown stored zone '%s' falls back to UTC", bad[i]);
        ok(wall_clock_is("2026-07-01T12:00:00+0000"), what);

        snprintf(what, sizeof(what), "boot with unknown stored zone '%s' does not overwrite it", bad[i]);
        ok(0 == fake_nvs_writes, what);
    }
}

/* hub.location.set has to take effect immediately, not at the next
** hub.info.get. */
static void test_set_applies_the_zone_immediately(void)
{
    const struct
    {
        const char *name;
        const char *expect;
    } cases[] = {
        {"Europe/Istanbul", "2026-07-01T15:00:00+0300"},
        {"America/Los_Angeles", "2026-07-01T05:00:00-0700"},
        {"Asia/Kolkata", "2026-07-01T17:30:00+0530"},
        {"Europe/London", "2026-07-01T13:00:00+0100"},
        {NULL, NULL},
    };

    for (int i = 0; cases[i].name; i++)
    {
        char what[160];

        fake_nvs_reset();
        poison_the_timezone();

        ezlopi_error_t error = EZPI_core_sntp_set_location(cases[i].name);

        snprintf(what, sizeof(what), "set %s succeeds", cases[i].name);
        ok(EZPI_SUCCESS == error, what);

        snprintf(what, sizeof(what), "set %s moves localtime at once (want %s)",
                 cases[i].name, cases[i].expect);
        ok(wall_clock_is(cases[i].expect), what);

        snprintf(what, sizeof(what), "set %s stored it", cases[i].name);
        ok(0 == strcmp(cases[i].name, fake_nvs), what);

        snprintf(what, sizeof(what), "set %s wrote exactly once", cases[i].name);
        ok(1 == fake_nvs_writes, what);
    }
}

/* The validate-before-store property. An unknown name must not reach flash,
** and must not disturb the zone already in effect. */
static void test_set_refuses_an_unknown_zone_without_storing_it(void)
{
    const char *bad[] = {"Mars/Olympus_Mons", "", "Not/A/Zone", "America/New_Yor",
                         "Europe/istanbu", "GMT+3", "0", "../../etc/passwd", NULL};

    for (int i = 0; bad[i]; i++)
    {
        char what[160];

        /* A good zone is in place and in flash first, so the test can tell
        ** "refused" from "cleared". */
        fake_nvs_reset();
        ok(EZPI_SUCCESS == EZPI_core_sntp_set_location("Asia/Tokyo"), "fixture: Asia/Tokyo accepted");
        unsigned writes_before = fake_nvs_writes;

        ezlopi_error_t error = EZPI_core_sntp_set_location(bad[i]);

        snprintf(what, sizeof(what), "set '%s' is refused", bad[i]);
        ok(EZPI_SUCCESS != error, what);

        snprintf(what, sizeof(what), "set '%s' does not reach flash", bad[i]);
        ok(writes_before == fake_nvs_writes, what);

        snprintf(what, sizeof(what), "set '%s' leaves the stored zone alone", bad[i]);
        ok(0 == strcmp("Asia/Tokyo", fake_nvs), what);

        snprintf(what, sizeof(what), "set '%s' leaves the live zone alone", bad[i]);
        ok(wall_clock_is("2026-07-01T21:00:00+0900"), what);
    }
}

/* A NULL name is a client sending no location at all. */
static void test_set_refuses_null(void)
{
    fake_nvs_reset();
    ok(EZPI_SUCCESS == EZPI_core_sntp_set_location("Asia/Tokyo"), "fixture: Asia/Tokyo accepted");
    unsigned writes_before = fake_nvs_writes;

    ok(EZPI_SUCCESS != EZPI_core_sntp_set_location(NULL), "set NULL is refused");
    ok(writes_before == fake_nvs_writes, "set NULL does not reach flash");
    ok(wall_clock_is("2026-07-01T21:00:00+0900"), "set NULL leaves the live zone alone");
}

/* A failed write must not leave the live zone describing something that is not
** in flash. This is the same disagreement as the original defect, arriving
** from the other direction. */
static void test_a_failed_write_does_not_move_the_live_zone(void)
{
    fake_nvs_reset();
    ok(EZPI_SUCCESS == EZPI_core_sntp_set_location("Asia/Tokyo"), "fixture: Asia/Tokyo accepted");
    ok(wall_clock_is("2026-07-01T21:00:00+0900"), "fixture: live zone is Asia/Tokyo");

    fake_nvs_write_fails = true;

    ezlopi_error_t error = EZPI_core_sntp_set_location("Europe/Istanbul");

    ok(EZPI_SUCCESS != error, "set reports failure when the write fails");
    ok(EZPI_ERR_NVS_WRITE_FAILED == error, "set reports the write failure specifically");
    ok(wall_clock_is("2026-07-01T21:00:00+0900"),
       "a failed write leaves the live zone matching what is still in flash");
    ok(0 == strcmp("Asia/Tokyo", fake_nvs), "a failed write leaves flash alone");
}

/* The specific symptom: set a zone, then reboot, and it has to still be there
** without anyone calling hub.info.get. */
static void test_a_set_zone_survives_a_reboot(void)
{
    fake_nvs_reset();

    ok(EZPI_SUCCESS == EZPI_core_sntp_set_location("Europe/Istanbul"), "set Europe/Istanbul");
    ok(wall_clock_is("2026-07-01T15:00:00+0300"), "live zone is Europe/Istanbul before the reboot");

    char flash_held[FAKE_NVS_MAX];
    snprintf(flash_held, sizeof(flash_held), "%s", fake_nvs);

    /* The reboot. TZ is poisoned first: a process that restarted would have no
    ** TZ at all, and the thing being tested is that init installs one, so the
    ** test must not let a leftover correct value answer for it. */
    poison_the_timezone();
    fake_nvs_reset();
    fake_nvs_preload(flash_held);

    ok(EZPI_SUCCESS == EZPI_core_sntp_apply_stored_timezone(), "boot resolves the stored zone");
    ok(wall_clock_is("2026-07-01T15:00:00+0300"), "live zone is Europe/Istanbul after the reboot");
    ok(0 == fake_nvs_writes, "the reboot wrote nothing");
}

/* EZPI_core_sntp_get_local_time has to report the same zone as everything
** else, now that it no longer installs one of its own. */
static void test_get_local_time_agrees_with_the_installed_zone(void)
{
    char buf[64];

    fake_nvs_reset();
    poison_the_timezone();
    ok(EZPI_SUCCESS != EZPI_core_sntp_apply_stored_timezone(),
       "fixture: nothing stored, so boot reports the fallback");

    fake_nvs_preload("Asia/Tokyo");
    ok(EZPI_SUCCESS == EZPI_core_sntp_apply_stored_timezone(), "fixture: Asia/Tokyo applied");

    memset(buf, 0, sizeof(buf));
    EZPI_core_sntp_get_local_time(buf, sizeof(buf));

    /* The clock is the host's real one, so the time of day is not predictable
    ** -- but the zone offset is, and that is the part that was wrong. */
    ok('\0' != buf[0], "get_local_time wrote something");
    ok(NULL != strstr(buf, "+0900"), "get_local_time reports the installed zone's offset");

    /* And it must not have re-read or re-installed anything to do it. */
    ok(0 == fake_nvs_writes, "get_local_time writes nothing");

    fake_nvs_reset();
    fake_nvs_preload("America/New_York");
    ok(EZPI_SUCCESS == EZPI_core_sntp_apply_stored_timezone(), "fixture: America/New_York applied");

    memset(buf, 0, sizeof(buf));
    EZPI_core_sntp_get_local_time(buf, sizeof(buf));
    ok(NULL != strstr(buf, "-0400"), "get_local_time follows a later zone change");
}

/* get_local_time's guards. A zero length or a NULL buffer must not write. */
static void test_get_local_time_rejects_a_useless_buffer(void)
{
    char buf[64];

    fake_nvs_reset();
    fake_nvs_preload("Asia/Tokyo");
    ok(EZPI_SUCCESS == EZPI_core_sntp_apply_stored_timezone(), "fixture: Asia/Tokyo applied");

    memset(buf, 0x7f, sizeof(buf));
    EZPI_core_sntp_get_local_time(buf, 0);
    ok(0x7f == (unsigned char)buf[0], "get_local_time with buf_len 0 writes nothing");

    /* NULL with a non-zero length: asserting it returns rather than faults. */
    EZPI_core_sntp_get_local_time(NULL, sizeof(buf));
    ok(true, "get_local_time with a NULL buffer returns");

    /* A buffer too small for the format. strftime returns 0 and writes no
    ** terminator it can be trusted on, so the only property asserted is that
    ** it does not run past the end -- ASan is the judge of that. */
    char tiny[4];
    memset(tiny, 0, sizeof(tiny));
    EZPI_core_sntp_get_local_time(tiny, sizeof(tiny));
    ok(true, "get_local_time into a 4-byte buffer does not overrun");
}

/* Every name hub.location.list publishes must be settable, and setting it must
** install that entry's own POSIX string.
**
** hub.location.list and hub.location.set read the same table, so a mismatch
** would mean the list offers names set refuses -- a dead end in whatever UI a
** user picks from, which is why this sweeps rather than samples.
**
** The second assertion is the one worth the trouble. "set succeeded" is cheap
** to satisfy and says nothing: a subject that resolved the name and then
** installed the wrong string, or no string, passes it. So for each entry the
** wall clock is read twice -- once after the subject has had the name, and
** once after this file installs tz[i].posix_str itself with setenv+tzset --
** and the two readings must agree. That pins the subject to the table's own
** POSIX string for all 425 entries rather than to the test's idea of what any
** of them should be.
**
** Poisoning between the two readings matters for the same reason it does
** everywhere else here: without it the second reading could be the first one
** left in place. Note that it cannot be used as the comparison itself --
** Asia/Kathmandu is +0545 and so produces exactly the poisoned wall clock,
** and "changed from the poison" would report it as a failure. */
static void test_every_published_zone_is_settable(void)
{
    const micro_tz_db_pair *tz = get_tz_db();
    int total = 0;
    int accepted = 0;
    int matched_the_table = 0;

    while (tz[total].name != NULL)
    {
        char from_subject[64];
        char from_table[64];

        fake_nvs_reset();
        poison_the_timezone();

        if (EZPI_SUCCESS == EZPI_core_sntp_set_location(tz[total].name))
        {
            accepted++;
            wall_clock_at_fixed_epoch(from_subject, sizeof(from_subject));

            poison_the_timezone();
            if (0 != setenv("TZ", tz[total].posix_str, 1))
            {
                printf("  TEST BUG: could not install %s\n", tz[total].posix_str);
                exit(2);
            }
            tzset();
            wall_clock_at_fixed_epoch(from_table, sizeof(from_table));

            if (0 == strcmp(from_subject, from_table))
            {
                matched_the_table++;
            }
            else
            {
                printf("    %s: subject gave %s, table's %s gives %s\n",
                       tz[total].name, from_subject, tz[total].posix_str, from_table);
            }
        }
        else
        {
            printf("    not settable: %s\n", tz[total].name);
        }

        total++;
    }

    printf("  swept %d published zones\n", total);
    ok(425 == total, "the zone table still has 425 entries");
    ok(total == accepted, "every published zone is accepted by hub.location.set");
    ok(total == matched_the_table,
       "every published zone installs that entry's own POSIX string");
}

/* A zone that is stored, then replaced, then the hub reboots: the reboot has
** to produce the second one. Catches an init that reads a stale copy. */
static void test_a_replaced_zone_is_what_survives(void)
{
    fake_nvs_reset();

    ok(EZPI_SUCCESS == EZPI_core_sntp_set_location("Asia/Tokyo"), "set Asia/Tokyo first");
    ok(EZPI_SUCCESS == EZPI_core_sntp_set_location("America/Denver"), "then set America/Denver");
    ok(wall_clock_is("2026-07-01T06:00:00-0600"), "live zone followed the second set");
    ok(2 == fake_nvs_writes, "two sets wrote twice");

    char flash_held[FAKE_NVS_MAX];
    snprintf(flash_held, sizeof(flash_held), "%s", fake_nvs);

    poison_the_timezone();
    fake_nvs_reset();
    fake_nvs_preload(flash_held);

    ok(EZPI_SUCCESS == EZPI_core_sntp_apply_stored_timezone(), "boot after the replacement");
    ok(wall_clock_is("2026-07-01T06:00:00-0600"), "the reboot produced America/Denver, not Asia/Tokyo");
}

/* The table lookup is case- and underscore-insensitive by design
** (tz_name_cmp in zones.c folds case and skips underscores). Recorded as a
** property of the subject rather than asserted one way on taste: a client that
** sends "europe/istanbul" gets the zone, which is the friendlier behaviour and
** is what the comparator was written to do. */
static void test_name_matching_is_lenient_by_design(void)
{
    const struct
    {
        const char *name;
        bool accepted;
        const char *note;
    } cases[] = {
        {"Europe/Istanbul", true, "the canonical spelling"},
        {"europe/istanbul", true, "lower case is accepted -- tz_name_cmp folds case"},
        {"EUROPE/ISTANBUL", true, "upper case is accepted"},
        {"America/New_York", true, "the canonical spelling, with an underscore"},
        {"America/NewYork", true, "a missing underscore is accepted -- tz_name_cmp skips them"},
        {"Europe/Istanbul ", false, "a trailing space is not accepted"},
        {" Europe/Istanbul", false, "a leading space is not accepted"},
        {NULL, false, NULL},
    };

    for (int i = 0; cases[i].name; i++)
    {
        char what[200];

        fake_nvs_reset();
        ezlopi_error_t error = EZPI_core_sntp_set_location(cases[i].name);

        snprintf(what, sizeof(what), "'%s': %s", cases[i].name, cases[i].note);
        ok(cases[i].accepted == (EZPI_SUCCESS == error), what);
    }
}

/* EZPI_core_sntp_init is compiled in the same translation unit, so it is worth
** one check that it is reachable and does not depend on a timezone being
** installed. The stub never reports a completed sync, so the failure return is
** the honest expectation -- see stubs-sntp/esp_sntp.h. */
static void test_sntp_init_is_reachable_and_independent_of_tz(void)
{
    fake_nvs_reset();
    fake_nvs_preload("Asia/Tokyo");
    ok(EZPI_SUCCESS == EZPI_core_sntp_apply_stored_timezone(), "fixture: Asia/Tokyo applied");

    unsigned init_before = fake_sntp_init_calls;
    ezlopi_error_t error = EZPI_core_sntp_init();

    ok(init_before + 1 == fake_sntp_init_calls, "sntp_init reached esp_sntp_init");
    ok(EZPI_SUCCESS != error, "sntp_init reports failure when no sync completes");
    ok(fake_sntp_status_polls > 0, "sntp_init polled the sync status");
    ok(NULL != fake_sntp_notify_cb, "sntp_init registered a sync notification callback");
    ok(wall_clock_is("2026-07-01T21:00:00+0900"), "sntp_init did not disturb the installed zone");
    ok(0 == fake_nvs_writes, "sntp_init wrote nothing to the stored location");
}

/* get_location is the reader hub.location.get uses. It has to return what was
** stored, and the caller owns the result -- so a leak here is a leak on
** target, and LeakSanitizer is what says so. */
static void test_get_location_returns_what_was_stored(void)
{
    fake_nvs_reset();

    char *none = EZPI_core_sntp_get_location();
    ok(NULL == none, "get_location returns NULL when nothing is stored");

    ok(EZPI_SUCCESS == EZPI_core_sntp_set_location("Europe/Istanbul"), "fixture: set Europe/Istanbul");

    char *got = EZPI_core_sntp_get_location();
    ok(NULL != got, "get_location returns a string after a set");
    ok((NULL != got) && (0 == strcmp("Europe/Istanbul", got)), "get_location returns the stored name");
    free(got);
}

/*******************************************************************************
 *                          Entry Point
 *******************************************************************************/
int main(void)
{
    printf("test_tz: the hub's timezone\n");

    test_a_stored_zone_is_installed_at_boot();
    test_boot_with_nothing_stored_falls_back_to_utc();
    test_boot_with_an_unknown_stored_zone_falls_back();
    test_set_applies_the_zone_immediately();
    test_set_refuses_an_unknown_zone_without_storing_it();
    test_set_refuses_null();
    test_a_failed_write_does_not_move_the_live_zone();
    test_a_set_zone_survives_a_reboot();
    test_get_local_time_agrees_with_the_installed_zone();
    test_get_local_time_rejects_a_useless_buffer();
    test_every_published_zone_is_settable();
    test_a_replaced_zone_is_what_survives();
    test_name_matching_is_lenient_by_design();
    test_sntp_init_is_reachable_and_independent_of_tz();
    test_get_location_returns_what_was_stored();

    printf("%d check(s), %d failure(s)\n", checks, failures);

    return (0 == failures) ? 0 : 1;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
