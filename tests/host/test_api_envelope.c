/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/
/**
 * @file    test_api_envelope.c
 * @brief   EZPI_core_api_consume_cjson() -- the reply envelope every local API
 *          call is wrapped in, and the one member a client reads first.
 *
 * WHY THIS FILE EXISTS
 *
 * Every method in this firmware answers through one function. It takes
 * whatever the handler built and adds the four members a caller correlates and
 * checks against: id, sender, method, error. Nothing in the tree tested it,
 * and the handlers underneath it were tested in isolation -- so a defect in
 * the envelope was a defect in all 200-odd methods at once, invisible to every
 * per-method test.
 *
 * There was one. The error member used to be added unconditionally:
 *
 *     cj_response = __execute_method(cj_request, method);
 *     if (cj_response)
 *     {
 *         cJSON_AddNullToObject(__FUNCTION__, cj_response, ezlopi_error_str);
 *
 * and cjext's add_item_to_object() ends in add_item_to_array(). It appends.
 * There is no replace and no duplicate check -- see add_item_to_array()'s own
 * "append to the end" at cjext.c:2479, and the test for it in test_cjext.c.
 * (This read "around line 2386", which was already stale before this commit:
 * written as prose rather than as "cjext.c:N", it was invisible to every sweep
 * that renumbered the citations around it.) So a handler that had already
 * reported a failure produced a reply with two members of that name:
 *
 *     {"result":{...},"error":{"code":-32602,...},"error":null}
 *
 * Which one a reader sees is decided by its parser, and the two common answers
 * disagree. cjext's own get_object_item() returns the first match, so every C
 * caller inside this firmware saw the failure and the bug stayed invisible.
 * JavaScript's JSON.parse keeps the last duplicate, as do Python's json and
 * Go's encoding/json. A browser therefore read every single error reply from
 * this API as a success with error === null.
 *
 * That direction is the one that now matters, because the local UI this
 * firmware serves is a browser. A configuration the device refused would have
 * been reported to the user as saved.
 *
 * WHAT IS ASSERTED, AND WHY IN THIS SHAPE
 *
 * The specific regression is checked both ways round -- one error member
 * holding the object on a failure, one error member holding null on a success
 * -- because a fix that simply stopped adding the null would pass the first
 * check and silently break every client that tests `error === null`.
 *
 * Underneath that is the general invariant, and it is the more valuable of the
 * two: NO member name may appear twice anywhere in a reply, checked
 * recursively over the whole tree. That is the property the defect violated,
 * it holds for reasons that have nothing to do with the error member, and it
 * will catch the next append-instead-of-replace without anyone having thought
 * of it in advance. assert_no_duplicate_members() therefore runs on every
 * reply this file produces, not only the two it was written for.
 *
 * WHAT IS FAKED
 *
 * The method table (four functions), the broadcast queue, the event group and
 * esp_restart(). All of them are either hardware or the rest of the firmware;
 * none of them is where the envelope is decided. The fakes record, because
 * several checks here are about which calls the subject *chooses* to make --
 * that a reply never reaches the broadcast queue, that an updater's reply
 * never reaches the caller, that a refused request runs no handler at all.
 *
 * cjext is linked real, not faked, and that is load-bearing rather than
 * convenient: the whole defect lives in cjext's append semantics, so a fake
 * JSON tree would have let this file agree with itself about what
 * cJSON_AddNullToObject does to an object that already has that key.
 *
 * EZPI_core_ezlopi_methods_rpc_method_notfound() is the one fake that mirrors
 * real logic rather than replacing it -- it has to add an error object, since
 * that is what the not-found path relies on for its reply to carry one at all.
 * It copies that function's five cJSON calls verbatim (an error object holding
 * code/data/message, then an empty result object) from
 * ezlopi_core_api_methods.c:145-156, so it can drift from the original;
 * test_notfound_reply_carries_an_error() is written to fail
 * if the real behaviour it stands for is what changes, not to prove the copy
 * is current.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cjext.h"

#include "ezlopi_core_api.h"
#include "ezlopi_core_api_methods.h"
#include "ezlopi_core_errors.h"
#include "ezlopi_core_event_group.h"

#include "ezlopi_cloud_constants.h"

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
** test_devcfg_api.c uses. */
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
 * The real definitions live in ezlopi_cloud_keywords.c among several hundred
 * others. Defining the nine this subject and these fakes use is cheaper than
 * linking that file; the spellings are the values at ezlopi_cloud_keywords.c
 * lines 69, 75, 79, 97 and 163 plus four the fake not-found handler needs.
 *******************************************************************************/
