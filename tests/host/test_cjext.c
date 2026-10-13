/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**
** 1. Redistributions of source code must retain the above copyright notice,
**    this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. Neither the name of the copyright holder nor the names of its
**    contributors may be used to endorse or promote products derived from
**    this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
** POSSIBILITY OF SUCH DAMAGE.
** ===========================================================================
*/

/**
 * @file    test_cjext.c
 * @brief   Pins the cJSON/cjext invariants that the firmware's JSON guards
 *          depend on.
 *
 * A large batch of crash fixes across ezlopi-cloud and ezlopi-core-scenes
 * rests on four facts about this library. None of them was written down
 * anywhere, and each had been got wrong at least once in shipped code:
 *
 *   1. cJSON_GetObjectItem returns a node for *any* JSON type, and
 *      ->valuestring is NULL on every node that is not a string. This is why
 *      "if (cj_x)" is not a sufficient guard before strtoul(cj_x->valuestring)
 *      -- 30 sites had exactly that bug, crashing on {"_id": 1} where
 *      {"_id": "1"} was expected.
 *
 *   2. ->string is the member *key*, not the value, and is non-NULL for every
 *      object member regardless of type. One guard tested ->string instead of
 *      ->valuestring and was therefore a no-op that always passed.
 *
 *   3. A borrowed cJSON_GetObjectItem pointer is still linked into its parent,
 *      and cJSON_Delete follows the ->next sibling chain. Deleting a borrowed
 *      node therefore frees it *and every sibling after it*, while the parent
 *      still points at them. This corrupted the heap on a BLE write.
 *
 *   4. Detaching an array element clears that element's ->next and shifts the
 *      remaining indices down. Deleting the current element from inside
 *      cJSON_ArrayForEach therefore both frees the node the loop is about to
 *      dereference and truncates the walk -- and an index counter that keeps
 *      incrementing afterwards addresses the wrong element.
 *
 * The tests below assert these properties structurally. They deliberately do
 * *not* perform the use-after-free itself: the point is to document the shape
 * of the hazard in a form that keeps passing, not to trip the sanitizer.
 *
 * Compiles the real cjext.c, not a copy -- see tests/host/Makefile and the
 * stub headers in tests/host/stubs.
 */

#include <stdio.h>
#include <string.h>

#include "cjext.h"

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
static unsigned int tests_run = 0;
static unsigned int tests_failed = 0;

/*******************************************************************************
 *                          Macros
 *******************************************************************************/
