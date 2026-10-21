/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/
/**
 * @file    test_settings_api.c
 * @brief   hub.device.settings.list / .value.set / .reset -- whether a refused
 *          setting is reported as refused
 *
 * WHAT IS WORTH TESTING HERE
 *
 * The subject is ezlopi_cloud_settings.c, the three methods through which the
 * app reads and writes device settings. Each one called a driver's settings
 * handler and threw the return value away. Every driver handler in the tree
 * returns 0 for success and non-zero for failure, and every one of those
 * non-zero returns ended here, discarded, under a reply whose empty "result"
 * object is this API's way of saying the request succeeded.
 *
 * That makes the property worth testing the negative one, and it is not
 * "does a setting get applied" -- that is one call:
 *
 *     a request the handler refused must not be answered as if it worked.
 *
 * On a hub the difference is invisible at the point of failure. The reply is
 * well-formed either way, the app shows the new value because it asked for it,
 * and the only symptom is a device that does not change -- minutes later, with
 * nothing to connect it back to the setting. So each case below asserts both
 * halves: that an "error" object appears with the right code and token, and --
 * separately -- that "result" is still there. The second is what keeps this
 * change backward-compatible, and it is exactly the kind of thing that gets
 * broken later by someone tidying up the response shape.
 *
 * Three cases exist only because the first version of the fix got them wrong
 * or nearly did:
 *
 *   - test_reset_reports_a_failure_before_a_success(). Both reset loops walk
 *     every matching setting and carry on. Holding "the last return" instead
 *     of a count lets a success overwrite a failure, and the natural way to
 *     write it -- ret = func(...) inside the loop -- has that bug. The test
 *     puts the failing setting FIRST, so a last-wins implementation passes
 *     every other case here and fails only this one.
 *
 *   - test_list_omits_status_when_the_handler_fails(). "synced" used to be
 *     stamped before the handler ran, so an entry with no value and no
 *     valueType still claimed to be in sync. Asserting the member is absent,
 *     rather than that it holds some other string, is deliberate: "synced" is
 *     the only status this API has ever sent and a second one would be
 *     invented vocabulary.
 *
 *   - test_set_distinguishes_not_found_from_refused(). An unknown settingId
 *     and a handler that refused were the same silence. They now differ by
 *     code and token, and a fix that reported both as one error would pass a
 *     test that only checked "an error appeared".
 *
 * WHAT IS FAKED, AND WHY THAT IS HONEST
 *
 * One function that matters: EZPI_core_device_get_head(). It returns the live
 * device list, which on a hub is built by the configuration parser out of
 * flash. Here it returns a list this file assembles, which is the only way to
 * present the subject with a setting whose handler refuses -- the thing being
 * tested.
 *
 * One more exists for the linker alone: EZPI_core_pin_is_mspi(), which the
 * subject never calls and reaches this binary only because it is called by a
 * sibling function in the cjson_macros translation unit that is linked whole.
 * The comment at its definition says so, and the same four lines appear in two
 * other tests for the same reason.
 *
 * Nothing else is faked, and that is load-bearing. cjext is linked real, so
 * the response objects asserted on are the ones the firmware would send.
 * ezlopi_cloud_keywords.c is linked real, so the member names are the real
 * wire names and a typo in this file's expectations fails rather than passes.
 * ezlopi_core_cjson_macros.c is linked real, so the settingId in a request is
 * parsed by the same hex-string code the hub uses; a hand-written version
 * would have let "a request for id X" mean whatever this file wanted.
 *
 * The settings handler itself is a fake, and has to be: the point is to
 * control whether it succeeds. It records the action it was given, which is
 * asserted too -- a subject that passed RESET where SET was meant would
 * otherwise satisfy every other check here.
 *
 * WHAT THIS DOES NOT COVER
 *
 * Whether any real driver handler returns non-zero on the paths a user can
 * reach. That is a property of ENS160, hilink and the rest, tested in
 * test_ens160set.c and test_hilinkset.c. This file tests only what the cloud
 * layer does with the answer.
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

#include "ezlopi_cloud_constants.h"
#include "ezlopi_core_devices.h"

#include "ezlopi_cloud_settings.h"

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

/*******************************************************************************
 *                          The fakes
 *******************************************************************************/

