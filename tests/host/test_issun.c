/* Host tests for the isSunState when-condition: the flag arithmetic that
 * decides whether a sunrise/sunset scene fires, and the two field readers in
 * front of it.
 *
 * WHY THIS FILE EXISTS
 *
 * Until now the isSunState path had no test that could fail. The CHANGELOG's
 * *Known gaps* entry said so in those words, and said it of three functions:
 * ISSUNSTATE_get_offs_tmval, ISSUNSTATE_eval_range and
 * ISSUNSTATE_check_flag_result. They were all in
 * ezlopi_core_scenes_when_methods_helper_functions.c, which was recorded as
 * un-hostable. That turned out to be wrong, and cheaply so: with the include
 * set test_httpurl already uses for the sibling then-methods helper, the
 * preprocessor wants *no* absent header -- the entire cost was eight "case
 * label does not reduce to an integer constant" errors in
 * ISSUNSTATE_check_flag_result, where eight bit flags were `const uint8_t`
 * locals rather than an enum. A const-qualified object is not an integer
 * constant expression in C, so those labels were never valid C; the firmware
 * builds only because the xtensa GCC the IDF ships folds them as an extension.
 * Turning them into an enum cost nothing and made the file hostable.
 *
 * AND WHAT THAT IMMEDIATELY FOUND
 *
 * ISSUNSTATE_check_flag_result returned 0 for every input it could be given.
 * Not on an edge case -- always. It gates on MASK_TIME_FLAG (0x10), which
 * ISSUNSTATE_get_suntime sets for the sunrise/sunset field, the field that
 * makes a scene an isSunState scene at all; and not one of its eight case
 * labels includes that bit, so `switch (flag_check)` could only ever reach
 * `default`. No isSunState condition had ever fired, with or without weekdays,
 * month-days or a midnight range.
 *
 * Test 1 is the exhaustive statement of that: all 256 flag values against both
 * answers to "does the clock match the defined moment". Against the code as it
 * stood, 0 of those 512 return 1. It is the kind of claim worth making
 * exhaustively rather than by sampling, because the whole defect is a gap in
 * an enumeration -- a test that tried eight plausible combinations could have
 * missed it in exactly the way the eight case labels did.
 *
 * Run: make -C tests/host build/test_issun && tests/host/build/test_issun
 * ...or `make -C tests/host test` for the whole suite.
 */

/* sdkconfig.h first, as every firmware translation unit does: the scenes
 * headers are guarded by CONFIG_EZPI_SERV_ENABLE_MESHBOTS and expand to
 * nothing without it. */
#include "sdkconfig.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cjext.h"
#include "ezlopi_core_scenes_v2.h"
#include "ezlopi_core_scenes_when_methods_helper_functions.h"

static int checks = 0;
static int failures = 0;
#define CHECK(cond, what)                                         \
    do                                                            \
    {                                                             \
        checks++;                                                 \
        if (!(cond))                                              \
        {                                                         \
            failures++;                                           \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what);  \
        }                                                         \
    } while (0)

/* The eight flag bits, named as ISSUNSTATE_check_flag_result names them. Kept
 * here as literals rather than pulled from the subject: the subject's copy is
 * function-local, and a test that shared the definition could not catch a
 * change to it. */
enum
{
    TIME_FLAG = (1 << 0),          /* the clock is at the defined moment     */
    WEEKDAYS_FLAG = (1 << 1),      /* today is one of the chosen weekdays    */
    DAYS_FLAG = (1 << 2),          /* today is one of the chosen month-days  */
    MIDNIGHT_FLAG = (1 << 3),      /* the clock is inside the chosen range   */
    MASK_TIME_FLAG = (1 << 4),     /* a sunrise/sunset field is present      */
    MASK_WEEKDAYS_FLAG = (1 << 5), /* a weekdays field is present            */
    MASK_DAYS_FLAG = (1 << 6),     /* a month-days field is present          */
    MASK_MIDNIGHT_FLAG = (1 << 7), /* a range field is present               */
};

