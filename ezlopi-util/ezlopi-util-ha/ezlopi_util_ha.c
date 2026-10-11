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
 * @file    ezlopi_util_ha.c
 * @brief   Describe an EzloPi item as a Home Assistant MQTT entity.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <string.h>

#include "ezlopi_util_ha.h"

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/
#define EZPI_HA_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* "measurement" is the only state class that applies here: every numeric item
 * in this firmware is an instantaneous reading, not a total that accumulates.
 * Setting it is what lets Home Assistant keep long-term statistics, which is
 * most of the reason someone would want these values in Home Assistant at all. */
static const char *const EZPI_HA_STATE_CLASS_MEASUREMENT = "measurement";

typedef struct s_ezpi_ha_pair
{
    const char *key;
    const char *value;
} s_ezpi_ha_pair_t;

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/* EzloPi scale name -> unit symbol. Only scales a driver in this tree actually
 * assigns need to be here for the firmware to be correct, but the rest of the
 * published scale list is included too: a driver added later should get a unit
 * without anyone remembering this table exists. */
static const s_ezpi_ha_pair_t sg_units[] = {
    {"celsius", "\xC2\xB0" "C"},
    {"fahrenheit", "\xC2\xB0" "F"},
    {"percent", "%"},
    {"lux", "lx"},
    {"volt", "V"},
    {"milli_volt", "mV"},
    {"ampere", "A"},
    {"milli_ampere", "mA"},
    {"ohm_meter", "\xCE\xA9\xE2\x8B\x85m"},
    {"newton", "N"},
    {"kilo_gram", "kg"},
    {"pounds", "lb"},
    {"meter", "m"},
    {"centi_meter", "cm"},
    {"feet", "ft"},
    {"kilo_pascal", "kPa"},
    {"parts_per_million", "ppm"},
    {"micro_gram_per_cubic_meter", "\xC2\xB5g/m\xC2\xB3"},
    {"gram_per_cubic_meter", "g/m\xC2\xB3"},
    {"milli_gram_per_liter", "mg/L"},
    {"mole_per_cubic_meter", "mol/m\xC2\xB3"},
    {"liter_per_hour", "L/h"},
    {"cubic_meter_per_hour", "m\xC2\xB3/h"},
    {"cubic_feet_per_minute", "ft\xC2\xB3/min"},
    {"watt_per_square_meter", "W/m\xC2\xB2"},
    {"meter_per_square_second", "m/s\xC2\xB2"},
    {"revolutions_per_minute", "rpm"},
    /* Spelled "degress" in ezlopi_cloud_scales_str.c. Matched as the firmware
     * spells it, deliberately: correcting it here would mean matching a string
     * no item ever carries, and the typo is in a published API. */
    {"north_pole_degress", "\xC2\xB0"},
    {"guass", "G"},
    {"tesla", "T"},
    {"weber_per_square_meter", "Wb/m\xC2\xB2"},
    {"hertz", "Hz"},
    {"kilo_hertz", "kHz"},
    {"particles_per_deciliter", "particles/dL"},
    {"beats_per_minute", "bpm"},
    {"breaths_per_minute", "br/min"},
};

/* Item name -> binary_sensor device class. An alarm whose specific class is
 * not one Home Assistant knows is better left unclassified than forced into a
 * near neighbour: the class drives the words shown to the user ("Detected" vs
 * "Wet" vs "Unsafe"), so a wrong one is actively misleading. */
static const s_ezpi_ha_pair_t sg_binary_classes[] = {
    {"motion", "motion"},
    {"dw_state", "door"},
    {"water_leak_alarm", "moisture"},
    {"gas_alarm", "gas"},
    {"co_alarm", "carbon_monoxide"},
    {"smoke_alarm", "smoke"},
    {"heat_alarm", "heat"},
    {"light_alarm", "light"},
    {"sound_alarm", "sound"},
    {"tamper_alarm", "tamper"},
    /* No Home Assistant class covers "the water is too high" or "this filter
     * is due"; "problem" is the documented catch-all for exactly that. */
    {"water_level_alarm", "problem"},
    {"water_filter_replacement_alarm", "problem"},
};

/* Item name -> sensor device class, consulted before the value type. These are
 * the cases where the value type is too generic to classify: three different
 * particulate sizes all arrive as the same substance_amount. */
static const s_ezpi_ha_pair_t sg_sensor_classes_by_item[] = {
    {"particulate_matter_1", "pm1"},
    {"particulate_matter_2_dot_5", "pm25"},
    {"particulate_matter_10", "pm10"},
    {"co2_level", "carbon_dioxide"},
    /* Home Assistant distinguishes VOC by mass (volatile_organic_compounds,
     * µg/m³) from VOC by fraction (..._parts, ppm/ppb). EzloPi reports this
     * one in parts per million. If a driver ever reports it by mass the
     * pairing check below drops the class rather than publishing a rejected
     * payload. */
    {"volatile_organic_compound_level", "volatile_organic_compounds_parts"},
    {"solar_radiation", "irradiance"},
    {"atmospheric_pressure", "atmospheric_pressure"},
    {"lux", "illuminance"},
    {"humidity", "humidity"},
    {"soil_humidity", "moisture"},
    {"moisture", "moisture"},
    {"temp", "temperature"},
    {"voltage", "voltage"},
    {"current", "current"},
    {"distance", "distance"},
    {"weight", "weight"},
};