const char *ezlopi_id_str = "id";
const char *ezlopi_sender_str = "sender";
const char *ezlopi_method_str = "method";
const char *ezlopi_error_str = "error";
const char *ezlopi_null_str = "null";
const char *ezlopi_result_str = "result";
const char *ezlopi_code_str = "code";
const char *ezlopi_data_str = "data";
const char *ezlopi_message_str = "message";

/*******************************************************************************
 *                          Recording fakes
 *******************************************************************************/

/* What the method table is told to hand back for the next call. */
static uint32_t g_method_id = 0;
static f_method_func_t g_method = NULL;
static f_method_func_t g_updater = NULL;
static bool g_is_registered = false;

/* What the rest of the firmware was asked to do. */
static int g_nma_events = 0;
static int g_restarts = 0;
static int g_broadcasts = 0;
static ezlopi_error_t g_broadcast_rc = EZPI_SUCCESS;
static char g_broadcast_json[512];

/* How the handlers were called. */
static int g_handler_calls = 0;
static bool g_handler_got_null_response = false;

static void reset_fakes(void)
{
    g_method_id = 0;
    g_method = NULL;
    g_updater = NULL;
    g_is_registered = false;
    g_nma_events = 0;
    g_restarts = 0;
    g_broadcasts = 0;
    g_broadcast_rc = EZPI_SUCCESS;
    g_broadcast_json[0] = '\0';
    g_handler_calls = 0;
    g_handler_got_null_response = false;
}

uint32_t EZPI_core_ezlopi_methods_search_in_list(cJSON *cj_method)
{
    (void)cj_method;
    return g_method_id;
}

f_method_func_t EZPI_core_ezlopi_methods_get_by_id(uint32_t method_id)
{
    (void)method_id;
    return g_method;
}

f_method_func_t EZPI_core_ezlopi_methods_get_updater_by_id(uint32_t method_id)
{
    (void)method_id;
    return g_updater;
}

bool EZPI_core_ezlopi_methods_check_method_register(f_method_func_t method)
{
    (void)method;
    return g_is_registered;
}

/* Mirrors ezlopi_core_api_methods.c:145-156. See the note at the top of this
** file about why this one is a copy and what that does and does not prove. */
void EZPI_core_ezlopi_methods_rpc_method_notfound(cJSON *cj_request, cJSON *cj_response)
{
    (void)cj_request;
    g_handler_calls++;

    if (NULL == cj_response)
    {
        g_handler_got_null_response = true;
        return;
    }

    cJSON *cj_error = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_error_str);
    if (cj_error)
    {
        cJSON_AddNumberToObject(__FUNCTION__, cj_error, ezlopi_code_str, -32602);
        cJSON_AddStringToObject(__FUNCTION__, cj_error, ezlopi_data_str, "rpc.method.notfound");
        cJSON_AddStringToObject(__FUNCTION__, cj_error, ezlopi_message_str, "Unknown method");
    }

    cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);
}

