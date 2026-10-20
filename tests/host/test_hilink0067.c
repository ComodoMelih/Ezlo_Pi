/* What the Hilink presence sensor's item driver does when the radar stops
 * answering.
 *
 * WHAT WAS WRONG
 *
 * sensor_0067_hilink_presence_sensor.c wrapped six calls in ESP_ERROR_CHECK.
 * In the shipped configuration -- CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y
 * with no NDEBUG -- that macro is the _esp_error_check_failed() variant, which
 * panics and reboots the hub. Three of the six are reached by ordinary runtime
 * conditions rather than by programming errors:
 *
 *   :178  ESP_ERROR_CHECK(ld2410_get_data(&present_hilink_data));   in __notify
 *   :305  ESP_ERROR_CHECK(HILINK_presence_sensor_apply_settings()); in __init
 *   :306  ESP_ERROR_CHECK(ld2410_get_data(hilink_data));            in __init
 *
 * The one at :178 is the serious one, and the reason is the cadence rather than
 * the call. __notify is the EZLOPI_ACTION_NOTIFY_1000_MS handler, driven from
 * __device_loop in ezlopi_core_ezlopi.c. So a presence sensor whose radar stops
 * answering over UART does not degrade and does not go quiet -- it reboots the
 * hub, comes back up, polls again, and reboots again. Every other device on the
 * hub goes down with it, on a timer, indefinitely. A loose UART jumper on one
 * sensor takes out the whole product.
 *
 * The two in __init reboot once per boot, which with a genuinely dead radar is
 * also a loop, just a slower one: the device is re-initialised on every boot.
 *
 * The remaining three are wrapped around functions returning ezlopi_error_t,
 * which is not esp_err_t at all -- see WHY THE TYPE IS WRONG TOO below.
 *
 * WHY THE TYPE IS WRONG TOO
 *
 * ezlopi_error_t is `typedef int32_t`, and its failure codes are *positive bit
 * flags*: EZPI_ERR_PREP_DEVICE_PREP_FAILED is 0x100, EZPI_ERR_INIT_DEVICE_FAILED
 * is 0x200, and so on. Only EZPI_FAILED (-1) and EZPI_NOT_AVAILABLE (-2) are
 * negative. esp_err_t codes are a different space entirely. So
 *
 *     ESP_ERROR_CHECK(__add_value_to_cjson(item, cj_params, true));
 *
 * panics on EZPI_FAILED while printing it as though it were an ESP error name.
 * Two of these (:266, :281) are reachable whenever an item's user_arg is NULL,
 * which is the state a failed payload allocation in __prepare leaves behind --
 * __prepare allocates one ld2410_outputs_t for three items and does not undo
 * the device when it fails.
 *
 * A SEVENTH DEFECT, FOUND WHILE READING THE ABOVE
 *
 * __add_value_to_cjson dereferences the pointer it is about to test:
 *
 *     ld2410_outputs_t *hilink_data = (ld2410_outputs_t *)item->user_arg;
 *     if (item && cj_params && hilink_data)
 *
 * `item->user_arg` is read on the line above the `item &&` that is supposed to
 * make reading it safe. This is the same shape as the guard fixed in
 * hilink_presence_sensor_setting.c's reset path, and it is fixed the same way.
 * Honest about the severity: the core always passes a non-NULL item from the
 * device list, so no current caller can reach it. It is checked below because a
 * guard that tests a pointer it has already used is wrong whether or not
 * today's callers happen to spare it, and because the check costs one case.
 *
 * WHAT THE FIX IS, AND WHY NOT JUST DELETE THE MACRO
 *
 * Deleting ESP_ERROR_CHECK and nothing else would turn the :178 reboot into
 * something quieter but worse. __notify memcpy's the stored reading into
 * present_hilink_data *before* the radar read, so a failed read leaves
 * present == stored, __notify_if_needed sees no change and broadcasts nothing --
 * and sec_count keeps advancing. On the 90th tick the forced-update branch
 * memcpy's that stale copy back over the stored data and broadcasts all three
 * items, publishing a reading that no radar produced as if it were current. So
 * the failed read has to stop the update, not merely avoid panicking.
 *
 * WHAT THIS SUITE CANNOT SHOW
 *
 * The return value of __notify is discarded by the caller:
 * ezlopi_core_ezlopi.c:332 calls item_node->func(EZLOPI_ACTION_NOTIFY_1000_MS,
 * ...) without assigning it. So "reports the failure" below is checked at the
 * dispatcher's boundary and goes nowhere in the firmware. The part that matters
 * on a hub is the part that is observable here anyway: it no longer reboots, and
 * it no longer broadcasts.
 *
 * The __init return is also, in effect, discarded -- not by omission but by
 * sign. The core drops a device only when an item's init returns < 0
 * (ezlopi_core_ezlopi.c:239), and this file returns EZPI_ERR_INIT_DEVICE_FAILED,
 * which is +0x200. That is a tree-wide defect recorded under Known gaps in
 * CHANGELOG.md, not something to fix inside one driver: no driver in the tree
 * returns a negative code, so changing this one would make it the only device
 * that can be dropped. The checks below therefore assert what this file
 * *returns*, deliberately not that the device is removed.
 *
 * HOW IT WAS CONFIRMED TO BE ABLE TO FAIL
 *
 * The unfixed file does not fail these checks, it aborts the process: the
 * esp_err.h stub in tests/host/stubs faithfully reproduces the panicking
 * expansion, so the first ESP_ERROR_CHECK to be handed a non-zero value takes
 * the whole binary down with SIGABRT. That was confirmed before the fix, and it
 * is why the checks could not all be seen red at once. See the note above each
 * case.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "cjext.h"
#include "ld2410.h"
#include "ezlopi_core_devices.h"
#include "ezlopi_core_errors.h"
#include "ezlopi_cloud_items.h"
#include "ezlopi_cloud_item_name_str.h"
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

/* The subject's only exported symbol. Declared here rather than included,
** because sensor_0067_hilink_presence_sensor.h declares the prepare/init
** registration shape and this is the one entry point the tests drive. */
extern ezlopi_error_t SENSOR_0067_hilink_presence_sensor_v3(
    e_ezlopi_actions_t action, l_ezlopi_item_t *item, void *arg, void *user_arg);