/* Value type -> sensor device class, for the types that name a quantity. */
static const s_ezpi_ha_pair_t sg_sensor_classes_by_type[] = {
    {"temperature", "temperature"},
    {"humidity", "humidity"},
    {"illuminance", "illuminance"},
    {"pressure", "atmospheric_pressure"},
    {"electric_potential", "voltage"},
    {"electric_current", "current"},
    {"frequency", "frequency"},
    {"moisture", "moisture"},
    {"length", "distance"},
    {"mass", "weight"},
};

/* Which units Home Assistant accepts for each device class. A payload whose
 * device_class and unit_of_measurement disagree is rejected outright and the
 * entity never appears, so this is checked rather than assumed: the item's
 * value type and its scale are set independently by each driver and nothing
 * else makes them agree. */
typedef struct s_ezpi_ha_class_units
{
    const char *device_class;
    const char *units[8]; /* NULL-terminated */
} s_ezpi_ha_class_units_t;

static const s_ezpi_ha_class_units_t sg_class_units[] = {
    {"temperature", {"\xC2\xB0" "C", "\xC2\xB0" "F", "K", NULL}},
    {"humidity", {"%", NULL}},
    {"moisture", {"%", NULL}},
    {"illuminance", {"lx", NULL}},
    {"atmospheric_pressure", {"kPa", "hPa", "Pa", "mbar", "bar", "psi", "inHg", NULL}},
    {"voltage", {"V", "mV", "kV", NULL}},
    {"current", {"A", "mA", NULL}},
    {"frequency", {"Hz", "kHz", "MHz", "GHz", NULL}},
    {"distance", {"m", "cm", "mm", "km", "in", "ft", "yd", NULL}},
    {"weight", {"kg", "g", "mg", "lb", "oz", NULL}},
    {"carbon_dioxide", {"ppm", NULL}},
    {"volatile_organic_compounds_parts", {"ppm", "ppb", NULL}},
    {"pm1", {"\xC2\xB5g/m\xC2\xB3", NULL}},
    {"pm25", {"\xC2\xB5g/m\xC2\xB3", NULL}},
    {"pm10", {"\xC2\xB5g/m\xC2\xB3", NULL}},
    {"irradiance", {"W/m\xC2\xB2", NULL}},
};

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static bool ezpi_ha_streq(const char *a, const char *b);
static const char *ezpi_ha_lookup(const s_ezpi_ha_pair_t *table, size_t count, const char *key);
static bool ezpi_ha_class_allows_unit(const char *device_class, const char *unit);

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
const char *EZPI_ha_component_str(e_ezpi_ha_component_t component)
{
    switch (component)
    {
    case EZPI_HA_COMPONENT_SENSOR:
        return "sensor";
    case EZPI_HA_COMPONENT_BINARY_SENSOR:
        return "binary_sensor";
    case EZPI_HA_COMPONENT_SWITCH:
        return "switch";
    case EZPI_HA_COMPONENT_NUMBER:
        return "number";
    case EZPI_HA_COMPONENT_NONE:
    default:
        return NULL;
    }
}

const char *EZPI_ha_unit_from_scale(const char *scale)
{
    return ezpi_ha_lookup(sg_units, EZPI_HA_COUNT(sg_units), scale);
}

