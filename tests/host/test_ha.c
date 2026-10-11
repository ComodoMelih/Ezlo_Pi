/**
 * @file    test_ha.c
 * @brief   Tests for the EzloPi item -> Home Assistant entity mapping.
 *
 * Every failure this mapping can have is silent. Home Assistant rejects a
 * discovery payload whose device_class and unit_of_measurement disagree, and
 * it does so quietly -- the entity simply never appears, with nothing on the
 * device to say why. A wrong-but-accepted class is worse: the entity shows up
 * with the wrong words on it ("Wet" instead of "Detected"), and looks fine
 * until someone writes an automation against it.
 *
 * None of that is reachable from the firmware's own tests, because none of it
 * happens on the device. So the mapping is kept free of ESP-IDF dependencies
 * and checked here instead, over the combinations the tree can actually
 * produce -- the 59 item names and 26 value types that drivers in
 * ezlopi-sensors-devices assign, rather than an invented sample.
 *
 * The object-id reducer is tested separately and harder than it looks like it
 * deserves. It takes a user-supplied device name and has to produce something
 * valid for an MQTT topic segment inside a fixed buffer, which is a truncation
 * bug waiting to happen; the cases below include every buffer size from zero
 * up to longer than the input.
 *
 * Deliberately has no test-framework dependency -- plain C and a counter -- to
 * match the rest of tests/host.
 */

#include <stdio.h>
#include <string.h>

#include "ezlopi_util_ha.h"

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

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/
static int streq(const char *a, const char *b)
{
    if (NULL == a || NULL == b)
    {
        return (a == b);
    }
    return (0 == strcmp(a, b));
}

static const char *or_null(const char *s)
{
    return (NULL == s) ? "(null)" : s;
}

/* ---------------------------------------------------------------------------
 * Booleans
 * ------------------------------------------------------------------------ */
static void test_bool_setter_is_a_switch(void)
{
    s_ezpi_ha_entity_t e;

    CHECK(EZPI_ha_describe("switch", "bool", NULL, true, &e), "settable bool should map");
    CHECK(EZPI_HA_COMPONENT_SWITCH == e.component, "settable bool should be a switch");
    CHECK(streq("switch", EZPI_ha_component_str(e.component)), "switch platform name");

    /* The same item name without a setter is a different platform entirely.
     * This is the distinction that makes a relay controllable and a dry
     * contact readable, and it comes from has_setter alone. */
    CHECK(EZPI_ha_describe("switch", "bool", NULL, false, &e), "read-only bool should map");
    CHECK(EZPI_HA_COMPONENT_BINARY_SENSOR == e.component, "read-only bool should be a binary_sensor");
}

static void test_binary_sensor_device_classes(void)
{
    static const struct
    {
        const char *item;
        const char *expect;
    } cases[] = {
        {"motion", "motion"},
        {"dw_state", "door"},
        {"water_leak_alarm", "moisture"},
        {"gas_alarm", "gas"},
        {"co_alarm", "carbon_monoxide"},
        {"heat_alarm", "heat"},
        {"light_alarm", "light"},
        {"water_level_alarm", "problem"},
        {"water_filter_replacement_alarm", "problem"},
        /* Not in Home Assistant's list, and deliberately left unclassified
         * rather than forced into a near neighbour. */
        {"activity", NULL},
        {"basic", NULL},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        s_ezpi_ha_entity_t e;
        CHECK(EZPI_ha_describe(cases[i].item, "bool", NULL, false, &e),
              "%s should map", cases[i].item);
        CHECK(streq(cases[i].expect, e.device_class),
              "%s device_class: got %s, want %s", cases[i].item,
              or_null(e.device_class), or_null(cases[i].expect));
    }

    /* A device class belongs to the read-only form only. A switch is a thing
     * the user operates, and Home Assistant's binary-sensor classes describe
     * what a reading means, not what a relay does. */
    s_ezpi_ha_entity_t e;
    CHECK(EZPI_ha_describe("motion", "bool", NULL, true, &e), "settable motion should map");
    CHECK(NULL == e.device_class, "a switch should carry no binary-sensor device class");
}

/* ---------------------------------------------------------------------------
 * Units
 * ------------------------------------------------------------------------ */
static void test_units(void)
{
    CHECK(streq("\xC2\xB0" "C", EZPI_ha_unit_from_scale("celsius")), "celsius -> degree C");
    CHECK(streq("%", EZPI_ha_unit_from_scale("percent")), "percent -> %%");
    CHECK(streq("lx", EZPI_ha_unit_from_scale("lux")), "lux -> lx");
    CHECK(streq("\xC2\xB5g/m\xC2\xB3", EZPI_ha_unit_from_scale("micro_gram_per_cubic_meter")),
          "micro_gram_per_cubic_meter -> ug/m3");
    CHECK(streq("W/m\xC2\xB2", EZPI_ha_unit_from_scale("watt_per_square_meter")),
          "watt_per_square_meter -> W/m2");

    /* Spelled with the typo the firmware actually carries. Matching the
     * corrected spelling would mean matching a string no item ever has. */
    CHECK(streq("\xC2\xB0", EZPI_ha_unit_from_scale("north_pole_degress")),
          "north_pole_degress -> degree");

    /* An unknown scale must come back NULL rather than being passed through.
     * Passing it through would put the word "furlongs_per_fortnight" on every
     * reading in the user's dashboard. */
    CHECK(NULL == EZPI_ha_unit_from_scale("furlongs_per_fortnight"), "unknown scale -> NULL");
    CHECK(NULL == EZPI_ha_unit_from_scale(""), "empty scale -> NULL");
    CHECK(NULL == EZPI_ha_unit_from_scale(NULL), "NULL scale -> NULL");
}