/*******************************************************************************
 * The controllable allocator, shared with test_hilinkset.c and test_ens160set.c
 * through stubs-allocfail/EZLOPI_USER_CONFIG.h. Not used to force a failure in
 * this file -- none of the six sites is on an allocation path -- but the subject
 * is compiled against that header, so these two have to exist.
 ******************************************************************************/
void *EZPI_test_alloc(unsigned long size);
void EZPI_test_release(void *ptr);

void *EZPI_test_alloc(unsigned long size)
{
    return malloc((size_t)size);
}

void EZPI_test_release(void *ptr)
{
    free(ptr);
}

/*******************************************************************************
 * The radar.
 *
 * ld2410_get_data deliberately does *not* write to its output buffer when it is
 * told to fail. A driver that could not read a frame has nothing to report, and
 * modelling it as "fails but still fills the buffer" would hide the stale-data
 * half of the :178 defect described at the top of this file.
 ******************************************************************************/
static int ld2410_get_data_calls;
static esp_err_t ld2410_get_data_result = ESP_OK;
static ld2410_outputs_t ld2410_planted;

static int ld2410_setup_calls;
static esp_err_t ld2410_setup_result = ESP_OK;

esp_err_t ld2410_get_data(ld2410_outputs_t *output)
{
    ld2410_get_data_calls++;
    if ((ESP_OK == ld2410_get_data_result) && output)
    {
        *output = ld2410_planted;
    }
    return ld2410_get_data_result;
}

esp_err_t ld2410_setup(s_ezlopi_uart_t ld2410_uart)
{
    (void)ld2410_uart;
    ld2410_setup_calls++;
    return ld2410_setup_result;
}

/*******************************************************************************
 * The settings module. The real one is the subject of test_hilinkset.c; here it
 * only has to be able to report failure, because what is under test is whether
 * this file keeps that answer or panics on it.
 ******************************************************************************/
static int apply_settings_calls;
static int apply_settings_result;

int HILINK_presence_sensor_apply_settings(void)
{
    apply_settings_calls++;
    return apply_settings_result;
}

int HILINK_presence_sensor_initialize_settings(l_ezlopi_device_t *device)
{
    (void)device;
    return 0;
}

static bool in_range_result = true;

