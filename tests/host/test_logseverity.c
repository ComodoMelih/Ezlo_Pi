/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    test_logseverity.c
 * @brief   Host test: a log severity is an index into a six-element table.
 *
 * ezlopi_core_log.c keeps two severities in static storage and uses both of
 * them, unchecked, to index
 *
 *     const char *ezlopi_log_severity_enum[ENUM_EZLOPI_TRACE_SEVERITY_MAX]
 *
 * which has exactly ENUM_EZLOPI_TRACE_SEVERITY_MAX entries -- so MAX is the
 * length, and the valid indices stop one short of it at
 * ENUM_EZLOPI_TRACE_SEVERITY_TRACE. The file's own default for the serial
 * severity was ENUM_EZLOPI_TRACE_SEVERITY_MAX: the length, used as an index.
 *
 * WHAT THIS DEFECT IS AND IS NOT
 * -----------------------------
 * It is tempting to call that a live out-of-bounds read reachable from the
 * cloud, because EZPI_core_serial_log_get_current_severity_enum_str() returns
 * ezlopi_log_severity_enum[serial_log_severity] and
 * ezlopi_cloud_log.c:132 hands that pointer straight to
 * cJSON_AddStringToObject() in response to a hub.setting.list request. That
 * was the first conclusion here and it was wrong, so it is written down rather
 * than quietly dropped.
 *
 * Two lines stop it:
 *
 *     ezlopi_core_ezlopi.c:135-137
 *     ezlopi_core_setting_commands.c:256-258
 *
 * both of which read the stored severities and then immediately call
 *
 *     EZPI_core_read_set_log_severities_internal(ENUM_EZLOPI_TRACE_SEVERITY_TRACE);
 *
 * unconditionally. So on a booted hub the serial severity is TRACE (5), in
 * range, whatever NVS held. The MAX default never reaches the getter.
 *
 * What makes it worth fixing anyway is the comment directly above each of
 * those two calls:
 *
 *     // #warning "remove this in release"
 *
 * The thing masking an out-of-bounds read is a debug override that the source
 * itself marks for deletion. Remove it -- do the thing the comment asks for --
 * and the read goes live on any hub whose serial severity was never stored,
 * because EZPI_core_nvs_read_uint32() leaves its output parameter untouched
 * when the key is absent and EZPI_core_read_set_log_severities() discards its
 * return value. That is the out-of-the-box state.
 *
 * It also tells you what the right default is, for free: TRACE is already the
 * value every booted hub runs with. Setting the initialiser to TRACE makes the
 * static default agree with the runtime default instead of inventing a third
 * answer, and it is behaviour-identical for every real severity -- the only
 * gate that reads the value is (severity > NONE && severity <= threshold), and
 * MAX versus TRACE can only differ for a log emitted at severity exactly MAX,
 * which nothing in the tree does.
 *
 * WHAT IS ASSERTED HERE
 * ---------------------
 * Seven case functions, 108 checks. Five claims, in the order main() runs
 * them -- the fifth is three of the cases:
 *
 *   1. The table really is MAX long and MAX really is one past the last valid
 *      index. Every assertion below is written against the symbols, never
 *      against the literal 6, so renumbering the enum cannot silently
 *      invalidate the test.
 *
 *   2. The default serial severity, read before anything in this process has
 *      set it, is a valid index. This case must run first and does; see the
 *      comment on main().
 *
 *   3. EZPI_core_read_set_log_severities() leaves a valid index behind in all
 *      three cases that matter: the key absent (the fake leaves the output
 *      untouched, exactly as EZPI_core_nvs_read_uint32 does), the key present
 *      and valid, and the key present and outside the enum.
 *
 *   4. EZPI_core_send_cloud_log() does not index the table with a severity it
 *      was handed by a caller without checking it first.
 *
 *   5. Nothing above changed what the setters accept, what they clamp to, what
 *      the debug override does with the value both its callers pass, or which
 *      severities the serial upcall prints. Three cases, and this is the half
 *      that makes the fix a fix rather than a behaviour change -- it is why the
 *      file is this long. Measured: with the fix reverted one piece at a time,
 *      none of these three go red. See the Makefile rule for the table.
 *
 * HOW THE STATIC FUNCTION IS REACHED
 * ----------------------------------
 * EZPI_core_serial_log_upcall() is static, and the severity gate under test
 * lives in it. It is not copied here. EZPI_core_set_log_upcalls() passes it to
 * ezlopi_util_set_log_upcalls(), which this file fakes -- so the fake keeps the
 * two function pointers and the test calls the real static function through
 * them. Nothing about the gate is reimplemented.
 *
 * The upcall printf()s when it decides to log, which is the only observable it
 * has besides its return value. Both are used: stdout is redirected to a
 * temporary file around each call so the assertion can be on what was actually
 * written, and so a suite that prints 40 lines of fake log text is not what
 * anyone has to read.
 *
 * SANITIZERS, AND WHAT THEY ACTUALLY SAY
 * --------------------------------------
 * Measured against the unfixed subject rather than predicted, and the
 * prediction written here first was wrong in both directions, so here is what
 * the three runs reported:
 *
 *   With -fsanitize=undefined: a bounds check at the indexing line --
 *   "index 6 out of bounds for type 'char *[6]'". UBSan sees it because the
 *   array's declared bound is in scope at the point of the subscript.
 *
 *   With -fsanitize=address: a global-buffer-overflow, "0 bytes after global
 *   variable 'ezlopi_log_severity_enum' ... of size 48", READ of size 8. ASan
 *   halts on it, which is why the run never reaches the tally.
 *
 *   With no sanitizers at all: the assertions below catch it on their own --
 *   six FAIL lines, and then a hard segfault inside
 *   EZPI_core_cloud_log_get_current_severity_enum_str() once a case puts
 *   0xDEADBEEF in the NVS entry. So this file is a test, not a sanitizer
 *   wrapper; the sanitizers add the line number, not the finding.
 *
 * The rule still carries -fno-sanitize-recover=address and =undefined, because
 * gcc's default for both is to print "runtime error", carry on, and exit 0 --
 * which would put an out-of-bounds read in the log of a suite reporting
 * success.
 *
 * __is_a_table_entry() below is pointer identity rather than strcmp for a
 * related reason: index 6 lands on a live, NUL-terminated string belonging to
 * something else in .data, so a name-based check would have had to know in
 * advance what that string was. Identity against the array's own six elements
 * needs to know nothing.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cjext.h"
