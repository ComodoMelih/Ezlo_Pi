/* What happens the first time a Hilink presence sensor is configured.
 *
 * WHAT WAS WRONG
 *
 * hilink_presence_sensor_setting.c tested its NVS writes backwards, at two
 * sites:
 *
 *     if (EZPI_SUCCESS == EZPI_core_nvs_write_str(...))
 *     {
 *         TRACE_E("Failed to write to NVS");
 *         ret = 1;
 *     }
 *
 * EZPI_SUCCESS is 0 and the function returns ezlopi_error_t, so that branch is
 * the *success* branch. A write that worked was reported as a failure, and a
 * write that actually failed was reported as success. The third initializer in
 * the same file has it the right way round (`EZPI_SUCCESS !=`), which is what
 * makes these two a typo rather than a convention.
 *
 * WHY IT REBOOTED THE HUB
 *
 * That `ret` is not merely returned. HILINK_presence_sensor_initialize_settings
 * wraps all three initializers in ESP_ERROR_CHECK, and in the shipped
 * configuration -- CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y, no NDEBUG
 * -- that macro is the _esp_error_check_failed() variant, which panics and
 * reboots. So the sequence on a hub was: user adds a presence sensor, NVS has
 * no saved mode yet, the default is written, the write succeeds, the inverted
 * test turns that into ret = 1, ESP_ERROR_CHECK(1) panics.
 *
 * It reboots once rather than looping, and the reason is worth stating because
 * it is why this survived use: EZPI_core_nvs_write_str returns EZPI_SUCCESS only
 * after nvs_commit has succeeded, so the value is durable *before* the panic.
 * On the next boot EZPI_core_nvs_read_str finds it, the default-writing branch
 * is skipped, nothing is written, and nothing panics. One reboot in the middle
 * of onboarding, which then never recurs and leaves no trace.
 *
 * The device is not behind a Kconfig option -- ezlopi_core_device_macros.h has
 * `#define SENSOR_0067_UART_HILINK_PRESENCE_SENSOR 1` -- so this is shipped
 * code on a default build.
 *
 * WHY THE ESP_ERROR_CHECK COULD NOT JUST BE DELETED
 *
 * The same reason as in sensor_0068_ENS160_gas_sensor_settings.c, and it is the
 * same bug: on a failed payload allocation two of the three initializers call
 * ezlopi_free on the settings *node*, which
 * EZPI_core_device_add_settings_to_device_v3 has already appended to
 * device->settings. The list is left holding a dangling pointer, and the next
 * append walks it to find the tail. Removing the panic without removing those
 * frees converts a reboot into a live use-after-free.
 *
 * WHAT ELSE IS ON THESE PATHS
 *
 * Measured, not assumed, and each one has a check below:
 *
 *   - `ret` in HILINK_presence_sensor_initialize_settings is declared 0,
 *     returned, and never assigned, so the function reported success whatever
 *     the three initializers said. Even with the panic gone it was silent.
 *   - The predefined initializer has no `else` for a failed
 *     add_settings_to_device_v3, so a failed node allocation read as success.
 *     The user-defined one does set ret = 1 in that case.
 *   - The radar initializer sets `ret = 0` on a failed payload allocation where
 *     both its siblings set 1.
 *   - HILINK_presence_sensor_target_in_detectable_range dereferences the global
 *     node pointer and only checks the payload behind it, so the two silent
 *     successes above left it reading through NULL.
 *   - The predefined initializer never frees the string
 *     EZPI_core_nvs_read_str returned it. The user-defined one does.
 *   - __prepare_user_defined_setting_str leaks both its cJSON object and,
 *     through its caller, the string it returns; and it calls strlen on the
 *     result of cJSON_PrintBuffered without checking it for NULL.
 *
 * WHY THE REAL FILE IS THE SUBJECT
 *
 * It compiles on a host unmodified. The one thing it needs that an ordinary
 * stub cannot give is an NVS that can be told what it holds and whether its
 * writes work, because that is the whole subject: the defect is in how the file
 * reads a write result. So the NVS model below is controllable in both
 * directions, and the checks drive the real
 * HILINK_presence_sensor_initialize_settings rather than re-stating its
 * arithmetic.
 *
 * EZPI_core_device_add_settings_to_device_v3 is a deliberate copy of the real
 * one, for the reason given in test_ens160set.c: the append is what the
 * node-free defect acts on, so the list state here has to be the list state the
 * firmware would have. It can also be told to fail on a chosen call, which is
 * the only way in -- the three node pointers are `static`, so a test cannot
 * reach the uninitialized-setting state except by making an allocation fail.
 *
 * HOW IT WAS CONFIRMED TO BE ABLE TO FAIL
 *
 * See the comment above each test. They do not fail the same way: the inverted
 * write aborts the process through the esp_err stub, the leaks come back as a
 * free that never happened, and the silent successes come back as an ordinary
 * failed check.
 */
#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "cjext.h"
#include "ld2410.h"
#include "ezlopi_core_devices.h"
#include "ezlopi_core_errors.h"
#include "hilink_presence_sensor_setting.h"

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
 * The controllable allocator. stubs-allocfail/EZLOPI_USER_CONFIG.h points
 * ezlopi_malloc and ezlopi_free at these two.
 *
 * Both call libc underneath, so AddressSanitizer keeps its view of every block.
 * The free log is the witness for two different things here: that the subject
 * did *not* free a node the core had already linked, and that it *did* free the
 * string NVS handed it.
 ******************************************************************************/

static long alloc_budget = -1; /* -1 unlimited, else allocations still allowed */
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
        free_log_overflowed = 1;
    }
    free(ptr);
}

static int was_freed(void *ptr)
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

