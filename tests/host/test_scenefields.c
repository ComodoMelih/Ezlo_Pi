/* Host tests for EZPI_scenes_delete_fields() / EZPI_core_scenes_delete_field_user_arg().
 *
 * What is being pinned down: `l_fields_v2_t::user_arg` is a slot shared by
 * several when-methods, and they do not put the same kind of thing in it.
 * `isInterval` stores a FreeRTOS tick count straight in the pointer;
 * `isGroupState` stores a bool; three others store an owned struct; two store
 * an owned cJSON tree. The destructor used to call free() on the slot
 * unconditionally, which is wrong for all but the flat-struct case:
 *
 *   - a tick count is not an address, so free() got a value that was never
 *     returned by the allocator;
 *   - a cJSON tree got only its root node released, leaking every child node
 *     and every key string.
 *
 * Both paths are reachable from `hub.scenes.delete`, which calls
 * EZPI_core_scenes_depopulate_by_id_v2() -> EZPI_core_scenes_delete() ->
 * EZPI_core_scenes_delete_when_blocks() -> EZPI_scenes_delete_fields().
 *
 * These tests are mostly adjudicated by the sanitizers rather than by
 * assertions, which is deliberate and is why the suite's -fsanitize flags
 * matter here: the pre-fix defects are a free() of a non-heap value (ASan
 * "attempting free on address which was not malloc()-ed", which aborts) and a
 * leak of cJSON children (LSan at exit, which sets a non-zero status). Neither
 * is visible to a plain assert, so a version of this file that only asserted
 * would pass against the broken code.
 *
 * Tests 9 to 11 are about the one thing the slot can hold that is not flat.
 * `isItemStateChanged` remembers the last value it saw in an
 * `s_item_exp_data_t`, and when that value is a string the struct owns a copy
 * of it -- two allocations behind one pointer, which is why it has a tag of
 * its own rather than sharing FIELD_USER_ARG_HEAP. Deciding which of the two
 * to release means reading the struct's own type tag, and that tag only became
 * readable when `s_item_expn_data_t` stopped being a `union` of the tag and
 * the value it describes; test 9 is what pins that down, and it is the one
 * test here an assert is the right adjudicator for, being a layout claim.
 *
 * Run: make -C tests/host build/test_scenefields && tests/host/build/test_scenefields
 * ...or `make -C tests/host test` for the whole suite. This line used to name a
 * `run-scenefields` target, which the Makefile has never had.
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
#include "ezlopi_core_scenes_delete.h"
/* For s_item_exp_data_t -- the one thing the slot can hold that is not flat. */
#include "ezlopi_core_scenes_when_methods_helper_functions.h"

static int checks = 0;
#define CHECK(cond, what)                                      \
    do                                                         \
    {                                                          \
        checks++;                                              \
        if (!(cond))                                           \
        {                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
            exit(1);                                           \
        }                                                      \
    } while (0)

/* A field as the populate path produces one: zeroed, so user_arg_type starts
 * at FIELD_USER_ARG_NONE. Allocated with malloc because ezlopi_malloc is
 * malloc on a host build (see stubs/EZLOPI_USER_CONFIG.h) and because the
 * destructor will release it with ezlopi_free. */
static l_fields_v2_t *new_field(const char *name)
{
    l_fields_v2_t *f = malloc(sizeof(l_fields_v2_t));
    assert(f);
    memset(f, 0, sizeof(l_fields_v2_t));
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->field_value.e_type = VALUE_TYPE_UNDEFINED;
    return f;
}

/* 1. The defect that corrupts the heap: a scalar in the pointer.
 *
 * isInterval stores xTaskGetTickCount(). Any plausible tick count is a small
 * non-zero integer, so the old `if (user_arg) free(user_arg)` passed the test
 * and then freed address 0x4e20. Under ASan the pre-fix code aborts here. */