#include "ezlopi_core_errors.h"
#include "ezlopi_core_event_group.h"
#include "ezlopi_core_log.h"
#include "ezlopi_core_nvs.h"

/*******************************************************************************
 *                          Fakes
 *******************************************************************************/

/* The subject's whole NVS surface. Read behaviour is controllable because the
** defect is about what the static keeps when a read does nothing, and the real
** EZPI_core_nvs_read_uint32() does exactly nothing to *i when nvs_get_u32
** fails -- including the case where ezlopi_nvs_handle is NULL, which is a hub
** whose NVS never opened. g_nvs_read_result == EZPI_ERR_NVS_READ_FAILED
** reproduces that, and the fake deliberately does not write through the
** pointer in that case. Getting this wrong in the convenient direction -- a
** fake that zeroes the output on failure -- would make the subject look safe
** for a reason the firmware does not have. */
static ezlopi_error_t g_nvs_read_result = EZPI_ERR_NVS_READ_FAILED;
static uint32_t g_nvs_cloud_value = 0;
static uint32_t g_nvs_serial_value = 0;
static int g_nvs_cloud_reads = 0;
static int g_nvs_serial_reads = 0;

/* What the subject last persisted. Checked rather than ignored: a setter that
** updates the static but not NVS, or the reverse, is a defect this file is
** cheap enough to notice. */
static uint32_t g_nvs_cloud_written = 0xFFFFFFFFu;
static uint32_t g_nvs_serial_written = 0xFFFFFFFFu;

ezlopi_error_t EZPI_core_nvs_read_cloud_log_severity(uint32_t *severity)
{
    g_nvs_cloud_reads++;

    if (EZPI_SUCCESS == g_nvs_read_result)
    {
        *severity = g_nvs_cloud_value;
    }

    return g_nvs_read_result;
}

ezlopi_error_t EZPI_core_nvs_read_serial_log_severity(uint32_t *severity)
{
    g_nvs_serial_reads++;

    if (EZPI_SUCCESS == g_nvs_read_result)
    {
        *severity = g_nvs_serial_value;
    }

    return g_nvs_read_result;
}

ezlopi_error_t EZPI_core_nvs_write_cloud_log_severity(uint32_t severity)
{
    g_nvs_cloud_written = severity;
    return EZPI_SUCCESS;
}

ezlopi_error_t EZPI_core_nvs_write_serial_log_severity(uint32_t severity)
{
    g_nvs_serial_written = severity;
    return EZPI_SUCCESS;
}

/* The cloud-broadcast path. g_registered decides whether
** EZPI_core_send_cloud_log() gets past its event-bit check, which is the only
** way to reach the ezlopi_log_severity_enum[severity] at the bottom of it. */
static bool g_registered = true;
static cJSON *g_queued = NULL;
static int g_queue_calls = 0;

e_ezlopi_event_t EZPI_core_event_group_get_eventbit_status(void)
{
    return g_registered ? EZLOPI_EVENT_NMA_REG : (e_ezlopi_event_t)0;
}