static void reset_alloc_state(void)
{
    alloc_budget = -1;
    allocs_served = 0;
    allocs_refused = 0;
    free_log_len = 0;
    free_log_overflowed = 0;
    memset(free_log, 0, sizeof(free_log));
}

/*******************************************************************************
 * The NVS model. Three keys, because the subject uses exactly three: "predef"
 * and "userdef" as strings, "rdrsens" as an int32.
 *
 * Both directions are controllable. What it holds decides which branch of each
 * initializer runs, and what its writes return is the thing the subject reads
 * backwards.
 ******************************************************************************/

#define NVS_KEYS 3
static const char *const nvs_names[NVS_KEYS] = {"predef", "userdef", "rdrsens"};

static char *nvs_planted[NVS_KEYS];    /* NULL means the key is absent */
static int32_t nvs_planted_int;
static int nvs_planted_int_present;

/* The pointer each read_str call handed out, so a check can ask whether the
** subject ever gave it back. */
static void *nvs_handed_out[NVS_KEYS];

static char nvs_written[NVS_KEYS][512];
static int nvs_writes[NVS_KEYS];

/* Per key, not one value for all three. The first version of this was a single
** nvs_write_result, and with it the "a failed write is reported" check passed
** against the unfixed file -- which looked like the check being wrong and was
** worse than that. Only two of the three write tests are inverted; the radar
** one at the bottom of the file is written correctly. So failing all three
** writes at once produces a non-zero return from the one correct test, and a
** check on the return alone cannot see that the other two said nothing.
**
** Failing one key at a time makes each test answerable on its own. */
static ezlopi_error_t nvs_write_result[NVS_KEYS];

static int nvs_index(const char *name)
{
    int i;
    for (i = 0; i < NVS_KEYS; i++)
    {
        if (0 == strcmp(name, nvs_names[i]))
        {
            return i;
        }
    }
    return -1;
}

static void reset_nvs_state(void)
{
    int i;
    for (i = 0; i < NVS_KEYS; i++)
    {
        nvs_planted[i] = NULL;
        nvs_handed_out[i] = NULL;
        nvs_written[i][0] = '\0';
        nvs_writes[i] = 0;
        nvs_write_result[i] = EZPI_SUCCESS;
    }
    nvs_planted_int = 0;
    nvs_planted_int_present = 0;
}

/* Allocated with libc malloc, deliberately: the subject is supposed to release
** this with ezlopi_free, which lands in EZPI_test_release and so in the free
** log. If this used EZPI_test_alloc it would also sit inside the failure
** budget, and a test about a leak would start depending on the budget. */
char *EZPI_core_nvs_read_str(const char *nvs_name)
{
    int i = nvs_index(nvs_name);
    if (i < 0 || NULL == nvs_planted[i])
    {
        return NULL;
    }
    {
        size_t n = strlen(nvs_planted[i]) + 1;
        char *out = malloc(n);
        if (out)
        {
            memcpy(out, nvs_planted[i], n);
        }
        nvs_handed_out[i] = out;
        return out;
    }
}

ezlopi_error_t EZPI_core_nvs_write_str(const char *data, uint32_t len, const char *nvs_name)
{
    int i = nvs_index(nvs_name);
    (void)len;
    if (i < 0)
    {
        return EZPI_ERR_NVS_WRITE_FAILED;
    }
    nvs_writes[i]++;
    if (data)
    {
        snprintf(nvs_written[i], sizeof(nvs_written[i]), "%s", data);
    }
    return nvs_write_result[i];
}

/* int32_t in both signatures, because that is what ezlopi_core_nvs.h declares.
** These two previously said `int *` and `int`, copied from the caller rather
** than from the header, and nothing here could have caught it: on x86-64 Linux
** int32_t is int, so the two spellings are the same type and the compiler has
** nothing to say. The disagreement was only visible on the esp32c3 build,
** where int32_t is long int -- and what it flagged there was the caller, in
** hilink_presence_sensor_setting.c, not this stub. A stub that copies the
** caller's type can never contradict the caller, which is the whole reason to
** take stub signatures from the header instead. test_tz.c does this
** deliberately and says so; this file did not. */
ezlopi_error_t EZPI_core_nvs_read_int32(int32_t *i_val, const char *nvs_name)
{
    (void)nvs_name;
    if (nvs_planted_int_present && i_val)
    {
        *i_val = nvs_planted_int;
        return EZPI_SUCCESS;
    }
    return EZPI_ERR_NVS_READ_FAILED;
}

ezlopi_error_t EZPI_core_nvs_write_int32(int32_t i_val, const char *nvs_name)
{
    int i = nvs_index(nvs_name);
    if (i < 0)
    {
        return EZPI_ERR_NVS_WRITE_FAILED;
    }
    nvs_writes[i]++;
    snprintf(nvs_written[i], sizeof(nvs_written[i]), "%" PRId32, i_val);
    return nvs_write_result[i];
}

/*******************************************************************************
 * The remaining stubs.
 ******************************************************************************/

/* A faithful copy of EZPI_core_device_add_settings_to_device_v3, with one
** addition: it can be told to return NULL on a chosen call. The real one does
** that when its malloc fails, and it is the only route to the state where a
** settings node pointer stays NULL. */
static int add_settings_calls;
static int add_settings_fail_on_call; /* 0 = never fail */

l_ezlopi_device_settings_v3_t *EZPI_core_device_add_settings_to_device_v3(
    l_ezlopi_device_t *device,
    int (*setting_func)(e_ezlopi_settings_action_t action,
                        struct l_ezlopi_device_settings_v3 *setting,
                        void *arg, void *user_arg))
{
    l_ezlopi_device_settings_v3_t *new_setting = NULL;

    add_settings_calls++;
    if (add_settings_fail_on_call == add_settings_calls)
    {
        return NULL;
    }

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
    static uint32_t next = 0x2000;
    return next++;
}