static void test_scalar_user_arg_is_not_freed(void)
{
    l_fields_v2_t *f = new_field("interval");

    f->user_arg = (void *)(size_t)20000; /* a tick count, not an address */
    f->user_arg_type = FIELD_USER_ARG_NONE;

    EZPI_core_scenes_delete_field_user_arg(f);

    CHECK(NULL == f->user_arg, "scalar slot cleared");
    CHECK(FIELD_USER_ARG_NONE == f->user_arg_type, "scalar tag reset");

    /* And again through the list destructor, which is the path hub.scenes.delete
     * actually takes. */
    f->user_arg = (void *)(size_t)20001;
    f->user_arg_type = FIELD_USER_ARG_NONE;
    EZPI_scenes_delete_fields(f); /* frees f */
}

/* 2. A bool stored the same way, by the isGroupState when-method. The stored
 * value is 1, so the old code called free((void *)1). */
static void test_bool_user_arg_is_not_freed(void)
{
    l_fields_v2_t *f = new_field("groupState");
    f->user_arg = (void *)(size_t)1;
    f->user_arg_type = FIELD_USER_ARG_NONE;
    EZPI_scenes_delete_fields(f);
    checks++; /* reaching here without an ASan abort is the result */
}

/* 3. A flat struct, which is the one case the old unconditional free got right.
 * LSan catches it if the new tag-driven path skips it. */
static void test_heap_user_arg_is_freed(void)
{
    l_fields_v2_t *f = new_field("function");
    void *blk = malloc(64);
    assert(blk);
    memset(blk, 0xA5, 64);
    f->user_arg = blk;
    f->user_arg_type = FIELD_USER_ARG_HEAP;
    EZPI_scenes_delete_fields(f);
    checks++;
}

/* 4. A cJSON tree. The old free() released the root node only; the children
 * and their key strings stayed allocated. That is a leak LSan reports, so this
 * test fails against the pre-fix code by exit status rather than by assert.
 *
 * The tree is deliberately deep enough that a root-only free leaves several
 * distinct allocations behind -- a single-node object would be released almost
 * correctly by accident and would not distinguish the two implementations. */
static void test_cjson_user_arg_is_deleted(void)
{
    l_fields_v2_t *f = new_field("deviceItems");

    cJSON *root = cJSON_CreateObject(__FUNCTION__);
    assert(root);
    for (int i = 0; i < 8; i++)
    {
        char key[16];
        snprintf(key, sizeof(key), "item%08x", i);
        cJSON *child = cJSON_AddObjectToObject(__FUNCTION__, root, key);
        assert(child);
        cJSON_AddStringToObject(__FUNCTION__, child, "value", "a string long enough to be its own allocation");
        cJSON_AddNumberToObject(__FUNCTION__, child, "valueType", 3);
    }

    f->user_arg = root;
    f->user_arg_type = FIELD_USER_ARG_CJSON;

    EZPI_core_scenes_delete_field_user_arg(f);
    CHECK(NULL == f->user_arg, "cjson slot cleared");
    CHECK(FIELD_USER_ARG_NONE == f->user_arg_type, "cjson tag reset");

    EZPI_scenes_delete_fields(f);
}

/* 5. Releasing twice must be safe, because that is exactly what the sunstate
 * when-method does: it discards its own state mid-evaluation and the scene is
 * torn down later. The old code freed without clearing the pointer, so the
 * second release was a double free. */
static void test_double_release_is_safe(void)
{
    l_fields_v2_t *f = new_field("sunstate");
    void *blk = malloc(32);
    assert(blk);
    f->user_arg = blk;
    f->user_arg_type = FIELD_USER_ARG_HEAP;

    EZPI_core_scenes_delete_field_user_arg(f); /* the mid-evaluation discard */
    CHECK(NULL == f->user_arg, "slot cleared on first release");

    EZPI_core_scenes_delete_field_user_arg(f); /* must be a no-op */
    EZPI_scenes_delete_fields(f);              /* and so must this */
    checks++;
}

/* 6. A list, since the destructor recurses and a mixed list is the realistic
 * shape: one when-block's fields can hold a scalar in one field and nothing in
 * the others. */