/* ---------------------------------------------------------------------------
 * Sensors
 * ------------------------------------------------------------------------ */
static void test_sensor_classes_and_units(void)
{
    s_ezpi_ha_entity_t e;

    CHECK(EZPI_ha_describe("temp", "temperature", "celsius", false, &e), "temp should map");
    CHECK(EZPI_HA_COMPONENT_SENSOR == e.component, "temp is a sensor");
    CHECK(streq("temperature", e.device_class), "temp device_class");
    CHECK(streq("\xC2\xB0" "C", e.unit), "temp unit");
    CHECK(streq("measurement", e.state_class),
          "a reading needs state_class measurement or Home Assistant keeps no statistics");

    CHECK(EZPI_ha_describe("humidity", "humidity", "percent", false, &e), "humidity should map");
    CHECK(streq("humidity", e.device_class), "humidity device_class");
    CHECK(streq("%", e.unit), "humidity unit");

    /* Three particulate sizes arrive as the same value type with the same
     * scale, so the item name is the only thing that distinguishes them. All
     * three are checked: with only one covered, a table entry copy-pasted and
     * not edited -- the most likely way this breaks -- goes unnoticed, which
     * is exactly what mutation testing caught here. */
    static const struct
    {
        const char *item;
        const char *expect;
    } pm[] = {
        {"particulate_matter_1", "pm1"},
        {"particulate_matter_2_dot_5", "pm25"},
        {"particulate_matter_10", "pm10"},
    };

    for (size_t i = 0; i < sizeof(pm) / sizeof(pm[0]); i++)
    {
        s_ezpi_ha_entity_t p;
        CHECK(EZPI_ha_describe(pm[i].item, "substance_amount",
                               "micro_gram_per_cubic_meter", false, &p),
              "%s should map", pm[i].item);
        CHECK(streq(pm[i].expect, p.device_class),
              "%s device_class: got %s, want %s", pm[i].item,
              or_null(p.device_class), pm[i].expect);
    }

    CHECK(EZPI_ha_describe("co2_level", "substance_amount", "parts_per_million", false, &e),
          "co2 should map");
    CHECK(streq("carbon_dioxide", e.device_class), "co2 device_class");
    CHECK(streq("ppm", e.unit), "co2 unit");

    CHECK(EZPI_ha_describe("solar_radiation", "general_purpose", "watt_per_square_meter", false, &e),
          "solar radiation should map");
    CHECK(streq("irradiance", e.device_class), "solar radiation device_class");

    /* A real quantity Home Assistant has no class for. It is still a sensor
     * with a unit -- dropping the entity would be worse than an unclassified
     * one. */
    CHECK(EZPI_ha_describe("angle_position", "angle", "north_pole_degress", false, &e),
          "angle should still map");
    CHECK(EZPI_HA_COMPONENT_SENSOR == e.component, "angle is a sensor");
    CHECK(NULL == e.device_class, "angle has no Home Assistant class");
    CHECK(streq("\xC2\xB0", e.unit), "angle keeps its unit");
}

/* This is the case the pairing rule exists for, and the one most likely to
 * regress, because it only happens when two independently-set driver fields
 * disagree. Home Assistant rejects the entire payload rather than ignoring the
 * class, so getting this wrong costs the whole entity. */
static void test_mismatched_unit_drops_the_class(void)
{
    s_ezpi_ha_entity_t e;

    CHECK(EZPI_ha_describe("temp", "temperature", "percent", false, &e),
          "a temperature in percent should still map");
    CHECK(NULL == e.device_class,
          "device_class must be dropped when the unit does not belong to it");
    CHECK(streq("%", e.unit), "the unit is kept -- it is the part that came from the driver");
    CHECK(EZPI_HA_COMPONENT_SENSOR == e.component, "still a sensor");

    /* A class with a unit requirement and no unit at all fails the same way. */
    CHECK(EZPI_ha_describe("temp", "temperature", NULL, false, &e),
          "a temperature with no scale should still map");
    CHECK(NULL == e.device_class, "device_class needs a unit to be legal");

    /* And the inverse: a class with no unit constraint survives having none. */
    CHECK(EZPI_ha_describe("motion", "bool", NULL, false, &e), "motion should map");
    CHECK(streq("motion", e.device_class), "binary classes have no unit to disagree with");

    /* Celsius and Fahrenheit are both legal for temperature; the check must
     * not have collapsed to a single accepted unit. */
    CHECK(EZPI_ha_describe("temp", "temperature", "fahrenheit", false, &e), "fahrenheit should map");
    CHECK(streq("temperature", e.device_class), "fahrenheit is legal for temperature");
    CHECK(streq("\xC2\xB0" "F", e.unit), "fahrenheit unit");
}

/* ---------------------------------------------------------------------------
 * Writable numbers, and things that are not entities
 * ------------------------------------------------------------------------ */
