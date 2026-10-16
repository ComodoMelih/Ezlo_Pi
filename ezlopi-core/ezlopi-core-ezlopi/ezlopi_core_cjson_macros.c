/* ===========================================================================
** Copyright (C) 2024 Ezlo Innovation Inc
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
 * @file    ezlopi_core_cjson_macros.c
 * @brief   perform some function on data
 * @author  Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 0.1
 * @date    12th DEC 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

#include "ezlopi_core_cjson_macros.h"

#ifndef EZPI_USE_CJSON_MACRO

#include "cjext.h"
#include "ezlopi_util_trace.h"
#include "ezlopi_cloud_constants.h"

/* For GPIO_NUM_MAX. Included for the pin-number bound in
** EZPI_core_cjson_get_value_gpio() below and nothing else -- no pin is driven
** from this file. */
#include "driver/gpio.h"

/**
 *
 */
void EZPI_core_cjson_get_value_uint8(cJSON *cj_root, const char *item_name_str, uint8_t *item_val)
{
    *item_val = 0;
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
    if (o_item && (o_item->type & cJSON_Number))
    {
        *item_val = o_item->valuedouble;
    }
}

/**
 *
 */
void EZPI_core_cjson_get_value_float(cJSON *cj_root, const char *item_name_str, float *item_val)
{
    *item_val = 0;
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
    if (o_item)
    {
        *item_val = (float)o_item->valuedouble;
    }
}

void EZPI_core_cjson_get_value_double(cJSON *cj_root, const char *item_name_str, double *item_val)
{
    *item_val = 0;
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
    if (o_item && (o_item->type & cJSON_Number))
    {
        *item_val = o_item->valuedouble;
    }
}

void EZPI_core_cjson_get_value_int(cJSON *cj_root, const char *item_name_str, int *item_val)
{
    *item_val = 0;
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
    if (o_item && (o_item->type & cJSON_Number))
    {
        *item_val = (int)o_item->valuedouble;
    }
}

void EZPI_core_cjson_get_value_uint16(cJSON *cj_root, const char *item_name_str, uint16_t *item_val)
{
    *item_val = 0;
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
    if (o_item && (o_item->type & cJSON_Number))
    {
        *item_val = (uint16_t)o_item->valuedouble;
    }
}

void EZPI_core_cjson_get_value_uint32(cJSON *cj_root, const char *item_name_str, uint32_t *item_val)
{
    *item_val = 0;
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
    if (o_item && (o_item->type & cJSON_Number))
    {
        *item_val = (uint32_t)o_item->valuedouble;
    }
}

uint32_t EZPI_core_cjson_get_id(cJSON *cj_root, const char *item_name_str)
{
    uint32_t item_val = 0;
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
    if (o_item && (o_item->type & cJSON_String))
    {
        item_val = (uint32_t)strtoul(o_item->valuestring, NULL, 16);
    }

    return item_val;
}

void EZPI_core_cjson_get_value_bool(cJSON *cj_root, const char *item_name_str, bool *item_val)
{
    *item_val = false;
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
    if (o_item)
    {
        if (o_item->type & cJSON_True)
        {
            *item_val = true;
        }
    }
}

void EZPI_core_cjson_trace(const char *name_str, cJSON *cj_object)
{
    if (cj_object)
    {
        char *obj_str = cJSON_Print(__FUNCTION__, cj_object);
        if (obj_str)
        {
            TRACE_D("%s[%d]: %s", name_str ? name_str : ezlopi__str, strlen(obj_str), obj_str);
            ezlopi_free(__FUNCTION__, obj_str);
        }
    }
    else
    {
        TRACE_E("%s: Null", name_str ? name_str : ezlopi__str);
    }
}