ezlopi_error_t EZPI_core_broadcast_add_to_queue(cJSON *cj_data, time_t timestamp)
{
    (void)timestamp;

    g_queue_calls++;

    /* Kept, not freed, so a case can read the severity string the subject put
    ** on the wire. Returning EZPI_SUCCESS means the subject does not delete it
    ** either, so ownership is this file's; __release_queued() below is the
    ** matching free and every case that queues calls it. */
    g_queued = cj_data;

    return EZPI_SUCCESS;
}

time_t EZPI_core_sntp_get_current_time_sec_abc(const char *filename)
{
    (void)filename;

    /* A fixed epoch rather than time(NULL): the broadcast carries the
    ** timestamp into a string, and a test that reads that string should not
    ** depend on when it ran. 1700000000 = 2023-11-14T22:13:20Z. */
    return (time_t)1700000000;
}

void EZPI_core_sntp_epoch_to_iso8601(char *buf, size_t len, time_t epoch)
{
    (void)epoch;

    if (buf && len)
    {
        snprintf(buf, len, "1970-01-01T00:00:00Z");
    }
}

/* ezlopi_util_set_log_upcalls() is faked for two reasons, and the second is
** the interesting one. The first: ezlopi_util_trace.c does not compile on a
** host. The second: this is the only handle on EZPI_core_serial_log_upcall(),
** which is static.
**
** f_ezlopi_log_upcall_t is the real typedef, reached through
** stubs-log/ezlopi_util_trace.h, not a local one of the same shape -- so a
** signature change on target breaks this file loudly instead of letting the
** fake and the subject disagree about what they are passing. */
static f_ezlopi_log_upcall_t g_cloud_upcall = NULL;
static f_ezlopi_log_upcall_t g_serial_upcall = NULL;

void ezlopi_util_set_log_upcalls(f_ezlopi_log_upcall_t cloud_log_upcall, f_ezlopi_log_upcall_t serial_log_upcall)
{
    g_cloud_upcall = cloud_log_upcall;
    g_serial_upcall = serial_log_upcall;
}

/*******************************************************************************
 *                          Test harness
 *******************************************************************************/

static int checks = 0;
static int failures = 0;

