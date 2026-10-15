/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/
/**
 * @file    test_devcfg_api.c
 * @brief   hub.ezlopi.config.get / .set -- what they refuse, and what they
 *          touch when they refuse
 *
 * WHAT IS WORTH TESTING HERE
 *
 * The subject is ezlopi_cloud_devcfg.c, which is the first thing in this
 * firmware able to replace the device configuration over the network. The
 * property that matters is not "does it store a config" -- that is one call --
 * but the negative one:
 *
 *     a request that is refused must leave the stored configuration alone.
 *
 * Writing as you walk is the natural way to write this kind of handler, and on
 * a real hub the difference is invisible: the error reply looks identical
 * either way, and the damage only shows up on the next boot, as devices that
 * are quietly missing. Every refusal case below therefore asserts the write
 * count as well as the error, which is the only way that distinction can fail
 * a test rather than fail a user.
 *
 * The second property is that the reason survives the trip. The validator
 * distinguishes ten ways a configuration can be wrong and names the offending
 * entry; a handler that collapsed those into one "invalid params" would pass
 * any test that only looked at the error code. So the token and the index are
 * checked for each one.
 *
 * WHAT IS FAKED, AND WHY THAT IS HONEST
 *
 * The six ezlopi_cloud_devcfg_port.c functions. They exist precisely so that
 * the flash partition and the HAL stay out of this file -- the real ones
 * forward to EZPI_core_factory_info_v3_* and to the compiled-in device table,
 * neither of which can run on a host and neither of which is where the
 * decisions are. The fakes record what was asked of them, which is the thing
 * being asserted: not whether flash works, but which calls the handler decides
 * to make and in which order.
 *
 * The validator underneath is NOT faked. ezlopi-util-devcfgval is linked in
 * real, so a configuration this file says is accepted is a configuration the
 * firmware accepts, and the two cannot drift.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjext.h"

#include "ezlopi_util_devcfgval.h"

#include "ezlopi_cloud_devcfg.h"
#include "ezlopi_cloud_devcfg_port.h"

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
** subject being wrong, and exiting 2 keeps the two apart -- the same split
** check_finfo_bounded_store.py uses. A test that silently passed NULL to the
** subject would be testing the NULL path while claiming to test a shape. */
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
 * The real definitions live in ezlopi_cloud_keywords.c alongside several
 * hundred others. Defining the eleven this subject uses is cheaper than linking
 * that file, and the spellings are not invented here -- they are the values at
 * ezlopi_cloud_keywords.c lines 94, 103, 104, 117, 178, 179, 180, 223, 262, 263
 * and 480.
 *******************************************************************************/
const char *ezlopi_result_str = "result";
const char *ezlopi_name_str = "name";
const char *ezlopi_params_str = "params";
const char *ezlopi_error_str = "error";
const char *ezlopi_code_str = "code";
const char *ezlopi_data_str = "data";
const char *ezlopi_message_str = "message";
const char *ezlopi_device_id_str = "device_id";
const char *ezlopi_dev_detail_str = "dev_detail";
const char *ezlopi_id_item_str = "id_item";
const char *ezlopi_chipset_str = "chipset";

/*******************************************************************************
 *                          Recording fakes for the port
 *******************************************************************************/

/* The field size the real port reports: EZLOPI_FINFO_LEN_EZLOPI_CONFIG_JSON,
** which is 0x1000. Settable so the zero case -- "do not check the size" -- can
** be exercised, which is a documented behaviour of the port contract and would
** otherwise only be reachable on a hypothetical target. */
#define REAL_REGION_SIZE 4096u

/* The chipset this build pretends to be. ESP32 because that is the target the
** images are built for, and because the mismatch case needs a second name. */
#define THIS_CHIP "ESP32"

static uint32_t fake_region_size = REAL_REGION_SIZE;
static const char *fake_chipset = THIS_CHIP;

/* What read() hands back; NULL means an unconfigured hub. */
static const char *fake_stored = NULL;

static int read_calls = 0;
static int free_calls = 0;
static int write_calls = 0;
static bool fake_write_succeeds = true;

/* The last thing write() was given, serialised at the moment of the call. The
** handler must not be able to pass the validator and then store something
** else, and comparing the serialisation is how that is checked rather than
** trusted. */
static char last_written[8192];

/* id_items the fake device table claims. 1 and 2 are drivers "this image was
** built with"; 7777 deliberately is not, and is the hole the handler's extra
** check exists to find. */
static const uint32_t fake_driver_ids[] = {1u, 2u, 3u, 1001u};

/* The table the enumeration walks.
**
** Padded the way the real one is: every name in
** ezlopi_core_devices_list_modules.h is spaced out to exactly 36 characters so
** the table lines up in the source, and the handler's job is to not send that
** padding. A fake with tidy names could not tell whether the trimming works or
** whether there was simply nothing to trim.
**
** The ids match fake_driver_ids above, which is how the real port behaves --
** both of its functions read the one device table -- and lets
** test_drivers_list_agrees_with_the_write_path() check that the list a client
** is shown is the list .set will accept. They are separate variables rather
** than one, so that the agreement is asserted rather than made true by
** construction in the fake. */
struct fake_driver_row
{
    const char *name;
    uint32_t id;
};

static const struct fake_driver_row default_fake_table[] = {
    {"digital-out generic                 ", 1u},
    {"digital-out relay                   ", 2u},
    {"sensor-DHT11-1-wire-temp-humid      ", 3u},
    {"sensor-dummy-switch                 ", 1001u},
};

static const struct fake_driver_row *fake_table = default_fake_table;
static int fake_table_rows =
    (int)(sizeof(default_fake_table) / sizeof(default_fake_table[0]));

/* Every index driver_at() was asked for, in order. The handler must walk from
** 0 upwards and stop one past the end; a handler that indexed straight into the
** table, or asked twice for a row, or ran off, shows up here rather than in a
** response that happens to look right. */
#define MAX_RECORDED_INDICES 256
static int driver_at_indices[MAX_RECORDED_INDICES];
static int driver_at_calls = 0;

static void fakes_reset(void)
{
    fake_region_size = REAL_REGION_SIZE;
    fake_chipset = THIS_CHIP;
    fake_stored = NULL;
    read_calls = 0;
    free_calls = 0;
    write_calls = 0;
    fake_write_succeeds = true;
    last_written[0] = '\0';
    fake_table = default_fake_table;
    fake_table_rows =
        (int)(sizeof(default_fake_table) / sizeof(default_fake_table[0]));
    driver_at_calls = 0;
}

char *EZPI_CLOUD_devcfg_port_read(void)
{
    read_calls++;

    if (NULL == fake_stored)
    {
        return NULL;
    }

    /* A fresh allocation per call, because that is what the real one does and
    ** because the free count below is only meaningful if there is something to
    ** free. strdup is not C99, hence the explicit pair. */
    {
        const size_t len = strlen(fake_stored);
        char *copy = malloc(len + 1u);

        if (copy)
        {
            memcpy(copy, fake_stored, len + 1u);
        }

        return copy;
    }
}

void EZPI_CLOUD_devcfg_port_free(char *str)
{
    if (str)
    {
        free_calls++;
        free(str);
    }
}

bool EZPI_CLOUD_devcfg_port_write(cJSON *root)
{
    char *serialised = cJSON_PrintUnformatted(__FUNCTION__, root);

    write_calls++;

    last_written[0] = '\0';

    if (serialised)
    {
        snprintf(last_written, sizeof(last_written), "%s", serialised);
        free(serialised);
    }

    return fake_write_succeeds;
}

const char *EZPI_CLOUD_devcfg_port_chipset(void)
{
    return fake_chipset;
}

uint32_t EZPI_CLOUD_devcfg_port_region_size(void)
{
    return fake_region_size;
}

bool EZPI_CLOUD_devcfg_port_driver_exists(uint32_t id_item)
{
    size_t i;

    for (i = 0u; i < (sizeof(fake_driver_ids) / sizeof(fake_driver_ids[0]));
         i++)
    {
        if (id_item == fake_driver_ids[i])
        {
            return true;
        }
    }

    return false;
}

bool EZPI_CLOUD_devcfg_port_driver_at(int index, const char **name,
                                      uint32_t *id_item)
{
    if (driver_at_calls < MAX_RECORDED_INDICES)
    {
        driver_at_indices[driver_at_calls] = index;
    }

    driver_at_calls++;

    if ((index < 0) || (index >= fake_table_rows))
    {
        /* Nothing written on a miss, which is what the header promises. The
        ** handler clears its own locals before each call for the same reason,
        ** so a port that forgot to write them cannot show up as the previous
        ** row repeated. */
        return false;
    }

    if (name)
    {
        *name = fake_table[index].name;
    }

    if (id_item)
    {
        *id_item = fake_table[index].id;
    }

    return true;
}

/*******************************************************************************
 *                          Shorthands for the shapes
 *******************************************************************************/

/* A device entry naming a driver the fake table has. */
#define GOOD_DEVICE "{\"device_id\":\"dev-1\",\"id_item\":1}"
#define GOOD_DEVICE_2 "{\"device_id\":\"dev-2\",\"id_item\":2}"

