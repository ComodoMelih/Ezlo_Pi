/* Host tests for ezlopi-core-ble's chunk accumulator.
 *
 * WHY THIS FILE EXISTS
 *
 * Four BLE characteristics accept a payload larger than one ATT write by
 * reassembling it from chunks, and all four use this linked list to hold the
 * chunks until the last one arrives. Two of them -- provisioning and
 * dynamic-config -- keep the list in a file-static across writes, and those two
 * are reachable from anyone in radio range: the write upcall runs before any
 * authentication, because the whole point of those characteristics is to
 * configure a device that has not been configured yet.
 *
 * So the list grows on attacker-supplied input, and until this commit nothing
 * bounded it. The completion test in both services is arithmetic on three
 * numbers the client itself sends:
 *
 *     if (((sequence - 1) * 400 + len) >= tot_len)
 *
 * A client that never satisfies it -- by sending the same sequence number
 * forever, or simply by stopping one chunk short -- kept appending. There were
 * two ways that ended, and the second is the one worth the test:
 *
 *   1. The heap ran out.
 *
 *   2. The transfer did eventually complete, and the accumulator was freed by
 *      a function that recursed once per node before freeing anything. The
 *      BLE host task's stack is a few kilobytes, so a long enough transfer
 *      turned "free the buffer" into a stack overflow at the moment of
 *      success. A crash while freeing is not a leak that someone notices in
 *      a heap trace; it is a reboot with no obvious cause, and on a device
 *      that reboots back into an unprovisioned state it looks like the phone
 *      app failing rather than like a defect here.
 *
 * test_free_does_not_recurse_per_node() is the regression for (2) and it is
 * the reason this file is worth its stubs. The node count it uses is chosen to
 * be past what the old recursive version survives rather than past what a
 * radio can deliver -- see the comment on the test.
 *
 * WHAT IS DELIBERATELY NOT TESTED HERE
 *
 * The bounds themselves (EZPI_BLE_PROV_MAX_ACCUM_LEN,
 * EZPI_BLE_DYNCFG_MAX_ACCUM_LEN) and the discard-on-new-connection logic live
 * in the two service files, which cannot be host-compiled: they pull in cjext,
 * NVS, the factory-info partition, FreeRTOS tasks and the GATT profile. What
 * this file can do is test the primitive those services were given to make the
 * bound expressible -- EZPI_core_ble_buffer_total_len() -- and the free that
 * must not blow up however large the list got. The service-level arithmetic is
 * checked by reading, and is recorded as validation debt in CHANGELOG.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ezlopi_core_ble_buffer.h"

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

/* Builds the callback parameter the GATT layer would hand the write upcall.
** The cast away from const is what the real ESP-IDF type requires: its `value`
** member is a non-const uint8_t*, even though the buffer code only reads it. */
static esp_ble_gatts_cb_param_t make_write(const char *data, uint16_t len)
{
    esp_ble_gatts_cb_param_t param;
    memset(&param, 0, sizeof(param));
    param.write.value = (uint8_t *)(void *)(uintptr_t)(const void *)data;
    param.write.len = len;
    return param;
}

static esp_ble_gatts_cb_param_t make_write_str(const char *data)
{
    return make_write(data, (uint16_t)strlen(data));
}

static void test_total_len(void)
{
    printf("-- total_len --\n");

    /* NULL is 0 and not a crash, because both services call this on the
    ** accumulator *before* knowing whether a transfer is in progress. If it
    ** faulted on NULL the bound check would have to be duplicated into an
    ** if/else at every call site, which is how the first chunk of a transfer
    ** ends up skipping the check. */
    ok(0 == EZPI_core_ble_buffer_total_len(NULL), "NULL holds 0 bytes");

    esp_ble_gatts_cb_param_t p = make_write_str("abcde");
    s_linked_buffer_t *head = EZPI_core_ble_buffer_create(&p);
    ok(NULL != head, "create returns a node");
    ok(5 == EZPI_core_ble_buffer_total_len(head), "one 5-byte chunk is 5 bytes");

    esp_ble_gatts_cb_param_t q = make_write_str("fghijkl");
    ok(true == EZPI_core_ble_buffer_add_to_buffer(head, &q), "a second chunk appends");
    ok(12 == EZPI_core_ble_buffer_total_len(head), "5 + 7 is 12 bytes held");

    /* Ten more of 10 bytes each. The services' bound is a running total, so
    ** the value has to stay right as the list grows rather than only for the
    ** two-node case. */
    for (int i = 0; i < 10; i++)
    {
        esp_ble_gatts_cb_param_t r = make_write_str("0123456789");
        ok(true == EZPI_core_ble_buffer_add_to_buffer(head, &r), "chunk appends");
    }
    ok(112 == EZPI_core_ble_buffer_total_len(head), "12 + 100 is 112 bytes held");

    EZPI_core_ble_buffer_free_buffer(head);
}

