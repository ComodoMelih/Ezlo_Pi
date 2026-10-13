/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    test_mqtt_api.c
 * @brief   Host tests for the hub.mqtt.config.* API methods.
 *
 * WHAT THIS COVERS THAT test_mqttcfg.c DOES NOT
 *
 * test_mqttcfg.c sweeps the validators: which URIs and prefixes are
 * well formed. This file tests the layer above -- the part that decides, given
 * a JSON request, *which* settings get written and whether any get written at
 * all. The validators being right does not make that part right.
 *
 * THE CHECK THAT MATTERS
 *
 * test_a_rejected_request_writes_nothing(). The set method takes several
 * fields at once, so the obvious implementation walks the request storing each
 * field as it goes -- and then a request with a good URI and a bad topic
 * prefix leaves the URI written and the prefix not. The user sees an error,
 * assumes nothing happened, and the device reboots onto a half-applied
 * configuration.
 *
 * The implementation validates every supplied field before storing any of
 * them. That is a property of the control flow, invisible in a passing manual
 * test, and it is what these checks pin: the store fakes below count calls, and
 * the count after a rejected request must be zero.
 *
 * THE CHECK THAT WOULD HAVE CAUGHT A SHORTCUT
 *
 * test_overlong_values_are_rejected_not_truncated(). The tree already has
 * EZPI_core_cjson_get_value_string_by_copy(), which is what one would reach
 * for to pull a string out of params. It uses snprintf, so it *truncates*. A
 * 200-character URI truncated to 128 characters is still a syntactically valid
 * URI, so it would pass validation and be stored -- the device would end up
 * pointed at an address nobody typed, with a success reply. The same helper
 * also cannot tell an absent field from an empty one, because it leaves the
 * caller's buffer untouched in both cases, and "" is a meaningful value for
 * three of these five fields. Both are why the implementation reads cJSON
 * directly instead.
 *
 * WHAT IS FAKED, AND WHY THAT IS HONEST
 *
 * The seven EZPI_core_mqtt_config_* functions are replaced with recording
 * fakes. NVS cannot run on a host, and the point here is not whether NVS
 * works -- it is which calls the handler decides to make. The fakes record
 * every call so the tests can assert on exactly that, including the negative
 * case of no call at all. ezlopi-util-mqttcfg is the real implementation, not
 * a fake, so a value this file accepts is a value the firmware accepts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjext.h"
#include "ezlopi_cloud_mqtt_config.h"
#include "ezlopi_core_mqtt_config.h"
#include "ezlopi_util_mqttcfg.h"

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

/*******************************************************************************
 *                          The cloud keyword strings
 *
 * The real definitions live in ezlopi_cloud_keywords.c, which also defines
 * several hundred others. Defining the six this file's subject actually uses
 * is cheaper than linking that, and the spellings are not invented here -- they
 * are the values from ezlopi_cloud_keywords.c lines 103, 104, 117, 178, 179 and
 * 180. If one of them changed upstream this file would still link, which is
 * why the request/response shape is asserted below rather than assumed.
 *******************************************************************************/
const char *ezlopi_result_str = "result";
const char *ezlopi_params_str = "params";
const char *ezlopi_error_str = "error";
const char *ezlopi_code_str = "code";
const char *ezlopi_data_str = "data";
const char *ezlopi_message_str = "message";

/*******************************************************************************
 *                          Recording fakes for the store
 *******************************************************************************/

typedef struct s_store_call
{
    char field[32];
    char value[EZPI_MQTTCFG_URI_MAX + 1u];
} s_store_call_t;

#define MAX_CALLS 16

static s_store_call_t calls[MAX_CALLS];
static size_t call_count = 0u;
static size_t reset_count = 0u;

/* When true, every setter reports a storage failure, so the "the write did not
 * stick" branch can be reached without an NVS. */
static bool fail_every_store = false;

/* What the fake get() hands back. */
static s_ezpi_mqtt_config_t fake_current;

static void calls_clear(void)
{
    memset(calls, 0, sizeof(calls));
    call_count = 0u;
    reset_count = 0u;
    fail_every_store = false;
}

static void record(const char *field, const char *value)
{
    if (call_count < MAX_CALLS)
    {
        snprintf(calls[call_count].field, sizeof(calls[call_count].field), "%s",
                 field);
        snprintf(calls[call_count].value, sizeof(calls[call_count].value), "%s",
                 value);
    }

    call_count++;
}