bool EZPI_ha_describe(const char *item_name,
                      const char *value_type,
                      const char *scale,
                      bool has_setter,
                      s_ezpi_ha_entity_t *out)
{
    if (NULL == out)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));

    /* An item with no declared value type cannot be described. This is not
     * defensive padding: cloud_properties.value_type is a plain pointer that a
     * driver may simply never set. */
    if (NULL == value_type || '\0' == value_type[0])
    {
        return false;
    }

    if (NULL == item_name)
    {
        item_name = "";
    }

    /* --- booleans ------------------------------------------------------- */
    if (ezpi_ha_streq(value_type, "bool"))
    {
        if (has_setter)
        {
            out->component = EZPI_HA_COMPONENT_SWITCH;
        }
        else
        {
            out->component = EZPI_HA_COMPONENT_BINARY_SENSOR;
            out->device_class = ezpi_ha_lookup(sg_binary_classes,
                                               EZPI_HA_COUNT(sg_binary_classes),
                                               item_name);
        }
        return true;
    }

    /* --- things with no sensible representation ------------------------- */
    /* Reported rather than published. An array of enrolled fingerprint ids, an
     * opaque token, or a colour triple is not a number and not a switch; Home
     * Assistant would show a permanently "unknown" entity. Colour is the one
     * worth calling out -- it needs the item combined with its device's
     * on/off and brightness items to become a light, which is recorded as a
     * gap rather than guessed at. */
    if (ezpi_ha_streq(value_type, "array") ||
        ezpi_ha_streq(value_type, "token") ||
        ezpi_ha_streq(value_type, "rgb") ||
        ezpi_ha_streq(value_type, "fingerprint_action"))
    {
        return false;
    }

    /* --- strings -------------------------------------------------------- */
    if (ezpi_ha_streq(value_type, "string"))
    {
        out->component = EZPI_HA_COMPONENT_SENSOR;
        return true;
    }

    /* --- numbers -------------------------------------------------------- */

    /* The dimmer level is the only writable number whose range this component
     * can state as fact: the driver publishes minValue 0 and maxValue 100. The
     * momentary dimmer_up / dimmer_down / dimmer_stop items are commands, not
     * levels, and are left out rather than rendered as sliders. Any other
     * writable number is published read-only, because a number entity with a
     * made-up range is worse than a reading. */
    if (ezpi_ha_streq(item_name, "dimmer") && has_setter)
    {
        out->component = EZPI_HA_COMPONENT_NUMBER;
        out->min = 0;
        out->max = 100;
        out->unit = EZPI_ha_unit_from_scale(scale);
        return true;
    }

    if (ezpi_ha_streq(item_name, "dimmer_up") ||
        ezpi_ha_streq(item_name, "dimmer_down") ||
        ezpi_ha_streq(item_name, "dimmer_stop"))
    {
        return false;
    }

    out->component = EZPI_HA_COMPONENT_SENSOR;
    out->unit = EZPI_ha_unit_from_scale(scale);
    out->state_class = EZPI_HA_STATE_CLASS_MEASUREMENT;

    out->device_class = ezpi_ha_lookup(sg_sensor_classes_by_item,
                                       EZPI_HA_COUNT(sg_sensor_classes_by_item),
                                       item_name);
    if (NULL == out->device_class)
    {
        out->device_class = ezpi_ha_lookup(sg_sensor_classes_by_type,
                                           EZPI_HA_COUNT(sg_sensor_classes_by_type),
                                           value_type);
    }

    /* The pairing rule. Keep the unit, drop the class. */
    if (NULL != out->device_class && !ezpi_ha_class_allows_unit(out->device_class, out->unit))
    {
        out->device_class = NULL;
    }

    return true;
}

size_t EZPI_ha_object_id(char *dst, size_t dst_size, const char *src)
{
    size_t written = 0;
    bool pending_separator = false;

    if (NULL == dst || 0 == dst_size)
    {
        return 0;
    }

    dst[0] = '\0';

    if (NULL == src)
    {
        return 0;
    }

    for (size_t i = 0; '\0' != src[i]; i++)
    {
        unsigned char c = (unsigned char)src[i];
        char keep;

        if (c >= 'A' && c <= 'Z')
        {
            keep = (char)(c - 'A' + 'a');
        }
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || '-' == c)
        {
            keep = (char)c;
        }
        else
        {
            /* Includes '_' itself, which is how a run of underscores in the
             * source collapses to one. Nothing is emitted yet: a separator is
             * only worth a byte once a kept character follows it, which is
             * also what drops leading and trailing underscores without a
             * second pass. */
            if (written > 0)
            {
                pending_separator = true;
            }
            continue;
        }

        if (pending_separator)
        {
            /* Two bytes are needed now -- the separator and the character it
             * separates -- plus the NUL. Stopping here rather than emitting a
             * lone trailing underscore is what keeps the result valid when the
             * buffer runs out mid-word. */
            if (written + 2 >= dst_size)
            {
                break;
            }
            dst[written++] = '_';
            pending_separator = false;
        }

        if (written + 1 >= dst_size)
        {
            break;
        }

        dst[written++] = keep;
    }

    dst[written] = '\0';
    return written;
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/
static bool ezpi_ha_streq(const char *a, const char *b)
{
    if (NULL == a || NULL == b)
    {
        return false;
    }

    return (0 == strcmp(a, b));
}

static const char *ezpi_ha_lookup(const s_ezpi_ha_pair_t *table, size_t count, const char *key)
{
    if (NULL == key || '\0' == key[0])
    {
        return NULL;
    }

    for (size_t i = 0; i < count; i++)
    {
        if (ezpi_ha_streq(table[i].key, key))
        {
            return table[i].value;
        }
    }

    return NULL;
}

static bool ezpi_ha_class_allows_unit(const char *device_class, const char *unit)
{
    for (size_t i = 0; i < EZPI_HA_COUNT(sg_class_units); i++)
    {
        if (!ezpi_ha_streq(sg_class_units[i].device_class, device_class))
        {
            continue;
        }

        /* The class is one with a unit requirement, so a missing unit fails it
         * too: Home Assistant rejects device_class temperature with no
         * unit_of_measurement just as firmly as it rejects the wrong one. */
        if (NULL == unit)
        {
            return false;
        }

        for (size_t u = 0; NULL != sg_class_units[i].units[u]; u++)
        {
            if (ezpi_ha_streq(sg_class_units[i].units[u], unit))
            {
                return true;
            }
        }

        return false;
    }

    /* A class with no entry here has no unit constraint to violate. */
    return true;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