#define CONFIG_WITH(entries) \
    "{\"chipset\":\"" THIS_CHIP "\",\"dev_detail\":[" entries "]}"

#define GOOD_CONFIG CONFIG_WITH(GOOD_DEVICE)

#define REQUEST_WITH(config) \
    "{\"method\":\"hub.ezlopi.config.set\",\"params\":{\"config\":" config "}}"

/*******************************************************************************
 *                          Response readers
 *******************************************************************************/

/**
 * @brief Find a member by its exact key, walking the child list directly.
 *
 * Not cJSON_GetObjectItem, and this is the one place in this file worth
 * explaining at length, because the reason is a cjext defect rather than a
 * preference.
 *
 * cjext keeps two incompatible conventions for cJSON.str_key_len:
 *
 *   - the parser stores the length *including* the NUL -- cjext.c:1897 and
 *     :2024 assign str_key_len = str_value_len, which the string parser sets
 *     to the byte count plus the terminator;
 *   - add_item_to_object stores it *excluding* the NUL -- cjext.c:2423 and
 *     :2785 assign strlen(key).
 *
 * cJSON_GetObjectItem is the case-insensitive lookup, and it passes the
 * stored key's str_key_len to case_insensitive_strcmp (cjext.c:2271) as the
 * number of bytes it is allowed to compare. That function can only reject a
 * requested name that is *longer* than the stored key by reaching the stored
 * key's NUL and comparing it against a real character -- which it can only do
 * if the budget includes the NUL.
 *
 * So on a parsed tree the budget is inclusive and the lookup is correct; on a
 * tree built with cJSON_Add*ToObject the budget runs out exactly at the prefix
 * boundary, and a lookup of a longer name matches a shorter key. Measured, on
 * a built object holding "config" then "configSize":
 *
 *     cJSON_GetObjectItem(o, "configSize")          -> the "config" node
 *     cJSON_GetObjectItemCaseSensitive(o, "config"  -> the "configSize" node
 *                                           "Size")
 *
 * The case-sensitive variant is unaffected because it compares
 * max(strlen(name), str_key_len) bytes (cjext.c:2263) and so always sees past
 * the shorter key.
 *
 * That matters here because a response is a *built* tree and the result
 * carries both "config" and "configSize". Reading it with the lookup the rest
 * of the firmware uses would make this file assert on cjext's prefix handling
 * instead of on the subject, and two checks would fail for a reason that has
 * nothing to do with the code under test.
 *
 * The defect does not reach the subject's own behaviour: requests arrive
 * parsed, so every lookup the handler and the validator perform on input --
 * "params", "config", "dev_detail", "device_id", "id_item" -- runs against the
 * inclusive convention and is correct. The serialised response is correct too;
 * the ambiguity exists only inside cjext's lookup, not in the JSON. It is not
 * fixed here: add_item_to_object's length also feeds the key printer at
 * cjext.c:2126, so changing the convention is a change to how all 123 methods
 * print, and belongs in its own commit with its own evidence rather than
 * riding along with a new API.
 *
 * test_cjext_prefix_lookup_is_still_broken() below pins the behaviour, so that
 * the day cjext is fixed this file says so instead of quietly changing
 * meaning.
 */
static cJSON *member(cJSON *cj_object, const char *name)
{
    cJSON *cj_child = NULL;

    if ((NULL == cj_object) || (NULL == name))
    {
        return NULL;
    }

    for (cj_child = cj_object->child; NULL != cj_child;
         cj_child = cj_child->next)
    {
        if ((NULL != cj_child->string) &&
            (0 == strcmp(cj_child->string, name)))
        {
            return cj_child;
        }
    }

    return NULL;
}

static cJSON *result_of(cJSON *cj_response)
{
    return member(cj_response, ezlopi_result_str);
}

static cJSON *error_of(cJSON *cj_response)
{
    return member(cj_response, ezlopi_error_str);
}

/* The error's "data" token, or NULL if there is no error. */
static const char *token_of(cJSON *cj_response)
{
    cJSON *cj_error = error_of(cj_response);
    cJSON *cj_data = NULL;

    if (NULL == cj_error)
    {
        return NULL;
    }

    cj_data = member(cj_error, ezlopi_data_str);

    return cJSON_IsString(cj_data) ? cj_data->valuestring : NULL;
}

static int code_of(cJSON *cj_response)
{
    cJSON *cj_error = error_of(cj_response);
    cJSON *cj_code = NULL;

    if (NULL == cj_error)
    {
        return 0;
    }

    cj_code = member(cj_error, ezlopi_code_str);

    return cJSON_IsNumber(cj_code) ? (int)cj_code->valuedouble : 0;
}

/* The error's "badIndex", or -1 when the member is absent -- which is the
** documented way of saying "not about one entry". */
static int bad_index_of(cJSON *cj_response)
{
    cJSON *cj_error = error_of(cj_response);
    cJSON *cj_index = NULL;

    if (NULL == cj_error)
    {
        return -1;
    }

    cj_index = member(cj_error, "badIndex");

    return cJSON_IsNumber(cj_index) ? (int)cj_index->valuedouble : -1;
}

static double number_in_result(cJSON *cj_response, const char *name)
{
    cJSON *cj_result = result_of(cj_response);
    cJSON *cj_value = NULL;

    if (NULL == cj_result)
    {
        return -1.0;
    }

    cj_value = member(cj_result, name);

    return cJSON_IsNumber(cj_value) ? cj_value->valuedouble : -1.0;
}

/*******************************************************************************
 *                          Drivers
 *******************************************************************************/

/* Run .set on a request document and hand back the response, which the caller
** deletes. The request is parsed here so that every test goes through the same
** path the websocket layer uses -- a hand-built tree could accidentally differ
** from a parsed one in exactly the member types under test. */
static cJSON *run_set(const char *request_json)
{
    cJSON *cj_request = must_parse(request_json);
    cJSON *cj_response = cJSON_CreateObject(__FUNCTION__);

    if (NULL == cj_response)
    {
        printf("  FATAL: out of memory building a response\n");
        exit(2);
    }

    EZPI_CLOUD_devcfg_set(cj_request, cj_response);
    cJSON_Delete(__FUNCTION__, cj_request);

    return cj_response;
}

static cJSON *run_get(void)
{
    cJSON *cj_request = must_parse("{\"method\":\"hub.ezlopi.config.get\"}");
    cJSON *cj_response = cJSON_CreateObject(__FUNCTION__);

    if (NULL == cj_response)
    {
        printf("  FATAL: out of memory building a response\n");
        exit(2);
    }

    EZPI_CLOUD_devcfg_get(cj_request, cj_response);
    cJSON_Delete(__FUNCTION__, cj_request);

    return cj_response;
}

static cJSON *run_drivers_list(void)
{
    cJSON *cj_request =
        must_parse("{\"method\":\"hub.ezlopi.config.drivers.list\"}");
    cJSON *cj_response = cJSON_CreateObject(__FUNCTION__);

    if (NULL == cj_response)
    {
        printf("  FATAL: out of memory building a response\n");
        exit(2);
    }

    EZPI_CLOUD_devcfg_drivers_list(cj_request, cj_response);
    cJSON_Delete(__FUNCTION__, cj_request);

    return cj_response;
}

/* The "drivers" array from a drivers.list response, or NULL. */
static cJSON *drivers_of(cJSON *cj_response)
{
    cJSON *cj_result = result_of(cj_response);
    cJSON *cj_drivers = NULL;

    if (NULL == cj_result)
    {
        return NULL;
    }

    cj_drivers = member(cj_result, "drivers");

    return cJSON_IsArray(cj_drivers) ? cj_drivers : NULL;
}

/* The nth entry of that array, or NULL. */
static cJSON *driver_at(cJSON *cj_response, int index)
{
    cJSON *cj_drivers = drivers_of(cj_response);
    cJSON *cj_entry = NULL;
    int i = 0;

    for (cj_entry = (NULL != cj_drivers) ? cj_drivers->child : NULL;
         NULL != cj_entry; cj_entry = cj_entry->next)
    {
        if (i == index)
        {
            return cj_entry;
        }

        i++;
    }

    return NULL;
}

/* The nth entry's "name" as a string, or NULL if it is absent, null, or not a
** string. The three are deliberately not distinguished here; the tests that
** care check the node directly. */
static const char *driver_name_at(cJSON *cj_response, int index)
{
    cJSON *cj_name = member(driver_at(cj_response, index), "name");

    return cJSON_IsString(cj_name) ? cj_name->valuestring : NULL;
}

/*******************************************************************************
 *                          The tests
 *******************************************************************************/

/**
 * @brief The subject is actually linked in.
 *
 * Both handlers live outside any #ifdef, so unlike the MQTT methods this file
 * cannot preprocess away to nothing. It can still be linked against a stale
 * object, and a response with neither result nor error is the signature of a
 * handler that did not run.
 */