static void test_dimmer_is_a_number(void)
{
    s_ezpi_ha_entity_t e;

    CHECK(EZPI_ha_describe("dimmer", "int", "percent", true, &e), "dimmer should map");
    CHECK(EZPI_HA_COMPONENT_NUMBER == e.component, "a settable dimmer is a number");
    CHECK(streq("number", EZPI_ha_component_str(e.component)), "number platform name");
    CHECK(0 == e.min && 100 == e.max, "dimmer range is the 0..100 the driver publishes");

    /* Without a setter it is a reading, not a control. */
    CHECK(EZPI_ha_describe("dimmer", "int", "percent", false, &e), "read-only dimmer should map");
    CHECK(EZPI_HA_COMPONENT_SENSOR == e.component, "a read-only dimmer is a sensor");

    /* The momentary commands are not levels. Rendering them as sliders would
     * put three useless controls next to every dimmable lamp. */
    CHECK(!EZPI_ha_describe("dimmer_up", "int", NULL, true, &e), "dimmer_up is not an entity");
    CHECK(!EZPI_ha_describe("dimmer_down", "int", NULL, true, &e), "dimmer_down is not an entity");
    CHECK(!EZPI_ha_describe("dimmer_stop", "int", NULL, true, &e), "dimmer_stop is not an entity");
}

static void test_unrepresentable_types(void)
{
    s_ezpi_ha_entity_t e;

    CHECK(!EZPI_ha_describe("fingerprint_ids", "array", NULL, false, &e), "array is not an entity");
    CHECK(!EZPI_ha_describe("rgbcolor", "rgb", NULL, true, &e), "colour is not an entity yet");
    CHECK(!EZPI_ha_describe("fingerprint_action", "fingerprint_action", NULL, true, &e),
          "fingerprint action is not an entity");
    CHECK(!EZPI_ha_describe("anything", "token", NULL, false, &e), "token is not an entity");

    CHECK(NULL == EZPI_ha_component_str(EZPI_HA_COMPONENT_NONE), "NONE has no platform name");

    /* out is zeroed before the decision, so a caller that reads it after a
     * false return sees NONE rather than whatever was there before.
     *
     * The struct is deliberately poisoned first. Checking a fresh stack
     * variable proves nothing -- it is usually already zero, so deleting the
     * memset passes, which is what mutation testing found. Poisoning makes the
     * assertion mean what it says. */
    s_ezpi_ha_entity_t poisoned;
    poisoned.component = EZPI_HA_COMPONENT_SWITCH;
    poisoned.device_class = "stale";
    poisoned.unit = "stale";
    poisoned.state_class = "stale";
    poisoned.min = -1;
    poisoned.max = -1;

    CHECK(!EZPI_ha_describe("fingerprint_ids", "array", NULL, false, &poisoned),
          "array is still not an entity");
    CHECK(EZPI_HA_COMPONENT_NONE == poisoned.component, "a rejected item leaves component NONE");
    CHECK(NULL == poisoned.device_class, "a rejected item leaves no stale device class");
    CHECK(NULL == poisoned.unit, "a rejected item leaves no stale unit");
    CHECK(NULL == poisoned.state_class, "a rejected item leaves no stale state class");
    CHECK(0 == poisoned.min && 0 == poisoned.max, "a rejected item leaves no stale range");

    /* The same applies on the success path for the members that case does not
     * set: a switch has no range, and must not inherit the last caller's. */
    poisoned.min = -1;
    poisoned.max = -1;
    poisoned.unit = "stale";
    CHECK(EZPI_ha_describe("switch", "bool", NULL, true, &poisoned), "switch should map");
    CHECK(0 == poisoned.min && 0 == poisoned.max, "a switch has no range");
    CHECK(NULL == poisoned.unit, "a switch has no unit");

    /* An item with no declared value type. cloud_properties.value_type is a
     * plain pointer a driver can leave unset. */
    CHECK(!EZPI_ha_describe("temp", NULL, "celsius", false, &e), "no value type -> no entity");
    CHECK(!EZPI_ha_describe("temp", "", "celsius", false, &e), "empty value type -> no entity");

    /* A NULL out must not be dereferenced. */
    CHECK(!EZPI_ha_describe("temp", "temperature", "celsius", false, NULL), "NULL out -> false");

    /* A NULL item name is survivable: the value type still classifies it. */
    CHECK(EZPI_ha_describe(NULL, "temperature", "celsius", false, &e), "NULL item name still maps");
    CHECK(streq("temperature", e.device_class), "value type classifies it without a name");
}

/* Strings are published, but as plain sensors with no unit and no state class
 * -- Home Assistant cannot keep statistics for something that is not a number,
 * and claiming otherwise fills the log with warnings. */
static void test_string_sensor(void)
{
    s_ezpi_ha_entity_t e;

    CHECK(EZPI_ha_describe("motion_direction", "string", NULL, false, &e), "string should map");
    CHECK(EZPI_HA_COMPONENT_SENSOR == e.component, "string is a sensor");
    CHECK(NULL == e.unit, "a string has no unit");
    CHECK(NULL == e.state_class, "a string has no state class");
}

/* ---------------------------------------------------------------------------
 * Every item name and value type the tree actually produces
 * ------------------------------------------------------------------------ */

/* Extracted from ezlopi-sensors-devices by grepping the item_name and
 * value_type assignments, rather than chosen. The point is not to assert a
 * particular mapping for each -- the cases above do that for the interesting
 * ones -- but that no combination the firmware can emit makes this crash, read
 * uninitialised memory under ASan, or produce a device class that contradicts
 * its own unit. */