void EZPI_core_cjson_get_value_string_by_copy(cJSON *cj_root, const char *item_name_str, char *item_val, uint32_t item_val_len)
{
    if (cj_root && item_name_str && item_val && item_val_len)
    {
        cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
        if (o_item && o_item->valuestring && o_item->str_value_len)
        {
            /* "%.*s" reads its precision as an int from the variable argument
            ** list, and str_value_len is a size_t. On every ESP target the two
            ** are the same 32-bit type, which is why this has shipped; it stops
            ** being the same type as soon as the file is compiled anywhere
            ** size_t is wider, and then the precision is read from the wrong
            ** four bytes. The cast is a no-op on target and the difference
            ** between right and wrong on a host, where tests/host now builds
            ** this file. */
            snprintf(item_val, item_val_len, "%.*s", (int)o_item->str_value_len, o_item->valuestring);
        }
    }
}

void EZPI_core_cjson_get_value_string_by_alloc(cJSON *cj_root, const char *item_name_str, char **item_val_ptr_address)
{
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);

    if (o_item && o_item->valuestring && o_item->str_value_len)
    {
        uint32_t value_len = o_item->str_value_len + 1;
        ezlopi_free(__func__, *item_val_ptr_address);
        *item_val_ptr_address = ezlopi_malloc(__func__, value_len);
        if (NULL != *item_val_ptr_address)
        {
            /* (int) for the reason given in the function above. */
            snprintf(*item_val_ptr_address, value_len, "%.*s", (int)o_item->str_value_len, o_item->valuestring);
        }
    }
}

void EZPI_core_cjson_assign_id_as_string(cJSON *cj_object, uint32_t id, const char *id_str)
{
    if (id && cj_object && id_str)
    {
        char tmp_str[32];
        snprintf(tmp_str, sizeof(tmp_str), "%08x", id);
        cJSON_AddStringToObject(__FUNCTION__, cj_object, id_str, tmp_str);
    }
}

void EZPI_core_cjson_assign_number_as_string(cJSON *cj_obj, uint32_t num, const char *name_str)
{
    if (cj_obj && name_str)
    {
        char tmp_str[32];
        snprintf(tmp_str, sizeof(tmp_str), "%u", (uint32_t)num);
        cJSON_AddStringToObject(__FUNCTION__, cj_obj, name_str, tmp_str);
    }
}

