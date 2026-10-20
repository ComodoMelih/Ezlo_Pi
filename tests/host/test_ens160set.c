/* What ENS160_get_ambient_temp_setting() actually returns.
 *
 * WHAT WAS WRONG
 *
 * Both getters in sensor_0068_ENS160_gas_sensor_settings.c cast the *settings
 * node* to the driver's payload type:
 *
 *     s_sensor_..._ambient_temperature_t *t =
 *         (s_sensor_..._ambient_temperature_t *)ens160_..._sensor_setting;
 *     ret = t->ambient_temperature;
 *
 * The payload is not the node. Initialization allocates it separately and
 * hangs it off the node: `node->user_arg = ambient_temperature;`. The correct
 * read is through `->user_arg`, which is how the cloud get- and set-callbacks
 * in the same file already do it.
 *
 * WHY IT WAS SILENT RATHER THAN A CRASH
 *
 * `void *user_arg` is the *first* member of l_ezlopi_device_settings_v3_t, and
 * `float ambient_temperature` is the first member of the payload. So the wrong
 * read is in bounds, correctly aligned, and yields the first four bytes of the
 * node -- the payload pointer itself, reinterpreted as a float. Nothing traps.
 * Both offsets are asserted below rather than assumed, because the whole
 * defect rests on them being 0.
 *
 * And the number it produces looks usable, which is the worst part. Every
 * ESP32 heap address decodes as an IEEE-754 float in [1.0, 2.0): internal DRAM
 * (0x3FF80000-0x3FFFFFFF) lands in [1.9375, 2.0), and mapped PSRAM
 * (0x3F800000-0x3FBFFFFF) in [1.0, 1.5). Both ranges are swept here. So on
 * target these getters returned something between 1 and 2 where the file's own
 * defaults are 25 degrees C and 50 %RH -- a plausible small number, not a NaN
 * or an infinity that something downstream might have rejected.
 *
 * This does not reproduce on the host and is not meant to: an x86-64 heap
 * pointer's low four bytes are arbitrary, and under ASan they came out as a
 * denormal that prints as 0.000000. That is why the target claim is swept over
 * the address range rather than observed from a live allocation here.
 *
 * WHERE IT WENT
 *
 * sensor_0068_ENS160_gas_sensor.c calls both getters and feeds the results to
 * DFROBOT_ens160_set_temp_and_hum(), which writes the sensor's temperature and
 * humidity compensation registers -- at __init(), so every boot, and again in
 * __notify() whenever a setting changes. The ENS160 compensates its AQI, TVOC
 * and eCO2 outputs with those registers, so the readings were compensated
 * against a heap address.
 *
 * WHY THE REAL FILE IS THE SUBJECT
 *
 * It compiles on a host unmodified, with no warnings, and needs six functions
 * stubbed. So this test calls the real ENS160_gas_sensor_settings_init() and
 * the real getters; it does not re-type the expression and check its own
 * arithmetic, which would have passed on the broken file.
 *
 * EZPI_core_device_add_settings_to_device_v3 is the one stub that is a
 * deliberate copy rather than a refusal: the real one in ezlopi_core_devices.c
 * mallocs a node and appends it to device->settings, and that linking matters
 * to a second defect in this file (the payload-malloc failure path frees a
 * node the core has already linked). Compiling the real ezlopi_core_devices.c
 * here would drag in the cloud, NVS and FreeRTOS; the 20 lines are copied, and
 * flagged as a copy, so that the list state this test sees is the list state
 * the firmware would have.
 *
 * HOW IT WAS CONFIRMED TO BE ABLE TO FAIL
 *
 * Written against the unfixed file and seen red first: 18 checks, 2 failures,
 * both of them the value read-backs, with get_ambient_temp reporting a
 * pointer-derived float instead of 25. The other 16 passed on the broken file,
 * which is correct -- they are about the layout and the address arithmetic
 * that made the defect silent, and those were true before the fix and after
 * it. The fix turned the 2 green without this file changing.
 *
 * Re-confirmed after the sweep below grew to cover both heap regions, by
 * dropping `->user_arg` from the two getter casts again -- which is the defect
 * verbatim -- rebuilding, and restoring: 18/2 broken, 18/0 fixed, the same two
 * lines red both times.
 *
 * The file is at 29 checks now. The 11 added for the allocation-failure
 * defects have their own account of being seen red, in the comment on the last
 * test -- three separate re-breaks, because those three defects fail in three
 * different ways and only one of them is a failed check.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "cjext.h"
#include "ezlopi_core_devices.h"
#include "sensor_0068_ENS160_gas_sensor_settings.h"

static int checks;
static int failures;

#define CHECK(cond, what)                                         \
    do                                                            \
    {                                                             \
        checks++;                                                 \
        if (!(cond))                                              \
        {                                                         \
            failures++;                                           \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what); \
        }                                                         \
    } while (0)

/*******************************************************************************
 * The controllable allocator. stubs-ens160/EZLOPI_USER_CONFIG.h points
 * ezlopi_malloc and ezlopi_free at these two, which is what makes the
 * allocation-failure paths of settings initialization reachable at all.
 *
 * Both call libc underneath so AddressSanitizer keeps its view of every block.
 *
 * The free log is how "the subject freed a node the core had already linked"
 * gets reported as an ordinary failed check rather than an ASan abort, since
 * discovering it by reading the node would be the use-after-free itself. It
 * earns its place on the second of the two settings: nothing walks the list
 * after the second node is appended, so ASan sees nothing there and the log is
 * the only witness. On the first node ASan gets there first -- see the note on
 * the last test below.
 ******************************************************************************/

