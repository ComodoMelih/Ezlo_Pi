/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    test_devcfgval.c
 * @brief   Host tests for the device-configuration check.
 *
 * WHAT THIS IS FOR
 *
 * Two paths in the shipped firmware write a device configuration to flash --
 * the BLE dynamic-config characteristic and the UART command channel -- and
 * neither checks anything about what it was handed beyond "cJSON parsed it".
 * So `{"hello":"world"}` is stored. Nothing complains until the next boot, where
 * the hub comes up with no devices at all and no indication that the
 * configuration sent some minutes earlier is the reason.
 *
 * test_the_shapes_stored_today() is therefore the test that matters most: it
 * takes the shapes those two paths accept right now and asserts each is
 * refused, with a reason that names what is wrong.
 *
 * WHY A HOST TEST AT ALL
 *
 * There is no dry run on hardware. The function that consumes a configuration,
 * ezlopi_device_parse_json_v3() in ezlopi_core_devices.c, is static and
 * *instantiates devices as it walks* -- it calls each driver's
 * EZLOPI_ACTION_PREPARE. The only way to ask a board whether a configuration is
 * acceptable is to store it and reboot, and the answer arrives as an absence:
 * a device that is not there. Here a few hundred shapes go through the real
 * decision in a few milliseconds.
 *
 * THE OTHER HALF, WHICH IS EASY TO FORGET
 *
 * The Ezlo app writes these configurations and must keep working. Every check
 * added to this path is a chance to refuse something that works, and that
 * failure mode is worse than the one being fixed, because it breaks a hub that
 * was fine. test_unknown_members_are_accepted() and
 * test_member_lookup_matches_the_parser() exist for that, and the second one is
 * the subtle one: cJSON_GetObjectItem is case insensitive, and the boot parser
 * uses it, so a configuration with "ChipSet" works on a board today and must
 * not be refused here.
 *
 * ON "NO ALLOCATION"
 *
 * The header promises no side effects and no allocation. The side-effect half
 * is asserted directly, in test_the_tree_is_not_modified(): the tree is
 * serialised before and after and the two must be identical. The allocation
 * half is not asserted -- it is covered by LeakSanitizer, which these flags
 * enable, since the contract gives the caller nothing to free and so anything
 * allocated and kept would show up as a leak. An allocation that is also freed
 * would pass; that is the limit of this file on that point.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjext.h"
#include "ezlopi_util_devcfgval.h"

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
 * @brief   Parse a literal from this file, or give up.
 *
 * A typo in a test's own JSON parses to NULL, which the subject reports as
 * EZPI_DEVCFGVAL_NOT_OBJECT -- so a broken test case would quietly turn into a
 * passing one. Exiting 2 keeps "this file is wrong" distinct from "the subject
 * is wrong", the same split tests/ci/check_finfo_bounded_store.py uses.
 */
static cJSON *must_parse(const char *json)
{
    cJSON *root = cJSON_Parse(__FUNCTION__, json);

    if (NULL == root)
    {
        printf("  FATAL: this test file's own JSON does not parse: %s\n", json);
        exit(2);
    }

    return root;
}

/*******************************************************************************
 *                          The cloud keyword strings
 *
 * The real definitions are in ezlopi_cloud_keywords.c, which also defines
 * several hundred others. Defining the four the subject uses is cheaper than
 * linking that file, and the spellings are not invented here -- they are the
 * values at ezlopi_cloud_keywords.c lines 223, 262, 263 and 480. If one of
 * them were changed upstream this file would still link, which is why the JSON
 * below is written out as text with the literal member names rather than built
 * from these variables.
 *******************************************************************************/
const char *ezlopi_device_id_str = "device_id";
const char *ezlopi_dev_detail_str = "dev_detail";
const char *ezlopi_id_item_str = "id_item";
const char *ezlopi_chipset_str = "chipset";

/*******************************************************************************
 *                          Shorthands
 *******************************************************************************/

/** @brief The chipset name the esp32 branch of ezlopi_core_devices.c uses. */
#define THIS_CHIP "ESP32"

/** @brief One well formed device entry, as the Ezlo app writes them. */
#define GOOD_DEVICE                                     \
    "{\"device_id\":\"ZZZZZZZZ\",\"id_item\":1001,"      \
    "\"dev_name\":\"Living room\",\"dev_type\":1,"       \
    "\"gpio\":12,\"val_ip\":1,\"logic_inv\":0}"