static void test_mixed_list(void)
{
    l_fields_v2_t *a = new_field("a");
    l_fields_v2_t *b = new_field("b");
    l_fields_v2_t *c = new_field("c");
    a->next = b;
    b->next = c;

    a->user_arg = (void *)(size_t)123456; /* scalar */
    a->user_arg_type = FIELD_USER_ARG_NONE;

    b->user_arg = malloc(16); /* owned block */
    assert(b->user_arg);
    b->user_arg_type = FIELD_USER_ARG_HEAP;

    c->user_arg = cJSON_CreateObject(__FUNCTION__); /* owned tree */
    assert(c->user_arg);
    cJSON_AddStringToObject(__FUNCTION__, (cJSON *)c->user_arg, "k", "v");
    c->user_arg_type = FIELD_USER_ARG_CJSON;

    EZPI_scenes_delete_fields(a);
    checks++;
}

/* 7. NULL is a no-op rather than a crash, because the sunstate discard path
 * reaches the helper through scene_node->when_block->fields. */
static void test_null_is_a_noop(void)
{
    EZPI_core_scenes_delete_field_user_arg(NULL);
    EZPI_scenes_delete_fields(NULL);
    checks++;
}

/* 8. The same thing through the entry point hub.scenes.delete actually
 * reaches, rather than through the field destructor directly.
 *
 * This matters because the tests above could all pass while the when-block
 * destructor still called something else. The real chain is
 * EZPI_scenes_delete() -> EZPI_core_scenes_depopulate_by_id_v2() ->
 * EZPI_core_scenes_delete() -> EZPI_core_scenes_delete_when_blocks() ->
 * EZPI_scenes_delete_fields(), and this test joins it at the last public link
 * -- the two above it need the scene list and the meshbot task, neither of
 * which exists on a host.
 *
 * The when-block holds the field whose user_arg is a tick count, which is the
 * exact shape the isInterval when-method leaves behind. */
static void test_when_block_path(void)
{
    l_when_block_v2_t *wb = malloc(sizeof(l_when_block_v2_t));
    assert(wb);
    memset(wb, 0, sizeof(l_when_block_v2_t));

    wb->fields = new_field("interval");
    wb->fields->user_arg = (void *)(size_t)31337; /* a tick count */
    wb->fields->user_arg_type = FIELD_USER_ARG_NONE;

    wb->fields->next = new_field("deviceItems");
    wb->fields->next->user_arg = cJSON_CreateObject(__FUNCTION__);
    assert(wb->fields->next->user_arg);
    cJSON_AddStringToObject(__FUNCTION__, (cJSON *)wb->fields->next->user_arg,
                            "item0000002a", "a string long enough to be its own allocation");
    wb->fields->next->user_arg_type = FIELD_USER_ARG_CJSON;

    EZPI_core_scenes_delete_when_blocks(wb);
    checks++;
}

/* 9. The tag inside s_item_expn_data_t survives the value it describes.
 *
 * This is a layout check rather than a behaviour check, and it is the one test
 * here that an assert is the right adjudicator for. s_item_expn_data_t used to
 * be a `union` of `e_type` and `u_value`, so the nine writers that set the tag
 * and then stored the value overwrote the tag with the value's bytes. Measured
 * against the union: a stored string left e_type holding pointer bytes, a
 * stored `true` left it reading as VALUE_TYPE_NUMBER, and 23.0 left it reading
 * as VALUE_TYPE_UNDEFINED.
 *
 * It matters here, and not only in the when-method, because the ITEM_EXP arm of
 * the destructor reads this tag to decide whether the struct owns a string. */
static void test_item_exp_tag_survives_its_value(void)
{
    s_item_exp_data_t d;
    char buf[] = "a string";

    memset(&d, 0, sizeof(d));
    d.sample_data.e_type = VALUE_TYPE_STRING;
    d.sample_data.u_value.value_string = buf;
    CHECK(VALUE_TYPE_STRING == d.sample_data.e_type, "string tag survives the pointer");

    memset(&d, 0, sizeof(d));
    d.sample_data.e_type = VALUE_TYPE_BOOL;
    d.sample_data.u_value.value_bool = true;
    CHECK(VALUE_TYPE_BOOL == d.sample_data.e_type, "bool tag survives a true");

    memset(&d, 0, sizeof(d));
    d.sample_data.e_type = VALUE_TYPE_NUMBER;
    d.sample_data.u_value.value_double = 23.0;
    CHECK(VALUE_TYPE_NUMBER == d.sample_data.e_type, "number tag survives 23.0");

    /* 0.1 rather than another integer: its low mantissa bits are non-zero, so
     * under the union this one corrupted the tag into a value that is not any
     * valid enumerator at all, rather than into a different valid one. */
    memset(&d, 0, sizeof(d));
    d.sample_data.e_type = VALUE_TYPE_NUMBER;
    d.sample_data.u_value.value_double = 0.1;
    CHECK(VALUE_TYPE_NUMBER == d.sample_data.e_type, "number tag survives 0.1");
}