/* The real queue takes ownership of the tree it is handed and the consumer
** frees it later; the subject only frees it when this returns a failure. So
** the fake has to free it on the success path or every updater check leaks,
** and the leak would be reported against the subject. Serialising first is
** what lets a check assert that the *update* reply went here rather than to
** the caller. */
ezlopi_error_t EZPI_core_broadcast_add_to_queue(cJSON *cj_data, time_t time_stamp)
{
    (void)time_stamp;
    g_broadcasts++;

    char *s = cJSON_PrintUnformatted(__FUNCTION__, cj_data);
    if (s)
    {
        snprintf(g_broadcast_json, sizeof(g_broadcast_json), "%s", s);
        ezlopi_free(__FUNCTION__, s);
    }

    if (EZPI_SUCCESS == g_broadcast_rc)
    {
        cJSON_Delete(__FUNCTION__, cj_data);
    }

    return g_broadcast_rc;
}

ezlopi_error_t EZPI_core_event_group_set_event(e_ezlopi_event_t event)
{
    if (EZLOPI_EVENT_NMA_REG == event)
    {
        g_nma_events++;
    }

    return EZPI_SUCCESS;
}

void esp_restart(void)
{
    g_restarts++;
}

/*******************************************************************************
 *                          Stand-in method handlers
 *******************************************************************************/

/* A handler that succeeded: a result and no error, which is what the great
** majority of this firmware's methods produce. */
static void handler_ok(cJSON *cj_request, cJSON *cj_response)
{
    (void)cj_request;
    g_handler_calls++;

    if (NULL == cj_response)
    {
        g_handler_got_null_response = true;
        return;
    }

    cJSON *result = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);
    cJSON_AddStringToObject(__FUNCTION__, result, "expectedChipset", "ESP32");
}

/* A handler that refused, shaped like __prepare_error() in
** ezlopi_cloud_devcfg.c: a result object, then an error object carrying a
** code, a message and a machine-readable token. This is the case the envelope
** used to corrupt. */
static void handler_fail(cJSON *cj_request, cJSON *cj_response)
{
    (void)cj_request;
    g_handler_calls++;

    if (NULL == cj_response)
    {
        g_handler_got_null_response = true;
        return;
    }

    cJSON *result = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_result_str);
    cJSON_AddStringToObject(__FUNCTION__, result, "expectedChipset", "ESP32");

    cJSON *err = cJSON_AddObjectToObject(__FUNCTION__, cj_response, ezlopi_error_str);
    cJSON_AddNumberToObject(__FUNCTION__, err, ezlopi_code_str, -32602);
    cJSON_AddStringToObject(__FUNCTION__, err, ezlopi_message_str, "Wrong params");
    cJSON_AddStringToObject(__FUNCTION__, err, ezlopi_data_str, "devcfg.chipset.mismatch");
}

/* An updater: the second function a method may register, whose reply is meant
** to be broadcast to every connected client rather than returned to the one
** that asked. */
static void updater_broadcast(cJSON *cj_request, cJSON *cj_response)
{
    (void)cj_request;

    if (NULL == cj_response)
    {
        g_handler_got_null_response = true;
        return;
    }

    cJSON_AddStringToObject(__FUNCTION__, cj_response, "updated", "yes");
}

/*******************************************************************************
 *                          Tree helpers
 *******************************************************************************/
static int count_members(cJSON *obj, const char *name)
{
    int n = 0;

    for (cJSON *c = obj ? obj->child : NULL; c; c = c->next)
    {
        if (c->string && (0 == strcmp(c->string, name)))
        {
            n++;
        }
    }

    return n;
}

/* The member a last-wins parser -- JSON.parse, Python's json, Go's
** encoding/json -- ends up with. cJSON_GetObjectItem() answers the opposite
** question, which is exactly why the duplicate was invisible from C. */
static cJSON *last_member(cJSON *obj, const char *name)
{
    cJSON *found = NULL;

    for (cJSON *c = obj ? obj->child : NULL; c; c = c->next)
    {
        if (c->string && (0 == strcmp(c->string, name)))
        {
            found = c;
        }
    }

    return found;
}