/** @brief A configuration that must be accepted, for contrast with the rest. */
#define GOOD_CONFIG \
    "{\"chipset\":\"ESP32\",\"dev_detail\":[" GOOD_DEVICE "]}"

/** @brief Wrap one or more device entries in a configuration. */
#define CONFIG_WITH(entries) \
    "{\"chipset\":\"ESP32\",\"dev_detail\":[" entries "]}"

/**
 * @brief   Run the real check over a JSON literal.
 *
 * Size checking is switched off here (0, 0); test_the_size_check() drives that
 * on its own.
 */
static e_ezpi_devcfgval_t judge(const char *json, const char *expected_chipset,
                                int *bad_index)
{
    cJSON *root = must_parse(json);
    e_ezpi_devcfgval_t reason = EZPI_util_devcfgval_check(root,
                                                          expected_chipset,
                                                          0u, 0u, bad_index);
    cJSON_Delete(__FUNCTION__, root);

    return reason;
}

/** @brief judge() for the common case: this chip, no interest in the index. */
static e_ezpi_devcfgval_t judge_here(const char *json)
{
    return judge(json, THIS_CHIP, NULL);
}

/*******************************************************************************
 *                          A table of every reason
 *
 * Driven by three tests: the per-case expectations, the coverage count in
 * test_every_reason_is_reachable(), and the index convention in
 * test_the_offending_entry_is_named(). Keeping them one table means a reason
 * cannot be added to the enum and then tested nowhere.
 *******************************************************************************/

typedef struct s_case
{
    const char *json;             /**< the configuration */
    e_ezpi_devcfgval_t want;      /**< the reason it must produce */
    int want_index;               /**< the bad_index it must report */
    const char *what;             /**< for the failure message */
} s_case_t;