/* 10. A remembered item state that owns a string: both allocations go.
 *
 * isItemStateChanged keeps the last value it saw in an s_item_exp_data_t, and
 * when that value is a string the struct holds its own copy. So the slot holds
 * two allocations behind one pointer, which is why it needs a tag of its own
 * rather than HEAP -- LSan reports the inner one if only the outer is freed. */
static void test_item_exp_user_arg_releases_inner_string(void)
{
    l_fields_v2_t *f = new_field("itemState");

    s_item_exp_data_t *d = malloc(sizeof(s_item_exp_data_t));
    assert(d);
    memset(d, 0, sizeof(*d));
    d->sample_data.e_type = VALUE_TYPE_STRING;
    d->sample_data.u_value.value_string = malloc(64);
    assert(d->sample_data.u_value.value_string);
    snprintf(d->sample_data.u_value.value_string, 64, "a value long enough to be its own allocation");

    f->user_arg = d;
    f->user_arg_type = FIELD_USER_ARG_ITEM_EXP;

    EZPI_core_scenes_delete_field_user_arg(f);
    CHECK(NULL == f->user_arg, "item-exp slot cleared");
    CHECK(FIELD_USER_ARG_NONE == f->user_arg_type, "item-exp tag reset");

    EZPI_scenes_delete_fields(f);
}

/* 11. The same struct holding a number, which must NOT have its value treated
 * as a pointer.
 *
 * The obvious wrong implementation of the arm above is to free value_string
 * unconditionally. For a remembered number that is a free() of the low half of
 * a double -- the same class of defect as the tick count in test 1 -- so ASan
 * adjudicates this one. 20000.0 is chosen so the bytes would form a plausible
 * small address rather than an obviously invalid one. */
static void test_item_exp_number_is_not_freed_as_a_pointer(void)
{
    l_fields_v2_t *f = new_field("itemState");

    s_item_exp_data_t *d = malloc(sizeof(s_item_exp_data_t));
    assert(d);
    memset(d, 0, sizeof(*d));
    d->sample_data.e_type = VALUE_TYPE_NUMBER;
    d->sample_data.u_value.value_double = 20000.0;

    f->user_arg = d;
    f->user_arg_type = FIELD_USER_ARG_ITEM_EXP;

    EZPI_scenes_delete_fields(f);
    checks++;
}

/* 12 to 14. Why a NULL test is not a type test.
 *
 * These are layout claims, like test 9, and they exist to pin down the premise
 * of a guard added in ezlopi_core_scenes_when_methods_helper_functions.c.
 *
 * `l_fields_v2_t` carries two type-ish members that are set from different
 * places and are never cross-checked. `value_type` is the declared schema type
 * and comes from the "type" string in the scene JSON;
 * `field_value.e_type` is the union discriminant and is set from the actual
 * JSON type of the "value" member, right beside the write into the union. So
 * {"type":"token","value":true} is an accepted scene field whose value_type is
 * EZLOPI_VALUE_TYPE_TOKEN and whose e_type is VALUE_TYPE_BOOL.
 *
 * Several readers in that file tested the schema type and then checked
 * `NULL != u_value.value_string` before using it, which looks like enough and
 * is not: the two members share bytes, so the question is not whether the
 * pointer is NULL but whether it is a pointer at all. These checks measure
 * what the non-pointer cases actually read back as.
 *
 * The exact values are little-endian-specific; the claim the guard rests on is
 * only "non-NULL", which holds on any byte order, so that is what is asserted
 * and the exact value is reported alongside rather than required. */