static int alloc_budget = -1; /* -1: unlimited. Otherwise, fail once it hits 0. */
static int allocs_served;
static int allocs_refused;

#define FREE_LOG_MAX 64
static void *free_log[FREE_LOG_MAX];
static int free_log_len;
static int free_log_overflowed;

void *EZPI_test_alloc(unsigned long size)
{
    if (0 == alloc_budget)
    {
        allocs_refused++;
        return NULL;
    }
    if (alloc_budget > 0)
    {
        alloc_budget--;
    }
    allocs_served++;
    return malloc((size_t)size);
}

void EZPI_test_release(void *ptr)
{
    if (free_log_len < FREE_LOG_MAX)
    {
        free_log[free_log_len++] = ptr;
    }
    else
    {
        /* Recorded rather than ignored: a full log would make was_freed()
        ** answer "no" for a pointer it simply never saw, which is the one
        ** answer this test must not get wrong silently. */
        free_log_overflowed = 1;
    }
    free(ptr);
}

static int was_freed(const void *ptr)
{
    int i;
    for (i = 0; i < free_log_len; i++)
    {
        if (free_log[i] == ptr)
        {
            return 1;
        }
    }
    return 0;
}

/*******************************************************************************
 * Stubs. Six functions, listed by what each one has to be for this test to
 * mean anything.
 ******************************************************************************/

/* A faithful copy of EZPI_core_device_add_settings_to_device_v3. See the note
** at the top: the append is the part that matters, so it is reproduced rather
** than replaced by a bare malloc. */
l_ezlopi_device_settings_v3_t *EZPI_core_device_add_settings_to_device_v3(
    l_ezlopi_device_t *device,
    int (*setting_func)(e_ezlopi_settings_action_t action,
                        struct l_ezlopi_device_settings_v3 *setting,
                        void *arg, void *user_arg))
{
    l_ezlopi_device_settings_v3_t *new_setting = NULL;
    if (device)
    {
        new_setting = malloc(sizeof(l_ezlopi_device_settings_v3_t));
        if (new_setting)
        {
            memset(new_setting, 0, sizeof(l_ezlopi_device_settings_v3_t));
            new_setting->func = setting_func;

            if (NULL == device->settings)
            {
                device->settings = new_setting;
            }
            else
            {
                l_ezlopi_device_settings_v3_t *curr = device->settings;
                while (curr->next)
                {
                    curr = curr->next;
                }
                curr->next = new_setting;
            }
        }
    }
    return new_setting;
}

uint32_t EZPI_core_cloud_generate_settings_id(void)
{
    static uint32_t next = 0x1000;
    return next++;
}

/* Reports "no stored value", so initialization takes its own default branch
** and the expected numbers below are the defaults the file itself writes: 25
** and 50. Reading a value out of a stubbed NVS would make this test agree with
** whatever it had just planted. */
ezlopi_error_t EZPI_core_nvs_read_float32(float *f, const char *key_name)
{
    (void)f;
    (void)key_name;
    return EZPI_ERR_NVS_READ_FAILED;
}

ezlopi_error_t EZPI_core_nvs_write_float32(float f, const char *key_name)
{
    (void)f;
    (void)key_name;
    return EZPI_SUCCESS;
}