static const s_case_t cases[] = {
    /* Not an object. Every one of these is valid JSON, so every one of these
     * is written to flash by the BLE and UART paths today. */
    {"{\"hello\":\"world\"}", EZPI_DEVCFGVAL_CHIPSET_MISSING, -1,
     "the canonical junk payload"},
    {"{}", EZPI_DEVCFGVAL_CHIPSET_MISSING, -1, "an empty object"},
    {"[]", EZPI_DEVCFGVAL_NOT_OBJECT, -1, "an empty array"},
    {"[1,2,3]", EZPI_DEVCFGVAL_NOT_OBJECT, -1, "an array of numbers"},
    {"\"ESP32\"", EZPI_DEVCFGVAL_NOT_OBJECT, -1, "a bare string"},
    {"42", EZPI_DEVCFGVAL_NOT_OBJECT, -1, "a bare number"},
    {"true", EZPI_DEVCFGVAL_NOT_OBJECT, -1, "a bare true"},
    {"null", EZPI_DEVCFGVAL_NOT_OBJECT, -1, "a bare null"},

    /* chipset */
    {"{\"dev_detail\":[]}", EZPI_DEVCFGVAL_CHIPSET_MISSING, -1,
     "a device list and no chipset"},
    {"{\"chipset\":32,\"dev_detail\":[]}",
     EZPI_DEVCFGVAL_CHIPSET_NOT_STRING, -1, "a numeric chipset"},
    {"{\"chipset\":null,\"dev_detail\":[]}",
     EZPI_DEVCFGVAL_CHIPSET_NOT_STRING, -1, "a null chipset"},
    {"{\"chipset\":true,\"dev_detail\":[]}",
     EZPI_DEVCFGVAL_CHIPSET_NOT_STRING, -1, "a boolean chipset"},
    {"{\"chipset\":[\"ESP32\"],\"dev_detail\":[]}",
     EZPI_DEVCFGVAL_CHIPSET_NOT_STRING, -1, "a chipset in an array"},
    {"{\"chipset\":{\"name\":\"ESP32\"},\"dev_detail\":[]}",
     EZPI_DEVCFGVAL_CHIPSET_NOT_STRING, -1, "a chipset in an object"},
    {"{\"chipset\":\"ESP32S3\",\"dev_detail\":[]}",
     EZPI_DEVCFGVAL_CHIPSET_MISMATCH, -1, "a configuration for an S3"},
    {"{\"chipset\":\"ESP32C3\",\"dev_detail\":[]}",
     EZPI_DEVCFGVAL_CHIPSET_MISMATCH, -1, "a configuration for a C3"},
    {"{\"chipset\":\"ESP3\",\"dev_detail\":[]}",
     EZPI_DEVCFGVAL_CHIPSET_MISMATCH, -1, "a prefix of this chip's name"},
    {"{\"chipset\":\"\",\"dev_detail\":[]}",
     EZPI_DEVCFGVAL_CHIPSET_MISMATCH, -1, "an empty chipset"},

    /* dev_detail */
    {"{\"chipset\":\"ESP32\"}", EZPI_DEVCFGVAL_DEVLIST_MISSING, -1,
     "a chipset and no device list"},
    /* The parser walks this with cJSON_ArrayForEach, which is a walk of
     * ->child, so it would iterate the *members* of this object. */
    {"{\"chipset\":\"ESP32\",\"dev_detail\":{\"0\":" GOOD_DEVICE "}}",
     EZPI_DEVCFGVAL_DEVLIST_NOT_ARRAY, -1, "a device list as an object"},
    {"{\"chipset\":\"ESP32\",\"dev_detail\":\"none\"}",
     EZPI_DEVCFGVAL_DEVLIST_NOT_ARRAY, -1, "a device list as a string"},
    {"{\"chipset\":\"ESP32\",\"dev_detail\":0}",
     EZPI_DEVCFGVAL_DEVLIST_NOT_ARRAY, -1, "a device list as a number"},
    {"{\"chipset\":\"ESP32\",\"dev_detail\":null}",
     EZPI_DEVCFGVAL_DEVLIST_NOT_ARRAY, -1, "a device list as null"},

    /* Device entries. */
    {CONFIG_WITH("1"), EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT, 0,
     "a number where an entry should be"},
    {CONFIG_WITH("\"sensor\""), EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT, 0,
     "a string where an entry should be"},
    {CONFIG_WITH("[]"), EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT, 0,
     "an array where an entry should be"},
    {CONFIG_WITH("null"), EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT, 0,
     "a null where an entry should be"},
    {CONFIG_WITH(GOOD_DEVICE ",null"), EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT, 1,
     "a null after a good entry"},

    {CONFIG_WITH("{\"id_item\":1001}"), EZPI_DEVCFGVAL_DEVICE_ID_BAD, 0,
     "an entry with no device_id"},
    {CONFIG_WITH("{\"device_id\":7,\"id_item\":1001}"),
     EZPI_DEVCFGVAL_DEVICE_ID_BAD, 0, "a numeric device_id"},
    {CONFIG_WITH("{\"device_id\":null,\"id_item\":1001}"),
     EZPI_DEVCFGVAL_DEVICE_ID_BAD, 0, "a null device_id"},
    {CONFIG_WITH("{\"device_id\":[\"a\"],\"id_item\":1001}"),
     EZPI_DEVCFGVAL_DEVICE_ID_BAD, 0, "a device_id in an array"},

    /* The quiet failure: the parser skips these entries in silence. */
    {CONFIG_WITH("{\"device_id\":\"ZZZZZZZZ\"}"),
     EZPI_DEVCFGVAL_ITEM_ID_BAD, 0, "an entry with no id_item"},
    {CONFIG_WITH("{\"device_id\":\"ZZZZZZZZ\",\"id_item\":0}"),
     EZPI_DEVCFGVAL_ITEM_ID_BAD, 0, "an id_item of zero"},
    {CONFIG_WITH("{\"device_id\":\"ZZZZZZZZ\",\"id_item\":\"1001\"}"),
     EZPI_DEVCFGVAL_ITEM_ID_BAD, 0, "an id_item as a string"},
    {CONFIG_WITH("{\"device_id\":\"ZZZZZZZZ\",\"id_item\":null}"),
     EZPI_DEVCFGVAL_ITEM_ID_BAD, 0, "a null id_item"},
    /* 0.4 is a non-zero number that the parser's (int) cast turns into zero,
     * so the entry builds nothing. Comparing valuedouble against 0.0 here
     * would accept it. */
    {CONFIG_WITH("{\"device_id\":\"ZZZZZZZZ\",\"id_item\":0.4}"),
     EZPI_DEVCFGVAL_ITEM_ID_BAD, 0, "an id_item that truncates to zero"},

    /* And the accepted ones, in the same table so that a subject which always
     * returned a reason would fail here rather than look thorough. */
    {GOOD_CONFIG, EZPI_DEVCFGVAL_OK, -1, "one well formed device"},
    {CONFIG_WITH(GOOD_DEVICE "," GOOD_DEVICE "," GOOD_DEVICE),
     EZPI_DEVCFGVAL_OK, -1, "three well formed devices"},
    {"{\"chipset\":\"ESP32\",\"dev_detail\":[]}", EZPI_DEVCFGVAL_OK, -1,
     "no devices at all, which is how a user starts over"},
    /* Non-zero but names no driver in this image. That is a real way to get a
     * missing device, and it is deliberately not this function's business --
     * see WHAT IT DOES NOT CHECK in the header. */
    {CONFIG_WITH("{\"device_id\":\"ZZZZZZZZ\",\"id_item\":999999}"),
     EZPI_DEVCFGVAL_OK, -1, "an id_item no driver claims"},
    {CONFIG_WITH("{\"device_id\":\"ZZZZZZZZ\",\"id_item\":-1001}"),
     EZPI_DEVCFGVAL_OK, -1, "a negative id_item, which the parser accepts"},
    {CONFIG_WITH("{\"device_id\":\"ZZZZZZZZ\",\"id_item\":1.9}"),
     EZPI_DEVCFGVAL_OK, -1, "an id_item of 1.9, which truncates to 1"},
    {CONFIG_WITH("{\"device_id\":\"\",\"id_item\":1001}"),
     EZPI_DEVCFGVAL_OK, -1, "an empty device_id, which the parser accepts"},
};