void EZPI_core_device_value_updated_settings_broadcast(l_ezlopi_device_settings_v3_t *setting)
{
    (void)setting;
}

/* The driver call the apply path makes. Recorded rather than ignored so a check
** can tell whether applying a setting reached the radar at all.
**
** The signature is ld2410.h's, taken from the header rather than guessed; note
** that its first parameter is named `template`, which is why that header is one
** of several in this tree that would not compile as C++. */
static int ld2410_set_template_calls;
static ld2410_template_t ld2410_last_template;
static esp_err_t ld2410_set_template_result = ESP_OK;

esp_err_t ld2410_set_template(ld2410_template_t template_type, ld2410_settings_t *p_settings)
{
    (void)p_settings;
    ld2410_set_template_calls++;
    ld2410_last_template = template_type;
    return ld2410_set_template_result;
}

/* Not reached by anything this test exercises. ezlopi_core_cjson_macros.c is
** linked whole, for the reason the Makefile records -- the subject reads a
** stored user-defined mode back through CJSON_GET_VALUE_FLOAT and friends, and
** those macros are the real extraction. EZPI_core_cjson_get_value_gpio() is a
** sibling in that same translation unit, it calls this, and the hilink subject
** never calls it. So this exists to satisfy the linker and nothing else.
**
** Returning false is the honest answer for a test that never asks: the real
** function reads the set out of efuse over esp_mspi_get_io(), and there is no
** efuse here. test_cjsongpio.c, where the question is the point, makes the pin
** set an input instead; the signature below is that file's and the header's.
** `const int` would not match -- the declaration at
** ezlopi_core_pin_reserved.h:45 takes a plain int. */
bool EZPI_core_pin_is_mspi(int gpio_num)
{
    (void)gpio_num;
    return false;
}

/*******************************************************************************
 * Helpers
 ******************************************************************************/

/* The nodes are reachable only through device->settings once the subject's own
** static pointers have been overwritten by a later init, and LeakSanitizer is
** live in this suite, so each device's list is released at the end of the case
** that built it.
**
** libc free, not EZPI_test_release: the free log is evidence about what the
** subject freed, and the test must not write to it. Nodes the subject already
** freed are skipped, because freeing them again would report as the test's own
** double free rather than as the subject's defect. */
static void release_settings_list(l_ezlopi_device_t *device)
{
    l_ezlopi_device_settings_v3_t *curr = device->settings;
    while (curr && !was_freed(curr))
    {
        l_ezlopi_device_settings_v3_t *next = curr->next;
        if (curr->user_arg && !was_freed(curr->user_arg))
        {
            free(curr->user_arg);
        }
        free(curr);
        curr = next;
    }
    device->settings = NULL;
}

static int list_length(l_ezlopi_device_t *device)
{
    int n = 0;
    l_ezlopi_device_settings_v3_t *curr = device->settings;
    while (curr && n < 16)
    {
        n++;
        curr = curr->next;
    }
    return n;
}

static void reset_all(void)
{
    reset_alloc_state();
    reset_nvs_state();
    add_settings_calls = 0;
    add_settings_fail_on_call = 0;
    ld2410_set_template_calls = 0;
    ld2410_last_template = (ld2410_template_t)(-1);
    ld2410_set_template_result = ESP_OK;
}

/* The node for one of the three settings, by position in the device list --
** 0 predefined, 1 user-defined, 2 radar sensitivity, which is the order
** HILINK_presence_sensor_initialize_settings adds them in. */
static l_ezlopi_device_settings_v3_t *nth_setting(l_ezlopi_device_t *device, int n)
{
    l_ezlopi_device_settings_v3_t *curr = device->settings;
    while (curr && n--)
    {
        curr = curr->next;
    }
    return curr;
}

/* Give every key a value, so that no initializer writes anything.
**
** Any case whose subject is something other than an NVS write wants this.
** While the write tests were inverted, a *successful* write set ret = 1, so a
** check on the return value of initialization picked up a spurious 1 from the
** user-defined initializer no matter what the case was really about -- which
** made two of the cases below pass against the unfixed file for a reason that
** had nothing to do with them. Taking the writes out of the picture is what
** makes the return value attributable.
**
** The strings are the file's own defaults, so the "already exists" path parses
** values of the shape it would itself have stored. */
static void plant_everything(void)
{
    nvs_planted[0] = (char *)"basic_routine_mode_mid";
    nvs_planted[1] = (char *)"{\"min_move_distance\":0.5,\"timeout\":3,\"is_active\":true}";
    nvs_planted_int = 10;
    nvs_planted_int_present = 1;
}

/*******************************************************************************
 * The tests
 ******************************************************************************/