static void test_the_subject_exists(void)
{
    cJSON *cj_response = NULL;

    printf("test_the_subject_exists\n");
    fakes_reset();

    cj_response = run_get();
    ok(NULL != result_of(cj_response), "get produced a result object");
    cJSON_Delete(__FUNCTION__, cj_response);

    cj_response = run_set(REQUEST_WITH(GOOD_CONFIG));
    ok(NULL != result_of(cj_response), "set produced a result object");
    ok(NULL == error_of(cj_response), "a good config is not an error");
    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief An unconfigured hub answers, rather than failing.
 *
 * This is the first call a configuration UI makes, against a hub that has
 * nothing stored. Reporting that as an error would make the feature useless
 * exactly when it is needed, and "config": null is the honest answer. The
 * chipset is reported even here, because a client has to know which name to
 * put in the configuration it is about to send.
 */
static void test_get_on_an_unconfigured_hub(void)
{
    cJSON *cj_response = NULL;
    cJSON *cj_result = NULL;
    cJSON *cj_config = NULL;
    cJSON *cj_chipset = NULL;

    printf("test_get_on_an_unconfigured_hub\n");
    fakes_reset();
    fake_stored = NULL;

    cj_response = run_get();
    cj_result = result_of(cj_response);

    ok(NULL == error_of(cj_response), "no configuration is not an error");
    ok(NULL != cj_result, "there is a result");

    cj_config = member(cj_result, "config");
    ok(NULL != cj_config, "\"config\" is present");
    ok(cJSON_IsNull(cj_config), "\"config\" is null, not absent");

    ok(0.0 == number_in_result(cj_response, "configSize"), "configSize is 0");
    ok(0.0 == number_in_result(cj_response, "deviceCount"), "deviceCount is 0");
    ok((double)REAL_REGION_SIZE ==
           number_in_result(cj_response, "maxConfigSize"),
       "maxConfigSize is the field size");

    cj_chipset =
        member(cj_result, "expectedChipset");
    ok(cJSON_IsString(cj_chipset), "expectedChipset is a string");
    ok((NULL != cj_chipset) && (NULL != cj_chipset->valuestring) &&
           (0 == strcmp(cj_chipset->valuestring, THIS_CHIP)),
       "expectedChipset is this build's chipset");

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief A stored configuration comes back as an object, counted, and released.
 *
 * An object rather than a string because a client that has to parse the value
 * of a string member is a client that has to know this field is JSON, and
 * because a string would have to be escaped into the response and unescaped
 * again for no gain.
 *
 * The free count is the part that could silently rot: the real read() allocates
 * on every call, and one of the three existing readers of this field carries a
 * comment claiming it must not be freed -- copied from an unrelated field that
 * really is shared. A handler that believed that comment would leak four
 * kilobytes per call, which on a hub polled by a UI is a slow death with no
 * error anywhere.
 */
static void test_get_with_a_stored_configuration(void)
{
    const char *stored = CONFIG_WITH(GOOD_DEVICE "," GOOD_DEVICE_2);
    cJSON *cj_response = NULL;
    cJSON *cj_config = NULL;

    printf("test_get_with_a_stored_configuration\n");
    fakes_reset();
    fake_stored = stored;

    cj_response = run_get();

    ok(NULL == error_of(cj_response), "reading a good config is not an error");
    ok(1 == read_calls, "the field was read exactly once");
    ok(1 == free_calls, "and the string it returned was released");

    cj_config = cJSON_GetObjectItem(__FUNCTION__, result_of(cj_response),
                                    "config");
    ok(cJSON_IsObject(cj_config), "\"config\" is an object");

    ok((double)strlen(stored) == number_in_result(cj_response, "configSize"),
       "configSize is the stored length");
    ok(2.0 == number_in_result(cj_response, "deviceCount"),
       "deviceCount counts dev_detail");

    /* What came back must be what was stored, not a reconstruction of it. */
    if (cJSON_IsObject(cj_config))
    {
        char *round_trip = cJSON_PrintUnformatted(__FUNCTION__, cj_config);

        ok((NULL != round_trip) && (0 == strcmp(round_trip, stored)),
           "the config returned serialises back to exactly what was stored");

        free(round_trip);
    }
    else
    {
        ok(false, "the config returned serialises back to what was stored");
    }

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief Something unparseable in flash is reported as that, and still freed.
 *
 * Distinguished from "nothing stored" because the two need different responses
 * from whoever asked. An unconfigured hub wants a configuration; this one has
 * bytes in flash that the next boot will also fail to read, and a client told
 * "no configuration" would reasonably conclude the hub was fine.
 */
static void test_get_with_an_unreadable_field(void)
{
    cJSON *cj_response = NULL;

    printf("test_get_with_an_unreadable_field\n");
    fakes_reset();
    fake_stored = "{\"chipset\":\"ESP32\",";

    cj_response = run_get();

    ok(NULL != error_of(cj_response), "an unparseable field is an error");
    ok(-32500 == code_of(cj_response), "reported as an internal error");
    ok((NULL != token_of(cj_response)) &&
           (0 == strcmp(token_of(cj_response), "devcfg.stored.unreadable")),
       "with a token that says which internal error");
    ok(1 == free_calls, "the string is released on the failure path too");
    ok(0 == write_calls, "and nothing is written while reading");

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief A missing params, or a params without a usable config, is refused.
 *
 * Separated from the shape failures below because the codes differ and the
 * difference is meaningful: -32600 is "your request is malformed", -32602 is
 * "your parameters are wrong". The existing methods draw the line in the same
 * place and a new method drawing it elsewhere would be a surprise.
 */
static void test_the_request_envelope(void)
{
    static const struct
    {
        const char *request;
        int want_code;
        const char *what;
    } cases[] = {
        {"{\"method\":\"hub.ezlopi.config.set\"}", -32600,
         "no params at all"},
        {"{\"method\":\"hub.ezlopi.config.set\",\"params\":{}}", -32602,
         "params without a config"},
        {REQUEST_WITH("[]"), -32602, "config is an array"},
        {REQUEST_WITH("\"a string\""), -32602, "config is a string"},
        {REQUEST_WITH("42"), -32602, "config is a number"},
        {REQUEST_WITH("null"), -32602, "config is null"},
        {REQUEST_WITH("true"), -32602, "config is a boolean"},
    };

    size_t i;

    printf("test_the_request_envelope\n");

    for (i = 0u; i < (sizeof(cases) / sizeof(cases[0])); i++)
    {
        cJSON *cj_response = NULL;

        fakes_reset();
        cj_response = run_set(cases[i].request);

        ok(cases[i].want_code == code_of(cj_response), cases[i].what);
        ok(0 == write_calls, "nothing was written");
        ok(-1 == bad_index_of(cj_response),
           "no badIndex for an envelope problem");

        cJSON_Delete(__FUNCTION__, cj_response);
    }
}

/**
 * @brief Every way the validator can refuse reaches the caller intact.
 *
 * The table is the point. A handler that answered "rpc.params.invalid" to all
 * of these would pass a test that only checked the code, and would leave a user
 * with a configuration the hub will not take and no idea which part of it is
 * the problem. The expected tokens are read from EZPI_util_devcfgval_token()
 * rather than written out, so the two cannot be kept in step by accident -- the
 * test asserts the handler forwards the validator's answer, not that it
 * produces some string this file also knows.
 *
 * want_index is checked too, because naming the wrong entry is worse than
 * naming none.
 */
static void test_the_validator_reasons_are_forwarded(void)
{
    static const struct
    {
        const char *config;
        e_ezpi_devcfgval_t want;
        int want_index;
        const char *what;
    } cases[] = {
        {"{}", EZPI_DEVCFGVAL_CHIPSET_MISSING, -1, "no chipset"},
        {"{\"chipset\":7,\"dev_detail\":[]}",
         EZPI_DEVCFGVAL_CHIPSET_NOT_STRING, -1, "chipset is a number"},
        {"{\"chipset\":\"ESP32S3\",\"dev_detail\":[]}",
         EZPI_DEVCFGVAL_CHIPSET_MISMATCH, -1, "a config for another chip"},
        {"{\"chipset\":\"" THIS_CHIP "\"}", EZPI_DEVCFGVAL_DEVLIST_MISSING, -1,
         "no dev_detail"},
        {"{\"chipset\":\"" THIS_CHIP "\",\"dev_detail\":{}}",
         EZPI_DEVCFGVAL_DEVLIST_NOT_ARRAY, -1, "dev_detail is an object"},
        {CONFIG_WITH("5"), EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT, 0,
         "an entry is a number"},
        {CONFIG_WITH(GOOD_DEVICE ",7"), EZPI_DEVCFGVAL_DEVICE_NOT_OBJECT, 1,
         "the second entry is a number"},
        {CONFIG_WITH("{\"id_item\":1}"), EZPI_DEVCFGVAL_DEVICE_ID_BAD, 0,
         "an entry has no device_id"},
        {CONFIG_WITH("{\"device_id\":9,\"id_item\":1}"),
         EZPI_DEVCFGVAL_DEVICE_ID_BAD, 0, "device_id is not a string"},
        {CONFIG_WITH("{\"device_id\":\"d\"}"), EZPI_DEVCFGVAL_ITEM_ID_BAD, 0,
         "an entry has no id_item"},
        {CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":0}"),
         EZPI_DEVCFGVAL_ITEM_ID_BAD, 0, "id_item is zero"},
        {CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":0.4}"),
         EZPI_DEVCFGVAL_ITEM_ID_BAD, 0,
         "id_item truncates to zero, as the parser's cast would"},
        {CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":\"1\"}"),
         EZPI_DEVCFGVAL_ITEM_ID_BAD, 0, "id_item is a string"},
        {CONFIG_WITH(GOOD_DEVICE "," GOOD_DEVICE_2
                                 ",{\"device_id\":\"d\",\"id_item\":0}"),
         EZPI_DEVCFGVAL_ITEM_ID_BAD, 2, "the third entry is the bad one"},
    };

    size_t i;

    printf("test_the_validator_reasons_are_forwarded\n");

    for (i = 0u; i < (sizeof(cases) / sizeof(cases[0])); i++)
    {
        char request[2048];
        cJSON *cj_response = NULL;
        const char *want_token = EZPI_util_devcfgval_token(cases[i].want);
        const char *got_token = NULL;

        snprintf(request, sizeof(request), REQUEST_WITH("%s"),
                 cases[i].config);

        fakes_reset();
        cj_response = run_set(request);
        got_token = token_of(cj_response);

        ok(-32602 == code_of(cj_response), cases[i].what);
        ok((NULL != got_token) && (0 == strcmp(got_token, want_token)),
           cases[i].what);
        ok(cases[i].want_index == bad_index_of(cj_response), cases[i].what);
        ok(0 == write_calls, cases[i].what);

        cJSON_Delete(__FUNCTION__, cj_response);
    }
}

/**
 * @brief A well-formed config naming a driver this image lacks is refused.
 *
 * This is the check EZPI_util_devcfgval_check() deliberately does not make,
 * because the answer depends on which drivers were compiled in and that differs
 * between builds. It is also the quietest failure in the whole feature: the
 * boot parser looks for a matching row in the device table and, finding none,
 * moves to the next entry without a word -- no error, no device, no log line.
 *
 * A separate token from the shape failures on purpose. These two refusals need
 * different things from the user: one means "fix your JSON", the other means
 * "this firmware image does not contain that driver", and a configuration that
 * is already correct is the worst thing to be told to go and edit.
 */
static void test_an_id_item_with_no_driver(void)
{
    static const struct
    {
        const char *config;
        int want_index;
        const char *what;
    } cases[] = {
        {CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":7777}"), 0,
         "the only entry names no driver"},
        {CONFIG_WITH(GOOD_DEVICE ",{\"device_id\":\"d\",\"id_item\":7777}"), 1,
         "the second entry names no driver"},
        {CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":-1}"), 0,
         "a negative id_item matches no row"},
        {CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":8888},"
                     "{\"device_id\":\"e\",\"id_item\":9999}"),
         0, "the first of two unknown ids is the one named"},
    };

    size_t i;

    printf("test_an_id_item_with_no_driver\n");

    for (i = 0u; i < (sizeof(cases) / sizeof(cases[0])); i++)
    {
        char request[2048];
        cJSON *cj_response = NULL;
        const char *got_token = NULL;

        snprintf(request, sizeof(request), REQUEST_WITH("%s"),
                 cases[i].config);

        fakes_reset();
        cj_response = run_set(request);
        got_token = token_of(cj_response);

        ok(-32602 == code_of(cj_response), cases[i].what);
        ok((NULL != got_token) &&
               (0 == strcmp(got_token, "devcfg.id_item.unsupported")),
           cases[i].what);
        ok(cases[i].want_index == bad_index_of(cj_response), cases[i].what);
        ok(0 == write_calls, cases[i].what);

        cJSON_Delete(__FUNCTION__, cj_response);
    }

    /* The other direction, so that a subject which rejected everything would
    ** fail here rather than look thorough. Each of these ids is one the fake
    ** table claims. */
    {
        static const char *accepted[] = {
            CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":1}"),
            CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":3}"),
            CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":1001}"),
            CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":1001.0}"),
            CONFIG_WITH(GOOD_DEVICE "," GOOD_DEVICE_2),
            CONFIG_WITH(""),
        };

        size_t j;

        for (j = 0u; j < (sizeof(accepted) / sizeof(accepted[0])); j++)
        {
            char request[2048];
            cJSON *cj_response = NULL;

            snprintf(request, sizeof(request), REQUEST_WITH("%s"),
                     accepted[j]);

            fakes_reset();
            cj_response = run_set(request);

            ok(NULL == error_of(cj_response),
               "a config naming only known drivers is accepted");
            ok(1 == write_calls, "and it is written");

            cJSON_Delete(__FUNCTION__, cj_response);
        }
    }
}