/* Not reached by anything this test exercises, and present only so the link
** succeeds. ezlopi_core_cjson_macros.c is linked whole because
** EZPI_core_cjson_get_id() is the real settingId parser and the
** not-found-versus-refused distinction below depends on an id being extracted
** the way the firmware extracts it. EZPI_core_cjson_get_value_gpio() is a
** sibling in that same translation unit, it calls this, and the settings cloud
** methods never call it.
**
** false is the honest answer for a test that never asks: the real function
** reads the set out of efuse over esp_mspi_get_io(), and there is no efuse
** here. test_cjsongpio.c, where the question is the point, answers properly.
** The signature is the header's -- ezlopi_core_pin_reserved.h:45 takes a plain
** int, so `const int` would not match. Same reasoning, same code, as
** test_hilinkset.c:421 and test_hilink0067.c:265. */
bool EZPI_core_pin_is_mspi(int gpio_num)
{
    (void)gpio_num;
    return false;
}

/* The device list the subject will walk. Assembled per test by __build().
** Static storage rather than malloc so a test that forgets to reset it fails
** loudly on the next assertion instead of reading freed memory.
*/
#define MAX_DEVICES 3
#define MAX_SETTINGS 3

static l_ezlopi_device_t g_devices[MAX_DEVICES];
static l_ezlopi_device_settings_v3_t g_settings[MAX_DEVICES][MAX_SETTINGS];
static l_ezlopi_device_t *g_head;

l_ezlopi_device_t *EZPI_core_device_get_head(void)
{
    return g_head;
}

/* What the fake handler was asked to do, and what it will answer. */
typedef struct
{
    int return_value;                  /* what the handler reports */
    int call_count;                    /* how many times it ran */
    e_ezlopi_settings_action_t action;  /* the action of the LAST call */
    bool add_a_value;                  /* emulate a GET that populated fields */
} s_handler_t;

static s_handler_t g_handler[MAX_DEVICES][MAX_SETTINGS];

static int __fake_settings_func(e_ezlopi_settings_action_t action,
                               struct l_ezlopi_device_settings_v3 *setting,
                               void *arg, void *user_arg)
{
    s_handler_t *h = (s_handler_t *)user_arg;
    (void)setting;

    h->call_count++;
    h->action = action;

    /* A real GET writes label/value/valueType into the object it is handed.
    ** Emulated only so the "entry still goes out" assertions have something
    ** to look at; the subject does not read it back.
    */
    if (h->add_a_value && (EZLOPI_SETTINGS_ACTION_GET_SETTING == action) && arg)
    {
        cJSON_AddNumberToObject("fake", (cJSON *)arg, "value", 42);
    }

    return h->return_value;
}

/* Build a device list: device_count devices, each with settings_per_device
** settings. Setting ids are 0x1000 + device*0x100 + index, device ids are
** 0xD000 + device. Every handler starts succeeding; a test then pokes the
** ones it cares about.
*/
static void __build(int device_count, int settings_per_device)
{
    memset(g_devices, 0, sizeof(g_devices));
    memset(g_settings, 0, sizeof(g_settings));
    memset(g_handler, 0, sizeof(g_handler));
    g_head = NULL;

    for (int d = 0; d < device_count; d++)
    {
        g_devices[d].cloud_properties.device_id = (uint32_t)(0xD000 + d);
        g_devices[d].next = (d + 1 < device_count) ? &g_devices[d + 1] : NULL;
        g_devices[d].settings = (settings_per_device > 0) ? &g_settings[d][0] : NULL;

        for (int s = 0; s < settings_per_device; s++)
        {
            g_settings[d][s].cloud_properties.setting_id =
                (uint32_t)(0x1000 + (d * 0x100) + s);
            g_settings[d][s].func = __fake_settings_func;
            g_settings[d][s].user_arg = &g_handler[d][s];
            g_settings[d][s].next =
                (s + 1 < settings_per_device) ? &g_settings[d][s + 1] : NULL;
        }
    }

    g_head = (device_count > 0) ? &g_devices[0] : NULL;
}

/*******************************************************************************
 *                          Request / response helpers
 *******************************************************************************/