/* The headline. A hub being configured for the first time: NVS holds none of
** the three keys, and every write succeeds, which is the ordinary healthy case
** and not an error case at all.
**
** Against the unfixed file this test does not fail, it *aborts*: the inverted
** test at the predefined setting turns the successful write into ret = 1, and
** ESP_ERROR_CHECK(1) reaches the esp_err stub's abort. No tally is printed and
** rc is 134, which is the hub's panic reproduced. That is why setvbuf runs
** first in main -- see the note there. */
static void test_first_configuration_reports_success(void)
{
    l_ezlopi_device_t device;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    /* reset_all() already left every nvs_write_result[] at EZPI_SUCCESS --
    ** writes work, as they normally do. Stated rather than re-assigned, because
    ** this case is the healthy one and the reset is what defines it. */

    rc = HILINK_presence_sensor_initialize_settings(&device);

    CHECK(0 == rc, "first configuration with a working NVS reports success instead of panicking");
    CHECK(3 == list_length(&device), "all three settings were added to the device");
    CHECK(0 == allocs_refused, "no allocation was refused, so nothing here is an allocation-failure path");

    /* The defaults the file itself chooses. Asserted so that a later change to
    ** them is a decision rather than a drift. */
    CHECK(1 == nvs_writes[0], "the predefined mode was written to NVS exactly once");
    CHECK(0 == strcmp("basic_routine_mode_mid", nvs_written[0]),
          "the predefined default written to NVS is basic_routine_mode_mid");
    CHECK(1 == nvs_writes[1], "the user-defined mode was written to NVS exactly once");
    CHECK(1 == nvs_writes[2], "the radar sensitivity was written to NVS exactly once");
    CHECK(0 == strcmp("10", nvs_written[2]), "the radar sensitivity default written to NVS is 10");

    release_settings_list(&device);
}

/* The same path with a write failing, which is the case the inverted test was
** presumably meant to catch and instead hid. A failed write must reach the
** caller.
**
** One key at a time, and the other two keys pre-planted so that they take the
** "already exists" branch and do not write at all. That arrangement is the
** whole point of this case, and it took two attempts to get right:
**
**   - Failing all three writes at once, with one shared result, passes against
**     the unfixed file. Only two of the three tests are inverted; the radar one
**     is correct, so it alone returns 1 and the check on the return is content.
**   - Failing one write at a time, with the other keys still absent, *also*
**     passes against the unfixed file, and for a more interesting reason: an
**     inverted test reports failure when the write *succeeds*. So failing
**     "predef" leaves the predefined initializer silent, and the user-defined
**     one -- whose write succeeded -- sets ret = 1 instead. The return is
**     non-zero for exactly the wrong reason.
**
** With the other keys planted, only the key under test writes anything, so the
** return value answers for that initializer and nothing else. On the unfixed
** file the first two subcases come back rc == 0: the write failed and nobody
** said so. The third is the control -- it is the one of the three that was
** already written correctly, it passes before and after, and if it ever goes
** red the fix below has broken the case it was modelled on. */
static void test_a_failing_nvs_write_is_reported(void)
{
    static const char *const which[NVS_KEYS] = {
        "an NVS write that failed is reported to the caller: predefined mode",
        "an NVS write that failed is reported to the caller: user-defined mode",
        "an NVS write that failed is reported to the caller: radar sensitivity",
    };
    int k;

    for (k = 0; k < NVS_KEYS; k++)
    {
        l_ezlopi_device_t device;
        int rc;
        int j;

        reset_all();
        memset(&device, 0, sizeof(device));

        /* Everything except the key under test already has a value, so only
        ** the key under test reaches a write. */
        if (0 != k)
        {
            nvs_planted[0] = (char *)"basic_routine_mode_mid";
        }
        if (1 != k)
        {
            nvs_planted[1] = (char *)"{\"min_move_distance\":0.5,\"timeout\":3,\"is_active\":true}";
        }
        if (2 != k)
        {
            nvs_planted_int = 10;
            nvs_planted_int_present = 1;
        }

        nvs_write_result[k] = EZPI_ERR_NVS_WRITE_FAILED;

        rc = HILINK_presence_sensor_initialize_settings(&device);

        CHECK(0 != rc, which[k]);

        CHECK(1 == nvs_writes[k], "the key under test was written exactly once");
        for (j = 0; j < NVS_KEYS; j++)
        {
            if (j != k)
            {
                CHECK(0 == nvs_writes[j],
                      "a key that already had a value was not rewritten");
            }
        }

        release_settings_list(&device);
    }
}

/* The second boot. The keys are present, so no write happens -- and the string
** NVS hands over belongs to the caller.
**
** EZPI_core_nvs_read_str returns ezlopi_malloc'd memory; the user-defined
** initializer frees it, the predefined one never did. On the unfixed file this
** comes back as the free log missing a pointer, not as a sanitizer report,
** because the test is holding its own copy of the address. LeakSanitizer would
** have caught it too, but a named failed check says which string and which
** initializer. */
static void test_a_stored_value_is_read_and_released(void)
{
    l_ezlopi_device_t device;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    nvs_planted[0] = (char *)"movement_mode_long";
    nvs_planted[1] = (char *)"{\"min_move_distance\":0.75,\"timeout\":5,\"is_active\":true}";
    nvs_planted_int = 42;
    nvs_planted_int_present = 1;

    rc = HILINK_presence_sensor_initialize_settings(&device);

    CHECK(0 == rc, "reading stored settings reports success");
    CHECK(0 == nvs_writes[0] + nvs_writes[1] + nvs_writes[2],
          "nothing was written, because every key was already present");

    CHECK(NULL != nvs_handed_out[0], "the predefined initializer did read its stored string");
    CHECK((NULL != nvs_handed_out[0]) && was_freed(nvs_handed_out[0]),
          "the predefined initializer released the string NVS gave it");
    CHECK(NULL != nvs_handed_out[1], "the user-defined initializer did read its stored string");
    CHECK((NULL != nvs_handed_out[1]) && was_freed(nvs_handed_out[1]),
          "the user-defined initializer released the string NVS gave it");

    release_settings_list(&device);
}