bool HILINK_presence_sensor_target_in_detectable_range(const uint16_t moving_target_distance)
{
    (void)moving_target_distance;
    return in_range_result;
}

/*******************************************************************************
 * The core calls the subject makes. Only the broadcast is interesting: it is the
 * witness for "a failed radar read published nothing".
 ******************************************************************************/
static int broadcasts;

ezlopi_error_t EZPI_core_device_value_updated_from_device_broadcast(l_ezlopi_item_t *item)
{
    (void)item;
    broadcasts++;
    return EZPI_SUCCESS;
}

/* Referenced by __prepare, which no case below drives. Present so the link
** succeeds, and deliberately inert: if a future case does reach __prepare, a
** NULL device is the honest answer from a stub that allocates nothing, and the
** case will show up as a failure rather than quietly passing. */
l_ezlopi_device_t *EZPI_core_device_add_device(cJSON *cj_device, const char *last_name)
{
    (void)cj_device;
    (void)last_name;
    return NULL;
}

l_ezlopi_item_t *EZPI_core_device_add_item_to_device(l_ezlopi_device_t *device,
                                                    ezlopi_error_t (*func)(e_ezlopi_actions_t action, l_ezlopi_item_t *item, void *arg, void *user_arg))
{
    (void)device;
    (void)func;
    return NULL;
}

void EZPI_core_device_free_device(l_ezlopi_device_t *device)
{
    (void)device;
}

uint32_t EZPI_core_cloud_generate_item_id(void)
{
    static uint32_t next = 0x1000;
    return next++;
}

/* ezlopi_core_cjson_macros.c is linked whole for CJSON_GET_VALUE_GPIO, which
** __prepare uses to read the two UART pins. EZPI_core_cjson_get_value_gpio()
** calls this to reject a pin that belongs to the flash interface, and on a board
** the answer comes out of efuse over esp_mspi_get_io(). There is no efuse here.
**
** false is the honest answer for a file that never asks the question: no case
** below drives __prepare, so no pin is ever extracted. test_cjsongpio.c is where
** that question is the point, and it makes the pin set an input instead. The
** signature is a plain int, matching ezlopi_core_pin_reserved.h:45 -- `const
** int` would not match the declaration. */
bool EZPI_core_pin_is_mspi(int gpio_num)
{
    (void)gpio_num;
    return false;
}

/*******************************************************************************
 * Fixtures
 ******************************************************************************/
static ld2410_outputs_t payload;
static l_ezlopi_item_t item;

static void reset_all(void)
{
    ld2410_get_data_calls = 0;
    ld2410_get_data_result = ESP_OK;
    ld2410_setup_calls = 0;
    ld2410_setup_result = ESP_OK;
    apply_settings_calls = 0;
    apply_settings_result = 0;
    in_range_result = true;
    broadcasts = 0;

    memset(&ld2410_planted, 0, sizeof(ld2410_planted));
    memset(&payload, 0, sizeof(payload));
    memset(&item, 0, sizeof(item));

    item.user_arg = &payload;
    item.interface.uart.enable = true;
    item.interface.uart.tx = 17;
    item.interface.uart.rx = 16;
    /* The real keyword globals, not copies. __add_value_to_cjson selects its
    ** branch by comparing cloud_properties.item_name *by pointer* against
    ** ezlopi_item_name_motion and friends, so a strdup'd equal string would
    ** take no branch at all and every value check below would pass against
    ** nothing. This is why the test links the real ezlopi_cloud_item_name_str.c
    ** instead of stubbing the names. */
    item.cloud_properties.item_name = ezlopi_item_name_motion;
    item.cloud_properties.item_id = 0x2001;
}

/*******************************************************************************
 * __init
 ******************************************************************************/

/* The ordinary case, first, so that the failure cases below are known to differ
** from a working one rather than from a harness that cannot run at all. */
static void test_a_healthy_radar_initializes(void)
{
    ezlopi_error_t rc;

    reset_all();
    ld2410_planted.direction = APPROACHING;
    ld2410_planted.presence = true;
    ld2410_planted.moving_target_distance = 120;

    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_INITIALIZE, &item, NULL, NULL);

    CHECK(EZPI_SUCCESS == rc, "a healthy radar initializes successfully");
    CHECK(1 == ld2410_setup_calls, "the UART was set up once");
    CHECK(1 == apply_settings_calls, "the stored settings were applied once");
    CHECK(1 == ld2410_get_data_calls, "the first reading was taken");
    CHECK(APPROACHING == payload.direction, "the first reading reached the item payload");
    CHECK(120 == payload.moving_target_distance, "the distance reached the item payload");
}