/**
 * @brief What is stored is what was judged.
 *
 * The handler validates a tree and then hands the same tree to the storer. If
 * anything in between stamped a default, normalised a member or reordered
 * anything, the bytes in flash would not be the bytes that passed the check --
 * which is the one thing that must not be true of a validate-then-store path.
 */
static void test_what_is_stored_is_what_was_sent(void)
{
    const char *config = CONFIG_WITH(GOOD_DEVICE "," GOOD_DEVICE_2);
    char request[2048];
    cJSON *cj_response = NULL;

    printf("test_what_is_stored_is_what_was_sent\n");

    snprintf(request, sizeof(request), REQUEST_WITH("%s"), config);

    fakes_reset();
    cj_response = run_set(request);

    ok(NULL == error_of(cj_response), "the config is accepted");
    ok(1 == write_calls, "written exactly once");
    ok(0 == strcmp(last_written, config),
       "the bytes stored are the bytes sent, unchanged");
    ok(2.0 == number_in_result(cj_response, "deviceCount"),
       "deviceCount is reported");

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief The request tree is not modified.
 *
 * The dispatcher that calls this handler still owns cj_request and deletes it
 * afterwards; more to the point, params.config is a subtree of it, so a handler
 * that edited the config in place would be editing the caller's document. The
 * validator underneath is already checked for this in test_devcfgval.c -- this
 * is the same property one layer up, where the handler could reintroduce it.
 */
static void test_the_request_is_not_modified(void)
{
    static const char *requests[] = {
        REQUEST_WITH(GOOD_CONFIG),
        REQUEST_WITH(CONFIG_WITH(GOOD_DEVICE "," GOOD_DEVICE_2)),
        REQUEST_WITH("{}"),
        REQUEST_WITH("{\"chipset\":\"ESP32S3\",\"dev_detail\":[]}"),
        REQUEST_WITH(CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":7777}")),
        REQUEST_WITH("[]"),
        "{\"method\":\"hub.ezlopi.config.set\",\"params\":{}}",
    };

    size_t i;

    printf("test_the_request_is_not_modified\n");

    for (i = 0u; i < (sizeof(requests) / sizeof(requests[0])); i++)
    {
        cJSON *cj_request = must_parse(requests[i]);
        cJSON *cj_response = cJSON_CreateObject(__FUNCTION__);
        char *before = cJSON_PrintUnformatted(__FUNCTION__, cj_request);
        char *after = NULL;

        fakes_reset();

        EZPI_CLOUD_devcfg_set(cj_request, cj_response);

        after = cJSON_PrintUnformatted(__FUNCTION__, cj_request);

        ok((NULL != before) && (NULL != after) && (0 == strcmp(before, after)),
           "the request is unchanged by .set");

        free(before);
        free(after);
        cJSON_Delete(__FUNCTION__, cj_request);
        cJSON_Delete(__FUNCTION__, cj_response);
    }
}

/**
 * @brief The size limit is the field's, and the boundary is where it belongs.
 *
 * Built by padding a device_id until the serialised document is exactly the
 * length wanted, so the number being checked is the length of the bytes that
 * would be written rather than an estimate of them. 4095 fits a 4096-byte
 * field; 4096 does not, because the terminator has to go somewhere.
 *
 * The off-by-one here is not academic: the storer that these bytes reach used
 * to write strlen + 1 bytes into that field, and a configuration serialising to
 * exactly 4095 wrote 4097 bytes into 4096. Getting this boundary wrong in the
 * other direction would hand it documents it cannot store and report success.
 */