void EZPI_core_device_value_updated_settings_broadcast(l_ezlopi_device_settings_v3_t *setting)
{
    (void)setting;
}

void EZPI_core_cjson_get_value_float(cJSON *cj_root, const char *item_name_str, float *item_val)
{
    (void)cj_root;
    (void)item_name_str;
    (void)item_val;
}

/*******************************************************************************
 * The tests
 ******************************************************************************/

/* Hands the list back to libc at the end of each case.
**
** Not politeness: LeakSanitizer is on, and it only stays quiet about the nodes
** and payloads because the subject's two file-scope pointers keep them
** reachable. The second case below re-runs initialization and overwrites those
** pointers, which would strand the first case's allocations and turn a passing
** test red for a reason that has nothing to do with the subject.
**
** libc free, not EZPI_test_release: the free log is evidence about what the
** *subject* freed, and the teardown must not write to it. The was_freed() guard
** is for the broken-subject case -- if the subject has already freed a node,
** reading its ->next is the use-after-free this test is looking for, and
** freeing it again would abort before any check could be printed. */
static void release_settings_list(l_ezlopi_device_t *device)
{
    l_ezlopi_device_settings_v3_t *curr = device->settings;

    while (curr && !was_freed(curr))
    {
        l_ezlopi_device_settings_v3_t *next = curr->next;
        free(curr->user_arg);
        free(curr);
        curr = next;
    }
    device->settings = NULL;
}

/* The two offsets the defect rests on. If either were non-zero the wrong cast
** would have read some other member, or read out of bounds and been caught by
** a sanitizer years ago. They are 0, which is why it was invisible. */
static void test_the_layout_that_hid_it(void)
{
    CHECK(0 == offsetof(l_ezlopi_device_settings_v3_t, user_arg),
          "user_arg is the first member of the settings node");
    CHECK(0 == offsetof(s_sensor_ens160_gas_sensor_setting_ambient_temperature_t,
                        ambient_temperature),
          "ambient_temperature is the first member of its payload");
    CHECK(0 == offsetof(s_sensor_ens160_gas_sensor_setting_relative_humidity_t,
                        relative_humidity),
          "relative_humidity is the first member of its payload");
}

/* Every address an ESP32 heap can hand out decodes to a float in [1.0, 2.0).
** That is what makes the bug a plausible reading rather than an obvious one,
** and it is the only claim in the entry above that needs arithmetic, so it is
** swept rather than asserted. Both heap regions are covered: internal DRAM and
** the PSRAM window, each with its own narrower sub-bound.
**
** The bounds hold across each whole region because the top nine bits -- sign
** and exponent -- are constant over it, but sweeping says so without the reader
** having to take that on trust. The stride is 4 KiB, which is a page and the
** smallest granularity either region is mapped at; the endpoints are visited
** explicitly on top of that, because the top of a range is where a bound like
** this fails if it fails at all and stepping from the base overshoots it. */
static void test_both_heap_regions_read_as_floats_under_two(void)
{
    static const struct
    {
        const char *what;
        uint32_t base;
        uint32_t top;
        float lo_bound;
        float hi_bound;
    } regions[] = {
        {"PSRAM window", 0x3F800000u, 0x3FBFFFFFu, 1.0f, 1.5f},
        {"internal DRAM", 0x3FF80000u, 0x3FFFFFFFu, 1.9375f, 2.0f},
    };
    size_t r;

    for (r = 0; r < sizeof(regions) / sizeof(regions[0]); r++)
    {
        uint32_t addr;
        float lo = 3.0f;
        float hi = 0.0f;
        int outside_region = 0;
        int outside_heap = 0;
        int seen = 0;

        for (addr = regions[r].base;; addr += 0x1000u)
        {
            float f;
            uint32_t probe = (addr > regions[r].top) ? regions[r].top : addr;

            memcpy(&f, &probe, sizeof(f));
            seen++;
            if (f < lo)
            {
                lo = f;
            }
            if (f > hi)
            {
                hi = f;
            }
            if (!((f >= regions[r].lo_bound) && (f < regions[r].hi_bound)))
            {
                outside_region++;
            }
            if (!((f >= 1.0f) && (f < 2.0f)))
            {
                outside_heap++;
            }
            if (addr > regions[r].top)
            {
                break;
            }
        }

        /* Seven decimals, not four: the top of each region is a hair under the
        ** bound asserted below -- 0x3FBFFFFF is 1.4999999 and 0x3FFFFFFF is
        ** 1.9999999 -- and %.4f rounds both up to the bound itself, which would
        ** read as a contradiction of the check on the next line. */
        printf("      %6d %s addresses (0x%08X-0x%08X) read as floats in "
               "[%.7f, %.7f]\n",
               seen, regions[r].what, regions[r].base, regions[r].top, lo, hi);
        CHECK(0 == outside_heap,
              "every address in this region reads as a float in [1.0, 2.0)");
        CHECK(0 == outside_region,
              "and within the narrower bound recorded for this region");
        CHECK((lo >= regions[r].lo_bound) && (hi < regions[r].hi_bound),
              "the observed range sits where the defect's plausible value "
              "came from");
    }
}