/* The payload-allocation failure path, and the node-free defect on it.
**
** The budget is set so that the three node allocations in the stub -- which use
** libc malloc directly -- are unaffected, and only the subject's own
** ezlopi_malloc payloads are refused.
**
** Against the unfixed file with the ESP_ERROR_CHECK removed, the first node's
** free is an ASan heap-use-after-free inside the copy of
** add_settings_to_device_v3 on the *next* call, exactly as in the ENS160 file:
** the node is dangling before initialization has finished. With the frees
** restored one at a time the later ones come back through the free log. */
static void test_a_failed_payload_allocation_leaves_the_nodes_linked(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *curr;
    int rc;
    int n;

    reset_all();
    memset(&device, 0, sizeof(device));

    alloc_budget = 0; /* every ezlopi_malloc in the subject is refused */
    rc = HILINK_presence_sensor_initialize_settings(&device);
    alloc_budget = -1;

    CHECK(3 == allocs_refused, "all three payload allocations were refused, so all three paths ran");
    CHECK(0 != rc, "a failed payload allocation is reported instead of aborting or passing");

    n = list_length(&device);
    CHECK(3 == n, "all three nodes are still on the device list");

    for (curr = device.settings; curr; curr = curr->next)
    {
        if (was_freed(curr))
        {
            CHECK(0, "a settings node was freed while the device list still pointed at it");
            break;
        }
        CHECK(NULL == curr->user_arg,
              "a node whose payload could not be allocated reads as unavailable, not as a default");
    }

    CHECK(0 == free_log_overflowed, "the free log did not overflow, so its answers above are complete");

    release_settings_list(&device);
}

/* The radar initializer's payload-failure path on its own.
**
** The case above refuses all three payloads, and cannot see this: the
** predefined and user-defined initializers both report 1, so the return is
** non-zero whatever the radar one says. The radar one said `ret = 0` -- a
** payload that could not be allocated reported as success.
**
** A budget of 2 lets the first two payloads through and refuses only the third,
** which makes the return value the radar initializer's answer alone. Keys are
** planted for the reason at plant_everything(). */
static void test_a_failed_radar_payload_is_reported(void)
{
    l_ezlopi_device_t device;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything();
    alloc_budget = 2; /* the third payload allocation is refused */
    rc = HILINK_presence_sensor_initialize_settings(&device);
    alloc_budget = -1;

    CHECK(1 == allocs_refused, "exactly one payload allocation was refused");
    CHECK(0 != rc, "a radar payload that could not be allocated is reported, not passed off as success");
    CHECK(3 == list_length(&device), "all three nodes are still on the device list");

    release_settings_list(&device);
}

/* The uninitialized-setting state, reached the only way it can be reached from
** outside: the node allocation for the third setting fails.
**
** Two defects meet here. The radar initializer reports that as ret = 1, so the
** caller could have known -- but HILINK_presence_sensor_initialize_settings
** never assigns its own ret, so the information was discarded. And
** HILINK_presence_sensor_target_in_detectable_range reads
** `...setting->user_arg` while checking only the payload behind it, so with the
** node pointer NULL it dereferences NULL.
**
** The call below segfaults against the unfixed file -- a hard SEGV at the
** dereference, which UBSan names as a null member access first. That is the
** half of this case that was unambiguous. The CHECK on rc was not: see
** plant_everything(), which is why it is called here.
**
** Both defects had to be fixed together for this case to report rather than
** crash, so neither is proven red on its own by this run; the rc half is proven
** by test_a_failed_first_node_is_reported(), which reaches the same missing
** propagation without touching the dereference. */
static void test_an_unallocated_node_is_survivable(void)
{
    l_ezlopi_device_t device;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything(); /* so the return value answers for the node, not a write */
    add_settings_fail_on_call = 3; /* the radar distance sensitivity setting */

    rc = HILINK_presence_sensor_initialize_settings(&device);

    CHECK(0 != rc, "a settings node that could not be allocated is reported to the caller");
    CHECK(2 == list_length(&device), "the two settings that were allocated are on the device");

    /* Must not dereference the node it never got. */
    CHECK(false == HILINK_presence_sensor_target_in_detectable_range(100),
          "an uninitialized distance setting reports out-of-range rather than dereferencing NULL");

    release_settings_list(&device);
}

/* The predefined initializer's missing `else`. Its node allocation fails, so it
** has no node at all -- and on the unfixed file it returns 0 regardless,
** because only the user-defined initializer sets ret = 1 in that case.
**
** Seen red as a plain failed check on the unfixed file: rc came back 0. That
** reading required plant_everything(); without it this case passed, because
** the inverted user-defined write test contributed a 1 of its own. The added
** check on the write counts is there to keep that from coming back silently --
** if the planting ever stops taking effect, it fails before the rc check does
** and says so. */
static void test_a_failed_first_node_is_reported(void)
{
    l_ezlopi_device_t device;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything(); /* so the return value answers for the node, not a write */
    add_settings_fail_on_call = 1; /* the predefined setting */

    rc = HILINK_presence_sensor_initialize_settings(&device);

    CHECK(0 == nvs_writes[0] + nvs_writes[1] + nvs_writes[2],
          "nothing was written, so the return value is the node result alone");
    CHECK(0 != rc, "a failed node allocation in the predefined initializer is reported");
    CHECK(2 == list_length(&device), "the other two settings were still added");

    release_settings_list(&device);
}

/*******************************************************************************
 * The set and reset paths.
 *
 * Everything above this point is about initialization. These two cases are
 * about what happens afterwards, when the app changes a setting -- and they are
 * here because four more inverted NVS-write tests live on those paths, where
 * the inversion costs more than a wrong return value.
 *
 * On the set and reset paths the branch guarded by the write test is the branch
 * that programs the radar. Written as `EZPI_SUCCESS ==`, a *successful* write
 * reported failure and skipped it, and only a *failed* write went on to
 * configure the sensor. So choosing a presence mode in the app stored the value
 * and left the radar exactly as it was, every time, and the stored value was
 * then read back on the next boot -- so the setting looked correct in the app
 * while the sensor behaved as it always had. That is a defect a user would
 * describe as "the modes do nothing", and no amount of reading the settings
 * back would have shown it.
 *
 * The radar is reached through ld2410_set_template(), which the stub counts.
 * That counter is the whole evidence here: NVS holding the right string proves
 * nothing, because NVS held the right string before the fix too.
 ******************************************************************************/