/* :305. On the unfixed file this aborts: apply_settings returns 1, and
** ESP_ERROR_CHECK(1) panics. Confirmed by running this case against the unfixed
** subject and seeing SIGABRT rather than a FAIL line. */
static void test_settings_that_cannot_be_applied_are_reported(void)
{
    ezlopi_error_t rc;

    reset_all();
    apply_settings_result = 1;

    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_INITIALIZE, &item, NULL, NULL);

    CHECK(EZPI_SUCCESS != rc, "settings that could not be applied are reported, not panicked on");
    CHECK(1 == apply_settings_calls, "the settings were attempted");
}

/* :306. The radar answered the setup handshake and then refused to produce a
** frame. Aborts on the unfixed file. */
static void test_a_radar_that_will_not_read_at_init_is_reported(void)
{
    ezlopi_error_t rc;

    reset_all();
    ld2410_get_data_result = ESP_FAIL;

    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_INITIALIZE, &item, NULL, NULL);

    CHECK(EZPI_SUCCESS != rc, "a radar that cannot be read at init is reported");
    CHECK(1 == ld2410_get_data_calls, "the read was attempted exactly once");
}

/* Not a defect -- the pre-existing behaviour, pinned so that the fixes above
** cannot quietly change it. A radar that fails the setup handshake was already
** reported, because that call was tested with `if (ESP_OK == ...)` rather than
** wrapped in ESP_ERROR_CHECK. */
static void test_a_radar_that_will_not_set_up_is_still_reported(void)
{
    ezlopi_error_t rc;

    reset_all();
    ld2410_setup_result = ESP_FAIL;

    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_INITIALIZE, &item, NULL, NULL);

    CHECK(EZPI_SUCCESS != rc, "a radar that cannot be set up is reported");
    CHECK(0 == apply_settings_calls, "settings are not applied when setup failed");
    CHECK(0 == ld2410_get_data_calls, "no reading is attempted when setup failed");
}

static void test_an_item_without_a_payload_does_not_initialize(void)
{
    ezlopi_error_t rc;

    reset_all();
    item.user_arg = NULL;

    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_INITIALIZE, &item, NULL, NULL);

    CHECK(EZPI_SUCCESS != rc, "an item with no payload does not initialize");
    CHECK(0 == ld2410_setup_calls, "the UART is not set up without a payload");
}

/*******************************************************************************
 * __notify -- the 1-second path
 ******************************************************************************/

/* The working case: a changed direction reaches the cloud. Establishes that the
** broadcast counter moves at all, without which the zero asserted in the next
** case would prove nothing. */
static void test_a_changed_reading_is_broadcast(void)
{
    ezlopi_error_t rc;

    reset_all();
    payload.direction = STATIONARY;
    ld2410_planted.direction = APPROACHING;
    ld2410_planted.moving_target_distance = 90;

    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_NOTIFY_1000_MS, &item, NULL, NULL);

    CHECK(EZPI_SUCCESS == rc, "a healthy poll succeeds");
    CHECK(1 == ld2410_get_data_calls, "the radar was polled");
    CHECK(1 == broadcasts, "the changed direction was broadcast");
    CHECK(APPROACHING == payload.direction, "the stored reading was updated");
}

/* :178, the one that reboot-loops a hub. On the unfixed file this aborts.
**
** The two counters are both load-bearing. `broadcasts == 0` is the half that
** would still be wrong if the macro had simply been deleted: with the panic
** gone but no early return, the function proceeds on a copy of the *stored*
** reading, and while this single call broadcasts nothing, sec_count advances
** toward the 90-tick forced update that republishes it as current. The non-zero
** return is what stops that. */
static void test_a_radar_that_stops_answering_does_not_reboot_the_hub(void)
{
    ezlopi_error_t rc;

    reset_all();
    payload.direction = STATIONARY;
    payload.moving_target_distance = 50;
    ld2410_get_data_result = ESP_FAIL;

    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_NOTIFY_1000_MS, &item, NULL, NULL);

    CHECK(EZPI_SUCCESS != rc, "a failed radar read is reported rather than panicked on");
    CHECK(1 == ld2410_get_data_calls, "the radar was polled once");
    CHECK(0 == broadcasts, "nothing was published from a reading that never arrived");
    CHECK(STATIONARY == payload.direction, "the stored reading was left alone");
    CHECK(50 == payload.moving_target_distance, "the stored distance was left alone");
}