/* ------------------------------------------------------------------------- *
 * A scene node shaped the way the five ISSUNSTATE_* functions reach into it:
 * scene_node->when_block->fields->user_arg holds the s_sunstate_data_t.
 * ------------------------------------------------------------------------- */
typedef struct
{
    l_scenes_list_v2_t scene;
    l_when_block_v2_t when_block;
    l_fields_v2_t field;
    s_sunstate_data_t user_data;
} harness_t;

/* A harness whose user_data satisfies every part of the gate in
 * ISSUNSTATE_check_flag_result, so that the flag arithmetic behind it is what
 * a test is actually measuring. defined_moment is a time with no zero
 * components, so that "the clock matches" has to be arranged deliberately
 * rather than arriving by way of a zeroed struct -- the failure mode the
 * moment_valid field was added to stop. */
static void harness_init(harness_t *h)
{
    memset(h, 0, sizeof(*h));
    h->scene.when_block = &h->when_block;
    h->when_block.fields = &h->field;
    h->field.user_arg = &h->user_data;

    h->user_data.sunstate_mode = 1; /* sunrise */
    h->user_data.curr_tm_day = 14;
    h->user_data.moment_valid = true;
    h->user_data.defined_moment.tm_hour = 6;
    h->user_data.defined_moment.tm_min = 31;
    h->user_data.defined_moment.tm_sec = 12;
}

static struct tm at_the_moment(void)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_hour = 6;
    t.tm_min = 31;
    t.tm_sec = 12;
    t.tm_mday = 14;
    return t;
}

static struct tm not_at_the_moment(void)
{
    struct tm t = at_the_moment();
    t.tm_sec = 13;
    return t;
}

/* ------------------------------------------------------------------------- *
 * 1. Exhaustive: which (flag_check, clock) pairs make a scene fire.
 *
 * The sixteen below are derived from the eight case labels by hand, not read
 * off the implementation, which is the point of writing them out. For a label
 * value L the function fires when `(flag_check | maybe TIME_FLAG) & ~0x10`
 * equals L, so each label admits one input if the clock does not matter to it
 * and three if it does -- the caller may or may not have pre-set TIME_FLAG,
 * and the clock may set it.
 * ------------------------------------------------------------------------- */
typedef struct
{
    unsigned flag_check;
    int clock_matches;
    const char *why;
} firing_t;

static const firing_t k_should_fire[] = {
    /* switch 1: no month-days and no weekdays field present */
    {0x98, 0, "sunrise + range, clock inside the range"},
    {0x10, 1, "sunrise only, clock exactly at the defined moment"},
    {0x11, 0, "sunrise only, TIME_FLAG pre-set by the caller"},
    {0x11, 1, "sunrise only, TIME_FLAG pre-set and the clock agrees"},
    /* switch 2: at least one of month-days or weekdays present */
    {0xfe, 0, "month-days + weekdays + range, all three matched"},
    {0x76, 1, "month-days + weekdays matched, clock at the moment"},
    {0x77, 0, "as above with TIME_FLAG pre-set"},
    {0x77, 1, "as above, pre-set and the clock agrees"},
    {0xdc, 0, "month-days + range, both matched"},
    {0x54, 1, "month-days matched, clock at the moment"},
    {0x55, 0, "as above with TIME_FLAG pre-set"},
    {0x55, 1, "as above, pre-set and the clock agrees"},
    {0xba, 0, "weekdays + range, both matched"},
    {0x32, 1, "weekdays matched, clock at the moment"},
    {0x33, 0, "as above with TIME_FLAG pre-set"},
    {0x33, 1, "as above, pre-set and the clock agrees"},
};