/* The regression test proper: the real init, then the real getters. */
static void test_the_getters_return_the_stored_value(void)
{
    l_ezlopi_device_t device;
    s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *payload;
    float temp;
    float hum;
    float wrong;
    uint32_t bits;
    uintptr_t as_int;

    memset(&device, 0, sizeof(device));
    CHECK(0 == ENS160_gas_sensor_settings_init(&device, NULL),
          "settings init reports success");
    CHECK(NULL != device.settings,
          "a settings node was appended to the device");
    CHECK((NULL != device.settings) && (NULL != device.settings->next),
          "both settings were appended, in order");

    temp = ENS160_get_ambient_temp_setting();
    hum = ENS160_get_relative_humidity_setting();
    printf("      get_ambient_temp -> %.6f, get_relative_humidity -> %.6f\n",
           temp, hum);

    /* 25 and 50 are the file's own defaults, written at initialization and
    ** repeated as the cloud `valueDefault`. Exact equality is right here: the
    ** value makes one hop from a float literal into a float member and back,
    ** with no arithmetic on the way. */
    CHECK(25.0f == temp, "the ambient temperature setting reads back as 25");
    CHECK(50.0f == hum, "the relative humidity setting reads back as 50");

    /* And pin down what the wrong read produced, so this test says why it was
    ** wrong and not merely that something was. The old expression cast the
    ** node; its first four bytes are the payload pointer. */
    payload = (s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *)
        device.settings->user_arg;
    CHECK(NULL != payload, "the payload hangs off the node's user_arg");
    if (NULL != payload)
    {
        CHECK(25.0f == payload->ambient_temperature,
              "the payload itself holds 25");

        wrong = ((s_sensor_ens160_gas_sensor_setting_ambient_temperature_t *)
                     device.settings)
                    ->ambient_temperature;
        as_int = (uintptr_t)device.settings->user_arg;
        bits = (uint32_t)(as_int & 0xFFFFFFFFu);
        CHECK(0 == memcmp(&wrong, &bits, sizeof(wrong)),
              "casting the node yields the payload pointer as a float, "
              "which is what the defect returned");
        CHECK(25.0f != wrong,
              "and that is not the stored value");
    }

    release_settings_list(&device);
}

