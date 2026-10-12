/* Host tests for ezlopi-core-actions.
 *
 * WHY THIS FILE EXISTS, AND WHAT IT IS WORTH
 *
 * This component was found by a sweep, not by reading: host_reachable.py asks
 * `gcc -fsyntax-only` which first-party .c files compile with nothing but
 * their own directory on the include path, and 27 of them did while having no
 * tests. ezlopi-core-actions is the only one of those 27 that contains any
 * executable logic at all. The rest are string tables, four files that are a
 * licence header and nothing else, three that are a single #include, and
 * three -- ezlopi_cloud_data.c, ezlopi_core_event_queue.c and
 * ezlopi_core_ethernet.c -- that are wrapped in `#if 0` from first line to
 * last. "Compiles with no stubs" turned out to select overwhelmingly for
 * emptiness.
 *
 * The one file with code in it had a defect, and it is the kind a test finds
 * and a reader does not:
 *
 *     EZLOPI_ACTION(HUB_UPDATE, "EZLOPI_ACTION_HUB_GET_ITEM")
 *
 * Every other entry in ezlopi_core_actions_list.h spells its own name, so
 * EZPI_core_actions_to_string() reported EZLOPI_ACTION_HUB_UPDATE as
 * EZLOPI_ACTION_HUB_GET_ITEM -- two distinct enum values, one string. In a log
 * that is a wrong answer that looks like a right one.
 *
 * Which is why the checks below do NOT compare against the string column of
 * that table. They rebuild the expected text from the macro's first argument
 * with the stringify operator, so the table is checked against the token it
 * is named after rather than against itself. A test that read `name` would
 * have agreed with the bug.
 *
 * SEVERITY: none, today. All three call sites of EZPI_core_actions_to_string()
 * in the tree are commented out, and this is not merely a grep result -- the
 * object file defines both EZPI_core_actions_to_string (T) and
 * ezlopi_action_names (D), and neither name appears anywhere among the 13802
 * symbols of the linked Ezlo_Pi_v4x.elf. -ffunction-sections with
 * --gc-sections discards them. The e_ezlopi_actions_t *enum*, by contrast, is
 * load-bearing: 63 driver files in ezlopi-sensors-devices reference one of its
 * enumerators.
 * So the enum is live, the stringifier is dead, and the bug was in the dead
 * half. It is fixed because it is unambiguously wrong and the fix is one line,
 * not because anything currently misbehaves.
 */
#include <stdio.h>
#include <string.h>

#include "ezlopi_core_actions.h"

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

/* Derived from the list itself, so it cannot drift from the enum. An entry
   added after MAX would change this and trip the ordering checks in main(). */
enum
{
    ACTION_COUNT = 0
#define EZLOPI_ACTION(action, name) +1
#include "ezlopi_core_actions_list.h"
#undef EZLOPI_ACTION
};

static const char *g_got[ACTION_COUNT];
static const char *g_want[ACTION_COUNT];
static const char *g_token[ACTION_COUNT];

/* The range guard in EZPI_core_actions_to_string() is `EZLOPI_ACTION_MAX >
   action` -- an upper bound and no lower bound. For a negative argument that
   would index ezlopi_action_names[] out of bounds. It is safe today, but not
   because the function checks: it is safe because every enumerator in the list
   is non-negative, so the compiler gives e_ezlopi_actions_t an unsigned type
   and -1 converts to a value far above MAX.

   That is a property of the type, so assert it at compile time. If a future
   list adds a negative enumerator the build stops here and points at the
   missing lower bound, instead of the out-of-bounds read quietly becoming
   real.

   Spelled `> 0` rather than `< 0` on purpose. The first draft of this check
   was a runtime `ok((e_ezlopi_actions_t)-1 < 0 ? ... )` and it did not
   compile: gcc's -Wtype-limits rejects an unsigned `< 0` comparison as always
   false, which under this suite's -Werror is a build failure. The diagnostic
   was itself the proof that the enum is unsigned. `> 0` is not an always-true
   comparison (zero fails it), so it survives -Werror while still being false
   for a signed type. C99 has no _Static_assert, hence the negative array
   size. */
typedef char assert_actions_enum_is_unsigned
    [((e_ezlopi_actions_t) - 1 > (e_ezlopi_actions_t)0) ? 1 : -1];