static void test_the_whole_flag_space(void)
{
    const int expected_n = (int)(sizeof(k_should_fire) / sizeof(k_should_fire[0]));
    int fired = 0;
    int mismatches = 0;
    char what[192];

    for (int clock = 0; clock <= 1; clock++)
    {
        struct tm now = clock ? at_the_moment() : not_at_the_moment();
        for (unsigned v = 0; v <= 255u; v++)
        {
            harness_t h;
            harness_init(&h);

            int got = ISSUNSTATE_check_flag_result(&h.scene, &now, (uint8_t)v);

            int want = 0;
            for (int i = 0; i < expected_n; i++)
            {
                if (k_should_fire[i].flag_check == v && k_should_fire[i].clock_matches == clock)
                {
                    want = 1;
                    break;
                }
            }
            if (got)
            {
                fired++;
            }
            if (got != want)
            {
                mismatches++;
                if (mismatches <= 8)
                {
                    printf("  flag_check=0x%02x clock_matches=%d: got %d, expected %d\n",
                           v, clock, got, want);
                }
            }
        }
    }

    snprintf(what, sizeof(what),
             "across all 512 (flag_check, clock) pairs the firing set is exactly the "
             "16 derived from the case labels; %d pair(s) disagreed", mismatches);
    CHECK(0 == mismatches, what);

    snprintf(what, sizeof(what),
             "and %d of 512 pairs fire, not 0 -- 0 is what this function returned "
             "before MASK_TIME_FLAG was cleared, for every input", fired);
    CHECK(expected_n == fired, what);
}

/* 2. The eight the dispatcher can actually produce.
 *
 * Eight of the sixteen above need the caller to have pre-set TIME_FLAG, and no
 * field function returns it -- ISSUNSTATE_check_flag_result sets it itself.
 * Restricting to inputs the dispatch loop in ezlopi_core_scenes_when_methods.c
 * can build leaves exactly eight, one for each case label. That one-to-one
 * correspondence is the evidence that clearing the mask bit restores the
 * design the labels were written for, rather than inventing a new one. */
static void test_the_eight_reachable_combinations(void)
{
    /* Bits a field function can contribute: MASK_* always with its match bit
     * only when the condition held, and never TIME_FLAG. */
    static const struct
    {
        unsigned flag_check;
        int clock_matches;
        const char *label;
    } reachable[] = {
        {MASK_TIME_FLAG | MASK_MIDNIGHT_FLAG | MIDNIGHT_FLAG, 0, "daily + midnight range"},
        {MASK_TIME_FLAG, 1, "daily + exact time"},
        {MASK_TIME_FLAG | MASK_DAYS_FLAG | DAYS_FLAG | MASK_WEEKDAYS_FLAG | WEEKDAYS_FLAG | MASK_MIDNIGHT_FLAG | MIDNIGHT_FLAG, 0, "mdays + weekday + midnight"},
        {MASK_TIME_FLAG | MASK_DAYS_FLAG | DAYS_FLAG | MASK_WEEKDAYS_FLAG | WEEKDAYS_FLAG, 1, "mdays + weekday + exact"},
        {MASK_TIME_FLAG | MASK_DAYS_FLAG | DAYS_FLAG | MASK_MIDNIGHT_FLAG | MIDNIGHT_FLAG, 0, "mdays + midnight"},
        {MASK_TIME_FLAG | MASK_DAYS_FLAG | DAYS_FLAG, 1, "mdays + exact"},
        {MASK_TIME_FLAG | MASK_WEEKDAYS_FLAG | WEEKDAYS_FLAG | MASK_MIDNIGHT_FLAG | MIDNIGHT_FLAG, 0, "weekday + midnight"},
        {MASK_TIME_FLAG | MASK_WEEKDAYS_FLAG | WEEKDAYS_FLAG, 1, "weekday + exact"},
    };
    const int n = (int)(sizeof(reachable) / sizeof(reachable[0]));
    char what[192];

    for (int i = 0; i < n; i++)
    {
        harness_t h;
        harness_init(&h);
        struct tm now = reachable[i].clock_matches ? at_the_moment() : not_at_the_moment();

        int got = ISSUNSTATE_check_flag_result(&h.scene, &now, (uint8_t)reachable[i].flag_check);

        snprintf(what, sizeof(what), "%s (flag_check=0x%02x) fires",
                 reachable[i].label, reachable[i].flag_check);
        CHECK(1 == got, what);
    }

    snprintf(what, sizeof(what),
             "the eight reachable firing combinations are one per case label, and "
             "there are %d labels", n);
    CHECK(8 == n, what);
}