/* The second defect in the same file: what initialization does when the payload
** allocation fails.
**
** The core has already allocated the settings node and appended it to
** device->settings by then -- that is what
** EZPI_core_device_add_settings_to_device_v3 does, and the copy of it above
** reproduces the append for this reason. The failure path used to ezlopi_free()
** that node and leave it linked, so device->settings held a dangling pointer
** and so did the file-scope getter pointer.
**
** How soon that dangling pointer is read is sooner than this test was written
** to expect, and it is the sharper statement of the defect: re-breaking the
** first setting's free and running under ASan puts the use-after-free inside
** add_settings_to_device_v3's own tail walk, on the *second* call -- the next
** line of the same init function, looking for the list tail to append to. It
** does not wait for the cloud; the node is dangling before initialization has
** finished.
**
** On an unfixed tree none of that was reachable, which is the other half of
** this. ENS160_gas_sensor_settings_init used to wrap both initializers in
** ESP_ERROR_CHECK, and with CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
** and no NDEBUG -- this project's configuration -- that macro is the
** _esp_error_check_failed() variant: a panic and a reboot. The order is free,
** return 1, abort, so the hub rebooted at the point where the second call would
** have read the freed node. The use-after-free was real and unreachable at the
** same time.
**
** Which is why the two are fixed together and asserted together here: that the
** failure is *returned* rather than aborted on, and that the node is left
** linked and intact when it is. Fixing only the second leaves the reboot;
** fixing only the first turns a reboot into a live use-after-free.
**
** The three ways this case can fail were each produced on their own against the
** fixed file:
**
**   ESP_ERROR_CHECK put back on the first initializer -> abort inside
**     ENS160_gas_sensor_settings_init, no tally printed, rc=134. That is the
**     firmware's reboot reproduced, because stubs/esp_err.h keeps the abort
**     deliberately rather than stubbing it out.
**   the first node's free put back -> ASan heap-use-after-free in
**     add_settings_to_device_v3, rc=1. ASan gets there before any check does,
**     so for the first node it is the report and the free log is never
**     consulted.
**   the second node's free put back -> a printed tally, 27 checks and 1
**     failure, the red one being "the second node was not freed either".
**     Nothing walks the list after the second append, so ASan sees nothing and
**     the free log is the only witness. This is the row that earns the log.
**
** This case runs last, and only after the success case has torn its list down,
** because initialization overwrites the subject's two file-scope pointers. */
static void test_a_failed_payload_allocation_leaves_the_node_linked(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *first = NULL;
    l_ezlopi_device_settings_v3_t *second = NULL;
    int rc;

    memset(&device, 0, sizeof(device));

    /* 0 refuses every allocation the subject asks for from here on. The node
    ** allocations are not affected -- those go through the copy of the core
    ** function above, which calls libc malloc directly, so what fails is
    ** precisely the payload malloc on the line after the node was linked. */
    alloc_budget = 0;
    rc = ENS160_gas_sensor_settings_init(&device, NULL);
    alloc_budget = -1;

    CHECK(2 == allocs_refused,
          "both payload allocations were refused, so both failure paths ran");
    CHECK(0 != rc,
          "settings init reports the failure to its caller instead of "
          "aborting or returning success");

    first = device.settings;
    CHECK(NULL != first, "the first settings node is still on the device");

    /* Asserted before the node is read, not after: if it was freed, reading
    ** ->user_arg or ->next is the use-after-free, and under ASan that is an
    ** abort with no indication of which check was running. */
    CHECK((NULL != first) && !was_freed(first),
          "the first node was not freed while the device still points at it");

    if (first && !was_freed(first))
    {
        CHECK(NULL == first->user_arg,
              "and it carries no payload, which is what the getters test for");
        second = first->next;
        CHECK(NULL != second,
              "the second setting was attempted too, not skipped after the "
              "first failed");
        CHECK((NULL != second) && !was_freed(second),
              "the second node was not freed either");
        if (second && !was_freed(second))
        {
            CHECK(NULL == second->user_arg, "and it carries no payload");
        }
    }

    /* 0.0f, not 25: the file keeps the no-payload return distinct from the
    ** default on purpose, so a setting that failed to initialize cannot be
    ** mistaken for one that worked.
    **
    ** Each call is guarded on its own node, for the same reason the checks
    ** above are ordered the way they are. The getters read the subject's two
    ** file-scope pointers, which point at these nodes; if a node was freed the
    ** call is a use-after-free, and the guard is what keeps the failed check
    ** above as the report instead of an abort. Skipping them when a node was
    ** freed loses two checks out of the tally, which is the correct trade: the
    ** run has already failed by then. */
    if (first && !was_freed(first))
    {
        CHECK(0.0f == ENS160_get_ambient_temp_setting(),
              "the ambient-temperature getter reports 0 rather than reading a "
              "freed node");
    }
    if (second && !was_freed(second))
    {
        CHECK(0.0f == ENS160_get_relative_humidity_setting(),
              "the relative-humidity getter reports 0 rather than reading a "
              "freed node");
    }

    CHECK(0 == free_log_overflowed,
          "the free log held every free, so was_freed() above answered from "
          "a complete record");

    release_settings_list(&device);
}

int main(void)
{
    /* Line-buffered on purpose. The last case here can abort -- under ASan, or
    ** on the ESP_ERROR_CHECK stub's own abort -- and with stdout fully buffered
    ** into a pipe every FAIL line printed before that point is lost with the
    ** buffer. That happened on the first run of this binary against the unfixed
    ** subject: the abort was all there was to read, and the checks that had
    ** already gone red were invisible. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    test_the_layout_that_hid_it();
    test_both_heap_regions_read_as_floats_under_two();
    test_the_getters_return_the_stored_value();
    test_a_failed_payload_allocation_leaves_the_node_linked();

    printf("%d check(s), %d failure(s)\n", checks, failures);
    return (0 == failures) ? 0 : 1;
}