int main(void)
{
    printf("=== ezlopi-core-actions ===\n");

    /* 1. Every action stringifies to its own name.

       "EZLOPI_ACTION_" #action is built by the preprocessor from the token, so
       the only way for this to pass is for the table's text to agree with the
       identifier it defines. This is the check that fails on the HUB_UPDATE
       line, and it was run against the unfixed header to confirm that it does
       rather than assuming it would. */
#define EZLOPI_ACTION(action, name)                                 \
    g_token[EZLOPI_ACTION_##action] = #action;                      \
    g_want[EZLOPI_ACTION_##action] = "EZLOPI_ACTION_" #action;      \
    g_got[EZLOPI_ACTION_##action] =                                 \
        EZPI_core_actions_to_string(EZLOPI_ACTION_##action);
#include "ezlopi_core_actions_list.h"
#undef EZLOPI_ACTION

    printf("  %-28s %-34s %s\n", "token", "returned", "expected");
    for (int i = 0; i < ACTION_COUNT; i++)
    {
        printf("  %-28s %-34s %s\n", g_token[i],
               g_got[i] ? g_got[i] : "(NULL)", g_want[i]);
    }

    for (int i = 0; i < ACTION_COUNT; i++)
    {
        char what[160];

        if (EZLOPI_ACTION_MAX == (e_ezlopi_actions_t)i)
        {
            /* MAX is a count, not an action, and the guard in
               EZPI_core_actions_to_string() is `EZLOPI_ACTION_MAX > action`,
               so asking for it is out of range by one. Its row in the table is
               therefore unreachable by design. */
            snprintf(what, sizeof what,
                     "%s is rejected by the range guard (returns NULL)",
                     g_token[i]);
            ok(NULL == g_got[i], what);
            continue;
        }

        snprintf(what, sizeof what, "%s is non-NULL", g_token[i]);
        ok(NULL != g_got[i], what);

        snprintf(what, sizeof what, "%s stringifies to \"%s\", got \"%s\"",
                 g_token[i], g_want[i], g_got[i] ? g_got[i] : "(NULL)");
        ok(NULL != g_got[i] && 0 == strcmp(g_got[i], g_want[i]), what);
    }

    /* 2. No two actions share a string.

       Redundant with check 1 for the HUB_UPDATE case specifically, and kept
       anyway because it catches a different mistake: two entries whose text
       matches neither token but does match each other would slip past a
       per-entry comparison if someone ever "fixed" check 1 by relaxing it. */
    for (int i = 0; i < ACTION_COUNT; i++)
    {
        for (int j = i + 1; j < ACTION_COUNT; j++)
        {
            char what[200];

            if (NULL == g_got[i] || NULL == g_got[j])
            {
                continue;
            }

            snprintf(what, sizeof what,
                     "%s and %s do not share the string \"%s\"",
                     g_token[i], g_token[j], g_got[i]);
            ok(0 != strcmp(g_got[i], g_got[j]), what);
        }
    }

    /* 3. The list's shape, which the range guard silently depends on.

       ezlopi_action_names[] and e_ezlopi_actions_t are generated from the same
       X-macro, so they cannot desynchronise -- but their *ordering* can be
       broken by hand. An entry inserted before NONE shifts every value, and an
       entry added after MAX makes the guard reject a valid action while
       leaving a live row in the table past the end of the comparison. Neither
       is hypothetical: both are one careless line in a list header that
       nothing else validates. */
    ok(0 == (int)EZLOPI_ACTION_NONE, "EZLOPI_ACTION_NONE is still 0");
    ok((int)EZLOPI_ACTION_MAX == ACTION_COUNT - 1,
       "EZLOPI_ACTION_MAX is still the last entry in the list");
    ok(16 == ACTION_COUNT, "the list still has 16 entries");

    /* 4. Out-of-range input returns NULL rather than reading past the table. */
    {
        const e_ezlopi_actions_t over[] = {
            (e_ezlopi_actions_t)(ACTION_COUNT),
            (e_ezlopi_actions_t)(ACTION_COUNT + 1),
            (e_ezlopi_actions_t)100,
            (e_ezlopi_actions_t)65535,
        };

        for (size_t i = 0; i < sizeof over / sizeof over[0]; i++)
        {
            char what[120];
            snprintf(what, sizeof what, "action %lu is out of range -> NULL",
                     (unsigned long)over[i]);
            ok(NULL == EZPI_core_actions_to_string(over[i]), what);
        }
    }

    /* 5. The missing lower bound, which is covered by the compile-time
       assertion above. All that is left to check at runtime is that -1 really
       does come out the other side as NULL rather than as a read past the
       table -- ASan would report the latter. */
    ok(NULL == EZPI_core_actions_to_string((e_ezlopi_actions_t)-1),
       "(e_ezlopi_actions_t)-1 wraps above MAX and returns NULL");

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);

    /* Deliberately NOT tested here:

         - Whether any of this runs on a device. It does not. All three call
           sites are commented out and the linker drops both symbols; see the
           header comment. These tests cover a function that is currently
           compiled and discarded.

         - The enum's use by the 63 driver action handlers. Those switch on
           e_ezlopi_actions_t and never touch the string table, so they are
           unaffected by the defect fixed here and are not reachable from a
           host test anyway -- they pull in cJSON, the device/item core,
           FreeRTOS and GPIO.

         - ezlopi_action_names[] being writable. It is `char *[]`, not
           `const char *const[]`, so a caller could scribble over the table
           through the returned pointer. Tightening the type is a header change
           that ripples into the return type of
           EZPI_core_actions_to_string(), so it is left for its own commit
           rather than smuggled in with a one-line string fix. */
    if (g_fails)
    {
        printf("ACTIONS TESTS FAILED\n");
        return 1;
    }
    printf("ACTIONS TESTS PASSED\n");
    return 0;
}