/* Returns the first member name that appears twice in the same object,
** searching the whole tree, or NULL if every object is well formed. Arrays are
** walked too: their elements have no names, but an element may be an object.
**
** O(n^2) per object on purpose. These replies have a handful of members and
** the alternative is a hash table in a test file. */
static const char *first_duplicate_member(cJSON *node)
{
    if (NULL == node)
    {
        return NULL;
    }

    if (cJSON_IsObject(node) || cJSON_IsArray(node))
    {
        for (cJSON *a = node->child; a; a = a->next)
        {
            if (a->string)
            {
                for (cJSON *b = a->next; b; b = b->next)
                {
                    if (b->string && (0 == strcmp(a->string, b->string)))
                    {
                        return a->string;
                    }
                }
            }

            const char *deeper = first_duplicate_member(a);
            if (deeper)
            {
                return deeper;
            }
        }
    }

    return NULL;
}

/* Run on every reply this file produces, not only the two the duplicate-error
** defect was found in. The point of a general invariant is that it holds for
** cases nobody thought about. */
static void assert_no_duplicate_members(cJSON *reply, const char *where)
{
    const char *dup = first_duplicate_member(reply);

    checks++;
    if (dup)
    {
        failures++;
        printf("  FAIL: %s: member \"%s\" appears twice in one object\n", where, dup);
    }
}

static const char *str_of(cJSON *obj, const char *name)
{
    cJSON *m = cJSON_GetObjectItem(__FUNCTION__, obj, name);
    return (m && m->valuestring) ? m->valuestring : NULL;
}

/* Drive the subject with a request built from JSON text, and hand back the
** reply for inspection. The caller owns the reply; the request is this
** function's business. */
static cJSON *consume(const char *request_json)
{
    cJSON *cj_request = must_parse(request_json);
    cJSON *reply = EZPI_core_api_consume_cjson(__FUNCTION__, cj_request, (time_t)1760000000);
    cJSON_Delete(__FUNCTION__, cj_request);
    return reply;
}

#define REQ_OK "{\"method\":\"hub.ezlopi.config.set\",\"id\":\"ui-7\"," \
               "\"sender\":{\"conn_id\":\"abc\"},\"params\":{}}"

/*******************************************************************************
 *                          The checks
 *******************************************************************************/

/* The regression, in the direction that was broken. */
static void test_failing_handler_yields_one_error_object(void)
{
    reset_fakes();
    g_method = handler_fail;

    cJSON *reply = consume(REQ_OK);
    ok(NULL != reply, "a failing handler still produces a reply");

    ok(1 == count_members(reply, "error"),
       "exactly one \"error\" member on a failure (two is the defect)");

    /* Both parser conventions must now agree, which is the whole point. */
    cJSON *first = cJSON_GetObjectItem(__FUNCTION__, reply, "error");
    cJSON *last = last_member(reply, "error");
    ok(first == last, "first-wins and last-wins readers see the same \"error\"");
    ok(cJSON_IsObject(first), "a browser's JSON.parse sees the error object, not null");

    /* The reason has to survive, not just the fact. A client shows this. */
    ok((NULL != first) && (NULL != str_of(first, "data")) &&
           (0 == strcmp(str_of(first, "data"), "devcfg.chipset.mismatch")),
       "the handler's error token reaches the wire");

    assert_no_duplicate_members(reply, "failing handler");
    cJSON_Delete(__FUNCTION__, reply);
}

/* The regression, in the direction a careless fix would have broken. Clients
** test `error === null`; dropping the null would have made every success look
** like a reply with no verdict in it. */
static void test_succeeding_handler_still_yields_explicit_null(void)
{
    reset_fakes();
    g_method = handler_ok;

    cJSON *reply = consume(REQ_OK);
    ok(NULL != reply, "a succeeding handler produces a reply");

    ok(1 == count_members(reply, "error"), "exactly one \"error\" member on a success");
    ok(cJSON_IsNull(cJSON_GetObjectItem(__FUNCTION__, reply, "error")),
       "a success still carries an explicit error: null");
    ok(NULL != cJSON_GetObjectItem(__FUNCTION__, reply, "result"),
       "the handler's result is left alone");

    assert_no_duplicate_members(reply, "succeeding handler");
    cJSON_Delete(__FUNCTION__, reply);
}