#define CASE_COUNT ((int)(sizeof(cases) / sizeof(cases[0])))

/*******************************************************************************
 *                          Tests
 *******************************************************************************/

/**
 * @brief   Every case in the table, reason and index.
 */
static void test_the_table(void)
{
    int i;

    printf("-- every shape in the table --\n");

    for (i = 0; i < CASE_COUNT; i++)
    {
        int bad_index = 12345;
        e_ezpi_devcfgval_t got = judge(cases[i].json, THIS_CHIP, &bad_index);

        if (got != cases[i].want)
        {
            printf("    (%s: wanted %s, got %s)\n", cases[i].what,
                   EZPI_util_devcfgval_token(cases[i].want),
                   EZPI_util_devcfgval_token(got));
        }

        ok(got == cases[i].want, cases[i].what);
        ok(bad_index == cases[i].want_index, cases[i].what);
    }
}

/**
 * @brief   The shapes the shipped firmware stores without comment.
 *
 * Separate from the table even though the table covers them, because this is
 * the claim the whole component exists to make and it should be findable.
 */
static void test_the_shapes_stored_today(void)
{
    printf("-- what BLE and UART accept today --\n");

    /* ezlopi_service_ble_dynamic_config.c:243 and ezlopi_service_uart.c:1397
     * both check only that cJSON_Parse returned non-NULL before calling
     * EZPI_core_factory_info_v3_set_ezlopi_config. */
    ok(EZPI_DEVCFGVAL_OK != judge_here("{\"hello\":\"world\"}"),
       "{\"hello\":\"world\"} is refused");
    ok(EZPI_DEVCFGVAL_OK != judge_here("{}"), "{} is refused");
    ok(EZPI_DEVCFGVAL_OK != judge_here("[]"), "[] is refused");
    ok(EZPI_DEVCFGVAL_OK != judge_here("0"), "0 is refused");

    /* A null tree has no reason of its own; it lands on NOT_OBJECT. The real
     * callers cannot pass one, since they parse first, but a handler that
     * forgets to check cJSON_Parse's return must not get an OK. */
    ok(EZPI_DEVCFGVAL_NOT_OBJECT ==
           EZPI_util_devcfgval_check(NULL, THIS_CHIP, 0u, 0u, NULL),
       "a null tree is refused");
}