/* 3. The gate in front of the arithmetic. Each of its three conditions on
 * user_data has to be independently sufficient to keep a scene from firing,
 * because each stands for a different kind of "we do not know the suntime
 * yet" and none of them is implied by the others. */
static void test_each_gate_condition_alone_blocks_firing(void)
{
    const unsigned fires = MASK_TIME_FLAG | MASK_MIDNIGHT_FLAG | MIDNIGHT_FLAG;
    struct tm now = not_at_the_moment();

    {
        harness_t h;
        harness_init(&h);
        CHECK(1 == ISSUNSTATE_check_flag_result(&h.scene, &now, (uint8_t)fires),
              "control: with the gate satisfied this input does fire");
    }
    {
        harness_t h;
        harness_init(&h);
        h.user_data.curr_tm_day = 0;
        CHECK(0 == ISSUNSTATE_check_flag_result(&h.scene, &now, (uint8_t)fires),
              "curr_tm_day == 0 means no suntime has been read, so nothing fires");
    }
    {
        harness_t h;
        harness_init(&h);
        h.user_data.sunstate_mode = 0;
        CHECK(0 == ISSUNSTATE_check_flag_result(&h.scene, &now, (uint8_t)fires),
              "sunstate_mode == 0 means neither sunrise nor sunset was chosen");
    }
    {
        harness_t h;
        harness_init(&h);
        h.user_data.moment_valid = false;
        CHECK(0 == ISSUNSTATE_check_flag_result(&h.scene, &now, (uint8_t)fires),
              "moment_valid == false means the offset never resolved, so the "
              "all-zero defined_moment must not be read as midnight");
    }
    {
        harness_t h;
        memset(&h, 0, sizeof(h));
        h.scene.when_block = &h.when_block;
        h.when_block.fields = &h.field;
        h.field.user_arg = NULL;
        CHECK(0 == ISSUNSTATE_check_flag_result(&h.scene, &now, (uint8_t)fires),
              "a NULL user_arg -- a scene evaluated before get_suntime ran -- "
              "returns 0 rather than dereferencing it");
    }
}

/* 4. ISSUNSTATE_eval_range reads a union, and which member is live is said by
 * field_value.e_type, not by the declared schema type value_type. This is the
 * guard added in the previous change; until this file existed there was no way
 * to fail it. A TOKEN field holding a bool has value_string reading back as
 * 0x1 -- non-NULL -- so the `!= NULL` test that used to be the whole guard
 * passed it straight to strlen(). Under ASan the unguarded version dies here
 * rather than returning a wrong answer. */