/* The 128 CJSON_GET_VALUE_GPIO call sites hand whatever number the device
** configuration carried to GPIO_IS_VALID_GPIO(), which is
**
**     ((1ULL << (gpio_num)) & SOC_GPIO_VALID_GPIO_MASK) != 0
**
** -- a shift by that number. Shifting a 64-bit value by a count outside 0..63
** is undefined, and "undefined" here does not mean "rejected": an out-of-range
** pin number ALIASES onto a real pin, because every shift instruction involved
** truncates the count to the register width instead of saturating.
**
** Measured, not reasoned about. Running the macro's own expression against the
** esp32 mask for a runtime value gives:
**
**     n=70  -> accepted, pin_bit_mask 0x40  -> GPIO 6
**     n=64  -> accepted, pin_bit_mask 0x01  -> GPIO 0
**     n=72  -> accepted, pin_bit_mask 0x100 -> GPIO 8
**     n=100 -> accepted, pin_bit_mask 0x10.. -> GPIO 36
**
** GPIO 6 is the SPI flash clock on a standard esp32 module. A configuration
** naming pin 70 therefore passed validation and had a driver configure the
** flash bus as a push-pull output. The aliasing target is architecture
** specific -- it was read off the asm both target compilers emit, which
** contains no range check -- but "accepted when it should not be" is not.
**
** So the range is constrained here, at the one place the number enters, rather
** than at 128 call sites. Three things are deliberately NOT done:
**
**   - The mask is not consulted. Pins that are in range but reserved (24 and
**     28..31 on esp32) are left to the callers' existing GPIO_IS_VALID_GPIO,
**     which answers that correctly and without UB once the count is in range.
**     This fixes the undefined shift, it does not duplicate the valid-pin test.
**
**   - -1 and 255 are passed through untouched. Both are "no pin" spellings the
**     drivers already test for by hand (device_0001_digitalOut_generic.c:277
**     checks 255, :303 checks -1 and 255), and a configuration using either is
**     a working configuration today. Normalising them would be behaviourally
**     equivalent but is still a change to inputs that work, which is the one
**     cost this file must not pay. They remain technically out-of-range shift
**     counts; both were checked against the xtensa and riscv32 compilers at
**     -O2 and both fold to 0, so the callers' tests reach the right answer.
**     That is a measured accident, and it is why the sentinels are named here
**     instead of being left for a reader to rediscover.
**
**   - The configuration is not refused. An unusable pin number disables that
**     one pin, exactly as an absent key does; it does not reject the whole
**     document. Refusing configurations that work today is the failure mode
**     this area already has a correction recorded for in the changelog.
**
** Coverage is all 128 CJSON_GET_VALUE_GPIO sites, which between them read 10 of
** the parser's 15 pin keys: gpio (32 sites), gpio1 (28), gpio2 (23), gpio_sda
** (12), gpio_scl (12), gpio_tx (6), gpio_rx (6), gpio_out (3), gpio_in (3),
** gpio3 (3).
**
** Sixteen other pin reads do NOT come through here, and the gap is per call
** site rather than per key -- several of them read keys this accessor also
** reads, through a different macro:
**
**     device-0038 RGB            :360-362   gpio1..gpio3 via CJSON_GET_VALUE_INT
**     sensor-0037 pms5003        :548-551   gpio1..gpio3 and a bare "gpio4"
**                                           literal, via CJSON_GET_VALUE_UINT32
**     sensor-0040 TCS230         :185-190   gpio1..gpio6 via CJSON_GET_VALUE_UINT32
**     sensor-0047 HX711          :174-175   gpio1, gpio2 via CJSON_GET_VALUE_INT
**     sensor-0070 ADC dummy      :159       gpio via CJSON_GET_VALUE_INT
**
** Those accessors are general-purpose integer readers used by hundreds of
** non-pin keys, so the bound does not belong in them; the fix for those sixteen
** is to route them through here, which means changing their destination types
** and is a separate change.
**
** tests/host/test_cjsongpio.c compiles this file and drives both halves: that
** out-of-range numbers are refused, and that 255 still is not. Removing either
** clause fails it, which was checked by removing them. */
#define EZPI_GPIO_UNUSED_255 (255)

int EZPI_core_cjson_get_value_gpio(cJSON *cj_root, const char *item_name_str)
{
    int item_val = -1;
    cJSON *o_item = cJSON_GetObjectItem(__FUNCTION__, cj_root, item_name_str);
    if (o_item && (o_item->type & cJSON_Number))
    {
        /* Bounded before the narrowing cast, not after. valuedouble is whatever
        ** the document carried, and converting a double that does not fit int
        ** is itself undefined -- so the test has to be on the double. */
        const double raw = o_item->valuedouble;

        if (((raw >= 0.0) && (raw < (double)GPIO_NUM_MAX)) ||
            (raw == -1.0) || (raw == (double)EZPI_GPIO_UNUSED_255))
        {
            item_val = (int)raw;
        }
        else
        {
            /* Logged as a double, not narrowed for the message. The first
            ** version of this line cast the value so it could be printed with
            ** %d, which then needed an isfinite() test, a two-sided clamp and
            ** <math.h> to avoid committing a second piece of undefined
            ** behaviour while reporting the first -- all of it for a
            ** diagnostic. %g takes the value as it is: cJSON parses numbers
            ** with strtod, so "gpio":1e999 arrives here as +inf and prints as
            ** "inf", which is more use to whoever reads the log than a clamped
            ** 2147483647 would have been.
            **
            ** %g is safe on these builds rather than assumed to be:
            ** CONFIG_NEWLIB_NANO_FORMAT is unset in all four of
            ** sdkconfig.defaults.esp32/s2/s3/c3 -- nano formatting is what drops
            ** float support from printf -- and the tree already logs %f in
            ** ezlopi_main.c:233 and throughout the scenes code. */
            TRACE_W("%s: %g is not a GPIO on this target (0..%d); treating the pin as unused",
                    item_name_str, raw, (int)GPIO_NUM_MAX - 1);
        }
    }
    return item_val;
}
#endif // EZPI_USE_CJSON_MACRO