static void test_the_size_boundary(void)
{
    static const struct
    {
        size_t total;
        bool want_accepted;
        const char *what;
    } cases[] = {
        {4094u, true, "4094 bytes fit"},
        {4095u, true, "4095 bytes fit a 4096-byte field"},
        {4096u, false, "4096 bytes do not"},
        {4097u, false, "nor 4097"},
        {6000u, false, "nor anything larger"},
    };

    const char *prefix = "{\"chipset\":\"" THIS_CHIP
                         "\",\"dev_detail\":[{\"id_item\":1,\"device_id\":\"";
    const char *suffix = "\"}]}";
    size_t i;

    printf("test_the_size_boundary\n");

    for (i = 0u; i < (sizeof(cases) / sizeof(cases[0])); i++)
    {
        const size_t overhead = strlen(prefix) + strlen(suffix);
        char *config = malloc(cases[i].total + 1u);
        char request[8192];
        cJSON *cj_response = NULL;

        if ((NULL == config) || (cases[i].total <= overhead))
        {
            printf("  FATAL: cannot build a %lu-byte config\n",
                   (unsigned long)cases[i].total);
            exit(2);
        }

        memcpy(config, prefix, strlen(prefix));
        memset(config + strlen(prefix), 'x', cases[i].total - overhead);
        memcpy(config + cases[i].total - strlen(suffix), suffix,
               strlen(suffix));
        config[cases[i].total] = '\0';

        /* This file's own arithmetic, checked before the subject is asked.
        ** A padding bug here would silently turn a boundary test into a test
        ** of some other length. */
        if (strlen(config) != cases[i].total)
        {
            printf("  FATAL: built %lu bytes, wanted %lu\n",
                   (unsigned long)strlen(config),
                   (unsigned long)cases[i].total);
            exit(2);
        }

        snprintf(request, sizeof(request), REQUEST_WITH("%s"), config);

        fakes_reset();
        cj_response = run_set(request);

        if (cases[i].want_accepted)
        {
            ok(NULL == error_of(cj_response), cases[i].what);
            ok(1 == write_calls, cases[i].what);
        }
        else
        {
            const char *got_token = token_of(cj_response);

            ok((NULL != got_token) &&
                   (0 == strcmp(got_token,
                                EZPI_util_devcfgval_token(
                                    EZPI_DEVCFGVAL_TOO_LARGE))),
               cases[i].what);
            ok(0 == write_calls, cases[i].what);
            ok(-1 == bad_index_of(cj_response),
               "too_large is not about one entry");
        }

        cJSON_Delete(__FUNCTION__, cj_response);
        free(config);
    }
}

/**
 * @brief The length the handler measures is the length the storer will write.
 *
 * The handler measures with cJSON_PrintUnformatted and the storer writes what
 * cJSON_PrintPreallocated(..., format=false) produced. The size check is only
 * meaningful if those two agree, and "same printer, same flag" is an argument
 * rather than a fact until something checks it. If a future cjext made the
 * preallocated path differ by even one byte, every boundary above would be
 * measuring the wrong thing and nothing else in the suite would notice.
 */
static void test_the_two_printers_agree(void)
{
    static const char *shapes[] = {
        GOOD_CONFIG,
        CONFIG_WITH(GOOD_DEVICE "," GOOD_DEVICE_2),
        CONFIG_WITH(""),
        "{\"chipset\":\"" THIS_CHIP "\",\"dev_detail\":[],\"extra\":{\"a\":[1,"
        "2,3],\"b\":null,\"c\":true,\"d\":-1.5,\"e\":\"a \\\"quoted\\\" "
        "string\"}}",
    };

    size_t i;

    printf("test_the_two_printers_agree\n");

    for (i = 0u; i < (sizeof(shapes) / sizeof(shapes[0])); i++)
    {
        cJSON *root = must_parse(shapes[i]);
        char *unformatted = cJSON_PrintUnformatted(__FUNCTION__, root);
        char preallocated[8192];

        ok(true == cJSON_PrintPreallocated(__FUNCTION__, root, preallocated,
                                           (int)sizeof(preallocated), false),
           "the preallocated printer succeeds");
        ok((NULL != unformatted) &&
               (0 == strcmp(unformatted, preallocated)),
           "both printers produce identical bytes");

        free(unformatted);
        cJSON_Delete(__FUNCTION__, root);
    }
}

/**
 * @brief A storage failure is reported, and does not claim a reboot would help.
 *
 * rebootRequired is a promise that restarting will apply what was just stored.
 * Emitting it when the write failed would send someone to reboot a hub that is
 * about to come back up with the old configuration and no explanation.
 */