/* id, sender and method are what a client correlates a reply with. On a
** connection carrying several in flight, echoing the wrong one is worse than
** echoing none. */
static void test_envelope_echoes_the_request_fields(void)
{
    reset_fakes();
    g_method = handler_ok;

    cJSON *reply = consume(REQ_OK);

    ok((NULL != str_of(reply, "id")) && (0 == strcmp(str_of(reply, "id"), "ui-7")),
       "id is echoed");
    ok((NULL != str_of(reply, "method")) &&
           (0 == strcmp(str_of(reply, "method"), "hub.ezlopi.config.set")),
       "method is echoed");

    cJSON *sender = cJSON_GetObjectItem(__FUNCTION__, reply, "sender");
    ok(cJSON_IsObject(sender), "sender is echoed as an object");
    ok((NULL != str_of(sender, "conn_id")) &&
           (0 == strcmp(str_of(sender, "conn_id"), "abc")),
       "sender is echoed by value, contents intact");

    /* Echoed by duplication, not by reference: the request is freed before the
    ** reply is read, and ASan would report a use-after-free here if the
    ** subject had aliased the request's nodes into the reply. */
    assert_no_duplicate_members(reply, "echo");
    cJSON_Delete(__FUNCTION__, reply);
}

/* A reply that omits id/sender/method is a reply a UI cannot match to the call
** it made. This records what the subject does with a request that left them
** out -- it omits them rather than inventing or crashing, because cjext's
** add_item_to_object() refuses a NULL item (cjext.c:2517). Recorded rather
** than asserted as desirable: a client is on its own correlating this. */
static void test_request_without_id_does_not_crash(void)
{
    reset_fakes();
    g_method = handler_ok;

    cJSON *reply = consume("{\"method\":\"hub.info.get\"}");
    ok(NULL != reply, "a request with no id still gets a reply");
    ok(0 == count_members(reply, "id"), "no id in, no id out -- the member is omitted");
    ok(0 == count_members(reply, "sender"), "no sender in, no sender out");
    ok(1 == count_members(reply, "error"), "the error member is still there exactly once");

    assert_no_duplicate_members(reply, "no id");
    cJSON_Delete(__FUNCTION__, reply);
}

/* An unknown method must come back as an error, not as silence: the local UI
** calls methods this firmware may not have been built with. */
static void test_notfound_reply_carries_an_error(void)
{
    reset_fakes();
    g_method_id = UINT32_MAX;
    g_method = handler_ok; /* must not be reached */

    cJSON *reply = consume("{\"method\":\"hub.nonesuch\",\"id\":\"ui-9\",\"sender\":{}}");
    ok(NULL != reply, "an unknown method produces a reply");
    ok(1 == g_handler_calls, "only the not-found handler ran");
    ok(1 == count_members(reply, "error"), "exactly one \"error\" member when not found");
    ok(cJSON_IsObject(cJSON_GetObjectItem(__FUNCTION__, reply, "error")),
       "and it is the error object, so a browser sees the failure");
    ok((NULL != str_of(reply, "id")) && (0 == strcmp(str_of(reply, "id"), "ui-9")),
       "the not-found path echoes id too");

    assert_no_duplicate_members(reply, "method not found");
    cJSON_Delete(__FUNCTION__, reply);
}