/**
 * @brief   A configuration this build cannot use must not be adopted.
 *
 * The pin numbers in an S3 configuration were chosen for an S3. Honouring it
 * here would instantiate devices on whatever GPIOs those numbers name on this
 * part, which is worse than refusing it.
 */
static void test_a_foreign_configuration_is_refused(void)
{
    printf("-- a configuration for another chip --\n");

    ok(EZPI_DEVCFGVAL_CHIPSET_MISMATCH ==
           judge("{\"chipset\":\"ESP32S3\",\"dev_detail\":[" GOOD_DEVICE "]}",
                 "ESP32", NULL),
       "an S3 configuration is refused on an ESP32");
    ok(EZPI_DEVCFGVAL_CHIPSET_MISMATCH ==
           judge("{\"chipset\":\"ESP32\",\"dev_detail\":[" GOOD_DEVICE "]}",
                 "ESP32S3", NULL),
       "an ESP32 configuration is refused on an S3");
    ok(EZPI_DEVCFGVAL_OK ==
           judge("{\"chipset\":\"ESP32S3\",\"dev_detail\":[" GOOD_DEVICE "]}",
                 "ESP32S3", NULL),
       "and accepted on the chip it names");

    /* The boot parser compares over max(strlen(expected), str_value_len)
     * bytes precisely so that ESP32 does not match ESP32S3. The two must agree
     * on that, in both directions. */
    ok(EZPI_DEVCFGVAL_CHIPSET_MISMATCH ==
           judge("{\"chipset\":\"ESP32S3XX\",\"dev_detail\":[]}", "ESP32S3",
                 NULL),
       "a longer name is not a match for its prefix");
    ok(EZPI_DEVCFGVAL_CHIPSET_MISMATCH ==
           judge("{\"chipset\":\"esp32\",\"dev_detail\":[]}", "ESP32", NULL),
       "the chipset value is compared case sensitively, as at boot");

    /* A build with no name for its target cannot check the chipset. Refusing
     * outright would make such a target unconfigurable; the rest of the
     * configuration is still judged. */
    ok(EZPI_DEVCFGVAL_OK ==
           judge("{\"chipset\":\"WHATEVER\",\"dev_detail\":[" GOOD_DEVICE "]}",
                 NULL, NULL),
       "with no expected name, any chipset string passes");
    ok(EZPI_DEVCFGVAL_CHIPSET_MISSING ==
           judge("{\"dev_detail\":[]}", NULL, NULL),
       "but the member is still required");
    ok(EZPI_DEVCFGVAL_CHIPSET_NOT_STRING ==
           judge("{\"chipset\":7,\"dev_detail\":[]}", NULL, NULL),
       "and it must still be a string");
    ok(EZPI_DEVCFGVAL_ITEM_ID_BAD ==
           judge(CONFIG_WITH("{\"device_id\":\"Z\"}"), NULL, NULL),
       "and the device entries are still judged");
}

/**
 * @brief   Ezlo compatibility: do not refuse what we have no opinion about.
 *
 * An older hub refusing a configuration because a newer app added a member is
 * a worse failure than the one this component fixes, because it breaks a hub
 * that was working.
 */
static void test_unknown_members_are_accepted(void)
{
    printf("-- unknown members --\n");

    ok(EZPI_DEVCFGVAL_OK ==
           judge_here("{\"chipset\":\"ESP32\",\"schema_version\":3,"
                      "\"dev_detail\":[" GOOD_DEVICE "],"
                      "\"something_new\":{\"a\":[1,2,3]},\"x\":null}"),
       "extra top level members are ignored");
    ok(EZPI_DEVCFGVAL_OK ==
           judge_here(CONFIG_WITH("{\"device_id\":\"ZZZZZZZZ\","
                                  "\"id_item\":1001,"
                                  "\"future_field\":{\"deep\":[{\"x\":1}]},"
                                  "\"another\":null}")),
       "extra members inside a device entry are ignored");
    ok(EZPI_DEVCFGVAL_OK ==
           judge_here("{\"chipset\":\"ESP32\",\"dev_detail\":[" GOOD_DEVICE
                      "],\"dev_detail_v2\":\"not an array\"}"),
       "a member whose name merely resembles one we check is ignored");
}