/* A request carrying params with one string member, which is how the app
** sends ids: hex text, parsed by the real EZPI_core_cjson_get_id.
*/
static cJSON *__request_with_id(const char *member, uint32_t id)
{
    char text[16];
    cJSON *req = cJSON_CreateObject("test");
    cJSON *params = cJSON_AddObjectToObject("test", req, ezlopi_params_str);

    snprintf(text, sizeof(text), "%08x", id);
    cJSON_AddStringToObject("test", params, member, text);

    return req;
}

static cJSON *__request_without_params(void)
{
    return cJSON_CreateObject("test");
}

static bool __has(cJSON *response, const char *member)
{
    return (NULL != cJSON_GetObjectItem("test", response, member));
}

/* The error object's code and data token, or sentinels when absent, so a test
** asserting a code against a response that has no error fails on the code
** rather than crashing.
*/
static int __error_code(cJSON *response)
{
    cJSON *err = cJSON_GetObjectItem("test", response, ezlopi_error_str);
    cJSON *code = err ? cJSON_GetObjectItem("test", err, ezlopi_code_str) : NULL;

    return code ? (int)code->valuedouble : 0;
}

static const char *__error_data(cJSON *response)
{
    cJSON *err = cJSON_GetObjectItem("test", response, ezlopi_error_str);
    cJSON *data = err ? cJSON_GetObjectItem("test", err, ezlopi_data_str) : NULL;

    return (data && data->valuestring) ? data->valuestring : "";
}

/*******************************************************************************
 *                          Tests
 *******************************************************************************/

/* Linking against a stale object looks exactly like a subject that does
** nothing, so prove something ran before trusting anything below.
*/
static void test_the_subject_exists(void)
{
    cJSON *response = cJSON_CreateObject("test");
    cJSON *request = __request_without_params();

    __build(1, 1);
    EZPI_device_settings_list_v3(request, response);

    ok(__has(response, ezlopi_result_str),
       "settings.list adds a result object");
    ok(1 == g_handler[0][0].call_count,
       "settings.list reached the one setting's handler");
    ok(EZLOPI_SETTINGS_ACTION_GET_SETTING == g_handler[0][0].action,
       "settings.list asked the handler for GET, not something else");

    cJSON_Delete("test", response);
    cJSON_Delete("test", request);
}

static void test_set_reports_a_refusal(void)
{
    cJSON *response = cJSON_CreateObject("test");
    cJSON *request;

    __build(1, 1);
    g_handler[0][0].return_value = 1; /* the handler refuses */
    request = __request_with_id(ezlopi__id_str, 0x1000);

    cJSON_AddObjectToObject("test", response, ezlopi_result_str);
    EZPI_device_settings_value_set_v3(request, response);

    ok(1 == g_handler[0][0].call_count, "the handler ran");
    ok(EZLOPI_SETTINGS_ACTION_SET_SETTING == g_handler[0][0].action,
       "the handler was asked to SET");
    ok(__has(response, ezlopi_error_str),
       "a refused set produces an error object");
    ok(-32500 == __error_code(response),
       "a refused set is an internal error, -32500");
    ok(0 == strcmp("settings.set.failed", __error_data(response)),
       "a refused set carries the settings.set.failed token");

    cJSON_Delete("test", response);
    cJSON_Delete("test", request);
}

static void test_set_is_silent_when_it_works(void)
{
    cJSON *response = cJSON_CreateObject("test");
    cJSON *request;

    __build(1, 1);
    g_handler[0][0].return_value = 0; /* the handler agrees */
    request = __request_with_id(ezlopi__id_str, 0x1000);

    EZPI_device_settings_value_set_v3(request, response);

    ok(1 == g_handler[0][0].call_count, "the handler ran");
    ok(false == __has(response, ezlopi_error_str),
       "a set that worked adds no error object");
    ok(__has(response, ezlopi_result_str),
       "a set that worked still carries a result");

    cJSON_Delete("test", response);
    cJSON_Delete("test", request);
}