/* The gate at the front: a message that is itself an error report is not a
** request, and running a handler on it would act on params that were never
** sent. This is the path cloud replies arrive on. */
static void test_request_carrying_an_error_is_refused(void)
{
    reset_fakes();
    g_method = handler_ok;

    cJSON *reply = consume("{\"method\":\"hub.info.get\",\"id\":\"x\","
                           "\"error\":{\"code\":-1,\"message\":\"nope\"}}");
    ok(NULL == reply, "a request carrying an error object gets no reply");
    ok(0 == g_handler_calls, "and no handler runs");
    cJSON_Delete(__FUNCTION__, reply);
}

/* The two spellings of "no error" that the gate accepts. The second is a JSON
** null; the third is the four-character string, which the subject compares
** through EZPI_STRNCMP_IF_EQUAL and which real senders do emit. */
static void test_the_two_spellings_of_no_error_are_accepted(void)
{
    reset_fakes();
    g_method = handler_ok;
    cJSON *reply = consume("{\"method\":\"hub.info.get\",\"id\":\"x\",\"error\":null}");
    ok(NULL != reply, "error: null is accepted as no error");
    ok(1 == g_handler_calls, "and the handler runs");
    ok(1 == count_members(reply, "error"), "the reply's own error member is not doubled");
    assert_no_duplicate_members(reply, "request error: null");
    cJSON_Delete(__FUNCTION__, reply);

    reset_fakes();
    g_method = handler_ok;
    reply = consume("{\"method\":\"hub.info.get\",\"id\":\"x\",\"error\":\"null\"}");
    ok(NULL != reply, "error: \"null\" as a string is accepted too");
    ok(1 == g_handler_calls, "and the handler runs");
    assert_no_duplicate_members(reply, "request error: \"null\"");
    cJSON_Delete(__FUNCTION__, reply);
}

/* A method the registration machinery owns is called with a NULL response
** object and its answer goes out as an event, not as a reply. Worth pinning
** because a handler written to assume a non-NULL response would dereference
** NULL here, and this is the only path that passes one. */
static void test_registered_method_gets_no_reply_and_sets_the_event(void)
{
    reset_fakes();
    g_method = handler_ok;
    g_is_registered = true;

    cJSON *reply = consume(REQ_OK);
    ok(NULL == reply, "a registration method produces no reply to the caller");
    ok(1 == g_handler_calls, "the handler still ran");
    ok(g_handler_got_null_response, "and it was handed a NULL response object");
    ok(1 == g_nma_events, "the NMA_REG event was set exactly once");
}

/* An updater's reply belongs to every client, so it must go to the broadcast
** queue and must not be merged into the answer the caller is waiting for. */
static void test_updater_reply_is_broadcast_not_returned(void)
{
    reset_fakes();
    g_method = handler_ok;
    g_updater = updater_broadcast;

    cJSON *reply = consume(REQ_OK);
    ok(NULL != reply, "the caller still gets its own reply");
    ok(1 == g_broadcasts, "the updater's reply went to the broadcast queue");
    ok(NULL != strstr(g_broadcast_json, "\"updated\""),
       "and it is the updater's tree that went there");
    ok(NULL == cJSON_GetObjectItem(__FUNCTION__, reply, "updated"),
       "the updater's fields are not merged into the caller's reply");

    /* The update reply is NOT given an envelope -- no id, no error. Recorded
    ** because it is asymmetric with the reply path above and a client reading
    ** broadcasts has to cope with it. */
    ok(NULL == strstr(g_broadcast_json, "\"error\""),
       "the broadcast tree carries no error member of its own");

    assert_no_duplicate_members(reply, "with updater");
    cJSON_Delete(__FUNCTION__, reply);

    /* And when the queue refuses it, the tree is released rather than leaked.
    ** ASan is the assertion here; the check below just records that the path
    ** was taken. */
    reset_fakes();
    g_method = handler_ok;
    g_updater = updater_broadcast;
    g_broadcast_rc = EZPI_FAILED;

    reply = consume(REQ_OK);
    ok(1 == g_broadcasts, "a refused broadcast is still attempted exactly once");
    cJSON_Delete(__FUNCTION__, reply);
}