static const char *const ALL_ITEM_NAMES[] = {
    "acceleration_x_axis", "acceleration_y_axis", "acceleration_z_axis", "activity",
    "angle_position", "applied_force_on_sensor", "atmospheric_pressure", "basic",
    "co2_level", "co_alarm", "current", "dimmer", "dimmer_down", "dimmer_stop",
    "dimmer_up", "distance", "dw_state", "electrical_resistivity", "fingerprint_action",
    "fingerprint_ids", "gas_alarm", "gyroscope_x_axis", "gyroscope_y_axis",
    "gyroscope_z_axis", "heat_alarm", "humidity", "learn_fingerprint", "light_alarm",
    "lux", "magnetic_strength_x_axis", "magnetic_strength_y_axis",
    "magnetic_strength_z_axis", "moisture", "motion", "motion_direction",
    "particles_0_dot_3_um", "particles_0_dot_5_um", "particles_10_um", "particles_1_um",
    "particles_2_dot_5_um", "particles_5_um", "particulate_matter_1",
    "particulate_matter_10", "particulate_matter_2_dot_5", "rgbcolor", "smoke_density",
    "soil_humidity", "solar_radiation", "sound_volume", "switch", "temp",
    "temperature_changes", "volatile_organic_compound_level", "voltage",
    "water_filter_replacement_alarm", "water_flow", "water_leak_alarm",
    "water_level_alarm", "weight",
};

static const char *const ALL_VALUE_TYPES[] = {
    "acceleration", "angle", "array", "bool", "electric_current", "electric_potential",
    "electrical_resistance", "fingerprint_action", "float", "force", "frequency",
    "general_purpose", "humidity", "illuminance", "int", "length", "mass", "moisture",
    "pressure", "rgb", "string", "substance_amount", "temperature", "token",
    "ultraviolet", "volume_flow",
};

static const char *const ALL_SCALES[] = {
    NULL, "", "ampere", "celsius", "centi_meter", "guass", "kilo_gram", "kilo_pascal",
    "liter_per_hour", "lux", "meter", "meter_per_square_second",
    "micro_gram_per_cubic_meter", "milli_volt", "newton", "north_pole_degress",
    "ohm_meter", "particles_per_deciliter", "parts_per_million", "percent",
    "revolutions_per_minute", "volt", "watt_per_square_meter",
};

static void test_every_combination_is_self_consistent(void)
{
    unsigned int mapped = 0;
    unsigned int rejected = 0;
    int inconsistent = 0;
    int bad_platform = 0;

    for (size_t n = 0; n < sizeof(ALL_ITEM_NAMES) / sizeof(ALL_ITEM_NAMES[0]); n++)
    {
        for (size_t t = 0; t < sizeof(ALL_VALUE_TYPES) / sizeof(ALL_VALUE_TYPES[0]); t++)
        {
            for (size_t s = 0; s < sizeof(ALL_SCALES) / sizeof(ALL_SCALES[0]); s++)
            {
                for (int setter = 0; setter <= 1; setter++)
                {
                    s_ezpi_ha_entity_t e;

                    if (!EZPI_ha_describe(ALL_ITEM_NAMES[n], ALL_VALUE_TYPES[t],
                                          ALL_SCALES[s], setter ? true : false, &e))
                    {
                        rejected++;
                        continue;
                    }

                    mapped++;

                    /* Anything published must name a platform. */
                    if (NULL == EZPI_ha_component_str(e.component))
                    {
                        bad_platform++;
                    }

                    /* A device class that requires a unit must have been
                     * dropped if the unit does not suit it. Re-derived here
                     * from the same scale the caller passed, so the check is
                     * on the output rather than a restatement of the table. */
                    if (NULL != e.device_class)
                    {
                        const char *unit = EZPI_ha_unit_from_scale(ALL_SCALES[s]);

                        if (streq("temperature", e.device_class) &&
                            !(streq("\xC2\xB0" "C", unit) || streq("\xC2\xB0" "F", unit)))
                        {
                            inconsistent++;
                        }
                        if (streq("humidity", e.device_class) && !streq("%", unit))
                        {
                            inconsistent++;
                        }
                        if (streq("illuminance", e.device_class) && !streq("lx", unit))
                        {
                            inconsistent++;
                        }
                        if (streq("carbon_dioxide", e.device_class) && !streq("ppm", unit))
                        {
                            inconsistent++;
                        }
                    }
                }
            }
        }
    }

    CHECK(0 == inconsistent, "%d combination(s) kept a device class its unit contradicts",
          inconsistent);
    CHECK(0 == bad_platform, "%d mapped entit(ies) had no platform name", bad_platform);
    CHECK(mapped > 0 && rejected > 0,
          "the sweep should both map and reject: mapped %u, rejected %u", mapped, rejected);

    printf("  swept %u combination(s): %u mapped, %u rejected\n",
           mapped + rejected, mapped, rejected);
}

/* ---------------------------------------------------------------------------
 * Object ids
 * ------------------------------------------------------------------------ */