/** @brief Did the handler store this field with this exact value? */
static bool stored(const char *field, const char *value)
{
    size_t i;

    for (i = 0u; (i < call_count) && (i < MAX_CALLS); i++)
    {
        if ((0 == strcmp(calls[i].field, field)) &&
            (0 == strcmp(calls[i].value, value)))
        {
            return true;
        }
    }

    return false;
}

static ezlopi_error_t fake_store(const char *field, const char *value)
{
    record(field, value);
    return fail_every_store ? EZPI_ERR_NVS_WRITE_FAILED : EZPI_SUCCESS;
}

ezlopi_error_t EZPI_core_mqtt_config_set_uri(const char *uri)
{
    return fake_store("uri", uri);
}

ezlopi_error_t EZPI_core_mqtt_config_set_username(const char *username)
{
    return fake_store("username", username);
}

ezlopi_error_t EZPI_core_mqtt_config_set_password(const char *password)
{
    return fake_store("password", password);
}

ezlopi_error_t EZPI_core_mqtt_config_set_topic_prefix(const char *prefix)
{
    return fake_store("topicPrefix", prefix);
}

ezlopi_error_t EZPI_core_mqtt_config_set_discovery_prefix(const char *prefix)
{
    return fake_store("discoveryPrefix", prefix);
}

ezlopi_error_t EZPI_core_mqtt_config_reset(void)
{
    reset_count++;
    return EZPI_SUCCESS;
}

ezlopi_error_t EZPI_core_mqtt_config_get(s_ezpi_mqtt_config_t *out)
{
    if (NULL == out)
    {
        return EZPI_ERR_WRONG_PARAM;
    }

    *out = fake_current;
    return EZPI_SUCCESS;
}

/*******************************************************************************
 *                          Request / response helpers
 *******************************************************************************/

/**
 * @brief Run hub.mqtt.config.set against a JSON request given as text.
 *
 * The request is parsed from text rather than built with cJSON calls so that
 * the tests below read as the thing a client actually sends. Returns the
 * response, which the caller deletes.
 */
static cJSON *run_set(const char *request_json)
{
    cJSON *cj_request = cJSON_Parse(__FUNCTION__, request_json);
    cJSON *cj_response = cJSON_CreateObject(__FUNCTION__);

    /* A parse failure here would make every assertion about the response
     * vacuous, so it is a check of its own rather than an early return. */
    ok(NULL != cj_request, "the test's own request JSON parses");

    EZPI_CLOUD_mqtt_config_set(cj_request, cj_response);

    cJSON_Delete(__FUNCTION__, cj_request);

    return cj_response;
}

static bool has_error(cJSON *cj_response)
{
    return (NULL !=
            cJSON_GetObjectItem(__FUNCTION__, cj_response, ezlopi_error_str));
}

static bool has_result_field(cJSON *cj_response, const char *name)
{
    cJSON *cj_result =
        cJSON_GetObjectItem(__FUNCTION__, cj_response, ezlopi_result_str);

    return (cj_result &&
            (NULL != cJSON_GetObjectItem(__FUNCTION__, cj_result, name)));
}

/*******************************************************************************
 *                          The tests
 *******************************************************************************/

/**
 * @brief Prove the subject is actually compiled before testing it.
 *
 * ezlopi_cloud_mqtt_config.c is entirely inside #ifdef
 * CONFIG_EZPI_ENABLE_MQTT. If the stub sdkconfig.h stopped defining that macro
 * the file would become empty, the link would still succeed against the fakes
 * in this file, and every check below would pass while testing nothing. This
 * check fails in that case instead.
 */