static void test_eval_range_reads_the_discriminant_not_the_schema_type(void)
{
    char range_at[] = "at";

    {
        harness_t h;
        harness_init(&h);
        h.field.value_type = EZLOPI_VALUE_TYPE_TOKEN;
        h.field.field_value.e_type = VALUE_TYPE_STRING;
        h.field.field_value.u_value.value_string = range_at;
        struct tm now = at_the_moment();

        uint8_t got = ISSUNSTATE_eval_range(&h.scene, &h.field, &now, 1);
        CHECK((MASK_MIDNIGHT_FLAG | MIDNIGHT_FLAG) == got,
              "a real \"at\" token at the defined moment sets both the "
              "range-present and range-matched bits");
    }
    {
        harness_t h;
        harness_init(&h);
        h.field.value_type = EZLOPI_VALUE_TYPE_TOKEN;
        h.field.field_value.e_type = VALUE_TYPE_BOOL;
        h.field.field_value.u_value.value_bool = true;
        struct tm now = at_the_moment();

        CHECK(NULL != h.field.field_value.u_value.value_string,
              "a stored `true` reads back as a non-NULL value_string, which is "
              "why a NULL test could not catch this");

        uint8_t got = ISSUNSTATE_eval_range(&h.scene, &h.field, &now, 1);
        CHECK(0 == got,
              "a TOKEN field holding a bool contributes no flags -- the "
              "unguarded version called strlen() on address 0x1 instead");
    }
    {
        harness_t h;
        harness_init(&h);
        h.field.value_type = EZLOPI_VALUE_TYPE_TOKEN;
        h.field.field_value.e_type = VALUE_TYPE_NUMBER;
        h.field.field_value.u_value.value_double = 1.0;
        struct tm now = at_the_moment();

        CHECK(NULL != h.field.field_value.u_value.value_string,
              "a stored number reads back as a non-NULL value_string too");

        uint8_t got = ISSUNSTATE_eval_range(&h.scene, &h.field, &now, 1);
        CHECK(0 == got, "and a TOKEN holding a number contributes no flags either");
    }
    {
        harness_t h;
        harness_init(&h);
        h.field.value_type = EZLOPI_VALUE_TYPE_TOKEN;
        h.field.field_value.e_type = VALUE_TYPE_STRING;
        h.field.field_value.u_value.value_string = range_at;
        h.user_data.moment_valid = false;
        struct tm now = at_the_moment();

        uint8_t got = ISSUNSTATE_eval_range(&h.scene, &h.field, &now, 1);
        CHECK(MASK_MIDNIGHT_FLAG == got,
              "with no valid moment the range is still present but cannot have "
              "matched, so the all-zero moment is not compared against");
    }
}

/* 5. ISSUNSTATE_check_mdrn, the three range words. These are the comparisons
 * the flag above stands for, and "at" is the only one of the three that the
 * exact-moment case shares with TIME_FLAG. */
static void test_check_mdrn_range_words(void)
{
    struct tm moment;
    memset(&moment, 0, sizeof(moment));
    moment.tm_hour = 6;
    moment.tm_min = 31;
    moment.tm_sec = 12;

    char at[] = "at";
    char before[] = "before";
    char after[] = "after";

    struct tm now = at_the_moment();
    CHECK(MIDNIGHT_FLAG == ISSUNSTATE_check_mdrn(1, at, &now, &moment),
          "\"at\" matches the defined moment to the second");

    now.tm_sec = 11;
    CHECK(0 == ISSUNSTATE_check_mdrn(1, at, &now, &moment),
          "\"at\" does not match one second early");

    now.tm_hour = 5;
    CHECK(MIDNIGHT_FLAG == ISSUNSTATE_check_mdrn(1, before, &now, &moment),
          "\"before\" matches an hour earlier");

    now.tm_hour = 7;
    CHECK(MIDNIGHT_FLAG == ISSUNSTATE_check_mdrn(1, after, &now, &moment),
          "\"after\" matches an hour later");

    now.tm_hour = 5;
    CHECK(0 == ISSUNSTATE_check_mdrn(1, after, &now, &moment),
          "\"after\" does not match an hour earlier");

    now = at_the_moment();
    CHECK(0 == ISSUNSTATE_check_mdrn(0, at, &now, &moment),
          "sunstate_mode 0 means no sun time was chosen, so no range matches");
    CHECK(0 == ISSUNSTATE_check_mdrn(1, NULL, &now, &moment),
          "a NULL range word matches nothing rather than being dereferenced");
}

int main(void)
{
    test_the_whole_flag_space();
    test_the_eight_reachable_combinations();
    test_each_gate_condition_alone_blocks_firing();
    test_eval_range_reads_the_discriminant_not_the_schema_type();
    test_check_mdrn_range_words();

    /* Bare, with no "test_issun: " prefix, and that is not cosmetic: "N
     * check(s), M failure(s)" is the shape the suite's aggregate tally reads,
     * and a prefixed line matches nothing. This file printed a prefixed one
     * first and its 29 checks summed into the aggregate as 0 -- the same
     * mistake test_httpurl.c records having made, in the comment above its own
     * counter. Worth repeating here because the aggregate is quoted in the
     * CHANGELOG, so a line that matches nothing understates the suite. */
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return (0 == failures) ? 0 : 1;
}
