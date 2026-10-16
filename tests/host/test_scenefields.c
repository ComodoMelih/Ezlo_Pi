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
 * Run: make -C tests/host run-scenefields
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

    printf("test_scenefields: %d checks passed\n", checks);
    return 0;
}