static void test_object_id_basics(void)
{
    char buf[64];

    CHECK(7 == EZPI_ha_object_id(buf, sizeof(buf), "Kitchen"), "length of Kitchen");
    CHECK(streq("kitchen", buf), "Kitchen -> kitchen, got %s", buf);

    EZPI_ha_object_id(buf, sizeof(buf), "Living Room Lamp");
    CHECK(streq("living_room_lamp", buf), "spaces become underscores, got %s", buf);

    EZPI_ha_object_id(buf, sizeof(buf), "Temp / Humidity (outdoor)");
    CHECK(streq("temp_humidity_outdoor", buf), "punctuation collapses, got %s", buf);

    /* A run of unacceptable bytes is one underscore, not several. */
    EZPI_ha_object_id(buf, sizeof(buf), "a!!!???b");
    CHECK(streq("a_b", buf), "a run collapses to one underscore, got %s", buf);

    /* Leading and trailing separators are dropped, including underscores that
     * were already in the source. */
    EZPI_ha_object_id(buf, sizeof(buf), "  __trimmed__  ");
    CHECK(streq("trimmed", buf), "leading and trailing separators go, got %s", buf);

    /* Hyphens and digits are legal in a topic segment and must survive. */
    EZPI_ha_object_id(buf, sizeof(buf), "ESP32-S3 node 2");
    CHECK(streq("esp32-s3_node_2", buf), "hyphens and digits survive, got %s", buf);

    /* Non-ASCII is not legal and must not be passed through as raw bytes. */
    EZPI_ha_object_id(buf, sizeof(buf), "Caf\xC3\xA9 sensor");
    CHECK(streq("caf_sensor", buf), "non-ASCII collapses, got %s", buf);
}

static void test_object_id_nothing_usable(void)
{
    char buf[64];

    CHECK(0 == EZPI_ha_object_id(buf, sizeof(buf), ""), "empty input -> 0");
    CHECK(streq("", buf), "empty input -> empty output");

    CHECK(0 == EZPI_ha_object_id(buf, sizeof(buf), "!!!"), "all-punctuation -> 0");
    CHECK(streq("", buf), "all-punctuation -> empty output, not a lone underscore");

    CHECK(0 == EZPI_ha_object_id(buf, sizeof(buf), NULL), "NULL input -> 0");
    CHECK(streq("", buf), "NULL input still terminates the buffer");

    CHECK(0 == EZPI_ha_object_id(NULL, 0, "anything"), "NULL buffer -> 0");
    CHECK(0 == EZPI_ha_object_id(buf, 0, "anything"), "zero-size buffer -> 0");
}

/* Truncation is the part worth being thorough about: the input is a device
 * name the user chose, and the buffer is fixed. Every size from 1 upward is
 * checked rather than a representative few, because the interesting sizes are
 * exactly the ones that land mid-word or on a separator. */
static void test_object_id_truncation(void)
{
    static const char *src = "Living Room Lamp";
    int overran = 0;
    int unterminated = 0;
    int trailing_sep = 0;
    int wrong_prefix = 0;
    char full[64];

    EZPI_ha_object_id(full, sizeof(full), src);
    CHECK(streq("living_room_lamp", full), "baseline, got %s", full);

    for (size_t size = 1; size <= 24; size++)
    {
        char guarded[32];
        size_t written;

        memset(guarded, '\xAA', sizeof(guarded));
        written = EZPI_ha_object_id(guarded, size, src);

        /* Nothing beyond the declared size may be touched. */
        for (size_t i = size; i < sizeof(guarded); i++)
        {
            if ('\xAA' != guarded[i])
            {
                overran++;
                break;
            }
        }

        if (written >= size || '\0' != guarded[written])
        {
            unterminated++;
        }

        /* A truncated id must not end on a separator -- "living_room_" is a
         * valid topic segment but an ugly entity id, and it is the natural
         * thing to produce when the buffer runs out just after a space. */
        if (written > 0 && '_' == guarded[written - 1])
        {
            trailing_sep++;
        }

        /* Whatever fits must be a prefix of the untruncated result. */
        if (written > 0 && 0 != strncmp(guarded, full, written))
        {
            wrong_prefix++;
        }
    }

    CHECK(0 == overran, "%d size(s) wrote past the end of the buffer", overran);
    CHECK(0 == unterminated, "%d size(s) failed to NUL-terminate", unterminated);
    CHECK(0 == trailing_sep, "%d size(s) left a trailing underscore", trailing_sep);
    CHECK(0 == wrong_prefix, "%d size(s) produced something that is not a prefix", wrong_prefix);

    /* A buffer with room for one character gets one character, not nothing. */
    char tiny[2];
    CHECK(1 == EZPI_ha_object_id(tiny, sizeof(tiny), src), "2-byte buffer holds one character");
    CHECK(streq("l", tiny), "2-byte buffer -> l, got %s", tiny);
}

/*******************************************************************************
 *                          Topics
 *******************************************************************************/

static void test_item_object_id(void)
{
    char buf[64];

    CHECK(strlen("kitchen_sensor_200a0001_200a0002") ==
              EZPI_ha_item_object_id(buf, sizeof(buf), "Kitchen Sensor", 0x200a0001UL, 0x200a0002UL),
          "object id length");
    CHECK(streq("kitchen_sensor_200a0001_200a0002", buf), "object id -> %s", buf);

    /* Ids are always eight digits, including when they are small, because the
     * command topic parser splits the tail by position. */
    CHECK(0 < EZPI_ha_item_object_id(buf, sizeof(buf), "x", 1UL, 2UL), "small ids map");
    CHECK(streq("x_00000001_00000002", buf), "small ids are still padded, got %s", buf);

    /* A name that reduces to nothing leaves the ids alone rather than an
     * object id that starts with a separator. */
    CHECK(17 == EZPI_ha_item_object_id(buf, sizeof(buf), "!!!", 0xabcdef01UL, 0x23456789UL),
          "unusable name leaves only the ids");
    CHECK(streq("abcdef01_23456789", buf), "unusable name -> %s", buf);
    CHECK(17 == EZPI_ha_item_object_id(buf, sizeof(buf), NULL, 0xabcdef01UL, 0x23456789UL),
          "NULL name leaves only the ids");
    CHECK(streq("abcdef01_23456789", buf), "NULL name -> %s", buf);
}

