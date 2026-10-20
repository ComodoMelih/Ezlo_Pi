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
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
}

int main(void)
{
    test_the_layout_that_hid_it();
    test_both_heap_regions_read_as_floats_under_two();
    test_the_getters_return_the_stored_value();

    printf("%d check(s), %d failure(s)\n", checks, failures);
    return (0 == failures) ? 0 : 1;
}