static void ok(int condition, const char *what)
{
    checks++;

    if (!condition)
    {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/**
 * @brief   Is this pointer one of the six entries of the real table?
 *
 * The check that catches the defect, and it is pointer identity rather than
 * strcmp on purpose. ezlopi_log_severity_enum[ENUM_EZLOPI_TRACE_SEVERITY_MAX]
 * reads whatever the linker put after the array -- on the build measured here,
 * a pointer into .rodata belonging to another object in the same translation
 * unit, which is a perfectly valid C string. strcmp against a list of expected
 * names would therefore have had to know in advance what that string was.
 * Identity against the array's own six elements needs to know nothing.
 */
static bool __is_a_table_entry(const char *s)
{
    const char **table = EZPI_core_cloud_log_get_severity_enums();

    for (int i = 0; i < ENUM_EZLOPI_TRACE_SEVERITY_MAX; i++)
    {
        if (s == table[i])
        {
            return true;
        }
    }

    return false;
}

static void __release_queued(void)
{
    if (g_queued)
    {
        cJSON_Delete(__FUNCTION__, g_queued);
        g_queued = NULL;
    }
}

/**
 * @brief   Call the serial upcall with stdout pointed at a temporary file, and
 *          report both what it returned and how many bytes it printed.
 *
 * dup()/dup2() on fileno(stdout) rather than freopen(), because freopen would
 * also redirect this file's own FAIL lines if a case between the swap and the
 * restore ever called ok(). Nothing does, but the shape that cannot is better
 * than the shape that happens not to.
 */
static ezlopi_error_t __serial_upcall(int severity, const char *log_str, long *printed)
{
    ezlopi_error_t ret;
    long bytes = -1;

    if (NULL == g_serial_upcall)
    {
        printf("  FATAL: EZPI_core_set_log_upcalls() never handed over the serial upcall\n");
        exit(2);
    }

    FILE *sink = tmpfile();
    int saved = -1;

    if (sink)
    {
        fflush(stdout);
        saved = dup(fileno(stdout));
        if (dup2(fileno(sink), fileno(stdout)) < 0)
        {
            printf("  FATAL: could not redirect stdout\n");
            exit(2);
        }
    }

    ret = g_serial_upcall(severity, log_str);

    if (sink)
    {
        fflush(stdout);
        if (saved >= 0)
        {
            dup2(saved, fileno(stdout));
            close(saved);
        }
        bytes = ftell(sink);
        fclose(sink);
    }

    if (printed)
    {
        *printed = bytes;
    }

    return ret;
}

/*******************************************************************************
 *                          Cases
 *******************************************************************************/

/**
 * @brief   The table's shape, which every other case is written against.
 *
 * Nothing here can fail by accident: these are the facts the rest of the file
 * assumes, and if the enum is ever renumbered this case is where it shows.
 */
static void test_the_table(void)
{
    printf("\n-- the severity table\n");

    const char **table = EZPI_core_cloud_log_get_severity_enums();

    ok(NULL != table, "the table is reachable through the public getter");

    /* MAX is the array length. This is the whole defect in one assertion: it
    ** is not a severity, and the index of the last severity is MAX - 1. */
    ok(ENUM_EZLOPI_TRACE_SEVERITY_TRACE == (ENUM_EZLOPI_TRACE_SEVERITY_MAX - 1),
       "TRACE is the last valid index, one short of MAX");

    int non_null = 0;

    for (int i = 0; i < ENUM_EZLOPI_TRACE_SEVERITY_MAX; i++)
    {
        if (table[i])
        {
            non_null++;
        }
    }

    ok(ENUM_EZLOPI_TRACE_SEVERITY_MAX == non_null,
       "every index below MAX holds a name");

    /* The names, by symbol. A test that indexed with literals would still pass
    ** if the enum were reordered and every caller broke. */
    ok(0 == strcmp("NONE", table[ENUM_EZLOPI_TRACE_SEVERITY_NONE]), "index NONE is \"NONE\"");
    ok(0 == strcmp("ERROR", table[ENUM_EZLOPI_TRACE_SEVERITY_ERROR]), "index ERROR is \"ERROR\"");
    ok(0 == strcmp("WARNING", table[ENUM_EZLOPI_TRACE_SEVERITY_WARNING]), "index WARNING is \"WARNING\"");
    ok(0 == strcmp("INFO", table[ENUM_EZLOPI_TRACE_SEVERITY_INFO]), "index INFO is \"INFO\"");
    ok(0 == strcmp("DEBUG", table[ENUM_EZLOPI_TRACE_SEVERITY_DEBUG]), "index DEBUG is \"DEBUG\"");
    ok(0 == strcmp("TRACE", table[ENUM_EZLOPI_TRACE_SEVERITY_TRACE]), "index TRACE is \"TRACE\"");
}

/**
 * @brief   The defect. Runs before anything in this process sets a severity.
 *
 * This is the only case whose position in main() is load-bearing, and the
 * reason is that the value under test is a static initialiser: any setter
 * called before it would overwrite the thing being measured and the case would
 * pass while testing nothing. main() calls it second, after test_the_table(),
 * which calls no setter.
 */
static void test_the_default_severities_are_valid_indices(void)
{
    printf("\n-- the defaults, before anything has been set\n");

    /* The one that was wrong. Pre-fix this is
    ** ezlopi_log_severity_enum[ENUM_EZLOPI_TRACE_SEVERITY_MAX]. */
    const char *serial = EZPI_core_serial_log_get_current_severity_enum_str();

    ok(__is_a_table_entry(serial),
       "the default serial severity string is one of the table's own entries");

    ok(ENUM_EZLOPI_TRACE_SEVERITY_MAX > EZPI_core_serial_log_get_current_severity_enum_val(),
       "the default serial severity value is below MAX");

    /* The cloud default was already WARNING, so this half is a regression
    ** guard rather than a finding. Asserted anyway: the fix touches the same
    ** two lines, and "I changed the one next to it" is a cheap mistake to make
    ** and a cheap one to catch. */
    const char *cloud = EZPI_core_cloud_log_get_current_severity_enum_str();

    ok(__is_a_table_entry(cloud),
       "the default cloud severity string is one of the table's own entries");

    ok(ENUM_EZLOPI_TRACE_SEVERITY_WARNING == EZPI_core_cloud_log_get_current_severity_enum_val(),
       "the default cloud severity is still WARNING");

    /* The serial default has to be TRACE specifically, not merely in range.
    ** Both call sites of EZPI_core_read_set_log_severities() follow it with
    ** EZPI_core_read_set_log_severities_internal(ENUM_EZLOPI_TRACE_SEVERITY_TRACE),
    ** so TRACE is what a booted hub runs with today; a static default of
    ** anything else would be a behaviour change dressed up as a bounds fix. */
    ok(ENUM_EZLOPI_TRACE_SEVERITY_TRACE == EZPI_core_serial_log_get_current_severity_enum_val(),
       "the default serial severity is TRACE, which is what both boot paths already force");
}

/**
 * @brief   What the NVS read leaves behind, in the three cases that exist.
 */
static void test_reading_nvs_leaves_a_valid_index(void)
{
    printf("\n-- EZPI_core_read_set_log_severities()\n");

    /* (a) The key is absent. The fake does not write through the pointer, for
    ** the same reason the real one does not, so whatever the static held is
    ** what it still holds -- and the subject discards the return value, so it
    ** cannot tell. This is the out-of-the-box hub. */
    g_nvs_read_result = EZPI_ERR_NVS_READ_FAILED;
    g_nvs_cloud_reads = 0;
    g_nvs_serial_reads = 0;

    EZPI_core_read_set_log_severities();

    ok(1 == g_nvs_cloud_reads, "the cloud severity was read from NVS exactly once");
    ok(1 == g_nvs_serial_reads, "the serial severity was read from NVS exactly once");
    ok(__is_a_table_entry(EZPI_core_serial_log_get_current_severity_enum_str()),
       "key absent: the serial severity string is still a table entry");
    ok(__is_a_table_entry(EZPI_core_cloud_log_get_current_severity_enum_str()),
       "key absent: the cloud severity string is still a table entry");

    /* (b) The key is present and holds a real severity. It must be honoured --
    ** a bounds fix that threw away the stored configuration would be a worse
    ** defect than the one it fixed. */
    g_nvs_read_result = EZPI_SUCCESS;
    g_nvs_cloud_value = ENUM_EZLOPI_TRACE_SEVERITY_ERROR;
    g_nvs_serial_value = ENUM_EZLOPI_TRACE_SEVERITY_DEBUG;

    EZPI_core_read_set_log_severities();

    ok(ENUM_EZLOPI_TRACE_SEVERITY_ERROR == EZPI_core_cloud_log_get_current_severity_enum_val(),
       "a stored cloud severity of ERROR is honoured");
    ok(ENUM_EZLOPI_TRACE_SEVERITY_DEBUG == EZPI_core_serial_log_get_current_severity_enum_val(),
       "a stored serial severity of DEBUG is honoured");

    /* (c) The key is present and holds something that is not a severity. NVS
    ** is a uint32_t and nothing in the read path narrows it: the stored value
    ** survives a firmware downgrade, an enum renumbering, and a partially
    ** written entry. The write path clamps today, which is why this is a
    ** hardening case and not a second finding -- but "the only validation is
    ** at the other end of a flash write" is not validation. */
    g_nvs_cloud_value = 0xDEADBEEFu;
    g_nvs_serial_value = ENUM_EZLOPI_TRACE_SEVERITY_MAX; /* the length again */

    EZPI_core_read_set_log_severities();

    ok(ENUM_EZLOPI_TRACE_SEVERITY_MAX > EZPI_core_cloud_log_get_current_severity_enum_val(),
       "an out-of-enum stored cloud severity does not become the index");
    ok(ENUM_EZLOPI_TRACE_SEVERITY_MAX > EZPI_core_serial_log_get_current_severity_enum_val(),
       "a stored serial severity of exactly MAX does not become the index");
    ok(__is_a_table_entry(EZPI_core_cloud_log_get_current_severity_enum_str()),
       "garbage in NVS: the cloud severity string is still a table entry");
    ok(__is_a_table_entry(EZPI_core_serial_log_get_current_severity_enum_str()),
       "MAX in NVS: the serial severity string is still a table entry");

    g_nvs_read_result = EZPI_ERR_NVS_READ_FAILED;
}

/**
 * @brief   EZPI_core_send_cloud_log() indexes the table with its argument.
 *
 * The argument is a plain int from a caller. The only thing between it and
 * ezlopi_log_severity_enum[severity] is a threshold comparison against the
 * current cloud severity -- which bounds it above and, as written, bounded it
 * below only by accident: `int <= e_trace_severity_t` promotes the int to the
 * enum's unsigned underlying type, so -1 became a huge unsigned and failed the
 * test. That is a real effect and it does protect the index, but it depends on
 * the compiler having given the enum an unsigned underlying type, which the C
 * standard leaves to the implementation. These cases assert the outcome, so
 * they hold either way and they hold after the comparison is made explicit.
 */
static void test_send_cloud_log_does_not_index_with_its_argument(void)
{
    printf("\n-- EZPI_core_send_cloud_log()\n");

    g_registered = true;
    g_nvs_cloud_written = 0xFFFFFFFFu;

    /* Put the threshold at the top of the enum so the gate admits as much as
    ** it ever can. process_id clamps the cloud severity to WARNING, so go
    ** through the NVS path, which is the one that does not. */
    g_nvs_read_result = EZPI_SUCCESS;
    g_nvs_cloud_value = ENUM_EZLOPI_TRACE_SEVERITY_TRACE;
    g_nvs_serial_value = ENUM_EZLOPI_TRACE_SEVERITY_TRACE;
    EZPI_core_read_set_log_severities();
    g_nvs_read_result = EZPI_ERR_NVS_READ_FAILED;

    ok(ENUM_EZLOPI_TRACE_SEVERITY_TRACE == EZPI_core_cloud_log_get_current_severity_enum_val(),
       "the threshold for these cases is TRACE");

    /* A severity the hub could actually emit. Must still broadcast, and the
    ** severity on the wire must be the name of the severity asked for. */
    g_queue_calls = 0;
    ok(EZPI_SUCCESS == EZPI_core_send_cloud_log(ENUM_EZLOPI_TRACE_SEVERITY_WARNING, "a warning"),
       "a WARNING at threshold TRACE is broadcast");
    ok(1 == g_queue_calls, "exactly one broadcast was queued");

    if (g_queued)
    {
        cJSON *cj_result = cJSON_GetObjectItem(__FUNCTION__, g_queued, "result");
        cJSON *cj_sev = cj_result ? cJSON_GetObjectItem(__FUNCTION__, cj_result, "severity") : NULL;

        ok((NULL != cj_sev) && (NULL != cj_sev->valuestring) && (0 == strcmp("WARNING", cj_sev->valuestring)),
           "the broadcast carries severity \"WARNING\"");
    }
    else
    {
        ok(0, "the broadcast carries severity \"WARNING\" (nothing was queued)");
    }

    __release_queued();

    /* Above the threshold. Refused before it reaches the index, which was
    ** already true and is asserted so the fix cannot quietly widen the gate. */
    g_queue_calls = 0;
    ok(EZPI_SUCCESS != EZPI_core_send_cloud_log(ENUM_EZLOPI_TRACE_SEVERITY_MAX, "exactly MAX"),
       "a severity of exactly MAX is refused, not used as an index");
    ok(0 == g_queue_calls, "nothing was queued for MAX");
    __release_queued();

    g_queue_calls = 0;
    ok(EZPI_SUCCESS != EZPI_core_send_cloud_log(9999, "far out of range"),
       "a severity far above MAX is refused");
    ok(0 == g_queue_calls, "nothing was queued for 9999");
    __release_queued();

    /* Below zero. An index of -1 reads before the array, which ASan reports as
    ** a global-buffer-overflow just as readily as reading past the end. */
    g_queue_calls = 0;
    ok(EZPI_SUCCESS != EZPI_core_send_cloud_log(-1, "negative"),
       "a negative severity is refused, not used as an index");
    ok(0 == g_queue_calls, "nothing was queued for -1");
    __release_queued();

    g_queue_calls = 0;
    ok(EZPI_SUCCESS != EZPI_core_send_cloud_log(-99999, "very negative"),
       "a very negative severity is refused");
    ok(0 == g_queue_calls, "nothing was queued for -99999");
    __release_queued();

    /* Not registered with the cloud: the gate the subject already had, kept
    ** here so a case above cannot pass merely because nothing broadcasts. */
    g_registered = false;
    g_queue_calls = 0;
    ok(EZPI_SUCCESS != EZPI_core_send_cloud_log(ENUM_EZLOPI_TRACE_SEVERITY_ERROR, "unregistered"),
       "nothing is broadcast before the hub is registered");
    ok(0 == g_queue_calls, "nothing was queued while unregistered");
    __release_queued();
    g_registered = true;
}

/**
 * @brief   The setters, unchanged. Behaviour-preservation half of the fix.
 */
static void test_the_setters_still_accept_and_reject_what_they_did(void)
{
    printf("\n-- the setters\n");

    /* Every valid id round-trips through the serial setter, which does not
    ** clamp. This is also the strongest statement that the index is safe for
    ** the whole range: six assignments, six in-range reads. */
    for (int id = ENUM_EZLOPI_TRACE_SEVERITY_NONE; id < ENUM_EZLOPI_TRACE_SEVERITY_MAX; id++)
    {
        g_nvs_serial_written = 0xFFFFFFFFu;

        ok(EZPI_SUCCESS == EZPI_core_serial_log_severity_process_id((e_trace_severity_t)id),
           "the serial setter accepts a valid severity id");
        ok(id == (int)EZPI_core_serial_log_get_current_severity_enum_val(),
           "the serial setter stores the id it was given");
        ok((uint32_t)id == g_nvs_serial_written,
           "the serial setter persists the id it was given");
        ok(__is_a_table_entry(EZPI_core_serial_log_get_current_severity_enum_str()),
           "the serial severity string is a table entry at every valid id");
    }

    /* MAX is not a valid id. It never was -- both process_id functions already
    ** guard with (MAX > id) -- which is the sharpest evidence that MAX was
    ** never meant to be a severity, and that the initialiser was a mistake
    ** rather than a convention. */
    e_trace_severity_t before = EZPI_core_serial_log_get_current_severity_enum_val();

    ok(EZPI_SUCCESS != EZPI_core_serial_log_severity_process_id(ENUM_EZLOPI_TRACE_SEVERITY_MAX),
       "the serial setter rejects MAX");
    ok(before == EZPI_core_serial_log_get_current_severity_enum_val(),
       "a rejected id leaves the serial severity alone");

    /* The cloud setter clamps everything above WARNING to WARNING. Preserved
    ** exactly, including for the values the fix now range-checks. */
    ok(EZPI_SUCCESS == EZPI_core_cloud_log_severity_process_id(ENUM_EZLOPI_TRACE_SEVERITY_ERROR),
       "the cloud setter accepts ERROR");
    ok(ENUM_EZLOPI_TRACE_SEVERITY_ERROR == EZPI_core_cloud_log_get_current_severity_enum_val(),
       "the cloud setter stores ERROR");

    ok(EZPI_SUCCESS == EZPI_core_cloud_log_severity_process_id(ENUM_EZLOPI_TRACE_SEVERITY_TRACE),
       "the cloud setter accepts TRACE");
    ok(ENUM_EZLOPI_TRACE_SEVERITY_WARNING == EZPI_core_cloud_log_get_current_severity_enum_val(),
       "the cloud setter clamps TRACE down to WARNING");

    ok(EZPI_SUCCESS != EZPI_core_cloud_log_severity_process_id(ENUM_EZLOPI_TRACE_SEVERITY_MAX),
       "the cloud setter rejects MAX");

    /* The string setters, which match on a prefix of the stored name. */
    ok(EZPI_SUCCESS == EZPI_core_serial_log_severity_process_str("INFO"),
       "the serial string setter accepts \"INFO\"");
    ok(ENUM_EZLOPI_TRACE_SEVERITY_INFO == EZPI_core_serial_log_get_current_severity_enum_val(),
       "\"INFO\" becomes ENUM_EZLOPI_TRACE_SEVERITY_INFO");

    /* An unknown name leaves the severity alone. Note what this does NOT do:
    ** EZPI_core_serial_log_severity_process_str returns EZPI_SUCCESS for any
    ** non-NULL string, because it ignores what the inner helper returned.
    ** Observed, not fixed -- it is a separate defect in a separate function
    ** and the assertion below is on the severity, which is what this file is
    ** about. */
    before = EZPI_core_serial_log_get_current_severity_enum_val();
    (void)EZPI_core_serial_log_severity_process_str("NOT_A_SEVERITY");
    ok(before == EZPI_core_serial_log_get_current_severity_enum_val(),
       "an unknown severity name leaves the serial severity alone");

    ok(EZPI_SUCCESS == EZPI_core_serial_log_severity_process_str(NULL),
       "a NULL severity name is accepted as \"turn it off\"");
    ok(ENUM_EZLOPI_TRACE_SEVERITY_NONE == EZPI_core_serial_log_get_current_severity_enum_val(),
       "a NULL severity name sets the serial severity to NONE");

    ok(EZPI_SUCCESS == EZPI_core_cloud_log_severity_process_str(false, "DEBUG"),
       "the cloud string setter accepts a disable");
    ok(ENUM_EZLOPI_TRACE_SEVERITY_NONE == EZPI_core_cloud_log_get_current_severity_enum_val(),
       "a disable sets the cloud severity to NONE regardless of the name given");
}

/**
 * @brief   EZPI_core_read_set_log_severities_internal(), the debug override.
 *
 * The function the two "remove this in release" lines call. It assigns its
 * argument to the static with no check, which is the last unguarded write path
 * to the index.
 */
static void test_the_internal_override(void)
{
    printf("\n-- EZPI_core_read_set_log_severities_internal()\n");

    /* What the firmware actually passes, at both call sites. */
    EZPI_core_read_set_log_severities_internal(ENUM_EZLOPI_TRACE_SEVERITY_TRACE);

    ok(ENUM_EZLOPI_TRACE_SEVERITY_TRACE == EZPI_core_serial_log_get_current_severity_enum_val(),
       "the override sets the serial severity to what it was given");
    ok(__is_a_table_entry(EZPI_core_serial_log_get_current_severity_enum_str()),
       "after the override the serial severity string is a table entry");

    /* MAX through the override. No caller does this today; the point is that
    ** the function cannot be made to leave an out-of-range index behind, so
    ** that a future caller is not the one that finds out. */
    EZPI_core_read_set_log_severities_internal(ENUM_EZLOPI_TRACE_SEVERITY_MAX);

    ok(ENUM_EZLOPI_TRACE_SEVERITY_MAX > EZPI_core_serial_log_get_current_severity_enum_val(),
       "the override refuses MAX rather than storing it");
    ok(__is_a_table_entry(EZPI_core_serial_log_get_current_severity_enum_str()),
       "after an MAX override the serial severity string is still a table entry");
}

/**
 * @brief   Which severities the serial upcall prints, through the real static.
 *
 * The behaviour-equivalence case for the MAX-to-TRACE change. With the serial
 * severity at TRACE, every real severity above NONE must print -- which is
 * what MAX allowed too, because nothing emits at severity MAX. If the default
 * had been lowered to, say, WARNING instead, this case is what would have
 * caught the silently quieter hub.
 */
static void test_the_serial_upcall_gate(void)
{
    printf("\n-- EZPI_core_serial_log_upcall(), reached through the real pointer\n");

    EZPI_core_set_log_upcalls();

    ok(NULL != g_serial_upcall, "the serial upcall was handed over");
    ok(NULL != g_cloud_upcall, "the cloud upcall was handed over");
    ok(g_cloud_upcall == EZPI_core_send_cloud_log,
       "the cloud upcall is EZPI_core_send_cloud_log");

    ok(EZPI_SUCCESS == EZPI_core_serial_log_severity_process_id(ENUM_EZLOPI_TRACE_SEVERITY_TRACE),
       "the serial severity is TRACE for these cases");

    long printed = -1;

    /* Every severity a caller could emit, at the most permissive threshold. */
    for (int sev = ENUM_EZLOPI_TRACE_SEVERITY_ERROR; sev <= ENUM_EZLOPI_TRACE_SEVERITY_TRACE; sev++)
    {
        ok(EZPI_SUCCESS == __serial_upcall(sev, "x", &printed),
           "at TRACE, a severity from ERROR..TRACE prints");
        ok(printed > 0, "and it actually wrote to stdout");
    }

    /* NONE is excluded by the upcall's own lower bound, and always was. */
    ok(EZPI_SUCCESS != __serial_upcall(ENUM_EZLOPI_TRACE_SEVERITY_NONE, "x", &printed),
       "a severity of NONE does not print");
    ok(0 == printed, "and nothing was written for it");

    /* Above the threshold. MAX is the value the old default would have
    ** admitted and the new one does not -- the single observable difference
    ** between MAX and TRACE as a default, asserted here so the claim that
    ** nothing emits at MAX is a statement about the tree and not about this
    ** function. */
    ok(EZPI_SUCCESS != __serial_upcall(ENUM_EZLOPI_TRACE_SEVERITY_MAX, "x", &printed),
       "a severity of MAX does not print at threshold TRACE");
    ok(0 == printed, "and nothing was written for MAX");

    ok(EZPI_SUCCESS != __serial_upcall(-1, "x", &printed),
       "a negative severity does not print");
    ok(0 == printed, "and nothing was written for it");

    /* Lower the threshold and confirm it still gates, so that the cases above
    ** are not all passing because the gate is open. */
    ok(EZPI_SUCCESS == EZPI_core_serial_log_severity_process_id(ENUM_EZLOPI_TRACE_SEVERITY_WARNING),
       "the serial severity can be lowered to WARNING");

    ok(EZPI_SUCCESS == __serial_upcall(ENUM_EZLOPI_TRACE_SEVERITY_ERROR, "x", &printed),
       "at WARNING, an ERROR still prints");
    ok(printed > 0, "and it wrote to stdout");

    ok(EZPI_SUCCESS != __serial_upcall(ENUM_EZLOPI_TRACE_SEVERITY_INFO, "x", &printed),
       "at WARNING, an INFO does not print");
    ok(0 == printed, "and nothing was written for it");

    /* Off entirely. */
    ok(EZPI_SUCCESS == EZPI_core_serial_log_severity_process_id(ENUM_EZLOPI_TRACE_SEVERITY_NONE),
       "the serial severity can be set to NONE");

    ok(EZPI_SUCCESS != __serial_upcall(ENUM_EZLOPI_TRACE_SEVERITY_ERROR, "x", &printed),
       "at NONE, not even an ERROR prints");
    ok(0 == printed, "and nothing was written for it");
}

/*******************************************************************************
 *                          main
 *******************************************************************************/

int main(void)
{
    /* Line buffered, for the reason test_cjsongpio.c gives: this rule builds
    ** with -fno-sanitize-recover=address, so an ASan finding ends the process
    ** at once, and a block-buffered stdout would lose every FAIL line printed
    ** before it. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== log severity as an array index "
           "(ENUM_EZLOPI_TRACE_SEVERITY_MAX = %d, table length = %d) ===\n",
           (int)ENUM_EZLOPI_TRACE_SEVERITY_MAX,
           (int)ENUM_EZLOPI_TRACE_SEVERITY_MAX);

    /* ORDER MATTERS, for exactly one pair. test_the_default_severities_are_valid_indices()
    ** measures a static initialiser, so it must run before any case that
    ** assigns a severity. test_the_table() is the only thing allowed in front
    ** of it and it calls no setter. Everything after is order-independent and
    ** sets what it needs. */
    test_the_table();
    test_the_default_severities_are_valid_indices();

    test_reading_nvs_leaves_a_valid_index();
    test_send_cloud_log_does_not_index_with_its_argument();
    test_the_setters_still_accept_and_reject_what_they_did();
    test_the_internal_override();
    test_the_serial_upcall_gate();

    printf("\n%d check(s), %d failure(s)\n", checks, failures);

    if (failures)
    {
        printf("LOG SEVERITY TESTS FAILED\n");
        return 1;
    }

    printf("LOG SEVERITY TESTS PASSED\n");
    return 0;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