/* Setting a user-defined mode must reach the radar.
**
** The planted predefined mode is "user_defined_mode" rather than the default,
** and that is not incidental. __setting_set_change_setting_template only
** applies a custom template when the predefined setting names the user-defined
** mode; with any other predefined mode it calls
** __setting_set_change_user_defined_template(false), which deliberately
** touches nothing -- "Don't change is_active member if user defined setting is
** changed but predefined setting is not set to user_defined_mode". The first
** draft of this case planted the default and then asserted the radar was
** programmed, which made the check red against the *fixed* file for a
** behaviour the file documents as intended. The expectation was wrong, not the
** code. Written down because the two outcomes look identical from the
** counter alone: no radar call. */
static void test_setting_a_user_defined_mode_reaches_the_radar(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *node;
    cJSON *cj_properties;
    cJSON *cj_value;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything();
    nvs_planted[0] = (char *)"user_defined_mode";
    (void)HILINK_presence_sensor_initialize_settings(&device);

    node = nth_setting(&device, 1);
    CHECK(NULL != node, "the user-defined setting node exists");
    CHECK(0 == ld2410_set_template_calls, "initialization did not program the radar by itself");

    cj_properties = cJSON_CreateObject(__func__);
    cj_value = cJSON_AddObjectToObject(__func__, cj_properties, "value");
    cJSON_AddNumberToObject(__func__, cj_value, "min_move_distance", 0.75);
    cJSON_AddNumberToObject(__func__, cj_value, "max_move_distance", 3.0);
    cJSON_AddNumberToObject(__func__, cj_value, "min_still_distance", 0.75);
    cJSON_AddNumberToObject(__func__, cj_value, "max_still_distance", 3.0);
    cJSON_AddNumberToObject(__func__, cj_value, "timeout", 7);
    cJSON_AddBoolToObject(__func__, cj_value, "is_active", true);

    rc = node->func(EZLOPI_SETTINGS_ACTION_SET_SETTING, node, cj_properties, node->user_arg);

    CHECK(0 == rc, "setting a user-defined mode reports success");
    CHECK(ld2410_set_template_calls > 0,
          "setting a user-defined mode programmed the radar");
    CHECK(CUSTOM_TEMPLATE == ld2410_last_template,
          "the radar was given the custom template, not a predefined one");

    /* Two writes, not one, and that is the file's own behaviour rather than a
    ** defect: __setting_set_user_defined_setting stores what arrived, and the
    ** apply path it then calls stores the payload a second time with is_active
    ** set. The second write carries a different value, so it is not redundant.
    ** Pinned at 2 so that a later change to either write shows up here. */
    CHECK(2 == nvs_writes[1], "the user-defined mode was stored, then stored again as active");
    CHECK(NULL != strstr(nvs_written[1], "\"is_active\":true"),
          "the value left in NVS is the active one");

    cJSON_Delete(__func__, cj_properties);
    release_settings_list(&device);
}

/* And a failing write on that path must be reported and must not claim to have
** applied anything. The mirror of the case above: it is the subcase that tells
** the fix apart from simply deleting the test. */
static void test_a_failing_write_on_the_set_path_is_reported(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *node;
    cJSON *cj_properties;
    cJSON *cj_value;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything();
    (void)HILINK_presence_sensor_initialize_settings(&device);

    node = nth_setting(&device, 1);

    cj_properties = cJSON_CreateObject(__func__);
    cj_value = cJSON_AddObjectToObject(__func__, cj_properties, "value");
    cJSON_AddNumberToObject(__func__, cj_value, "min_move_distance", 0.75);
    cJSON_AddBoolToObject(__func__, cj_value, "is_active", true);

    nvs_write_result[1] = EZPI_ERR_NVS_WRITE_FAILED;

    rc = node->func(EZLOPI_SETTINGS_ACTION_SET_SETTING, node, cj_properties, node->user_arg);

    CHECK(0 != rc, "a failed write on the set path is reported to the caller");
    CHECK(0 == ld2410_set_template_calls,
          "a mode that could not be stored was not applied to the radar");

    cJSON_Delete(__func__, cj_properties);
    release_settings_list(&device);
}

/* Resetting the predefined mode must reach the radar too, with the default
** template rather than the custom one. */
static void test_resetting_the_predefined_mode_reaches_the_radar(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *node;
    cJSON *cj_params;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything();
    (void)HILINK_presence_sensor_initialize_settings(&device);

    node = nth_setting(&device, 0);
    CHECK(NULL != node, "the predefined setting node exists");

    cj_params = cJSON_CreateObject(__func__);

    rc = node->func(EZLOPI_SETTINGS_ACTION_RESET_SETTING, node, cj_params, node->user_arg);

    CHECK(0 == rc, "resetting the predefined mode reports success");
    CHECK(ld2410_set_template_calls > 0,
          "resetting the predefined mode programmed the radar");
    CHECK(BASIC_TEMPLATE_MID_RANGE == ld2410_last_template,
          "the radar was given the mid-range basic template, which is the documented default");
    CHECK(0 == strcmp("basic_routine_mode_mid", nvs_written[0]),
          "the default mode string was stored");

    cJSON_Delete(__func__, cj_params);
    release_settings_list(&device);
}