/* A dead radar polled repeatedly is the actual field condition -- one failed
** read is a glitch, sixty in a row is a loose jumper. Pinned because the
** forced-update branch counts ticks, so the interesting question is whether a
** long run of failures can still reach it. */
static void test_a_radar_dead_for_many_ticks_never_publishes(void)
{
    int i;
    ezlopi_error_t rc = EZPI_SUCCESS;

    reset_all();
    payload.direction = STATIONARY;
    ld2410_get_data_result = ESP_FAIL;

    for (i = 0; i < 200; i++)
    {
        rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_NOTIFY_1000_MS, &item, NULL, NULL);
    }

    CHECK(EZPI_SUCCESS != rc, "the last of 200 failed polls is still reported");
    CHECK(200 == ld2410_get_data_calls, "every tick polled the radar");
    CHECK(0 == broadcasts, "200 failed polls published nothing, including at the 90-tick forced update");
}

static void test_a_poll_without_a_payload_is_reported(void)
{
    ezlopi_error_t rc;

    reset_all();
    item.user_arg = NULL;

    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_NOTIFY_1000_MS, &item, NULL, NULL);

    CHECK(EZPI_SUCCESS != rc, "a poll with no payload is reported");
    CHECK(0 == ld2410_get_data_calls, "the radar is not polled without somewhere to put the answer");
    CHECK(0 == broadcasts, "nothing is published without a payload");
}

/*******************************************************************************
 * __get_hub_item_value / __get_cjson_value
 ******************************************************************************/

static void test_a_hub_request_carries_the_value_and_the_enum(void)
{
    ezlopi_error_t rc;
    cJSON *cj;

    reset_all();
    item.cloud_properties.item_name = ezlopi_item_name_motion_direction;
    payload.direction = APPROACHING;

    cj = cJSON_CreateObject(__FUNCTION__);
    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_HUB_GET_ITEM, &item, cj, NULL);

    CHECK(EZPI_SUCCESS == rc, "a hub request for a healthy item succeeds");
    CHECK(NULL != cJSON_GetObjectItem(__FUNCTION__, cj, "enum"),
          "a hub request carries the enum list");
    CHECK(NULL != cJSON_GetObjectItem(__FUNCTION__, cj, "value"),
          "a hub request carries the value");

    cJSON_Delete(__FUNCTION__, cj);
}

/* The same item through the non-hub path. The enum is deliberately absent --
** is_hub_request is false -- and pinning that is what makes the enum assertion
** in the case above mean something. */
static void test_a_value_request_carries_no_enum(void)
{
    ezlopi_error_t rc;
    cJSON *cj;

    reset_all();
    item.cloud_properties.item_name = ezlopi_item_name_motion_direction;
    payload.direction = MOVING_AWAY;

    cj = cJSON_CreateObject(__FUNCTION__);
    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_GET_EZLOPI_VALUE, &item, cj, NULL);

    CHECK(EZPI_SUCCESS == rc, "a value request for a healthy item succeeds");
    CHECK(NULL == cJSON_GetObjectItem(__FUNCTION__, cj, "enum"),
          "a value request does not carry the enum list");
    CHECK(NULL != cJSON_GetObjectItem(__FUNCTION__, cj, "value"),
          "a value request carries the value");

    cJSON_Delete(__FUNCTION__, cj);
}

/* :266. An item whose payload allocation failed in __prepare. On the unfixed
** file __add_value_to_cjson returns EZPI_FAILED, which is -1, and
** ESP_ERROR_CHECK(-1) panics -- while printing -1 as though it were an
** esp_err_t name. Aborts on the unfixed file. */
static void test_a_hub_request_without_a_payload_is_reported(void)
{
    ezlopi_error_t rc;
    cJSON *cj;

    reset_all();
    item.user_arg = NULL;

    cj = cJSON_CreateObject(__FUNCTION__);
    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_HUB_GET_ITEM, &item, cj, NULL);

    CHECK(EZPI_SUCCESS != rc, "a hub request for an item with no payload is reported");
    CHECK(NULL == cJSON_GetObjectItem(__FUNCTION__, cj, "value"),
          "no value is invented for an item with no payload");

    cJSON_Delete(__FUNCTION__, cj);
}