/**
 * @brief   Look members up the way the parser does, including its quirks.
 *
 * cJSON_GetObjectItem is case insensitive, and ezlopi_device_parse_json_v3()
 * uses it. So a configuration spelling the member "ChipSet" builds devices on
 * a board today. Refusing it here would be this check inventing a rule the
 * firmware does not have, and the user would see a configuration rejected for
 * no reason they can act on.
 */
static void test_member_lookup_matches_the_parser(void)
{
    printf("-- member lookup --\n");

    ok(EZPI_DEVCFGVAL_OK ==
           judge_here("{\"ChipSet\":\"ESP32\",\"DEV_DETAIL\":[" GOOD_DEVICE
                      "]}"),
       "member names are matched case insensitively, as the parser does");
    ok(EZPI_DEVCFGVAL_OK ==
           judge_here("{\"chipset\":\"ESP32\",\"dev_detail\":"
                      "[{\"Device_Id\":\"ZZZZZZZZ\",\"ID_ITEM\":1001}]}"),
       "including inside a device entry");
}

/**
 * @brief   Which entry was wrong, in document order.
 *
 * The caller reports this to whoever sent the configuration. An arbitrary bad
 * entry is much less useful than the first one, and "some entry is wrong" is
 * nearly useless on a list of thirty.
 */
static void test_the_offending_entry_is_named(void)
{
    int bad_index = 0;

    printf("-- naming the bad entry --\n");

    ok(EZPI_DEVCFGVAL_ITEM_ID_BAD ==
           judge(CONFIG_WITH(GOOD_DEVICE "," GOOD_DEVICE
                             ",{\"device_id\":\"Z\"},{\"device_id\":\"Y\"}"),
                 THIS_CHIP, &bad_index),
       "two good entries then two bad ones");
    ok(2 == bad_index, "the index reported is the first bad one");

    ok(EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT ==
           judge(CONFIG_WITH("3,{\"device_id\":\"Z\"}"), THIS_CHIP,
                 &bad_index),
       "two different faults, the first one wins");
    ok(0 == bad_index, "and its index is reported");

    /* Document order, not "the worst fault". A missing id_item on entry 1
     * must not be reported ahead of a non-object at entry 0, nor the reverse
     * when they are swapped. */
    ok(EZPI_DEVCFGVAL_ITEM_ID_BAD ==
           judge(CONFIG_WITH("{\"device_id\":\"Z\"},3"), THIS_CHIP,
                 &bad_index),
       "swapped, the other fault wins");
    ok(0 == bad_index, "still index zero");

    bad_index = 999;
    ok(EZPI_DEVCFGVAL_OK == judge(GOOD_CONFIG, THIS_CHIP, &bad_index),
       "an accepted configuration");
    ok(-1 == bad_index, "sets the index to -1 rather than leaving it alone");

    /* A caller that does not care must be able to pass NULL. Under ASan a
     * write through it would be a crash here, not a mystery on a board. */
    ok(EZPI_DEVCFGVAL_ITEM_ID_BAD ==
           judge(CONFIG_WITH("{\"device_id\":\"Z\"}"), THIS_CHIP, NULL),
       "a NULL bad_index is accepted");
}

/**
 * @brief   The 4 KB field, and the terminator that has to fit in it.
 *
 * EZLOPI_CONFIG_JSON is a 4096 byte field and the writer appends a NUL, so a
 * configuration serialising to 4096 bytes does not fit. Getting this boundary
 * wrong by one is what the two commits before this one were about.
 */