static void test_item_object_id_never_clips_the_ids(void)
{
    /* The whole point of the reservation: at every buffer size the function
     * either refuses outright or produces ids that are still parseable. A
     * clipped id would not fail visibly -- it would address a different item,
     * or an item that does not exist, which looks like a device that ignores
     * commands. */
    const char *name = "A Very Long Device Name Indeed";
    int clipped = 0;
    int overran = 0;
    int unparseable = 0;

    for (size_t size = 1; size <= 64; size++)
    {
        char buf[80];
        char topic[128];
        unsigned long dev = 0;
        unsigned long item = 0;
        size_t len;

        memset(buf, '@', sizeof(buf));
        len = EZPI_ha_item_object_id(buf, size, name, 0x1234abcdUL, 0x5678ef90UL);

        if (buf[size] != '@')
        {
            overran++;
        }

        if (0 == len)
        {
            /* Refusal must leave an empty string, not a half-written id. */
            if ('\0' != buf[0])
            {
                clipped++;
            }
            continue;
        }

        if (len != strlen(buf))
        {
            clipped++;
            continue;
        }

        /* Feed it through the real parser: that is the property that matters,
         * not the exact bytes. */
        snprintf(topic, sizeof(topic), "ezlopi/node/%s/set", buf);
        if (!EZPI_ha_parse_command_topic(topic, strlen(topic), &dev, &item) ||
            0x1234abcdUL != dev || 0x5678ef90UL != item)
        {
            unparseable++;
        }
    }

    CHECK(0 == overran, "%d size(s) wrote past the end of the buffer", overran);
    CHECK(0 == clipped, "%d size(s) left a malformed string", clipped);
    CHECK(0 == unparseable, "%d size(s) produced ids the parser could not recover", unparseable);

    /* Exactly enough for the ids and the NUL, and not a byte less. */
    char exact[18];
    CHECK(17 == EZPI_ha_item_object_id(exact, sizeof(exact), name, 1UL, 2UL),
          "18 bytes is enough for the ids");
    char one_short[17];
    CHECK(0 == EZPI_ha_item_object_id(one_short, sizeof(one_short), name, 1UL, 2UL),
          "17 bytes is refused rather than clipped");
    CHECK(streq("", one_short), "a refusal leaves an empty string");
}

static void test_parse_command_topic(void)
{
    unsigned long dev = 0;
    unsigned long item = 0;

    CHECK(EZPI_ha_parse_command_topic("ezlopi/hub_aabbcc/lamp_200a0001_200a0002/set",
                                      strlen("ezlopi/hub_aabbcc/lamp_200a0001_200a0002/set"),
                                      &dev, &item),
          "a well-formed command topic parses");
    CHECK(0x200a0001UL == dev, "device id, got %08lx", dev);
    CHECK(0x200a0002UL == item, "item id, got %08lx", item);

    /* No device name in the object id: the ids are preceded by '/'. */
    CHECK(EZPI_ha_parse_command_topic("ezlopi/hub/00000001_00000002/set",
                                      strlen("ezlopi/hub/00000001_00000002/set"), &dev, &item),
          "an object id of only ids parses");
    CHECK(1UL == dev && 2UL == item, "ids recovered without a name");

    /* A device name containing underscores must not confuse the split, which
     * is the reason the tail is taken by position. */
    CHECK(EZPI_ha_parse_command_topic("ezlopi/hub/a_b_c_d_deadbeef_cafe0001/set",
                                      strlen("ezlopi/hub/a_b_c_d_deadbeef_cafe0001/set"), &dev, &item),
          "underscores in the name do not confuse the split");
    CHECK(0xdeadbeefUL == dev && 0xcafe0001UL == item, "ids recovered past underscores");
}