/* :281, the same defect on the other dispatcher arm. Separate case because the
** two call sites are separate lines and re-breaking one must not be masked by
** the other. */
static void test_a_value_request_without_a_payload_is_reported(void)
{
    ezlopi_error_t rc;
    cJSON *cj;

    reset_all();
    item.user_arg = NULL;

    cj = cJSON_CreateObject(__FUNCTION__);
    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_GET_EZLOPI_VALUE, &item, cj, NULL);

    CHECK(EZPI_SUCCESS != rc, "a value request for an item with no payload is reported");
    CHECK(NULL == cJSON_GetObjectItem(__FUNCTION__, cj, "value"),
          "no value is invented for an item with no payload");

    cJSON_Delete(__FUNCTION__, cj);
}

/* The seventh defect: the guard that tests a pointer it has already read.
**
** On the unfixed file this is not a failed check and not a panic -- it is a NULL
** dereference at line 225, which under ASan is a SEGV on address 0x0 and takes
** the binary down before any check runs. As noted at the top of this file, no
** caller in the firmware passes a NULL item, so this is a latent guard fault
** rather than a live crash. It is checked because the fix is one line and a
** guard that uses what it is about to validate is wrong on its own terms. */
static void test_a_request_with_no_item_at_all_is_survivable(void)
{
    ezlopi_error_t rc;
    cJSON *cj;

    reset_all();

    cj = cJSON_CreateObject(__FUNCTION__);
    rc = SENSOR_0067_hilink_presence_sensor_v3(EZLOPI_ACTION_HUB_GET_ITEM, NULL, cj, NULL);

    CHECK(EZPI_SUCCESS != rc, "a request with no item is reported rather than dereferenced");

    cJSON_Delete(__FUNCTION__, cj);
}

/*******************************************************************************
 * What this suite does not prove
 *
 * - :243, ESP_ERROR_CHECK(__add_array_to_object(...)). __add_array_to_object
 *   returns EZPI_FAILED only when cj_params or the array is NULL. cj_params is
 *   checked by both callers before the call, and the array is a file-scope
 *   `static const char *[]` that cannot be NULL. So the panic is unreachable
 *   and the fix -- accumulating the return instead of discarding it -- is not
 *   observable from outside the function. Fixed anyway, because it is the same
 *   ESP_ERROR_CHECK-on-ezlopi_error_t error as the other two in that family,
 *   and left recorded here rather than claimed as proven.
 *
 * - That any of this stops a *reboot* on hardware. The esp_err.h stub aborts
 *   where the firmware panics, which is the right shape for a test and is still
 *   a model. No hilink presence sensor is attached to this board; the radar is a
 *   stub that counts calls and returns what it is told to.
 *
 * - That a dropped device is ever actually dropped. See the __init note at the
 *   top: the core's gate is `< 0` and this file returns +0x200, so the init
 *   failures asserted above are correctly *reported* and still ignored one layer
 *   up. Recorded under Known gaps in CHANGELOG.md.
 ******************************************************************************/

int main(void)
{
    test_a_healthy_radar_initializes();
    test_settings_that_cannot_be_applied_are_reported();
    test_a_radar_that_will_not_read_at_init_is_reported();
    test_a_radar_that_will_not_set_up_is_still_reported();
    test_an_item_without_a_payload_does_not_initialize();

    test_a_changed_reading_is_broadcast();
    test_a_radar_that_stops_answering_does_not_reboot_the_hub();
    test_a_radar_dead_for_many_ticks_never_publishes();
    test_a_poll_without_a_payload_is_reported();

    test_a_hub_request_carries_the_value_and_the_enum();
    test_a_value_request_carries_no_enum();
    test_a_hub_request_without_a_payload_is_reported();
    test_a_value_request_without_a_payload_is_reported();
    test_a_request_with_no_item_at_all_is_survivable();

    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