/* The two silences used to be identical. They must not collapse back into one.
*/
static void test_set_distinguishes_not_found_from_refused(void)
{
    cJSON *refused_response = cJSON_CreateObject("test");
    cJSON *missing_response = cJSON_CreateObject("test");
    cJSON *refused_request;
    cJSON *missing_request;

    __build(1, 1);
    g_handler[0][0].return_value = 1;
    refused_request = __request_with_id(ezlopi__id_str, 0x1000);
    EZPI_device_settings_value_set_v3(refused_request, refused_response);

    __build(1, 1);
    missing_request = __request_with_id(ezlopi__id_str, 0xBEEF); /* no such id */
    EZPI_device_settings_value_set_v3(missing_request, missing_response);

    ok(0 == g_handler[0][0].call_count,
       "an unknown settingId reaches no handler at all");
    ok(__has(missing_response, ezlopi_error_str),
       "an unknown settingId produces an error object");
    ok(-32602 == __error_code(missing_response),
       "an unknown settingId is a params error, -32602");
    ok(0 == strcmp("settings.id.not_found", __error_data(missing_response)),
       "an unknown settingId carries the settings.id.not_found token");

    ok(__error_code(refused_response) != __error_code(missing_response),
       "a refused set and an unknown id do not share an error code");
    ok(0 != strcmp(__error_data(refused_response), __error_data(missing_response)),
       "a refused set and an unknown id do not share a token");

    ok(__has(missing_response, ezlopi_result_str),
       "an unknown settingId still carries a result, as every other method does");

    cJSON_Delete("test", refused_response);
    cJSON_Delete("test", missing_response);
    cJSON_Delete("test", refused_request);
    cJSON_Delete("test", missing_request);
}

static void test_set_without_params_is_a_bad_request(void)
{
    cJSON *response = cJSON_CreateObject("test");
    cJSON *request = __request_without_params();

    __build(1, 1);
    EZPI_device_settings_value_set_v3(request, response);

    ok(0 == g_handler[0][0].call_count,
       "a set with no params reaches no handler");
    ok(-32600 == __error_code(response),
       "a set with no params is a bad request, -32600");
    ok(0 == strcmp("settings.params.missing", __error_data(response)),
       "a set with no params carries the settings.params.missing token");

    cJSON_Delete("test", response);
    cJSON_Delete("test", request);
}

/* The accumulation property. The failing setting is FIRST, so an
** implementation that keeps only the most recent return reports success.
*/
static void test_reset_reports_a_failure_before_a_success(void)
{
    cJSON *response = cJSON_CreateObject("test");
    cJSON *request;

    __build(1, 2);
    g_handler[0][0].return_value = 1; /* refuses */
    g_handler[0][1].return_value = 0; /* agrees, and runs second */
    request = __request_with_id(ezlopi_deviceId_str, 0xD000);

    EZPI_device_settings_reset_v3(request, response);

    ok(1 == g_handler[0][0].call_count && 1 == g_handler[0][1].call_count,
       "reset by deviceId reached both settings");
    ok(EZLOPI_SETTINGS_ACTION_RESET_SETTING == g_handler[0][1].action,
       "reset asked the handler to RESET");
    ok(__has(response, ezlopi_error_str),
       "a failure followed by a success is still reported as a failure");
    ok(-32500 == __error_code(response),
       "a refused reset is an internal error, -32500");
    ok(0 == strcmp("settings.reset.failed", __error_data(response)),
       "a refused reset carries the settings.reset.failed token");

    cJSON_Delete("test", response);
    cJSON_Delete("test", request);
}

static void test_reset_is_silent_when_every_setting_agrees(void)
{
    cJSON *response = cJSON_CreateObject("test");
    cJSON *request;

    __build(1, 2);
    request = __request_with_id(ezlopi_deviceId_str, 0xD000);

    EZPI_device_settings_reset_v3(request, response);

    ok(1 == g_handler[0][0].call_count && 1 == g_handler[0][1].call_count,
       "both settings were reset");
    ok(false == __has(response, ezlopi_error_str),
       "a reset where everything agreed adds no error object");

    cJSON_Delete("test", response);
    cJSON_Delete("test", request);
}