static void test_a_failed_write(void)
{
    cJSON *cj_response = NULL;
    cJSON *cj_result = NULL;

    printf("test_a_failed_write\n");
    fakes_reset();
    fake_write_succeeds = false;

    cj_response = run_set(REQUEST_WITH(GOOD_CONFIG));
    cj_result = result_of(cj_response);

    ok(1 == write_calls, "the write was attempted");
    ok(-32500 == code_of(cj_response), "a failed write is an internal error");
    ok((NULL != token_of(cj_response)) &&
           (0 == strcmp(token_of(cj_response), "devcfg.store.failed")),
       "with its own token");
    ok(NULL == member(cj_result, "rebootRequired"),
       "and no rebootRequired");

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief Success says a reboot is needed, because it is.
 *
 * ezlopi_device_parse_json_v3() runs once, from EZPI_device_prepare() during
 * EZPI_init(). Nothing re-reads this field afterwards, so the device list in
 * memory is still the one from the last boot and will be until the next one.
 */
static void test_success_asks_for_a_reboot(void)
{
    cJSON *cj_response = NULL;
    cJSON *cj_reboot = NULL;

    printf("test_success_asks_for_a_reboot\n");
    fakes_reset();

    cj_response = run_set(REQUEST_WITH(GOOD_CONFIG));

    cj_reboot = cJSON_GetObjectItem(__FUNCTION__, result_of(cj_response),
                                    "rebootRequired");

    ok(cJSON_IsBool(cj_reboot), "rebootRequired is a boolean");
    ok(cJSON_IsTrue(cj_reboot), "and it is true");
    ok(NULL == error_of(cj_response), "with no error");

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief The two documented port edge cases behave as documented.
 *
 * A region size of 0 means "do not check the size", and a NULL chipset means
 * this build has no name for its target -- which is a real configuration of the
 * boot parser, whose chipset comparison is compiled out entirely when no
 * CONFIG_IDF_TARGET_* matches. Both are reachable only through the port, so
 * they are untested everywhere except here, and a handler that treated either
 * as a failure would make such a target unconfigurable.
 */
static void test_the_port_edge_cases(void)
{
    cJSON *cj_response = NULL;

    printf("test_the_port_edge_cases\n");

    /* No name for this target: the chipset is not checked, so a config naming
    ** any chip is judged on everything else. */
    fakes_reset();
    fake_chipset = NULL;
    cj_response = run_set(
        REQUEST_WITH("{\"chipset\":\"ESP32S3\",\"dev_detail\":[" GOOD_DEVICE
                     "]}"));
    ok(NULL == error_of(cj_response),
       "with no expected chipset, any chipset is accepted");
    ok(1 == write_calls, "and the config is stored");
    cJSON_Delete(__FUNCTION__, cj_response);

    /* ...but the rest of the rules still apply. */
    fakes_reset();
    fake_chipset = NULL;
    cj_response = run_set(REQUEST_WITH("{\"chipset\":\"ESP32S3\"}"));
    ok(-32602 == code_of(cj_response),
       "a missing dev_detail is still refused with no expected chipset");
    ok(0 == write_calls, "and nothing is written");
    cJSON_Delete(__FUNCTION__, cj_response);

    /* get reports the absent name as null rather than omitting it, so a client
    ** can tell "this build has no name" from "the hub did not answer". */
    fakes_reset();
    fake_chipset = NULL;
    cj_response = run_get();
    ok(cJSON_IsNull(member(result_of(cj_response), "expectedChipset")),
       "get reports a missing chipset name as null");
    cJSON_Delete(__FUNCTION__, cj_response);

    /* Region size 0: the size check is skipped, and a document larger than the
    ** real field is accepted. Asserted rather than assumed because the handler
    ** has to substitute its own bound to serialise at all, and the easy bug is
    ** to pass 0 straight through as the limit. */
    fakes_reset();
    fake_region_size = 0u;
    {
        const char *prefix = "{\"chipset\":\"" THIS_CHIP
                             "\",\"dev_detail\":[{\"id_item\":1,\"device_id\":"
                             "\"";
        const char *suffix = "\"}]}";
        const size_t total = 5000u;
        char *config = malloc(total + 1u);
        char request[8192];

        if (NULL == config)
        {
            printf("  FATAL: out of memory\n");
            exit(2);
        }

        memcpy(config, prefix, strlen(prefix));
        memset(config + strlen(prefix), 'x',
               total - strlen(prefix) - strlen(suffix));
        memcpy(config + total - strlen(suffix), suffix, strlen(suffix));
        config[total] = '\0';

        snprintf(request, sizeof(request), REQUEST_WITH("%s"), config);

        cj_response = run_set(request);

        ok(NULL == error_of(cj_response),
           "with no region size, the size check is skipped");
        ok(1 == write_calls, "and an oversized config is stored");

        cJSON_Delete(__FUNCTION__, cj_response);
        free(config);
    }

    /* get passes the port's number through untouched, including a zero -- and
    ** that is deliberately *not* what set does with the same zero. set has to
    ** substitute DEVCFG_FALLBACK_LIMIT because it needs some bound to serialise
    ** against, but get is only reporting, and reporting the fallback would tell
    ** a UI the field is 64 KiB when the port just said it does not know. Zero
    ** is the honest answer and a client can recognise it.
    **
    ** fake_region_size is set *after* fakes_reset(), which restores it to
    ** REAL_REGION_SIZE. Asserting 0.0 straight after the reset is how this
    ** check was first written, and it failed for that reason rather than for
    ** anything to do with the subject. */
    fakes_reset();
    fake_region_size = 0u;
    cj_response = run_get();
    ok(0.0 == number_in_result(cj_response, "maxConfigSize"),
       "get passes a zero region size through instead of the fallback");
    cJSON_Delete(__FUNCTION__, cj_response);

    /* The companion half, so the asymmetry above is pinned from both sides
    ** rather than asserted in prose. */
    fakes_reset();
    cj_response = run_get();
    ok((double)REAL_REGION_SIZE ==
           number_in_result(cj_response, "maxConfigSize"),
       "and reports the real field size when the port knows it");
    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief An error reply is always complete, and every token is distinct.
 *
 * A client switching on "data" needs it to be there on every failure path, and
 * needs two different failures not to arrive under the same name. Sweeping the
 * refusals and collecting the tokens is how a duplicate gets caught -- it is
 * exactly the sort of thing a copy-pasted error line produces, and reading the
 * file does not reliably find it.
 *
 * Each row also says whether "badIndex" belongs in the reply, because the
 * header promises it is *absent* -- not present as -1 -- when the refusal is
 * not about one entry. bad_index_of() cannot see that difference: it returns
 * -1 for both. So the presence of the member is asserted directly here, which
 * is the only check that distinguishes the two. Without it, changing the
 * handler's `if (bad_index >= 0)` guard to `>= -1` leaves the whole suite
 * green while every error reply carries a meaningless "badIndex": -1.
 */
static void test_every_error_reply_is_complete(void)
{
    static const struct
    {
        const char *request;
        bool wants_index; /* is this refusal about one dev_detail entry? */
    } requests[] = {
        {"{\"method\":\"hub.ezlopi.config.set\"}", false},
        {"{\"method\":\"hub.ezlopi.config.set\",\"params\":{}}", false},
        {REQUEST_WITH("{}"), false},
        {REQUEST_WITH("{\"chipset\":9,\"dev_detail\":[]}"), false},
        {REQUEST_WITH("{\"chipset\":\"ESP32S3\",\"dev_detail\":[]}"), false},
        {REQUEST_WITH("{\"chipset\":\"" THIS_CHIP "\"}"), false},
        {REQUEST_WITH("{\"chipset\":\"" THIS_CHIP "\",\"dev_detail\":{}}"),
         false},
        {REQUEST_WITH(CONFIG_WITH("1")), true},
        {REQUEST_WITH(CONFIG_WITH("{\"id_item\":1}")), true},
        {REQUEST_WITH(CONFIG_WITH("{\"device_id\":\"d\"}")), true},
        {REQUEST_WITH(CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":7777}")),
         true},
    };

    /* Copies, not pointers. token_of() points into the response tree, and the
    ** response is deleted at the end of each iteration -- a table of those
    ** pointers would be a use-after-free that the sanitisers would catch and
    ** that would otherwise "work" often enough to be believed. */
    char seen[32][64];
    size_t seen_count = 0u;
    size_t i;

    printf("test_every_error_reply_is_complete\n");

    for (i = 0u; i < (sizeof(requests) / sizeof(requests[0])); i++)
    {
        cJSON *cj_response = NULL;
        cJSON *cj_error = NULL;
        const char *got_token = NULL;

        fakes_reset();
        cj_response = run_set(requests[i].request);
        cj_error = error_of(cj_response);
        got_token = token_of(cj_response);

        ok(NULL != cj_error, "this request is refused");
        ok(cJSON_IsNumber(
               member(cj_error, ezlopi_code_str)),
           "the error has a numeric code");
        ok(cJSON_IsString(member(cj_error,
                                              ezlopi_message_str)),
           "the error has a message");
        ok((NULL != got_token) && ('\0' != got_token[0]),
           "the error has a non-empty data token");
        ok(0 == write_calls, "and nothing was written");

        /* The documented shape of badIndex, checked on the member rather than
        ** on its value so that "absent" and "present as -1" are told apart. */
        if (requests[i].wants_index)
        {
            cJSON *cj_index = member(cj_error, "badIndex");

            ok(cJSON_IsNumber(cj_index),
               "an entry-specific refusal carries badIndex");
            ok((NULL != cj_index) && (cj_index->valuedouble >= 0.0),
               "and it is not negative");
        }
        else
        {
            ok(NULL == member(cj_error, "badIndex"),
               "a refusal that is not about one entry omits badIndex");
        }

        if (NULL != got_token)
        {
            size_t j;
            bool already = false;

            for (j = 0u; j < seen_count; j++)
            {
                if (0 == strcmp(seen[j], got_token))
                {
                    already = true;
                }
            }

            /* rpc.params.invalid is shared by the two envelope failures on
            ** purpose -- both mean "put a config object in params.config" --
            ** so a repeat is only interesting beyond that one. */
            if ((false == already) &&
                (seen_count < (sizeof(seen) / sizeof(seen[0]))))
            {
                snprintf(seen[seen_count], sizeof(seen[0]), "%s", got_token);
                seen_count++;
            }
        }

        cJSON_Delete(__FUNCTION__, cj_response);
    }

    /* Exactly eleven, not "at least eleven": the eleven requests above are one
    ** per reason, so a count of ten means two of them collapsed into a shared
    ** token -- which is what a copy-pasted error line produces, and what
    ** reading the file does not reliably find. A count of twelve would mean a
    ** request was refused for a reason other than the one it was written for,
    ** so this fails in both directions. */
    ok(11u == seen_count, "the eleven refusals arrive under eleven tokens");
}

/**
 * @brief A long device list is walked without complaint.
 *
 * The point is not the count: it is that the walk runs under the sanitisers
 * with a list long enough that an off-by-one at either end has somewhere to
 * land. Then the last entry is broken, because a handler that stopped early
 * would accept it.
 *
 * A thousand rather than the two thousand test_devcfgval.c uses, and the
 * difference is the subject, not timidity. That file calls the validator
 * directly with the size check switched off; this one goes through the handler,
 * which substitutes DEVCFG_FALLBACK_LIMIT -- 64 KiB -- when the port reports no
 * region size, so it cannot be asked to serialise an unbounded document. A
 * thousand of these entries is about 34 KB and fits inside that bound with room
 * to spare; two thousand would be refused as too large and the walk would never
 * run, which would have been a test that passed while testing nothing.
 */
static void test_a_long_device_list(void)
{
    const int count = 1000;
    size_t capacity = (size_t)count * 64u + 256u;
    char *config = malloc(capacity);
    char *request = malloc(capacity + 128u);
    size_t used = 0u;
    int i;
    cJSON *cj_response = NULL;

    printf("test_a_long_device_list\n");

    if ((NULL == config) || (NULL == request))
    {
        printf("  FATAL: out of memory\n");
        exit(2);
    }

    used += (size_t)snprintf(config + used, capacity - used,
                             "{\"chipset\":\"" THIS_CHIP
                             "\",\"dev_detail\":[");

    for (i = 0; i < count; i++)
    {
        used += (size_t)snprintf(config + used, capacity - used,
                                 "%s{\"device_id\":\"d%d\",\"id_item\":1}",
                                 (0 == i) ? "" : ",", i);
    }

    used += (size_t)snprintf(config + used, capacity - used, "]}");

    /* The real field is 4 KB, so this is well past it. The size check has to be
    ** out of the way for the walk to be what is under test. */
    fakes_reset();
    fake_region_size = 0u;

    snprintf(request, capacity + 128u, REQUEST_WITH("%s"), config);
    cj_response = run_set(request);

    ok(NULL == error_of(cj_response), "1000 good entries are accepted");
    ok((double)count == number_in_result(cj_response, "deviceCount"),
       "all 1000 are counted");
    ok(1 == write_calls, "and written");
    cJSON_Delete(__FUNCTION__, cj_response);

    /* Break the last one. Rewriting the tail is enough: the final entry's
    ** id_item becomes an id no driver claims. */
    used = 0u;
    used += (size_t)snprintf(config + used, capacity - used,
                             "{\"chipset\":\"" THIS_CHIP
                             "\",\"dev_detail\":[");

    for (i = 0; i < count; i++)
    {
        used += (size_t)snprintf(
            config + used, capacity - used,
            "%s{\"device_id\":\"d%d\",\"id_item\":%d}", (0 == i) ? "" : ",", i,
            ((count - 1) == i) ? 7777 : 1);
    }

    used += (size_t)snprintf(config + used, capacity - used, "]}");

    fakes_reset();
    fake_region_size = 0u;

    snprintf(request, capacity + 128u, REQUEST_WITH("%s"), config);
    cj_response = run_set(request);

    ok((NULL != token_of(cj_response)) &&
           (0 == strcmp(token_of(cj_response), "devcfg.id_item.unsupported")),
       "a bad last entry is still found");
    ok((count - 1) == bad_index_of(cj_response),
       "and it is named by its index");
    ok(0 == write_calls, "nothing is written");

    cJSON_Delete(__FUNCTION__, cj_response);
    free(config);
    free(request);
}

/**
 * @brief get never writes, and set never reads.
 *
 * Small, and worth pinning because the handlers share a port. A get that wrote
 * would be a read-only method with a side effect on flash -- the sort of thing
 * that turns a UI polling for status into a flash-wear problem.
 */
static void test_the_two_methods_stay_in_their_lanes(void)
{
    cJSON *cj_response = NULL;

    printf("test_the_two_methods_stay_in_their_lanes\n");

    fakes_reset();
    fake_stored = GOOD_CONFIG;
    cj_response = run_get();
    ok(0 == write_calls, "get does not write");
    ok(1 == read_calls, "get reads once, not repeatedly");
    cJSON_Delete(__FUNCTION__, cj_response);

    fakes_reset();
    fake_stored = GOOD_CONFIG;
    cj_response = run_set(REQUEST_WITH(GOOD_CONFIG));
    ok(0 == read_calls, "set does not read the old configuration");
    ok(0 == free_calls, "and so has nothing to free");
    cJSON_Delete(__FUNCTION__, cj_response);

    fakes_reset();
    fake_stored = GOOD_CONFIG;
    cj_response = run_drivers_list();
    ok(0 == write_calls, "drivers.list does not write");
    ok(0 == read_calls, "and does not read the stored configuration");
    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief drivers.list reports the table, in order, with usable types.
 */
static void test_drivers_list_reports_the_table(void)
{
    cJSON *cj_response = NULL;
    cJSON *cj_drivers = NULL;
    int i = 0;

    printf("test_drivers_list_reports_the_table\n");

    fakes_reset();
    cj_response = run_drivers_list();
    cj_drivers = drivers_of(cj_response);

    ok(NULL != result_of(cj_response), "drivers.list answers with a result");
    ok(NULL == error_of(cj_response),
       "and reading a compile-time table is not an error condition");
    ok(NULL != cj_drivers, "\"drivers\" is an array");
    ok(fake_table_rows == cJSON_GetArraySize(cj_drivers),
       "with one entry per table row");
    ok((double)fake_table_rows == number_in_result(cj_response, "count"),
       "and \"count\" agrees with the array it describes");

    for (i = 0; i < fake_table_rows; i++)
    {
        cJSON *cj_entry = driver_at(cj_response, i);
        cJSON *cj_id = member(cj_entry, "id_item");

        ok(cJSON_IsObject(cj_entry), "every entry is an object");
        ok(cJSON_IsNumber(cj_id), "carrying a numeric id_item");

        /* Table order, not sorted and not reversed. A client shows this list to
        ** a person, and an order that drifts between firmware versions for no
        ** reason is a list that cannot be compared against the source. */
        ok((NULL != cj_id) &&
               ((double)fake_table[i].id == cj_id->valuedouble),
           "in table order");

        ok(NULL != driver_name_at(cj_response, i),
           "and a name that is a string");
    }

    /* The member is "id_item", with the underscore, because that is the key a
    ** dev_detail entry uses. A client that has to rename a key while copying a
    ** value is a client that will eventually get it wrong. */
    ok(NULL != member(driver_at(cj_response, 0), "id_item"),
       "the id is spelled the way a configuration entry spells it");
    ok(NULL == member(driver_at(cj_response, 0), "idItem"),
       "and not camel-cased as well");

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief The table's source-alignment padding does not reach the client.
 *
 * Every name in the real table is padded to 36 characters. Without trimming,
 * each name arrives with a tail of spaces that any UI showing it verbatim
 * displays, and the padding alone is most of a kilobyte on the wire.
 */
static void test_drivers_list_trims_the_padding(void)
{
    cJSON *cj_response = NULL;
    int i = 0;

    printf("test_drivers_list_trims_the_padding\n");

    fakes_reset();
    cj_response = run_drivers_list();

    /* Exact values, not "shorter than the padded form". A length check alone
    ** would pass for a handler that trimmed one byte, or the wrong end. */
    ok((NULL != driver_name_at(cj_response, 0)) &&
           (0 == strcmp("digital-out generic",
                        driver_name_at(cj_response, 0))),
       "a padded name arrives with the padding removed");
    ok((NULL != driver_name_at(cj_response, 2)) &&
           (0 == strcmp("sensor-DHT11-1-wire-temp-humid",
                        driver_name_at(cj_response, 2))),
       "and so does one in the middle of the table");

    for (i = 0; i < fake_table_rows; i++)
    {
        const char *name = driver_name_at(cj_response, i);
        const size_t len = (NULL != name) ? strlen(name) : 0u;

        ok((NULL != name) && (len > 0u) && (' ' != name[len - 1u]),
           "no name ends in a space");
        ok((NULL != name) && (' ' != name[0]),
           "and none of them gained a leading one");
    }

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief The three name cases the trimming has to survive.
 *
 * None of these occur in the table today. They are here because the handler
 * runs on an unauthenticated method and "the table would never hold that" is
 * the kind of claim that is true until somebody adds a driver.
 */
static void test_drivers_list_name_edge_cases(void)
{
    /* 70 characters once trimmed, which is past DEVCFG_DRIVER_NAME_MAX. */
    static const char *too_long =
        "sensor-with-an-unreasonably-long-name-that-does-not-fit-the-bound-here"
        "   ";

    static const struct fake_driver_row edge_table[] = {
        {NULL, 11u},
        {"                                    ", 12u},
        {"unpadded", 13u},
        {NULL, 14u}, /* replaced below; a static initialiser cannot hold it */
    };

    struct fake_driver_row table[4];
    cJSON *cj_response = NULL;
    cJSON *cj_name = NULL;

    printf("test_drivers_list_name_edge_cases\n");

    memcpy(table, edge_table, sizeof(table));
    table[3].name = too_long;

    fakes_reset();
    fake_table = table;
    fake_table_rows = 4;

    cj_response = run_drivers_list();

    ok(4.0 == number_in_result(cj_response, "count"),
       "all four rows are reported");

    /* A missing name is null, not "". The two mean different things to a
    ** client: one is a driver whose name this image does not know, the other is
    ** a driver named the empty string. */
    cj_name = member(driver_at(cj_response, 0), "name");
    ok(cJSON_IsNull(cj_name), "a row with no name reports null");

    /* All padding and nothing else trims to empty. Not null -- the table does
    ** hold a name here, it is just made entirely of alignment. */
    cj_name = member(driver_at(cj_response, 1), "name");
    ok(cJSON_IsString(cj_name) && (0 == strcmp("", cj_name->valuestring)),
       "a name that is only padding trims to the empty string");

    ok((NULL != driver_name_at(cj_response, 2)) &&
           (0 == strcmp("unpadded", driver_name_at(cj_response, 2))),
       "a name with nothing to trim is passed through unchanged");

    /* The case the bound exists for, and the direction it fails in. A truncated
    ** driver name is one nobody can recognise and no client can match; a padded
    ** one is merely untidy. */
    ok((NULL != driver_name_at(cj_response, 3)) &&
           (0 == strcmp(too_long, driver_name_at(cj_response, 3))),
       "a name too long to trim is reported whole, padding and all");
    ok((NULL != driver_name_at(cj_response, 3)) &&
           (strlen(too_long) == strlen(driver_name_at(cj_response, 3))),
       "and specifically is not truncated to the bound");

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief An image with every driver switched off.
 *
 * Reachable: the 63 switches in ezlopi_core_device_macros.h can all be zero,
 * and such a build is a legitimate thing to make. The answer is an empty list,
 * not an error and not a crash.
 */
static void test_drivers_list_on_an_empty_table(void)
{
    cJSON *cj_response = NULL;

    printf("test_drivers_list_on_an_empty_table\n");

    fakes_reset();
    fake_table_rows = 0;

    cj_response = run_drivers_list();

    ok(NULL != drivers_of(cj_response), "\"drivers\" is still an array");
    ok(0 == cJSON_GetArraySize(drivers_of(cj_response)), "and it is empty");
    ok(0.0 == number_in_result(cj_response, "count"), "\"count\" is 0");
    ok(NULL == error_of(cj_response),
       "a build with no drivers is not an error");

    /* One call, which returns false. Finding the end of an empty table must not
    ** take a second look, and must not take none. */
    ok(1 == driver_at_calls, "the table is probed exactly once");

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief The walk starts at 0, steps by one, and stops one past the end.
 *
 * The port's contract is that an index past the end answers false, because the
 * device table's sentinel row lives inside the array and indexing past it reads
 * memory the table does not own. A handler that skipped a row, asked twice, or
 * kept going would produce a response that can still look plausible, so the
 * indices are checked rather than the output.
 */
static void test_drivers_list_walks_the_table_once(void)
{
    cJSON *cj_response = NULL;
    int i = 0;

    printf("test_drivers_list_walks_the_table_once\n");

    fakes_reset();
    cj_response = run_drivers_list();

    ok((fake_table_rows + 1) == driver_at_calls,
       "one call per row, plus one to discover the end");

    for (i = 0; i < driver_at_calls; i++)
    {
        ok(i == driver_at_indices[i], "indices are 0, 1, 2 ... with no gaps");
    }

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief Every driver the list offers is one .set will accept.
 *
 * The check with the most teeth in this file, because it is the whole reason
 * the method exists. A list that disagrees with the write path is worse than no
 * list: it invites a client to offer the user a driver, send a configuration
 * naming it, and be refused -- and the user has no way to tell whether they
 * made a mistake.
 *
 * Both directions. Everything enumerated is accepted, and an id_item that is
 * not enumerated is refused, so a list that simply reported every number would
 * fail the second half.
 */
static void test_drivers_list_agrees_with_the_write_path(void)
{
    cJSON *cj_response = NULL;
    int i = 0;

    printf("test_drivers_list_agrees_with_the_write_path\n");

    fakes_reset();
    cj_response = run_drivers_list();

    for (i = 0; i < fake_table_rows; i++)
    {
        cJSON *cj_id = member(driver_at(cj_response, i), "id_item");
        char request[256];
        cJSON *cj_set = NULL;

        if (false == cJSON_IsNumber(cj_id))
        {
            ok(false, "an enumerated entry has an id_item to test");
            continue;
        }

        snprintf(request, sizeof(request),
                 "{\"method\":\"hub.ezlopi.config.set\",\"params\":"
                 "{\"config\":{\"chipset\":\"" THIS_CHIP "\","
                 "\"dev_detail\":[{\"device_id\":\"d\",\"id_item\":%d}]}}}",
                 (int)cj_id->valuedouble);

        fakes_reset();
        cj_set = run_set(request);
        ok(NULL == error_of(cj_set),
           "a configuration naming an enumerated driver is accepted");
        cJSON_Delete(__FUNCTION__, cj_set);

        fakes_reset();
    }

    cJSON_Delete(__FUNCTION__, cj_response);

    /* The other direction, with an id_item the table does not hold. Without
    ** this, a handler that enumerated every id from 0 upwards would pass
    ** everything above. */
    fakes_reset();
    cj_response = run_drivers_list();
    {
        cJSON *cj_entry = NULL;
        bool found_7777 = false;
        int n = 0;

        for (n = 0; NULL != (cj_entry = driver_at(cj_response, n)); n++)
        {
            cJSON *cj_id = member(cj_entry, "id_item");

            if (cJSON_IsNumber(cj_id) && (7777.0 == cj_id->valuedouble))
            {
                found_7777 = true;
            }
        }

        ok(false == found_7777, "an unbuildable id_item is not in the list");
    }
    cJSON_Delete(__FUNCTION__, cj_response);

    fakes_reset();
    cj_response = run_set(REQUEST_WITH(
        CONFIG_WITH("{\"device_id\":\"d\",\"id_item\":7777}")));
    ok(NULL != error_of(cj_response),
       "and a configuration naming it is refused");
    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief The whole list fits the buffer the transport will serialise it into.
 *
 * ezpi_respond_cjson() prints a response with cJSON_PrintPreallocated() into
 * the one shared buffer from EZPI_core_buffer_acquire(), which is
 * CONFIG_EZPI_CORE_STATIC_BUFFER_SIZE bytes -- 10240 by default. That call
 * returns false if the document does not fit, and the caller sends nothing:
 * the response is dropped in silence, with the client left waiting.
 *
 * So the size of this response is a real constraint, and the failure mode is
 * invisible. This builds the worst case the real table can produce -- every
 * driver present, every name the full padded width -- and asserts it fits with
 * margin. If a future image grows the table past what the buffer holds, this
 * fails on a host instead of a hub going quiet.
 */
static void test_drivers_list_fits_the_transport(void)
{
    /* Named here rather than read from sdkconfig, which a host build has no
    ** access to. The Kconfig default is 10240; this is checked against the
    ** smaller figure so a build that lowered it is still covered by some
    ** margin. */
#define TRANSPORT_BUFFER 10240u

    /* 36 characters, the exact padded width every name in the real table uses,
    ** with no spaces so nothing is trimmed away -- the worst case for size. */
    static const char *widest = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

    struct fake_driver_row big[200];
    cJSON *cj_response = NULL;
    char *serialised = NULL;
    size_t len = 0u;
    int i = 0;

    printf("test_drivers_list_fits_the_transport\n");

    /* 63 rows is the real table with every switch on. */
    for (i = 0; i < 63; i++)
    {
        big[i].name = widest;
        big[i].id = (uint32_t)(i + 1);
    }

    fakes_reset();
    fake_table = big;
    fake_table_rows = 63;

    cj_response = run_drivers_list();

    ok(63.0 == number_in_result(cj_response, "count"),
       "a full table is reported in full");

    serialised = cJSON_PrintUnformatted(__FUNCTION__, cj_response);

    if (NULL == serialised)
    {
        printf("  FATAL: could not serialise the response\n");
        exit(2);
    }

    len = strlen(serialised);
    printf("  (a 63-driver list serialises to %u bytes; the transport buffer "
           "is %u)\n",
           (unsigned)len, (unsigned)TRANSPORT_BUFFER);

    ok(len < TRANSPORT_BUFFER,
       "the full driver list fits the shared response buffer");

    /* Half the buffer, so this is a warning shot rather than a cliff edge: the
    ** list has room to roughly double before anything breaks, and this check
    ** says so while there is still room to do something about it. */
    ok(len < (TRANSPORT_BUFFER / 2u),
       "and does so with room for the table to grow");

    free(serialised);
    cJSON_Delete(__FUNCTION__, cj_response);

#undef TRANSPORT_BUFFER
}

/**
 * @brief Pin the cjext lookup defect that member() exists to avoid.
 *
 * See the long comment on member(). This does not test the subject; it tests
 * the assumption the subject's response format is safe under, so that the
 * assumption cannot rot silently.
 *
 * Two things are asserted. First, that requests -- which always arrive parsed
 * -- are looked up correctly, because that is what makes the handler's own
 * input lookups sound and is the reason this defect is documented rather than
 * worked around in the subject. Second, that a *built* tree is still
 * prefix-ambiguous, which is why this file reads responses with member().
 *
 * If cjext is fixed so add_item_to_object stores the inclusive length, the
 * second check fails. That is the intended outcome: it is a prompt to drop
 * member() and the comment explaining it, not a regression. The failure
 * message says so.
 */
static void test_cjext_prefix_lookup_is_still_broken(void)
{
    cJSON *cj_built = cJSON_CreateObject(__FUNCTION__);
    cJSON *cj_parsed = must_parse("{\"config\":null,\"configSize\":7}");
    cJSON *cj_found = NULL;

    printf("test_cjext_prefix_lookup_is_still_broken\n");

    if (NULL == cj_built)
    {
        printf("  FATAL: out of memory\n");
        exit(2);
    }

    cJSON_AddNullToObject(__FUNCTION__, cj_built, "config");
    cJSON_AddNumberToObject(__FUNCTION__, cj_built, "configSize", 7);

    /* The half that matters for the subject: parsed input is looked up
    ** correctly, so every lookup the handler performs on a request is sound. */
    cj_found = cJSON_GetObjectItem(__FUNCTION__, cj_parsed, "configSize");
    ok(cJSON_IsNumber(cj_found),
       "cjext looks up a prefix-shadowed name correctly on a PARSED tree");

    /* The half that explains member(). Not an assertion that the defect is
    ** desirable -- an assertion that it is still there. */
    cj_found = cJSON_GetObjectItem(__FUNCTION__, cj_built, "configSize");
    ok((NULL != cj_found) && (NULL != cj_found->string) &&
           (0 == strcmp(cj_found->string, "config")),
       "cjext still returns \"config\" for \"configSize\" on a BUILT tree "
       "(if this fails, cjext was fixed -- delete member() and its comment)");

    /* The case-sensitive variant compares max(both lengths) and is unaffected,
    ** which is the detail that proves the diagnosis is about the comparison
    ** budget and not about the key strings. */
    cj_found = cJSON_GetObjectItemCaseSensitive(cj_built, "configSize");
    ok(cJSON_IsNumber(cj_found),
       "the case-sensitive variant is correct on a built tree");

    cJSON_Delete(__FUNCTION__, cj_built);
    cJSON_Delete(__FUNCTION__, cj_parsed);
}

/*******************************************************************************
 *                          main
 *******************************************************************************/
int main(void)
{
    printf("test_devcfg_api\n");

    test_cjext_prefix_lookup_is_still_broken();

    test_the_subject_exists();
    test_get_on_an_unconfigured_hub();
    test_get_with_a_stored_configuration();
    test_get_with_an_unreadable_field();
    test_the_request_envelope();
    test_the_validator_reasons_are_forwarded();
    test_an_id_item_with_no_driver();
    test_what_is_stored_is_what_was_sent();
    test_the_request_is_not_modified();
    test_the_size_boundary();
    test_the_two_printers_agree();
    test_a_failed_write();
    test_success_asks_for_a_reboot();
    test_the_port_edge_cases();
    test_every_error_reply_is_complete();
    test_a_long_device_list();
    test_the_two_methods_stay_in_their_lanes();

    test_drivers_list_reports_the_table();
    test_drivers_list_trims_the_padding();
    test_drivers_list_name_edge_cases();
    test_drivers_list_on_an_empty_table();
    test_drivers_list_walks_the_table_once();
    test_drivers_list_agrees_with_the_write_path();
    test_drivers_list_fits_the_transport();

    printf("%d checks, %d failures\n", checks, failures);

    return (0 == failures) ? 0 : 1;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