static void test_the_size_check(void)
{
    cJSON *root = must_parse(GOOD_CONFIG);

    printf("-- the size check --\n");

    ok(EZPI_DEVCFGVAL_OK ==
           EZPI_util_devcfgval_check(root, THIS_CHIP, 4094u, 4096u, NULL),
       "4094 bytes fits a 4096 byte field");
    ok(EZPI_DEVCFGVAL_OK ==
           EZPI_util_devcfgval_check(root, THIS_CHIP, 4095u, 4096u, NULL),
       "4095 bytes fits, with the terminator at 4095");
    ok(EZPI_DEVCFGVAL_TOO_LARGE ==
           EZPI_util_devcfgval_check(root, THIS_CHIP, 4096u, 4096u, NULL),
       "4096 bytes does not, because the terminator would not fit");
    ok(EZPI_DEVCFGVAL_TOO_LARGE ==
           EZPI_util_devcfgval_check(root, THIS_CHIP, 4097u, 4096u, NULL),
       "nor does 4097");

    /* No wrap. serialised_len + 1 would be 0 here and the check would pass. */
    ok(EZPI_DEVCFGVAL_TOO_LARGE ==
           EZPI_util_devcfgval_check(root, THIS_CHIP, 0xFFFFFFFFu, 4096u,
                                     NULL),
       "and neither does UINT32_MAX, which must not wrap to zero");

    /* Either zero switches the check off, which is what a caller that is not
     * about to write to flash wants. */
    ok(EZPI_DEVCFGVAL_OK ==
           EZPI_util_devcfgval_check(root, THIS_CHIP, 0u, 4096u, NULL),
       "a length of zero skips the size check");
    ok(EZPI_DEVCFGVAL_OK ==
           EZPI_util_devcfgval_check(root, THIS_CHIP, 999999u, 0u, NULL),
       "a region of zero skips it too");

    cJSON_Delete(__FUNCTION__, root);

    /* Size is a property of the whole configuration, so it is reported ahead
     * of anything inside it: there is no point naming a bad device entry in
     * something that could not be stored either way. */
    cJSON *junk = must_parse("{\"hello\":\"world\"}");
    int bad_index = 7;

    ok(EZPI_DEVCFGVAL_TOO_LARGE ==
           EZPI_util_devcfgval_check(junk, THIS_CHIP, 5000u, 4096u,
                                     &bad_index),
       "too large is reported ahead of the contents");
    ok(-1 == bad_index, "and it is not a per-entry reason");

    cJSON_Delete(__FUNCTION__, junk);
}

/**
 * @brief   The tree comes back exactly as it went in.
 *
 * The callers hold a tree they are about to serialise and write. A check that
 * stamped a default or deleted a member would mean the thing stored is not the
 * thing judged, and the difference would only show up on the next boot.
 */
static void test_the_tree_is_not_modified(void)
{
    int i;

    printf("-- no side effects on the tree --\n");

    for (i = 0; i < CASE_COUNT; i++)
    {
        cJSON *root = cJSON_Parse(__FUNCTION__, cases[i].json);

        if (NULL == root)
        {
            /* "null", "42" and friends parse; a NULL here would be a typo in
             * the table, which must_parse() would already have caught. */
            continue;
        }

        char *before = cJSON_PrintUnformatted(__FUNCTION__, root);
        (void)EZPI_util_devcfgval_check(root, THIS_CHIP, 0u, 0u, NULL);
        char *after = cJSON_PrintUnformatted(__FUNCTION__, root);

        ok((NULL != before) && (NULL != after) &&
               (0 == strcmp(before, after)),
           "the tree is unchanged by the check");

        free(before);
        free(after);
        cJSON_Delete(__FUNCTION__, root);
    }
}

/**
 * @brief   Every reason the enum declares is produced by some input.
 *
 * Without this, a reason could be declared, documented, reported in an error
 * reply and never actually returned -- and the shape it was meant to catch
 * would still be stored.
 */
static void test_every_reason_is_reachable(void)
{
    /* Indexed by enumerator, which is why the enum starts at OK = 0. */
    int seen[EZPI_DEVCFGVAL_ITEM_ID_BAD + 1];
    int i;
    int unreachable = 0;

    printf("-- every reason is reachable --\n");

    memset(seen, 0, sizeof(seen));

    for (i = 0; i < CASE_COUNT; i++)
    {
        e_ezpi_devcfgval_t got = judge(cases[i].json, THIS_CHIP, NULL);

        if ((int)got <= EZPI_DEVCFGVAL_ITEM_ID_BAD)
        {
            seen[(int)got]++;
        }
    }

    /* TOO_LARGE is the one reason no table entry can reach, because the table
     * runs with the size check off. test_the_size_check() covers it. */
    seen[EZPI_DEVCFGVAL_TOO_LARGE]++;

    for (i = 0; i <= (int)EZPI_DEVCFGVAL_ITEM_ID_BAD; i++)
    {
        if (0 == seen[i])
        {
            printf("    (never returned: %s)\n",
                   EZPI_util_devcfgval_token((e_ezpi_devcfgval_t)i));
            unreachable++;
        }
    }

    ok(0 == unreachable, "no reason goes untested");
    ok(seen[EZPI_DEVCFGVAL_OK] >= 7,
       "and a useful number of shapes are accepted, not just refused");
}