static void test_a_bool_in_the_union_reads_back_as_a_non_null_pointer(void)
{
    l_fields_v2_t *f = new_field("sunrise_range");
    char what[128];

    /* Exactly what the cJSON_True arm of EZPI_scenes_populate_fields_get_value
     * does for {"type":"token","value":true}: tag it a bool, write one byte
     * into a union that was zeroed at allocation. */
    f->value_type = EZLOPI_VALUE_TYPE_TOKEN;
    f->field_value.e_type = VALUE_TYPE_BOOL;
    f->field_value.u_value.value_bool = true;

    snprintf(what, sizeof(what),
             "a stored `true` reads back as value_string %p, which is not NULL",
             (void *)f->field_value.u_value.value_string);
    CHECK(NULL != f->field_value.u_value.value_string, what);

    /* So the guard that was there admitted it, and strlen() would have been
     * called on that address. This is the whole reason the discriminant has to
     * be read. */
    CHECK(VALUE_TYPE_STRING != f->field_value.e_type,
          "and the discriminant says it is not a string, which is the test that works");

    EZPI_scenes_delete_fields(f);
}

/* 13. The same for a number, which is the other arm that admits a scalar.
 *
 * A double with fractional bits sets low-order mantissa bytes, so the pointer
 * is non-NULL on a 32-bit target where only the low word aliases it as well as
 * on this 64-bit host. An exactly-representable value like 1234.0 has a zero
 * low word and would read back as NULL on a 32-bit build -- which is why the
 * bool above is the dependable primitive and this one is not asserted for a
 * specific value. */
static void test_a_number_in_the_union_reads_back_as_a_non_null_pointer(void)
{
    l_fields_v2_t *f = new_field("sunrise_range");
    char what[128];

    f->value_type = EZLOPI_VALUE_TYPE_TOKEN;
    f->field_value.e_type = VALUE_TYPE_NUMBER;
    f->field_value.u_value.value_double = 1234.5678;

    snprintf(what, sizeof(what),
             "a stored 1234.5678 reads back as value_string %p, which is not NULL",
             (void *)f->field_value.u_value.value_string);
    CHECK(NULL != f->field_value.u_value.value_string, what);

    EZPI_scenes_delete_fields(f);
}

/* 14. And the control: the case the readers were written for still passes the
 * discriminant test, so the added guard does not reject a legitimate token.
 *
 * Without this the two checks above would be satisfied by a guard that
 * rejected everything. */
static void test_a_real_string_token_still_passes_the_discriminant_test(void)
{
    l_fields_v2_t *f = new_field("sunrise_range");

    f->value_type = EZLOPI_VALUE_TYPE_TOKEN;
    f->field_value.e_type = VALUE_TYPE_STRING;
    f->field_value.u_value.value_string = malloc(3);
    assert(f->field_value.u_value.value_string);
    snprintf(f->field_value.u_value.value_string, 3, "at");

    CHECK(VALUE_TYPE_STRING == f->field_value.e_type,
          "a token that really is a string is admitted by the discriminant");
    CHECK(NULL != f->field_value.u_value.value_string,
          "and is a usable pointer");
    CHECK(0 == strcmp(f->field_value.u_value.value_string, "at"),
          "holding the value it was given");

    EZPI_scenes_delete_fields(f);
}

int main(void)
{
    test_scalar_user_arg_is_not_freed();
    test_bool_user_arg_is_not_freed();
    test_heap_user_arg_is_freed();
    test_cjson_user_arg_is_deleted();
    test_double_release_is_safe();
    test_mixed_list();
    test_null_is_a_noop();
    test_when_block_path();
    test_item_exp_tag_survives_its_value();
    test_item_exp_user_arg_releases_inner_string();
    test_item_exp_number_is_not_freed_as_a_pointer();
    test_a_bool_in_the_union_reads_back_as_a_non_null_pointer();
    test_a_number_in_the_union_reads_back_as_a_non_null_pointer();
    test_a_real_string_token_still_passes_the_discriminant_test();

    printf("test_scenefields: %d checks passed\n", checks);
    return 0;
}