#define CHECK(cond, ...)                                \
    do                                                  \
    {                                                   \
        tests_run++;                                    \
        if (!(cond))                                    \
        {                                               \
            tests_failed++;                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

#define WHO "test_cjext"

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * @brief Invariant 1, object members.
 *
 * Every one of these members exists, so cJSON_GetObjectItem returns non-NULL
 * for all of them -- a pointer test cannot distinguish a string from a number.
 * Only the string member has a usable ->valuestring.
 */
static void test_valuestring_is_null_on_non_strings(void)
{
    /* The "number" case is the one that actually crashed hubs: the phone app
     * and the cloud both send ids as strings, but a hand-rolled or older
     * client sending {"_id": 1} reached strtoul(NULL). */
    const char *json = "{\"s\":\"1\",\"n\":1,\"t\":true,\"f\":false,"
                       "\"z\":null,\"a\":[],\"o\":{}}";

    cJSON *root = cJSON_Parse(WHO, json);
    CHECK(NULL != root, "fixture failed to parse");
    if (NULL == root)
    {
        return;
    }

    static const char *const keys[] = {"s", "n", "t", "f", "z", "a", "o"};
    size_t i = 0;

    for (i = 0; i < (sizeof(keys) / sizeof(keys[0])); i++)
    {
        cJSON *item = cJSON_GetObjectItem(WHO, root, keys[i]);

        /* The whole reason "if (cj_x)" is an inadequate guard. */
        CHECK(NULL != item, "GetObjectItem(\"%s\") returned NULL for a present member", keys[i]);

        if (NULL == item)
        {
            continue;
        }

        if (0 == strcmp(keys[i], "s"))
        {
            CHECK(NULL != item->valuestring, "string member has a NULL valuestring");
            CHECK(cJSON_IsString(item), "string member is not reported as a string");
            CHECK(0 == strcmp(item->valuestring, "1"), "string member value is wrong");
        }
        else
        {
            CHECK(NULL == item->valuestring, "non-string member \"%s\" has a non-NULL valuestring", keys[i]);
            CHECK(!cJSON_IsString(item), "non-string member \"%s\" reported as a string", keys[i]);
        }
    }

    /* A missing member returns NULL. This is the other half of the guard:
     * ezlopi_cloud_devices.c dereferenced the result of a lookup for "name"
     * before testing it, so a request that simply omitted the member crashed. */
    CHECK(NULL == cJSON_GetObjectItem(WHO, root, "absent"), "absent member did not return NULL");

    /* ->valuedouble is a struct field, so reading it on a non-number is a
     * garbage value rather than a crash -- which is why number handling was
     * never the source of these faults. */
    cJSON *as_string = cJSON_GetObjectItem(WHO, root, "s");
    if (NULL != as_string)
    {
        CHECK(0 == as_string->valuedouble, "valuedouble on a string should read 0");
    }

    cJSON_Delete(WHO, root);
}

/**
 * @brief Invariant 1, array elements.
 *
 * The device-group and item-group case. hub.device_group.create validates only
 * cJSON_IsArray and stores the array verbatim, so element types are entirely
 * caller-controlled -- and the result is persisted to NVS, so a bad element
 * survives a reboot and re-crashes on every scene run.
 */
static void test_array_elements_may_be_any_type(void)
{
    const char *json = "[\"1\",1,true,null,[],{}]";

    cJSON *arr = cJSON_Parse(WHO, json);
    CHECK(NULL != arr, "fixture failed to parse");
    if (NULL == arr)
    {
        return;
    }

    CHECK(cJSON_IsArray(arr), "fixture is not an array");
    CHECK(6 == cJSON_GetArraySize(arr), "fixture should hold 6 elements");

    unsigned int strings = 0;
    unsigned int null_valuestrings = 0;
    cJSON *element = NULL;

    cJSON_ArrayForEach(element, arr)
    {
        if (cJSON_IsString(element))
        {
            strings++;
            CHECK(NULL != element->valuestring, "a string element had a NULL valuestring");
        }
        else
        {
            if (NULL == element->valuestring)
            {
                null_valuestrings++;
            }
        }
    }

    CHECK(1 == strings, "expected exactly 1 string element, got %u", strings);
    CHECK(5 == null_valuestrings, "expected 5 non-string elements with NULL valuestring, got %u", null_valuestrings);

    cJSON_Delete(WHO, arr);
}

/**
 * @brief Invariant 2: ->string is the key, so testing it guards nothing.
 *
 * ezlopi_core_scenes_scripts.c tested cj_script_code->string and then used
 * cj_script_code->valuestring in a strlen and two snprintf("%s") calls. Because
 * ->string is the key and is always populated for an object member, the guard
 * passed for every input and protected nothing.
 */
static void test_string_member_is_the_key_not_the_value(void)
{
    cJSON *good = cJSON_Parse(WHO, "{\"code\":\"print(1)\"}");
    cJSON *bad = cJSON_Parse(WHO, "{\"code\":123}");

    CHECK(NULL != good && NULL != bad, "fixtures failed to parse");
    if (NULL == good || NULL == bad)
    {
        cJSON_Delete(WHO, good);
        cJSON_Delete(WHO, bad);
        return;
    }

    cJSON *good_code = cJSON_GetObjectItem(WHO, good, "code");
    cJSON *bad_code = cJSON_GetObjectItem(WHO, bad, "code");

    CHECK(NULL != good_code && NULL != bad_code, "\"code\" lookup failed");

    if (NULL != good_code && NULL != bad_code)
    {
        /* ->string holds the key in both cases. */
        CHECK(NULL != good_code->string, "->string should hold the key");
        CHECK(0 == strcmp(good_code->string, "code"), "->string should be \"code\", got \"%s\"", good_code->string);

        /* And crucially it is just as non-NULL when the value is a number --
         * so "if (x->string)" cannot tell these two fixtures apart. */
        CHECK(NULL != bad_code->string, "->string is non-NULL even for a number value");
        CHECK(0 == strcmp(bad_code->string, "code"), "->string should be \"code\" for the number case too");

        /* ->valuestring is what actually differs, and is what the code used. */
        CHECK(NULL != good_code->valuestring, "string value should have a valuestring");
        CHECK(NULL == bad_code->valuestring, "number value must have a NULL valuestring");
    }

    cJSON_Delete(WHO, good);
    cJSON_Delete(WHO, bad);
}

/**
 * @brief The type predicates tolerate NULL, so the guard idiom is safe.
 *
 * Every fix in this batch is written as "cj_x && cJSON_IsString(cj_x)" or as a
 * bare "cJSON_IsString(element)" inside a walk. The bare form is only correct
 * because the predicate itself handles NULL (cjext.c:3420).
 */
static void test_type_predicates_are_null_safe(void)
{
    CHECK(!cJSON_IsString(NULL), "cJSON_IsString(NULL) must be false");
    CHECK(!cJSON_IsArray(NULL), "cJSON_IsArray(NULL) must be false");
    CHECK(!cJSON_IsObject(NULL), "cJSON_IsObject(NULL) must be false");
    CHECK(!cJSON_IsNumber(NULL), "cJSON_IsNumber(NULL) must be false");
    CHECK(!cJSON_IsBool(NULL), "cJSON_IsBool(NULL) must be false");
    CHECK(!cJSON_IsTrue(NULL), "cJSON_IsTrue(NULL) must be false");
    CHECK(!cJSON_IsFalse(NULL), "cJSON_IsFalse(NULL) must be false");
    CHECK(!cJSON_IsRaw(NULL), "cJSON_IsRaw(NULL) must be false");
    CHECK(!cJSON_IsInvalid(NULL), "cJSON_IsInvalid(NULL) must be false");

    /* This one is not idle box-ticking: the vendored copy had been changed to
     * return *true* here, making it the only predicate in the file where
     * "absent" and "JSON null" were indistinguishable. Because
     * cJSON_GetObjectItem yields NULL for a missing member,
     * cJSON_IsNull(cJSON_GetObjectItem(o, "k")) reported "k is null" for an
     * object with no "k" whatsoever. This check is what caught it; keep it. */
    CHECK(!cJSON_IsNull(NULL), "cJSON_IsNull(NULL) must be false, like every other predicate");
}

/**
 * @brief Invariant 3: a borrowed member pointer is still linked into the tree.
 *
 * ezpi_serv_ble_factory_reset_write_func called cJSON_Delete on the result of
 * cJSON_GetObjectItem. Since cJSON_Delete walks ->next, that freed the
 * borrowed node *and every member after it*, leaving the parent pointing at
 * freed memory before it was itself deleted.
 *
 * Asserted structurally: the borrowed node's ->next really is the next
 * sibling, and the parent really still reaches it. The destructive delete is
 * not performed.
 */
static void test_borrowed_member_is_still_linked_to_siblings(void)
{
    /* Mirrors the real payload shape: "sub_cmd" is not the last member, which
     * is what made the sibling chain matter. */
    cJSON *root = cJSON_Parse(WHO, "{\"cmd\":1,\"sub_cmd\":0,\"trailing\":2}");

    CHECK(NULL != root, "fixture failed to parse");
    if (NULL == root)
    {
        return;
    }

    cJSON *sub_cmd = cJSON_GetObjectItem(WHO, root, "sub_cmd");
    cJSON *trailing = cJSON_GetObjectItem(WHO, root, "trailing");

    CHECK(NULL != sub_cmd && NULL != trailing, "lookups failed");

    if (NULL != sub_cmd && NULL != trailing)
    {
        /* The borrowed pointer is not a copy -- it is the live node. */
        CHECK(sub_cmd->next == trailing,
              "the borrowed node's ->next must be the following sibling, "
              "which is what cJSON_Delete would have followed and freed");

        /* And the parent still owns it, so freeing via the borrowed pointer
         * leaves root->child pointing into freed memory. */
        unsigned int reachable = 0;
        cJSON *walk = root->child;
        while (NULL != walk)
        {
            if (walk == sub_cmd)
            {
                reachable++;
            }
            walk = walk->next;
        }
        CHECK(1 == reachable, "the borrowed node must still be reachable from the parent");
    }

    /* Deleting root alone is correct and sufficient: it owns the whole tree.
     * If this leaked or double-freed, ASan would fail the run. */
    cJSON_Delete(WHO, root);
}

/**
 * @brief Invariant 4: detaching clears ->next and shifts the later indices.
 *
 * Both halves of the expression-id removal bug. The loop deleted the current
 * element from inside cJSON_ArrayForEach, whose advance step is
 * "element = element->next" (cjext.h:246):
 *
 *   - ->next on the removed node is NULL, so the walk ended at the first match
 *     even though the loop's own comment said it was handling duplicates;
 *   - the surviving elements shift down, so an index that keeps incrementing
 *     afterwards names the wrong element on a second match.
 *
 * Detach rather than delete is used here so the node can be inspected without
 * reading freed memory -- cJSON_DeleteItemFromArray is just detach plus free
 * (cjext.c:2652), so this exercises the same unlink path.
 */
static void test_detach_clears_next_and_shifts_indices(void)
{
    cJSON *arr = cJSON_Parse(WHO, "[\"aa\",\"bb\",\"cc\"]");

    CHECK(NULL != arr, "fixture failed to parse");
    if (NULL == arr)
    {
        return;
    }

    cJSON *middle = cJSON_GetArrayItem(arr, 1);
    CHECK(NULL != middle, "index 1 lookup failed");

    if (NULL != middle)
    {
        CHECK(NULL != middle->next, "index 1 should have a following sibling before detaching");

        cJSON *detached = cJSON_DetachItemViaPointer(WHO, arr, middle);
        CHECK(detached == middle, "detach should return the same node");

        /* The truncation half: ArrayForEach would stop dead here. */
        CHECK(NULL == detached->next, "a detached node's ->next must be NULL (cjext.c:2635)");
        CHECK(NULL == detached->prev, "a detached node's ->prev must be NULL");

        /* The off-by-one half: "cc" has moved from index 2 to index 1. */
        CHECK(2 == cJSON_GetArraySize(arr), "array should hold 2 elements after the detach");

        cJSON *now_at_1 = cJSON_GetArrayItem(arr, 1);
        CHECK(NULL != now_at_1, "index 1 should still exist after the detach");

        if (NULL != now_at_1 && cJSON_IsString(now_at_1))
        {
            CHECK(0 == strcmp(now_at_1->valuestring, "cc"),
                  "the element after the removed one must shift down to its index, got \"%s\"",
                  now_at_1->valuestring);
        }

        cJSON_Delete(WHO, detached);
    }

    cJSON_Delete(WHO, arr);
}

/**
 * @brief str_value_len is not the length of the string, and the two parsers
 *        do not agree about it either.
 *
 * This is pinned down here because it is a trap rather than a feature, and
 * because the whole firmware is written around it without ever saying so.
 *
 * cJSON_Parse goes through parse_string(), which sets str_value_len from an
 * *allocation* calculation -- the span from the opening quote to the closing
 * quote, less any escape backslashes (cjext.c:723-726). For a string with no
 * escapes that is strlen + 1. cJSON_CreateString sets it from strlen()
 * (cjext.c:2890), so an item built in code and an item parsed off the wire
 * report different numbers for the same text. cJSON_ParseWithRef subtracts the
 * extra one (cjext.c:1154) and additionally leaves valuestring pointing into
 * the payload, terminated by a quote and not a NUL.
 *
 * Nothing in the firmware noticed, because str_value_len is only ever used
 * where an over-estimate is harmless: as a precision in "%.*s", as a length
 * handed to strncmp, or as a buffer size. All of those stop at the first NUL.
 * The tree's convention is visible in EZPI_STRNCMP_IF_EQUAL's own note in
 * ezlopi_cloud_constants.h ("Add '+1' ... consider the '\0' char also") and in
 * every caller that pairs str_value_len with strlen(literal) + 1.
 *
 * Code that does arithmetic on it does notice. A handler that bounds-checked
 * an incoming field with it rejected a value of exactly the maximum length as
 * one byte too long, and would have written its terminator one byte past a
 * buffer sized for the maximum. That is the bug this test exists to stop
 * someone rediscovering, and the reason the comment at
 * ezlopi_core_api.c:89 warns against switching to the reference parser.
 *
 * These checks assert current behaviour, not desired behaviour. If cjext is
 * ever made consistent, this test should fail -- and then every
 * str_value_len + 1 pairing in the tree has to be revisited in the same
 * commit.
 */
static void test_str_value_len_includes_the_terminator(void)
{
    const char *const text = "{\"k\":\"abcd\"}";

    cJSON *parsed = cJSON_Parse(WHO, text);

    CHECK(NULL != parsed, "fixture failed to parse");

    if (NULL != parsed)
    {
        cJSON *value = cJSON_GetObjectItem(WHO, parsed, "k");

        CHECK(NULL != value, "member lookup failed");

        if (NULL != value)
        {
            CHECK(4u == strlen(value->valuestring),
                  "the parsed value is NUL-terminated and four characters long");

            /* The claim itself. */
            CHECK(5u == value->str_value_len,
                  "a parsed 4-character string reports str_value_len 5, got %u",
                  (unsigned)value->str_value_len);
        }

        cJSON_Delete(WHO, parsed);
    }

    /* The other construction path, for the same four characters. */
    cJSON *created = cJSON_CreateString(WHO, "abcd");

    CHECK(NULL != created, "CreateString failed");

    if (NULL != created)
    {
        CHECK(4u == created->str_value_len,
              "a string built with cJSON_CreateString reports strlen, got %u",
              (unsigned)created->str_value_len);

        cJSON_Delete(WHO, created);
    }

    /* And an escape, where the over-estimate is larger than one: the source
     * spans six characters between the quotes, two of which are a backslash
     * pair that becomes a single byte. */
    cJSON *escaped = cJSON_Parse(WHO, "{\"k\":\"a\\tbcd\"}");

    CHECK(NULL != escaped, "escaped fixture failed to parse");

    if (NULL != escaped)
    {
        cJSON *value = cJSON_GetObjectItem(WHO, escaped, "k");

        CHECK(NULL != value, "escaped member lookup failed");

        if (NULL != value)
        {
            CHECK(5u == strlen(value->valuestring),
                  "the unescaped value is five bytes, got %u",
                  (unsigned)strlen(value->valuestring));
            CHECK(6u == value->str_value_len,
                  "and still reports one more than that, got %u",
                  (unsigned)value->str_value_len);
        }

        cJSON_Delete(WHO, escaped);
    }
}

/*******************************************************************************
 *                          Public Function Definitions
 *******************************************************************************/
int main(void)
{
    /* Unbuffered, not merely line-buffered, because this binary is normally run
     * with its stdout redirected into a pipe, where libc switches to block
     * buffering. A sanitizer abort terminates without flushing, so any FAIL line
     * already printed but still sitting in the buffer is lost, and the run looks
     * like a silent crash with no diagnostic at all.
     *
     * This is not hypothetical -- it is how a mutation test behaved. Deleting
     * "item->next = NULL" from cJSON_DetachItemViaPointer makes
     * test_detach_clears_next_and_shifts_indices report its FAIL correctly, and
     * then abort seconds later in its own cleanup, because deleting the detached
     * node walks the stale ->next and frees a node the parent still owns. With
     * block buffering that FAIL never reached the pipe, so the mutation appeared
     * to be caught only by the crash and not by the assertion written for it. */
    setvbuf(stdout, NULL, _IONBF, 0);

    test_valuestring_is_null_on_non_strings();
    test_array_elements_may_be_any_type();
    test_string_member_is_the_key_not_the_value();
    test_type_predicates_are_null_safe();
    test_borrowed_member_is_still_linked_to_siblings();
    test_detach_clears_next_and_shifts_indices();
    test_str_value_len_includes_the_terminator();

    printf("%u check(s), %u failure(s)\n", tests_run, tests_failed);

    return (0 == tests_failed) ? 0 : 1;
}