static void test_parse_command_topic_refuses_junk(void)
{
    /* Every one of these is a topic that must not be acted on. A command
     * topic comes off the network and names an item about to be written, so
     * guessing at a malformed one is how a broker typo turns into the wrong
     * relay switching. */
    static const char *const junk[] = {
        "",
        "/set",
        "ezlopi/hub/lamp/set",                        /* no ids at all */
        "ezlopi/hub/lamp_200a0001/set",               /* only one id */
        "ezlopi/hub/lamp_200a001_200a0002/set",       /* seven digits */
        "ezlopi/hub/lamp_200a00001_200a0002/set",     /* nine digits */
        "ezlopi/hub/lamp_200a0001_200a0002/state",    /* wrong leaf */
        "ezlopi/hub/lamp_200a0001_200a0002/set/more", /* leaf not last */
        /* These two matter more than they look. Both ids sit exactly where the
         * parser expects them and only the last four bytes are wrong, so the
         * leaf check is the only thing that can refuse them. Every other
         * wrong-leaf case above is a different length, which moves the ids
         * off-position and gets caught by the separator check instead --
         * leaving the leaf check itself unexercised, which is what mutation
         * testing found here. */
        "ezlopi/hub/lamp_200a0001_200a0002/get", /* four-byte wrong leaf */
        "ezlopi/hub/lamp_200a0001_200a0002_set", /* separator instead of a slash */
        "ezlopi/hub/lamp_200a0001-200a0002/set",      /* wrong separator */
        "ezlopi/hub/lamp_200g0001_200a0002/set",      /* g is not hex */
        "ezlopi/hub/lamp_200a0001_200a000z/set",      /* z is not hex */
        "200a0001_200a0002/set",                      /* no separator before */
        "ezlopi/hub/lampX200a0001_200a0002/set",      /* separator is a letter */
    };

    int accepted = 0;
    int left_dirty = 0;

    for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++)
    {
        unsigned long dev = 0xdeadUL;
        unsigned long item = 0xbeefUL;

        if (EZPI_ha_parse_command_topic(junk[i], strlen(junk[i]), &dev, &item))
        {
            printf("  accepted junk: %s\n", junk[i]);
            accepted++;
        }

        /* A refusal must not leave the caller holding the previous ids. */
        if (0 != dev || 0 != item)
        {
            left_dirty++;
        }
    }

    CHECK(0 == accepted, "%d malformed topic(s) were accepted", accepted);
    CHECK(0 == left_dirty, "%d refusal(s) left a stale id behind", left_dirty);

    /* The length is honoured rather than the NUL: a topic from esp-mqtt is a
     * pointer and a length into a larger buffer and is not terminated. */
    unsigned long dev = 0;
    unsigned long item = 0;
    const char *embedded = "ezlopi/hub/x_00000003_00000004/setTRAILING-GARBAGE";
    CHECK(EZPI_ha_parse_command_topic(embedded, strlen("ezlopi/hub/x_00000003_00000004/set"),
                                      &dev, &item),
          "an unterminated topic parses at the given length");
    CHECK(3UL == dev && 4UL == item, "ids recovered from an unterminated topic");

    CHECK(!EZPI_ha_parse_command_topic(NULL, 40, &dev, &item), "NULL topic is refused");
    CHECK(!EZPI_ha_parse_command_topic("ezlopi/hub/x_00000003_00000004/set",
                                       strlen("ezlopi/hub/x_00000003_00000004/set"), NULL, &item),
          "NULL output is refused");
}

static void test_object_id_and_topic_round_trip(void)
{
    /* The two functions are each other's inverse, so sweeping real device
     * names through both is worth more than asserting either alone. */
    static const char *const names[] = {
        "Kitchen Sensor", "", "!!!", "a", "Bo\xC3\xAFler", "LED___Strip",
        "_leading", "trailing_", "UPPER CASE", "123", "dev/with/slashes",
        "a very long device name that will certainly be truncated somewhere",
    };

    int failed = 0;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        for (unsigned long d = 0; d < 4; d++)
        {
            char object_id[48];
            char topic[128];
            unsigned long dev = 0;
            unsigned long item = 0;

            const unsigned long device_id = 0x200a0000UL + d;
            const unsigned long item_id = 0x30bb0000UL + (d * 7UL);

            if (0 == EZPI_ha_item_object_id(object_id, sizeof(object_id), names[i],
                                            device_id, item_id))
            {
                failed++;
                continue;
            }

            if (0 == EZPI_ha_topic_build(topic, sizeof(topic), "ezlopi", "hub_aabbcc",
                                         object_id, "set"))
            {
                failed++;
                continue;
            }

            if (!EZPI_ha_parse_command_topic(topic, strlen(topic), &dev, &item) ||
                device_id != dev || item_id != item)
            {
                printf("  round trip failed for '%s' -> %s\n", names[i], topic);
                failed++;
            }
        }
    }

    CHECK(0 == failed, "%d name/id pair(s) did not survive the round trip", failed);
}

static void test_topic_discovery(void)
{
    char buf[128];

    CHECK(0 < EZPI_ha_topic_discovery(buf, sizeof(buf), "homeassistant", "sensor",
                                      "hub_aabbcc", "kitchen_200a0001_200a0002"),
          "discovery topic builds");
    CHECK(streq("homeassistant/sensor/hub_aabbcc/kitchen_200a0001_200a0002/config", buf),
          "discovery topic -> %s", buf);

    /* Every argument is required: a missing one would produce a double slash,
     * which is a different topic, not a shorter one. */
    CHECK(0 == EZPI_ha_topic_discovery(buf, sizeof(buf), NULL, "sensor", "n", "o"),
          "NULL prefix refused");
    CHECK(0 == EZPI_ha_topic_discovery(buf, sizeof(buf), "", "sensor", "n", "o"),
          "empty prefix refused");
    CHECK(0 == EZPI_ha_topic_discovery(buf, sizeof(buf), "ha", NULL, "n", "o"),
          "NULL component refused -- this is what EZPI_ha_component_str returns for NONE");
    CHECK(0 == EZPI_ha_topic_discovery(buf, sizeof(buf), "ha", "sensor", "", "o"),
          "empty node refused");
    CHECK(0 == EZPI_ha_topic_discovery(buf, sizeof(buf), "ha", "sensor", "n", ""),
          "empty object refused");
}