static void test_the_subject_exists(void)
{
    printf("-- the subject is compiled in --\n");

    calls_clear();

    cJSON *cj_response = run_set("{\"params\":{\"uri\":\"mqtt://h:1883\"}}");

    /* An empty translation unit cannot have stored anything. */
    ok(1u == call_count, "the real handler ran, not an empty stub");
    ok(stored("uri", "mqtt://h:1883"), "and it stored the URI it was given");

    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief The headline property: a refused request leaves the store untouched.
 */
static void test_a_rejected_request_writes_nothing(void)
{
    printf("-- a rejected request writes nothing --\n");

    /* A good URI first, a wildcard prefix second. Walking the fields in order
     * and storing as it goes would write the URI before noticing the prefix.
     * The wildcard is the dangerous one: the service subscribes to
     * <prefix>/<node>/+/set and acts on what arrives there. */
    calls_clear();
    cJSON *r1 = run_set("{\"params\":{\"uri\":\"mqtt://10.0.0.5:1883\","
                        "\"topicPrefix\":\"ezlopi/+\"}}");
    ok(has_error(r1), "a wildcard topic prefix is refused");
    ok(0u == call_count, "and the valid URI alongside it was NOT stored");
    cJSON_Delete(__FUNCTION__, r1);

    /* The same the other way round, so the result does not depend on the bad
     * field coming second. */
    calls_clear();
    cJSON *r2 = run_set("{\"params\":{\"topicPrefix\":\"ezlopi/+\","
                        "\"uri\":\"mqtt://10.0.0.5:1883\"}}");
    ok(has_error(r2), "refused with the bad field first too");
    ok(0u == call_count, "and still nothing was stored");
    cJSON_Delete(__FUNCTION__, r2);

    /* Four good fields and one bad one. */
    calls_clear();
    cJSON *r3 = run_set("{\"params\":{\"uri\":\"mqtt://10.0.0.5:1883\","
                        "\"username\":\"ha\",\"password\":\"s3cret\","
                        "\"topicPrefix\":\"ezlopi\","
                        "\"discoveryPrefix\":\"home#assistant\"}}");
    ok(has_error(r3), "a wildcard in the discovery prefix is refused");
    ok(0u == call_count, "and none of the four good fields was stored");
    cJSON_Delete(__FUNCTION__, r3);

    /* A malformed URI, which is the typo people actually make. */
    calls_clear();
    cJSON *r4 = run_set("{\"params\":{\"uri\":\"192.168.1.10:1883\","
                        "\"username\":\"ha\"}}");
    ok(has_error(r4), "a URI with no scheme is refused");
    ok(0u == call_count, "and the username alongside it was not stored");
    cJSON_Delete(__FUNCTION__, r4);
}

/**
 * @brief Over-long values must be refused, not quietly shortened.
 */
static void test_overlong_values_are_rejected_not_truncated(void)
{
    printf("-- over-long values are rejected, not truncated --\n");

    /* A URI far past the limit whose first EZPI_MQTTCFG_URI_MAX characters are
     * a perfectly valid URI. Truncating would produce a value that passes
     * validation and gets stored, so the only thing standing between the user
     * and a device pointed somewhere they never typed is the refusal. */
    char host[256];
    char request[512];
    size_t i;

    for (i = 0u; i < (sizeof(host) - 1u); i++)
    {
        host[i] = 'a';
    }
    host[sizeof(host) - 1u] = '\0';

    snprintf(request, sizeof(request), "{\"params\":{\"uri\":\"mqtt://%s\"}}",
             host);

    /* Confirm the premise rather than trusting it: the truncation this guards
     * against is only dangerous because the short form IS valid.
     *
     * Built with memcpy rather than snprintf because snprintf truncating into
     * a smaller buffer is exactly what is being demonstrated, and
     * -Werror=format-truncation refuses to compile the demonstration. Doing it
     * by hand also says plainly that the truncation is the subject here and
     * not an accident. */
    char truncated[EZPI_MQTTCFG_URI_MAX + 1u];
    memcpy(truncated, request + strlen("{\"params\":{\"uri\":\""),
           EZPI_MQTTCFG_URI_MAX);
    truncated[EZPI_MQTTCFG_URI_MAX] = '\0';

    ok(EZPI_MQTTCFG_URI_MAX == strlen(truncated),
       "the truncated form is exactly the maximum length");
    ok(EZPI_util_mqttcfg_uri_is_valid(truncated),
       "the truncated form of the over-long URI would have been accepted");

    calls_clear();
    cJSON *r1 = run_set(request);
    ok(has_error(r1), "the over-long URI is refused");
    ok(0u == call_count, "and no truncated version of it was stored");
    cJSON_Delete(__FUNCTION__, r1);

    /* Exactly at the limit must still be accepted, or the bound is off by one
     * in the safe-looking direction and nobody would notice. */
    char at_limit[EZPI_MQTTCFG_URI_MAX + 1u];
    memset(at_limit, 0, sizeof(at_limit));
    memcpy(at_limit, "mqtt://", 7u);
    for (i = 7u; i < EZPI_MQTTCFG_URI_MAX; i++)
    {
        at_limit[i] = 'b';
    }

    ok(EZPI_MQTTCFG_URI_MAX == strlen(at_limit),
       "the at-limit URI really is the maximum length");

    snprintf(request, sizeof(request), "{\"params\":{\"uri\":\"%s\"}}",
             at_limit);

    calls_clear();
    cJSON *r2 = run_set(request);
    ok(!has_error(r2), "a URI of exactly the maximum length is accepted");
    ok(stored("uri", at_limit), "and stored in full");
    cJSON_Delete(__FUNCTION__, r2);

    /* One character more. */
    char over_by_one[EZPI_MQTTCFG_URI_MAX + 2u];
    memcpy(over_by_one, at_limit, EZPI_MQTTCFG_URI_MAX);
    over_by_one[EZPI_MQTTCFG_URI_MAX] = 'b';
    over_by_one[EZPI_MQTTCFG_URI_MAX + 1u] = '\0';

    snprintf(request, sizeof(request), "{\"params\":{\"uri\":\"%s\"}}",
             over_by_one);

    calls_clear();
    cJSON *r3 = run_set(request);
    ok(has_error(r3), "one character over the maximum is refused");
    ok(0u == call_count, "and nothing stored");
    cJSON_Delete(__FUNCTION__, r3);
}

/**
 * @brief An absent field is left alone; an empty one is a value.
 */
static void test_absent_is_not_the_same_as_empty(void)
{
    printf("-- absent is not the same as empty --\n");

    /* Only the URI supplied: the other four must not be touched. A
     * configuration screen that saves one field should not blank the rest. */
    calls_clear();
    cJSON *r1 = run_set("{\"params\":{\"uri\":\"mqtt://broker.local:1883\"}}");
    ok(!has_error(r1), "a single-field request succeeds");
    ok(1u == call_count, "exactly one field was stored");
    ok(stored("uri", "mqtt://broker.local:1883"), "and it was the URI");
    cJSON_Delete(__FUNCTION__, r1);

    /* An empty URI is how MQTT is switched off again without reflashing, so it
     * has to reach the store as "" rather than be mistaken for absent. */
    calls_clear();
    cJSON *r2 = run_set("{\"params\":{\"uri\":\"\"}}");
    ok(!has_error(r2), "an empty URI is accepted");
    ok(stored("uri", ""), "and clears the setting rather than being ignored");
    cJSON_Delete(__FUNCTION__, r2);

    /* Same for the two credentials: clearing them is how a user moves to an
     * anonymous broker. */
    calls_clear();
    cJSON *r3 = run_set("{\"params\":{\"username\":\"\",\"password\":\"\"}}");
    ok(!has_error(r3), "empty credentials are accepted");
    ok(stored("username", ""), "username cleared");
    ok(stored("password", ""), "password cleared");
    cJSON_Delete(__FUNCTION__, r3);

    /* But a prefix has no empty form -- every topic is built from it, and ""
     * would produce topics starting with '/'. */
    calls_clear();
    cJSON *r4 = run_set("{\"params\":{\"topicPrefix\":\"\"}}");
    ok(has_error(r4), "an empty topic prefix is refused");
    ok(0u == call_count, "and not stored");
    cJSON_Delete(__FUNCTION__, r4);

    calls_clear();
    cJSON *r5 = run_set("{\"params\":{\"discoveryPrefix\":\"\"}}");
    ok(has_error(r5), "an empty discovery prefix is refused");
    ok(0u == call_count, "and not stored");
    cJSON_Delete(__FUNCTION__, r5);
}

/**
 * @brief Everything supplied together is stored together.
 */
static void test_a_full_valid_request_stores_everything(void)
{
    printf("-- a full valid request stores everything --\n");

    calls_clear();
    cJSON *r = run_set("{\"params\":{\"uri\":\"mqtts://ha.lan:8883\","
                       "\"username\":\"ezlopi\",\"password\":\"p@ss w0rd\","
                       "\"topicPrefix\":\"ezlopi/hub1\","
                       "\"discoveryPrefix\":\"homeassistant\"}}");

    ok(!has_error(r), "the request succeeds");
    ok(5u == call_count, "all five fields were stored");
    ok(stored("uri", "mqtts://ha.lan:8883"), "uri");
    ok(stored("username", "ezlopi"), "username");
    /* A space inside a password is legitimate and is the kind of thing an
     * over-eager validator refuses. */
    ok(stored("password", "p@ss w0rd"), "password, spaces and all");
    ok(stored("topicPrefix", "ezlopi/hub1"), "topic prefix with a separator");
    ok(stored("discoveryPrefix", "homeassistant"), "discovery prefix");

    /* The settings are stored but the running client keeps its old broker, and
     * the reply has to say so or a UI will imply the move already happened. */
    ok(has_result_field(r, "rebootRequired"),
       "the result says a reboot is required");

    cJSON_Delete(__FUNCTION__, r);
}

/**
 * @brief Requests that are not well formed get an error, not a silent success.
 */
static void test_malformed_requests(void)
{
    printf("-- malformed requests --\n");

    /* No params at all. */
    calls_clear();
    cJSON *r1 = run_set("{\"method\":\"hub.mqtt.config.set\"}");
    ok(has_error(r1), "a request with no params is an error");
    ok(0u == call_count, "and stores nothing");
    cJSON_Delete(__FUNCTION__, r1);

    /* Params present but empty. The likeliest cause is a misspelled field
     * name, and answering "success" would send the caller looking anywhere
     * except at the spelling. */
    calls_clear();
    cJSON *r2 = run_set("{\"params\":{}}");
    ok(has_error(r2), "empty params is an error rather than a no-op success");
    ok(0u == call_count, "and stores nothing");
    cJSON_Delete(__FUNCTION__, r2);

    /* Only unrecognised keys -- i.e. a typo in every field name. */
    calls_clear();
    cJSON *r3 = run_set("{\"params\":{\"brokerUri\":\"mqtt://h:1883\"}}");
    ok(has_error(r3), "a request naming no known field is an error");
    ok(0u == call_count, "and stores nothing");
    cJSON_Delete(__FUNCTION__, r3);

    /* Right field name, wrong JSON type. A client that sends a port as a
     * number, or a null for "unset", must not be read as a string. */
    calls_clear();
    cJSON *r4 = run_set("{\"params\":{\"uri\":1883}}");
    ok(has_error(r4), "a numeric uri is refused");
    ok(0u == call_count, "and stores nothing");
    cJSON_Delete(__FUNCTION__, r4);

    calls_clear();
    cJSON *r5 = run_set("{\"params\":{\"uri\":null}}");
    ok(has_error(r5), "a null uri is refused");
    ok(0u == call_count, "and stores nothing");
    cJSON_Delete(__FUNCTION__, r5);

    calls_clear();
    cJSON *r6 = run_set("{\"params\":{\"username\":true}}");
    ok(has_error(r6), "a boolean username is refused");
    ok(0u == call_count, "and stores nothing");
    cJSON_Delete(__FUNCTION__, r6);

    calls_clear();
    cJSON *r7 = run_set("{\"params\":{\"topicPrefix\":{\"a\":1}}}");
    ok(has_error(r7), "an object topic prefix is refused");
    ok(0u == call_count, "and stores nothing");
    cJSON_Delete(__FUNCTION__, r7);
}

/**
 * @brief A storage failure is reported, not swallowed.
 */
static void test_a_storage_failure_is_reported(void)
{
    printf("-- a storage failure is reported --\n");

    calls_clear();
    fail_every_store = true;

    cJSON *r = run_set("{\"params\":{\"uri\":\"mqtt://h:1883\"}}");

    ok(has_error(r), "a failed write produces an error reply");
    ok(!has_result_field(r, "rebootRequired"),
       "and does not claim a reboot would apply anything");

    cJSON_Delete(__FUNCTION__, r);
}

/**
 * @brief hub.mqtt.config.get reports the settings and never the password.
 */
static void test_get_never_returns_the_password(void)
{
    printf("-- get never returns the password --\n");

    memset(&fake_current, 0, sizeof(fake_current));
    snprintf(fake_current.uri, sizeof(fake_current.uri), "%s",
             "mqtt://10.0.0.9:1883");
    snprintf(fake_current.username, sizeof(fake_current.username), "%s", "ha");
    snprintf(fake_current.password, sizeof(fake_current.password), "%s",
             "TOPSECRET");
    snprintf(fake_current.topic_prefix, sizeof(fake_current.topic_prefix), "%s",
             "ezlopi");
    snprintf(fake_current.discovery_prefix,
             sizeof(fake_current.discovery_prefix), "%s", "homeassistant");

    cJSON *cj_request = cJSON_CreateObject(__FUNCTION__);
    cJSON *cj_response = cJSON_CreateObject(__FUNCTION__);

    EZPI_CLOUD_mqtt_config_get(cj_request, cj_response);

    ok(!has_error(cj_response), "get succeeds");
    ok(has_result_field(cj_response, "uri"), "the URI is reported");
    ok(has_result_field(cj_response, "username"), "the username is reported");
    ok(has_result_field(cj_response, "topicPrefix"),
       "the topic prefix is reported");
    ok(has_result_field(cj_response, "discoveryPrefix"),
       "the discovery prefix is reported");

    /* The two halves of the password rule. */
    ok(!has_result_field(cj_response, "password"),
       "there is no password field in the result");
    ok(has_result_field(cj_response, "passwordSet"),
       "but whether one is set is reported");

    /* Belt and braces: serialise the whole response and look for the secret
     * anywhere in it. A field named something other than "password" that
     * happened to carry the value would pass the check above and fail this
     * one. */
    char *printed = cJSON_Print(__FUNCTION__, cj_response);
    ok(NULL != printed, "the response serialises");
    if (printed)
    {
        ok(NULL == strstr(printed, "TOPSECRET"),
           "the password appears nowhere in the serialised response");
        free(printed);
    }

    cJSON_Delete(__FUNCTION__, cj_request);
    cJSON_Delete(__FUNCTION__, cj_response);

    /* And with no password set, passwordSet must be false rather than absent,
     * so a UI can tell "none" from "unchanged". */
    memset(&fake_current.password, 0, sizeof(fake_current.password));

    cj_request = cJSON_CreateObject(__FUNCTION__);
    cj_response = cJSON_CreateObject(__FUNCTION__);

    EZPI_CLOUD_mqtt_config_get(cj_request, cj_response);

    cJSON *cj_result =
        cJSON_GetObjectItem(__FUNCTION__, cj_response, ezlopi_result_str);
    cJSON *cj_set = cj_result ? cJSON_GetObjectItem(__FUNCTION__, cj_result,
                                                    "passwordSet")
                              : NULL;

    ok(NULL != cj_set, "passwordSet is present when no password is set");
    ok((NULL != cj_set) && (false == cJSON_IsTrue(cj_set)),
       "and reads false");

    cJSON_Delete(__FUNCTION__, cj_request);
    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief hub.mqtt.config.reset clears the lot.
 */
static void test_reset(void)
{
    printf("-- reset --\n");

    calls_clear();

    cJSON *cj_request = cJSON_CreateObject(__FUNCTION__);
    cJSON *cj_response = cJSON_CreateObject(__FUNCTION__);

    EZPI_CLOUD_mqtt_config_reset(cj_request, cj_response);

    ok(!has_error(cj_response), "reset succeeds");
    ok(1u == reset_count, "and calls through to the store exactly once");
    ok(0u == call_count,
       "without writing individual fields on the way");
    ok(has_result_field(cj_response, "rebootRequired"),
       "and reports that a reboot is required");

    cJSON_Delete(__FUNCTION__, cj_request);
    cJSON_Delete(__FUNCTION__, cj_response);
}

/**
 * @brief The shipped defaults survive a round trip through the setters.
 *
 * A stock device reports its compiled-in prefixes from get(). If those values
 * were ones set() refused, a user could open a configuration screen, press
 * save without touching anything, and get an error.
 */
static void test_the_shipped_defaults_are_settable(void)
{
    printf("-- the shipped defaults can be set back --\n");

    calls_clear();
    cJSON *r = run_set("{\"params\":{\"topicPrefix\":\"ezlopi\","
                       "\"discoveryPrefix\":\"homeassistant\"}}");

    ok(!has_error(r), "saving the shipped defaults unchanged succeeds");
    ok(stored("topicPrefix", "ezlopi"), "the default topic prefix");
    ok(stored("discoveryPrefix", "homeassistant"),
       "the default discovery prefix");

    cJSON_Delete(__FUNCTION__, r);
}

int main(void)
{
    printf("=== hub.mqtt.config.* API methods ===\n");

    test_the_subject_exists();
    test_a_rejected_request_writes_nothing();
    test_overlong_values_are_rejected_not_truncated();
    test_absent_is_not_the_same_as_empty();
    test_a_full_valid_request_stores_everything();
    test_malformed_requests();
    test_a_storage_failure_is_reported();
    test_get_never_returns_the_password();
    test_reset();
    test_the_shipped_defaults_are_settable();

    printf("\n%d check(s), %d failure(s)\n", checks, failures);

    if (failures)
    {
        printf("MQTT API TESTS FAILED\n");
        return 1;
    }

    printf("MQTT API TESTS PASSED\n");
    return 0;
}