/* The byte-oriented entry point, which parses and then delegates. Linked and
** exercised mainly so that ASan sees the request tree allocated, used and
** freed exactly once -- the reply must not alias into it. */
static void test_consume_from_bytes(void)
{
    reset_fakes();
    g_method = handler_ok;

    const char *payload = REQ_OK;
    cJSON *reply = EZPI_core_api_consume(__FUNCTION__, payload,
                                         (uint32_t)strlen(payload), (time_t)1760000000);
    ok(NULL != reply, "a well-formed payload is answered");
    ok((NULL != str_of(reply, "id")) && (0 == strcmp(str_of(reply, "id"), "ui-7")),
       "and the envelope survives the byte path");
    assert_no_duplicate_members(reply, "byte path");
    cJSON_Delete(__FUNCTION__, reply);

    reset_fakes();
    g_method = handler_ok;
    ok(NULL == EZPI_core_api_consume(__FUNCTION__, "{not json", 9, 0),
       "a payload that does not parse is answered with nothing");
    ok(0 == g_handler_calls, "and no handler runs");

    ok(NULL == EZPI_core_api_consume(__FUNCTION__, NULL, 0, 0), "NULL payload is survivable");
    ok(NULL == EZPI_core_api_consume(__FUNCTION__, payload, 0, 0), "zero length is survivable");
    ok(NULL == EZPI_core_api_consume_cjson(__FUNCTION__, NULL, 0), "NULL request is survivable");
}

/* RECORDED BEHAVIOUR, NOT ENDORSED. __check_for_no_error() counts every
** request whose method begins with "register" and reboots the device on the
** sixth, and it increments that counter whether or not the request carried an
** error -- the name __register_fail_count says failures, the code counts
** attempts. So six successful cloud registrations in one uptime reboot the
** hub, which on a link that reconnects is a reboot loop with no error anywhere
** to explain it.
**
** Left alone deliberately: changing it alters Ezlo's cloud-registration
** behaviour, which is outside the scope of an envelope fix, and the counter
** has no reset so any change needs its own thought about when to clear it.
** Pinned here so the behaviour is written down and so a future change to it is
** a deliberate one. Runs last because the counter is a file static.
*/
static void test_register_counter_counts_attempts_not_failures(void)
{
    reset_fakes();
    g_method = handler_ok;

    /* Five clean, error-free registrations. */
    for (int i = 0; i < 5; i++)
    {
        cJSON *reply = consume("{\"method\":\"register\",\"id\":\"r\",\"error\":null}");
        cJSON_Delete(__FUNCTION__, reply);
    }

    ok(0 == g_restarts, "five successful registrations do not reboot");

    cJSON *reply = consume("{\"method\":\"register\",\"id\":\"r\",\"error\":null}");
    cJSON_Delete(__FUNCTION__, reply);

    ok(1 == g_restarts,
       "the sixth does -- the counter counts register attempts, not failures");
}

/* Linking against a stale object file looks exactly like a passing suite from
** here, so one check confirms the subject was reached at all. */
static void test_the_subject_exists(void)
{
    ok(checks > 0, "something in this file actually ran");
}

int main(void)
{
    printf("=== ezlopi_core_api.c -- reply envelope ===\n");

    test_failing_handler_yields_one_error_object();
    test_succeeding_handler_still_yields_explicit_null();
    test_envelope_echoes_the_request_fields();
    test_request_without_id_does_not_crash();
    test_notfound_reply_carries_an_error();
    test_request_carrying_an_error_is_refused();
    test_the_two_spellings_of_no_error_are_accepted();
    test_registered_method_gets_no_reply_and_sets_the_event();
    test_updater_reply_is_broadcast_not_returned();
    test_consume_from_bytes();
    test_register_counter_counts_attempts_not_failures();
    test_the_subject_exists();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