/* A radar that refuses the template must be reported rather than ignored. The
** apply path discarded ld2410_set_template()'s return value while its sibling
** checked it. */
static void test_a_radar_that_refuses_a_template_is_reported(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *node;
    cJSON *cj_params;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything();
    (void)HILINK_presence_sensor_initialize_settings(&device);

    node = nth_setting(&device, 0);
    cj_params = cJSON_CreateObject(__func__);
    ld2410_set_template_result = ESP_FAIL;

    rc = node->func(EZLOPI_SETTINGS_ACTION_RESET_SETTING, node, cj_params, node->user_arg);

    CHECK(0 != rc, "a radar that refused the template is reported to the caller");
    CHECK(ld2410_set_template_calls > 0, "the radar was asked, which is what makes the check above mean anything");

    cJSON_Delete(__func__, cj_params);
    release_settings_list(&device);
}

/*******************************************************************************
 * The apply path, reached through its caller.
 *
 * The four cases above left nine of the re-breaks in probe/hlbreak.py green --
 * an uncommitted throwaway in the audit working directory beside this
 * checkout, not a path in this tree. Five of those nine failed for one shared
 * reason rather than five:
 * __setting_set_change_setting_template discarded the return of all three of
 * its calls to __setting_set_change_user_defined_template and then returned a
 * `ret` that nothing in that branch had written. Every defect inside the callee
 * -- a failed NVS write, a radar refusing the custom template, a missing node,
 * a missing payload, an unserializable setting -- set ret = 1 and had it thrown
 * away one frame up.
 *
 * So the fixes in the callee were real but could not be observed from outside
 * it, and a test suite is the only thing that could have told me that. These
 * cases exist to reach the callee through its real caller, now that the caller
 * keeps the answer.
 ******************************************************************************/

/* Resetting the user-defined mode, with the predefined mode set to
** user_defined_mode so the apply path actually runs. Reaches the second
** inverted write and the second leaked setting string, neither of which the
** reset case above could see -- it resets the predefined node, which takes the
** non-custom branch. */
static void test_resetting_the_user_defined_mode_reaches_the_radar(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *node;
    cJSON *cj_params;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything();
    nvs_planted[0] = (char *)"user_defined_mode";
    (void)HILINK_presence_sensor_initialize_settings(&device);

    node = nth_setting(&device, 1);
    cj_params = cJSON_CreateObject(__func__);

    rc = node->func(EZLOPI_SETTINGS_ACTION_RESET_SETTING, node, cj_params, node->user_arg);

    CHECK(0 == rc, "resetting the user-defined mode reports success");
    CHECK(ld2410_set_template_calls > 0, "resetting the user-defined mode programmed the radar");
    CHECK(CUSTOM_TEMPLATE == ld2410_last_template,
          "the radar was given the custom template");
    CHECK(2 == nvs_writes[1], "the default was stored, then stored again as active");

    cJSON_Delete(__func__, cj_params);
    release_settings_list(&device);
}

/* The radar refusing a *custom* template. The earlier refusal case resets the
** predefined node, which calls ld2410_set_template(template, NULL) on the
** non-custom branch -- a different call site, whose return was already
** checked. This one reaches the custom-template call, whose return was
** discarded. */
static void test_a_radar_that_refuses_a_custom_template_is_reported(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *node;
    cJSON *cj_properties;
    cJSON *cj_value;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything();
    nvs_planted[0] = (char *)"user_defined_mode";
    (void)HILINK_presence_sensor_initialize_settings(&device);

    node = nth_setting(&device, 1);
    cj_properties = cJSON_CreateObject(__func__);
    cj_value = cJSON_AddObjectToObject(__func__, cj_properties, "value");
    cJSON_AddNumberToObject(__func__, cj_value, "min_move_distance", 0.75);
    cJSON_AddNumberToObject(__func__, cj_value, "max_move_distance", 3.0);
    cJSON_AddNumberToObject(__func__, cj_value, "timeout", 7);
    cJSON_AddBoolToObject(__func__, cj_value, "is_active", true);

    ld2410_set_template_result = ESP_FAIL;

    rc = node->func(EZLOPI_SETTINGS_ACTION_SET_SETTING, node, cj_properties, node->user_arg);

    CHECK(0 != rc, "a radar that refused the custom template is reported to the caller");
    CHECK(ld2410_set_template_calls > 0, "the radar was asked, which is what makes the check above mean anything");

    cJSON_Delete(__func__, cj_properties);
    release_settings_list(&device);
}

/* Choosing "user defined mode" in the app when the user-defined payload could
** not be allocated. This is the only route from outside the file to the apply
** path's missing-payload case, and it is a real one: the predefined setting is
** set to user_defined_mode, which sends the apply path at a node that has no
** payload to apply. */
static void test_choosing_user_defined_mode_without_a_payload_is_reported(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *node;
    cJSON *cj_properties;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything();
    alloc_budget = 1; /* the predefined payload is allocated; the others are refused */
    (void)HILINK_presence_sensor_initialize_settings(&device);
    alloc_budget = -1;

    node = nth_setting(&device, 1);
    CHECK(NULL != node, "the user-defined node is on the list");
    CHECK(NULL == node->user_arg, "with no payload, which is the state this case is about");

    node = nth_setting(&device, 0);
    CHECK(NULL != node->user_arg, "the predefined node does have a payload, so the set below gets that far");

    cj_properties = cJSON_CreateObject(__func__);
    cJSON_AddStringToObject(__func__, cj_properties, "value", "user_defined_mode");

    rc = node->func(EZLOPI_SETTINGS_ACTION_SET_SETTING, node, cj_properties, node->user_arg);

    CHECK(0 != rc, "choosing a mode that cannot be applied is reported rather than confirmed");
    CHECK(0 == ld2410_set_template_calls, "and the radar was left alone");

    cJSON_Delete(__func__, cj_properties);
    release_settings_list(&device);
}

