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
 * @file    ezlopi_core_cjson_macros.h
 * @brief   Macros for cjson operations
 * @author  Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 0.1
 * @date    December 7th, 2023 7:42 PM
 */
#ifndef _EZLOPI_CORE_CJSON_MACROS_H_
#define _EZLOPI_CORE_CJSON_MACROS_H_

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "cjext.h"
#include "ezlopi_util_trace.h"

/*******************************************************************************
 *                          C++ Declaration Wrapper
 *******************************************************************************/
#ifdef __cplusplus
extern "C"
{
#endif

    /*******************************************************************************
     *                          Type & Macro Declarations
     *******************************************************************************/

    void EZPI_core_cjson_trace(const char *name_str, cJSON *cj_object);

    /* 87 call sites pass the address of an enum-typed struct field to
    ** CJSON_GET_VALUE_INT, which forwards it as an int * -- for example
    ** CJSON_GET_VALUE_INT(cjson_device, ezlopi_dev_type_str,
    ** item->interface_type), where the field is an
    ** e_ezlopi_device_interface_type_t. That is why the build emits 87
    ** -Wincompatible-pointer-types warnings from the macro on line ~84.
    **
    ** Today that is a type pun and not a memory error: with this toolchain and
    ** this project's flags an enum is exactly int-sized and int-aligned, so
    ** EZPI_core_cjson_get_value_int() writes 4 bytes into a 4-byte field.
    ** Verified, not assumed -- the compiler was asked directly, and the same
    ** probe fails under -fshort-enums, which confirms it measures something.
    ** There is no -fshort-enums anywhere in the generated
    ** compile_commands.json.
    **
    ** But that safety is an ABI accident, not a property of the code. Narrow
    ** the enums -- -fshort-enums, a different target, a toolchain that packs
    ** them -- and all 87 sites start writing 4 bytes into fields as small as
    ** one, silently corrupting whatever struct members sit next to them.
    **
    ** The call sites are deliberately left alone: changing 87 of them across
    ** the device drivers is exactly the kind of churn that risks altering
    ** working Ezlo behaviour for no present defect. Instead the assumption
    ** they all rest on is made explicit here, so the day it stops holding the
    ** build stops rather than the firmware misbehaving in the field. */
    typedef enum e_ezpi_enum_abi_probe
    {
        EZPI_ENUM_ABI_PROBE_ZERO = 0,
        EZPI_ENUM_ABI_PROBE_ONE
    } e_ezpi_enum_abi_probe_t;

    _Static_assert(sizeof(e_ezpi_enum_abi_probe_t) == sizeof(int),
                   "An enum is no longer int-sized, so the 87 call sites that "
                   "pass an enum field to CJSON_GET_VALUE_INT now write past "
                   "it. Fix those call sites before changing this.");
    _Static_assert(__alignof__(e_ezpi_enum_abi_probe_t) == __alignof__(int),
                   "An enum is no longer int-aligned; see the comment above "
                   "CJSON_GET_VALUE_INT.");

    int EZPI_core_cjson_get_value_gpio(cJSON *cj_root, const char *item_name_str);
    void EZPI_core_cjson_get_value_int(cJSON *cj_root, const char *item_name_str, int *item_val);
    void EZPI_core_cjson_get_value_bool(cJSON *cj_root, const char *item_name_str, bool *item_val);
    void EZPI_core_cjson_get_value_float(cJSON *cj_root, const char *item_name_str, float *item_val);
    void EZPI_core_cjson_get_value_uint8(cJSON *cj_root, const char *item_name_str, uint8_t *item_val);
    void EZPI_core_cjson_get_value_double(cJSON *cj_root, const char *item_name_str, double *item_val);
    void EZPI_core_cjson_get_value_uint16(cJSON *cj_root, const char *item_name_str, uint16_t *item_val);
    void EZPI_core_cjson_get_value_uint32(cJSON *cj_root, const char *item_name_str, uint32_t *item_val);

    void EZPI_core_cjson_get_value_string_by_alloc(cJSON *cj_root, const char *item_name_str, char **item_val_ptr_address);
    void EZPI_core_cjson_get_value_string_by_copy(cJSON *cj_root, const char *item_name_str, char *item_val, uint32_t item_val_len);

    uint32_t EZPI_core_cjson_get_id(cJSON *cj_root, const char *item_name_str);
    void EZPI_core_cjson_assign_id_as_string(cJSON *cj_object, uint32_t id, const char *id_str);
    void EZPI_core_cjson_assign_number_as_string(cJSON *cj_obj, uint32_t num, const char *name_str);

#define CJSON_TRACE(name_str, cj_object) EZPI_core_cjson_trace(name_str, cj_object)

#define CJSON_ASSIGN_ID(cj_obj, id_num, id_name_str) EZPI_core_cjson_assign_id_as_string(cj_obj, id_num, id_name_str)
#define CJSON_ASSIGN_NUMBER_AS_STRING(cj_obj, num_val, name_str) EZPI_core_cjson_assign_number_as_string(cj_obj, num_val, name_str)

#define CJSON_GET_VALUE_GPIO(cj_root, item_name_str, gpio_num) gpio_num = EZPI_core_cjson_get_value_gpio(cj_root, item_name_str)

#define CJSON_GET_VALUE_INT(cj_root, item_name_str, item_val) EZPI_core_cjson_get_value_int(cj_root, item_name_str, &item_val)
#define CJSON_GET_VALUE_BOOL(cj_root, item_name_str, item_val) EZPI_core_cjson_get_value_bool(cj_root, item_name_str, &item_val)
#define CJSON_GET_VALUE_FLOAT(cj_root, item_name_str, item_val) EZPI_core_cjson_get_value_float(cj_root, item_name_str, &item_val)
#define CJSON_GET_VALUE_UINT8(cj_root, item_name_str, item_val) EZPI_core_cjson_get_value_uint8(cj_root, item_name_str, &item_val)
#define CJSON_GET_VALUE_DOUBLE(cj_root, item_name_str, item_val) EZPI_core_cjson_get_value_double(cj_root, item_name_str, &item_val)
#define CJSON_GET_VALUE_UINT16(cj_root, item_name_str, item_val) EZPI_core_cjson_get_value_uint16(cj_root, item_name_str, &item_val)
#define CJSON_GET_VALUE_UINT32(cj_root, item_name_str, item_val) EZPI_core_cjson_get_value_uint32(cj_root, item_name_str, &item_val)

#define CJSON_GET_VALUE_STRING_BY_COPY(cj_root, item_name_str, item_val, item_val_size) EZPI_core_cjson_get_value_string_by_copy(cj_root, item_name_str, item_val, item_val_size)
#define CJSON_GET_VALUE_STRING_BY_COPY_INTO_PTR(cj_root, item_name_str, item_val_ptr_address) EZPI_core_cjson_get_value_string_by_alloc(cj_root, item_name_str, item_val_ptr_address)

    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/

#ifdef __cplusplus
}
#endif

#endif // _EZLOPI_CORE_CJSON_MACROS_H_

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