/**
 * @brief   The tokens: present, distinct, and never NULL.
 *
 * These go into the "data" member of a JSON-RPC error, so an app matches on
 * them. Two reasons sharing a token would make them indistinguishable to
 * whoever is trying to fix their configuration.
 */
static void test_the_tokens(void)
{
    int i;
    int j;
    int clashes = 0;
    const int last = (int)EZPI_DEVCFGVAL_ITEM_ID_BAD;

    printf("-- reason tokens --\n");

    for (i = 0; i <= last; i++)
    {
        const char *t = EZPI_util_devcfgval_token((e_ezpi_devcfgval_t)i);

        ok(NULL != t, "a token is returned");
        ok((NULL != t) && (0 != strlen(t)), "and it is not empty");
        ok((NULL != t) && (0 == strncmp(t, "devcfg.", 7)),
           "and it is namespaced");

        for (j = 0; j < i; j++)
        {
            if (0 == strcmp(t, EZPI_util_devcfgval_token(
                                   (e_ezpi_devcfgval_t)j)))
            {
                clashes++;
            }
        }
    }

    ok(0 == clashes, "no two reasons share a token");

    /* Not reachable through the enum, but reachable through a cast or a value
     * read back from storage, and the callers put the result straight into a
     * reply -- so NULL would be a crash in an error path. */
    ok(NULL != EZPI_util_devcfgval_token((e_ezpi_devcfgval_t)999),
       "a value outside the enum still gets a token");
    ok(NULL != EZPI_util_devcfgval_token((e_ezpi_devcfgval_t)-1),
       "including a negative one");
    ok(0 == strcmp("devcfg.ok",
                   EZPI_util_devcfgval_token(EZPI_DEVCFGVAL_OK)),
       "and OK has one too, since callers log the accepted case as well");
}

/**
 * @brief   A long configuration, to put the walk under the sanitizers.
 *
 * The real limit is 4 KB of serialised JSON, which is a few dozen entries. The
 * walk is the only loop in the component and it is driven by data from off the
 * device, so running it over a list long enough to matter is cheap here.
 */
static void test_a_long_device_list(void)
{
    char *json = malloc(400000u);
    size_t used;
    int i;

    printf("-- a long device list --\n");

    if (NULL == json)
    {
        printf("  FATAL: out of memory in the test itself\n");
        exit(2);
    }

    used = (size_t)snprintf(json, 400000u,
                            "{\"chipset\":\"ESP32\",\"dev_detail\":[");

    for (i = 0; i < 2000; i++)
    {
        used += (size_t)snprintf(json + used, 400000u - used, "%s%s",
                                 (0 == i) ? "" : ",", GOOD_DEVICE);
    }

    snprintf(json + used, 400000u - used, "]}");

    ok(EZPI_DEVCFGVAL_OK == judge_here(json), "2000 good entries are accepted");

    /* And the same list with the last entry broken, which is the walk's worst
     * case: every entry examined before the fault is found. */
    used -= strlen(GOOD_DEVICE);
    snprintf(json + used, 400000u - used, "{\"device_id\":\"Z\"}]}");

    int bad_index = -1;
    ok(EZPI_DEVCFGVAL_ITEM_ID_BAD == judge(json, THIS_CHIP, &bad_index),
       "a fault in the last entry is found");
    ok(1999 == bad_index, "and that entry is the one named");

    free(json);
}

int main(void)
{
    printf("=== device configuration check ===\n");

    test_the_shapes_stored_today();
    test_the_table();
    test_a_foreign_configuration_is_refused();
    test_unknown_members_are_accepted();
    test_member_lookup_matches_the_parser();
    test_the_offending_entry_is_named();
    test_the_size_check();
    test_the_tree_is_not_modified();
    test_every_reason_is_reachable();
    test_the_tokens();
    test_a_long_device_list();

    printf("\n%d check(s), %d failure(s)\n", checks, failures);

    if (failures)
    {
        printf("DEVCFGVAL TESTS FAILED\n");
        return 1;
    }

    printf("DEVCFGVAL TESTS PASSED\n");
    return 0;
}