static void test_topic_build(void)
{
    char buf[128];

    CHECK(0 < EZPI_ha_topic_build(buf, sizeof(buf), "ezlopi", "hub_aabbcc", "lamp_1_2", "state"),
          "state topic builds");
    CHECK(streq("ezlopi/hub_aabbcc/lamp_1_2/state", buf), "state topic -> %s", buf);

    /* NULL object id is the hub-wide form, which is how availability works. */
    CHECK(0 < EZPI_ha_topic_build(buf, sizeof(buf), "ezlopi", "hub_aabbcc", NULL, "status"),
          "availability topic builds");
    CHECK(streq("ezlopi/hub_aabbcc/status", buf), "availability topic -> %s", buf);

    /* An empty object id is a bug, not a request for the hub-wide form. If it
     * were treated as hub-wide, one item's state would be published over the
     * hub's availability topic and the whole device would appear offline. */
    CHECK(0 == EZPI_ha_topic_build(buf, sizeof(buf), "ezlopi", "hub", "", "state"),
          "empty object id is refused, not treated as hub-wide");

    CHECK(0 == EZPI_ha_topic_build(buf, sizeof(buf), "ezlopi", "hub", "o", NULL),
          "NULL leaf refused");
}

static void test_topics_refuse_truncation(void)
{
    /* Truncation must clear the buffer, not return a prefix. A caller that
     * ignores the return value then publishes nothing, which is recoverable;
     * publishing a config to a truncated topic is not, because nothing
     * reports it and the entity just never appears. */
    int returned_prefix = 0;
    int not_cleared = 0;
    int overran = 0;

    for (size_t size = 1; size < 70; size++)
    {
        char buf[80];
        size_t len;

        memset(buf, '@', sizeof(buf));
        len = EZPI_ha_topic_discovery(buf, size, "homeassistant", "binary_sensor",
                                      "hub_aabbcc", "kitchen_200a0001_200a0002");

        if (buf[size] != '@')
        {
            overran++;
        }

        if (0 == len)
        {
            if ('\0' != buf[0])
            {
                not_cleared++;
            }
        }
        else if (len != strlen(buf) ||
                 !streq(buf, "homeassistant/binary_sensor/hub_aabbcc/kitchen_200a0001_200a0002"
                             "/config"))
        {
            returned_prefix++;
        }
    }

    CHECK(0 == overran, "%d size(s) wrote past the end of the buffer", overran);
    CHECK(0 == not_cleared, "%d refusal(s) left the buffer non-empty", not_cleared);
    CHECK(0 == returned_prefix, "%d size(s) returned a truncated topic as success", returned_prefix);

    /* One byte short of the full topic, and exactly enough. */
    const char *full = "homeassistant/sensor/hub/obj/config";
    char tight[36]; /* strlen(full) + 1 */
    CHECK(strlen(full) == sizeof(tight) - 1, "test buffer is sized to the topic");
    CHECK(strlen(full) == EZPI_ha_topic_discovery(tight, sizeof(tight), "homeassistant",
                                                  "sensor", "hub", "obj"),
          "exactly enough room succeeds");
    char short_by_one[35];
    CHECK(0 == EZPI_ha_topic_discovery(short_by_one, sizeof(short_by_one), "homeassistant",
                                       "sensor", "hub", "obj"),
          "one byte short is refused");
}

static void test_component_str_feeds_the_topic(void)
{
    /* The discovery topic takes the component name from EZPI_ha_component_str,
     * and NONE returns NULL there. Checking the two together is what proves an
     * unmappable item cannot produce a topic at all. */
    char buf[128];

    CHECK(NULL == EZPI_ha_component_str(EZPI_HA_COMPONENT_NONE), "NONE has no platform name");
    CHECK(0 == EZPI_ha_topic_discovery(buf, sizeof(buf), "homeassistant",
                                       EZPI_ha_component_str(EZPI_HA_COMPONENT_NONE),
                                       "hub", "obj"),
          "an unmappable item cannot produce a discovery topic");

    static const e_ezpi_ha_component_t mappable[] = {
        EZPI_HA_COMPONENT_SENSOR, EZPI_HA_COMPONENT_BINARY_SENSOR,
        EZPI_HA_COMPONENT_SWITCH, EZPI_HA_COMPONENT_NUMBER,
    };

    for (size_t i = 0; i < sizeof(mappable) / sizeof(mappable[0]); i++)
    {
        CHECK(0 < EZPI_ha_topic_discovery(buf, sizeof(buf), "homeassistant",
                                          EZPI_ha_component_str(mappable[i]), "hub", "obj"),
              "component %d produces a topic", (int)mappable[i]);
    }
}

/*******************************************************************************
 *                          Main
 *******************************************************************************/
int main(void)
{
    test_bool_setter_is_a_switch();
    test_binary_sensor_device_classes();
    test_units();
    test_sensor_classes_and_units();
    test_mismatched_unit_drops_the_class();
    test_dimmer_is_a_number();
    test_unrepresentable_types();
    test_string_sensor();
    test_every_combination_is_self_consistent();
    test_object_id_basics();
    test_object_id_nothing_usable();
    test_object_id_truncation();
    test_item_object_id();
    test_item_object_id_never_clips_the_ids();
    test_parse_command_topic();
    test_parse_command_topic_refuses_junk();
    test_object_id_and_topic_round_trip();
    test_topic_discovery();
    test_topic_build();
    test_topics_refuse_truncation();
    test_component_str_feeds_the_topic();

    printf("%u check(s), %u failure(s)\n", tests_run, tests_failed);

    return (0 == tests_failed) ? 0 : 1;
}