/* Resetting a setting whose payload could not be allocated. The guard here
** tested `setting` -- the pointer the line above had already tested -- so it
** was always true, and the memset below it wrote through a NULL payload. */
static void test_a_reset_without_a_payload_is_reported(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *node;
    cJSON *cj_params;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything();
    alloc_budget = 1; /* the predefined payload is allocated; the others are refused */
    (void)HILINK_presence_sensor_initialize_settings(&device);
    alloc_budget = -1;

    node = nth_setting(&device, 1);
    CHECK(NULL == node->user_arg, "the node has no payload");

    cj_params = cJSON_CreateObject(__func__);

    rc = node->func(EZLOPI_SETTINGS_ACTION_RESET_SETTING, node, cj_params, node->user_arg);

    CHECK(0 != rc, "resetting a setting with no payload is reported, not written through NULL");
    CHECK(0 == nvs_writes[1], "and nothing was stored");

    cJSON_Delete(__func__, cj_params);
    release_settings_list(&device);
}

/* A presence mode that could not be stored. The predefined set path's write
** test was the only one in the file written the right way round, and the line
** after it overwrote `ret` -- so the correct test was worth nothing, on every
** call with a mode name the app can actually send. A non-custom mode is used
** here deliberately: it takes the branch whose own return is 0, so the only
** thing a non-zero result can be reporting is the write. */
static void test_a_failing_write_on_the_predefined_set_path_is_reported(void)
{
    l_ezlopi_device_t device;
    l_ezlopi_device_settings_v3_t *node;
    cJSON *cj_properties;
    int rc;

    reset_all();
    memset(&device, 0, sizeof(device));

    plant_everything();
    (void)HILINK_presence_sensor_initialize_settings(&device);

    node = nth_setting(&device, 0);
    cj_properties = cJSON_CreateObject(__func__);
    cJSON_AddStringToObject(__func__, cj_properties, "value", "basic_routine_mode_long");

    nvs_write_result[0] = EZPI_ERR_NVS_WRITE_FAILED;

    rc = node->func(EZLOPI_SETTINGS_ACTION_SET_SETTING, node, cj_properties, node->user_arg);

    CHECK(1 == nvs_writes[0], "the write was attempted, so the result below is the write's");
    CHECK(0 != rc, "a presence mode that could not be stored is reported rather than confirmed");
    CHECK(BASIC_TEMPLATE_LONG_RANGE == ld2410_last_template,
          "the radar was still given the template, which is what makes the check above the write's alone");

    cJSON_Delete(__func__, cj_properties);
    release_settings_list(&device);
}

/*******************************************************************************
 * What this suite does not prove.
 *
 * probe/hlbreak.py -- again, an uncommitted throwaway beside this checkout and
 * not a path in this tree -- re-breaks each fix in the subject one at a time
 * and records what goes red. Five of its reversals leave this suite green, and
 * they are listed here rather than left to be rediscovered -- or worse,
 * papered over with a check that does not actually observe them.
 *
 *   O, CC  The two added NULL guards on the file-scope setting nodes
 *          (__setting_set_change_user_defined_template, and the predefined
 *          read inside __setting_set_change_setting_template). Both globals are
 *          NULL only before initialization has run, and every route into those
 *          functions from outside the file is through a settings node, which
 *          does not exist until initialization has run. The guards are
 *          defensive and consistent with the rest of the file; they are not
 *          provable from here, and saying so is better than pretending.
 *
 *   AA3    The `false` arm of the apply path. It differs from the broken
 *          version only when the global user-defined node's payload is missing
 *          while the node the caller was handed still has one. For a single
 *          device those are the same node, so no single-device test can tell
 *          the two apart.
 *
 *   Z      __setting_update_user_defined_setting reading its parameter rather
 *          than the global. Same shape: observable only with two hilink
 *          sensors on one hub, where the globals point at whichever was
 *          initialized last. That is a real defect -- and this file keeps every
 *          setting in file-scope statics, so a second hilink sensor is broken
 *          far more generally than this one read. Out of scope for this change,
 *          and not claimed as fixed.
 *
 *   Q      The missing `else` on an unserializable setting. Reaching it needs
 *          cJSON_PrintBuffered to fail, and cjext.c is compiled without
 *          stubs-allocfail on its include path, so its allocations go to libc
 *          malloc and are outside this test's budget. Injecting a cJSON
 *          failure is a harness change of its own.
 ******************************************************************************/

int main(void)
{
    /* Line-buffered before anything else. Several of the re-breaks behind these
    ** checks end in abort(), and with stdout fully buffered into a pipe every
    ** line already printed -- FAIL lines among them -- is lost with the buffer.
    ** That cost a wrong reading of a red run once already, in test_ens160set.c.
    ** Not worth rediscovering here. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    test_first_configuration_reports_success();
    test_a_failing_nvs_write_is_reported();
    test_a_stored_value_is_read_and_released();
    test_a_failed_payload_allocation_leaves_the_nodes_linked();
    test_a_failed_radar_payload_is_reported();
    test_an_unallocated_node_is_survivable();
    test_a_failed_first_node_is_reported();

    test_setting_a_user_defined_mode_reaches_the_radar();
    test_a_failing_write_on_the_set_path_is_reported();
    test_resetting_the_predefined_mode_reaches_the_radar();
    test_a_radar_that_refuses_a_template_is_reported();

    test_resetting_the_user_defined_mode_reaches_the_radar();
    test_a_radar_that_refuses_a_custom_template_is_reported();
    test_choosing_user_defined_mode_without_a_payload_is_reported();
    test_a_reset_without_a_payload_is_reported();
    test_a_failing_write_on_the_predefined_set_path_is_reported();

    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