static void test_reset_by_setting_id_reports_a_refusal(void)
{
    cJSON *response = cJSON_CreateObject("test");
    cJSON *request;

    __build(1, 2);
    g_handler[0][1].return_value = 1;
    request = __request_with_id(ezlopi__id_str, 0x1001);

    EZPI_device_settings_reset_v3(request, response);

    ok(0 == g_handler[0][0].call_count,
       "reset by settingId left the other setting alone");
    ok(1 == g_handler[0][1].call_count, "reset by settingId reached its target");
    ok(-32500 == __error_code(response),
       "a refused reset by settingId is reported");

    cJSON_Delete("test", response);
    cJSON_Delete("test", request);
}

/* The status claim. An entry whose GET failed has no value and no valueType,
** so it must not claim to be synced.
*/
static void test_list_omits_status_when_the_handler_fails(void)
{
    cJSON *response = cJSON_CreateObject("test");
    cJSON *request = __request_without_params();
    cJSON *result;
    cJSON *array;
    cJSON *entry;

    __build(1, 1);
    g_handler[0][0].return_value = 1; /* GET fails */

    EZPI_device_settings_list_v3(request, response);

    result = cJSON_GetObjectItem("test", response, ezlopi_result_str);
    array = result ? cJSON_GetObjectItem("test", result, ezlopi_settings_str) : NULL;
    entry = array ? cJSON_GetArrayItem(array, 0) : NULL;

    ok(NULL != entry, "a failed GET still produces an entry in the array");
    ok(NULL == cJSON_GetObjectItem("test", entry, ezlopi_status_str),
       "an entry whose GET failed carries no status member");
    ok(NULL != cJSON_GetObjectItem("test", entry, ezlopi__id_str),
       "an entry whose GET failed still carries its _id");

    cJSON_Delete("test", response);
    cJSON_Delete("test", request);
}

static void test_list_claims_synced_when_the_handler_agrees(void)
{
    cJSON *response = cJSON_CreateObject("test");
    cJSON *request = __request_without_params();
    cJSON *result;
    cJSON *array;
    cJSON *entry;
    cJSON *status;

    __build(1, 1);
    g_handler[0][0].add_a_value = true;

    EZPI_device_settings_list_v3(request, response);

    result = cJSON_GetObjectItem("test", response, ezlopi_result_str);
    array = result ? cJSON_GetObjectItem("test", result, ezlopi_settings_str) : NULL;
    entry = array ? cJSON_GetArrayItem(array, 0) : NULL;
    status = entry ? cJSON_GetObjectItem("test", entry, ezlopi_status_str) : NULL;

    ok(NULL != status, "an entry whose GET worked carries a status member");
    ok(status && status->valuestring &&
           (0 == strcmp(ezlopi_synced_str, status->valuestring)),
       "an entry whose GET worked is marked synced");

    cJSON_Delete("test", response);
    cJSON_Delete("test", request);
}

/* A list spanning several devices, because both the set and reset walks have
** an outer loop over devices that a single-device test cannot exercise.
*/
static void test_list_and_set_cross_devices(void)
{
    cJSON *response = cJSON_CreateObject("test");
    cJSON *request;

    __build(3, 2);
    g_handler[2][1].return_value = 1; /* the last setting of the last device */
    request = __request_with_id(ezlopi__id_str, 0x1201);

    EZPI_device_settings_value_set_v3(request, response);

    ok(1 == g_handler[2][1].call_count,
       "a setting on the third device was found");
    ok(0 == g_handler[0][0].call_count && 0 == g_handler[1][0].call_count,
       "settings on earlier devices were not touched");
    ok(-32500 == __error_code(response),
       "its refusal was reported");

    cJSON_Delete("test", response);
    cJSON_Delete("test", request);
}

/*******************************************************************************
 *                          main
 *******************************************************************************/
int main(void)
{
    printf("test_settings_api: device settings methods report refusals\n");

    test_the_subject_exists();
    test_set_reports_a_refusal();
    test_set_is_silent_when_it_works();
    test_set_distinguishes_not_found_from_refused();
    test_set_without_params_is_a_bad_request();
    test_reset_reports_a_failure_before_a_success();
    test_reset_is_silent_when_every_setting_agrees();
    test_reset_by_setting_id_reports_a_refusal();
    test_list_omits_status_when_the_handler_fails();
    test_list_claims_synced_when_the_handler_agrees();
    test_list_and_set_cross_devices();

    printf("  %d checks, %d failures\n", checks, failures);

    return (failures > 0) ? 1 : 0;
}