static void test_add_reports_failure(void)
{
    printf("-- append failure is reported --\n");

    /* This used to return void. A chunk that could not be stored was therefore
    ** indistinguishable from one that was, and the consequence is specific: the
    ** remaining chunks still arrive, the client's final-chunk arithmetic still
    ** satisfies the completion test, and what gets base64-decoded and written
    ** to the factory partition is a configuration with a hole in the middle of
    ** it. That surfaces one reboot later as a hub with no devices, and nothing
    ** connects it back to a dropped BLE write. */
    esp_ble_gatts_cb_param_t p = make_write_str("payload");
    s_linked_buffer_t *head = EZPI_core_ble_buffer_create(&p);
    ok(NULL != head, "create returns a node");

    /* A zero-length write is the reachable way to make create() fail: it
    ** refuses rather than allocating a node with no buffer. Appending one must
    ** now report false. */
    esp_ble_gatts_cb_param_t empty = make_write("", 0);
    ok(false == EZPI_core_ble_buffer_add_to_buffer(head, &empty),
       "appending a zero-length chunk reports failure");
    ok(7 == EZPI_core_ble_buffer_total_len(head),
       "and the list is unchanged by the failed append");

    ok(NULL == EZPI_core_ble_buffer_create(&empty),
       "create refuses a zero-length write");

    esp_ble_gatts_cb_param_t nul = make_write(NULL, 4);
    ok(NULL == EZPI_core_ble_buffer_create(&nul),
       "create refuses a NULL value with a non-zero length");

    /* A NULL head is not something the services can produce -- they test for it
    ** -- but the function is public and returning false beats dereferencing. */
    ok(false == EZPI_core_ble_buffer_add_to_buffer(NULL, &p),
       "appending to a NULL list reports failure");

    EZPI_core_ble_buffer_free_buffer(head);
    EZPI_core_ble_buffer_free_buffer(NULL); /* must be a no-op, not a fault */
    ok(1, "freeing NULL is a no-op");
}

static void test_free_does_not_recurse_per_node(void)
{
    printf("-- free is iterative --\n");

    /* THE NUMBER. 200000 is not an estimate of what a BLE client can deliver;
    ** it is chosen to be comfortably past the depth at which the previous
    ** recursive implementation dies on a host, so that this check fails
    ** against that version rather than merely passing against this one. The
    ** old free_buffer() recursed before freeing, so peak stack was one frame
    ** per node -- tens of bytes each, and more under -fsanitize=address, which
    ** also installs its own guard pages. Verified by reverting the function and
    ** watching this test abort with a stack-overflow report; that is the whole
    ** value of the check, because the fixed version cannot distinguish itself
    ** from the broken one on any smaller list.
    **
    ** On the device the reachable depth is bounded by heap rather than by this
    ** number, which is a lower bound and not a comfort: the point is that the
    ** depth was a function of what a client chose to send. */
    const int node_count = 200000;

    esp_ble_gatts_cb_param_t first = make_write_str("x");
    s_linked_buffer_t *head = EZPI_core_ble_buffer_create(&first);
    ok(NULL != head, "create returns a node");

    int appended = 0;
    for (int i = 1; i < node_count; i++)
    {
        esp_ble_gatts_cb_param_t p = make_write_str("x");
        if (false == EZPI_core_ble_buffer_add_to_buffer(head, &p))
        {
            break; /* out of memory on the host: still worth freeing what exists */
        }
        appended++;
    }

    /* Not asserted as equality with node_count. If the host is short of memory
    ** the list is shorter and the test is weaker, but reporting that honestly
    ** beats failing for a reason that has nothing to do with the code. */
    ok(appended > 1000, "built a list long enough to be worth freeing");
    printf("     (list length: %d nodes)\n", appended + 1);

    ok((uint32_t)(appended + 1) == EZPI_core_ble_buffer_total_len(head),
       "every node holds its one byte");

    /* The check. Reaching the next line at all is the result: the recursive
    ** version does not. */
    EZPI_core_ble_buffer_free_buffer(head);
    ok(1, "freeing a very long list returns instead of exhausting the stack");
}

static void test_accumulate_to_start(void)
{
    printf("-- accumulate_to_start --\n");

    /* Regression for a refactor rather than for a defect: the byte-summing
    ** loop inside this function was replaced by a call to
    ** EZPI_core_ble_buffer_total_len() so there is one definition of "how much
    ** is held" instead of two that can disagree. The concatenation is what
    ** would break if the shared version counted differently, and it is the
    ** path wifi and improv take on every write. */
    esp_ble_gatts_cb_param_t p = make_write_str("{\"ssid\":");
    s_linked_buffer_t *head = EZPI_core_ble_buffer_create(&p);
    ok(NULL != head, "create returns a node");

    esp_ble_gatts_cb_param_t q = make_write_str("\"home\"}");
    ok(true == EZPI_core_ble_buffer_add_to_buffer(head, &q), "second chunk appends");

    EZPI_core_ble_buffer_accumulate_to_start(head);

    ok(15 == head->len, "the head's length is the total");
    ok(NULL == head->next, "the tail is freed and unlinked");
    ok(0 == memcmp(head->buffer, "{\"ssid\":\"home\"}", 15),
       "the chunks are concatenated in arrival order");

    /* The services read head->buffer as a C string, so the terminator the
    ** function writes past the payload is load-bearing. */
    ok('\0' == (char)head->buffer[15], "a terminator follows the payload");

    EZPI_core_ble_buffer_free_buffer(head);

    /* Single node: must be left alone rather than reallocated into itself. */
    esp_ble_gatts_cb_param_t only = make_write_str("solo");
    s_linked_buffer_t *one = EZPI_core_ble_buffer_create(&only);
    EZPI_core_ble_buffer_accumulate_to_start(one);
    ok(4 == one->len, "a one-node list keeps its length");
    ok(0 == memcmp(one->buffer, "solo", 4), "and its contents");
    EZPI_core_ble_buffer_free_buffer(one);

    EZPI_core_ble_buffer_accumulate_to_start(NULL);
    ok(1, "accumulating NULL is a no-op");
}

int main(void)
{
    printf("ezlopi-core-ble buffer host tests\n\n");

    test_total_len();
    test_add_reports_failure();
    test_accumulate_to_start();
    test_free_does_not_recurse_per_node();

    printf("\n%d check(s), %d failure(s)\n", g_checks, g_fails);

    if (g_fails)
    {
        printf("BLE BUFFER TESTS FAILED\n");
        return 1;
    }
    printf("BLE BUFFER TESTS PASSED\n");
    return 0;
}
