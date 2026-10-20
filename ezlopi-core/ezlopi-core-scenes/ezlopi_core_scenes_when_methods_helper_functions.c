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
 * @file    ezlopi_core_scenes_when_methods_helper_functions.c
 * @brief   Functions that operates on scene-when-methods
 * @author  Krishna Kumar Sah (work.krishnasah@gmail.com)
 * @version 1.0
 * @date    12th DEC 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "sdkconfig.h"

#ifdef CONFIG_EZPI_SERV_ENABLE_MESHBOTS

// #include "ezlopi_core_devices.h"
// #include "ezlopi_core_scenes_v2.h"
#include "ezlopi_util_trace.h"
#include "ezlopi_core_nvs.h"
#include "ezlopi_core_http.h"
#include "ezlopi_core_device_group.h"
#include "ezlopi_core_scenes_edit.h"
#include "ezlopi_core_scenes_expressions.h"
#include "ezlopi_core_scenes_when_methods_helper_functions.h"
#include "ezlopi_core_scenes_delete.h"
#include "ezlopi_core_scenes_suntime.h"

#include "ezlopi_cloud_constants.h"
#include "EZLOPI_USER_CONFIG.h"

/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/
/* e_issunstate_offset_t and s_sunstate_data_t were defined here. They now live
** in ezlopi_core_scenes_when_methods_helper_functions.h, with the comment that
** explains the two day fields, so that a host test can construct the struct
** the five ISSUNSTATE_* functions read out of `fields->user_arg`. Nothing else
** moved. */

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static int __compare_item_values(l_ezlopi_item_t *curr_item, l_fields_v2_t *value_field);
static uint8_t __isdate_check_endweek_conditon(e_isdate_modes_t mode_type, struct tm *info);
static uint8_t __isdate_find_nth_week_curr_month(struct tm *info);
static void __issunsate_update_sunstate_tm(int tm_mday, s_sunstate_data_t *user_data);
static bool __issunstate_add_offs(e_issunstate_offset_t tmoffs_type, struct tm *choosen_suntime, struct tm *defined_moment, const char *tm_offs_val);
static bool __and_when_block_condition(l_scenes_list_v2_t *scene_node, l_when_block_v2_t *when_block);
static uint8_t __isitemState_vs_field_compare(s_item_exp_data_t *new_extract_data, l_fields_v2_t *tmp_field, uint8_t bit_mode_position);
static void __replace_old_with_new_data_val(s_item_exp_data_t *new_extract_data, s_item_exp_data_t *prev_extract_data);
static int ____old_vs_new_extract_data(s_item_exp_data_t *new_extract_data, s_item_exp_data_t *prev_extract_data);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
int ISITEM_state_single_condition(uint32_t item_id, l_fields_v2_t *value_field)
{
    int ret = 0;
    l_ezlopi_item_t *curr_item = EZPI_core_device_get_item_by_id(item_id);
    if (curr_item)
    {
        ret = __compare_item_values(curr_item, value_field);
    }
    return ret;
}
int ISITEM_state_with_grp_condition(uint32_t device_group_id, uint32_t item_group_id, l_fields_v2_t *value_field)
{
    int ret = 0;

    l_ezlopi_device_grp_t *curr_devgrp = EZPI_core_device_group_get_by_id(device_group_id);
    if (curr_devgrp)
    {
        cJSON *cj_get_devarr = NULL;
        cJSON_ArrayForEach(cj_get_devarr, curr_devgrp->devices)
        {
            /* Element types in devgrp->devices / itemgrp->item_names are not
             * validated when the group is created (only cJSON_IsArray is), and
             * ->valuestring is NULL on every non-string node, so a group
             * holding a numeric id reached strtoul(NULL). */
            if (false == cJSON_IsString(cj_get_devarr))
            {
                continue;
            }

            uint32_t curr_devce_id = strtoul(cj_get_devarr->valuestring, NULL, 16);
            l_ezlopi_device_t *curr_device = EZPI_core_device_get_by_id(curr_devce_id); // immediately goto "102ec000" ...
            if (curr_device)
            {
                l_ezlopi_item_t *curr_item_node = curr_device->items; // perform operation on items of above device --> "102ec000"
                while (curr_item_node)
                {
                    // compare with items_list stored in item_group_id
                    l_ezlopi_item_grp_t *curr_item_grp = EZPI_core_item_group_get_by_id(item_group_id); // get  "ll_itemgrp_node"
                    if (curr_item_grp)
                    {
                        cJSON *cj_item_names = NULL;
                        cJSON_ArrayForEach(cj_item_names, curr_item_grp->item_names)
                        {
                            if (false == cJSON_IsString(cj_item_names))
                            {
                                continue;
                            }

                            uint32_t req_item_id_from_itemgrp = strtoul(cj_item_names->valuestring, NULL, 16);
                            // if the item_ids match ; Then compare the "item_values" with that of the "scene's" requirement
                            if (req_item_id_from_itemgrp == curr_item_node->cloud_properties.item_id)
                            {
                                // compare the values of "when-condition"  & the "ll_item_node"
                                ret = __compare_item_values(curr_item_node, value_field);
                                break;
                            }
                        }
                    }
                    curr_item_node = curr_item_node->next;
                }
            }
        }
    }

    return ret;
}

uint8_t ISDATE_type_check(e_isdate_modes_t *mode_type, struct tm *info, l_fields_v2_t *curr_field)
{
    const char *field_type_name[] = {
        "daily",
        "weekly",
        "monthly",
        "weeks",
        "yearWeeks",
    };
    if ((EZLOPI_VALUE_TYPE_STRING == curr_field->value_type) && (NULL != curr_field->field_value.u_value.value_string))
    {
        const char *check_type_name = curr_field->field_value.u_value.value_string;
        for (uint8_t t = 0; t < 5; t++)
        {
            if (EZPI_STRNCMP_IF_EQUAL(field_type_name[t], check_type_name, strlen(field_type_name[t]) + 1, strlen(check_type_name) + 1))
            {
                *mode_type = (e_isdate_modes_t)t; // daily , weekly , monthly,... // 0-4
                break;
            }
        }
    }
    return 0;
}
uint8_t ISDATE_tm_check(e_isdate_modes_t *mode_type, struct tm *info, l_fields_v2_t *curr_field)
{
    uint8_t ret = 0;
    cJSON *cj_time_arr = curr_field->field_value.u_value.cj_value;
    if (cj_time_arr && (cJSON_Array == cj_time_arr->type))
    {
        ret |= (1 << 4);
        char field_hr_mm[10] = {0};
        strftime(field_hr_mm, 10, "%H:%M", info);
        field_hr_mm[9] = '\0';
        // TRACE_S("[field_hr_mm: %s]", field_hr_mm);

        int array_size = cJSON_GetArraySize(cj_time_arr);
        for (int i = 0; i < array_size; i++)
        {
            cJSON *array_item = cJSON_GetArrayItem(cj_time_arr, i);
            if (array_item && cJSON_IsString(array_item))
            {
                // TRACE_S("Time activate_%d: %s,  [field_hr_mm: %s]", i, array_item->valuestring, field_hr_mm);
                if (EZPI_STRNCMP_IF_EQUAL(array_item->valuestring, field_hr_mm, array_item->str_value_len, sizeof(field_hr_mm)))
                {
                    ret |= (1 << 0); // One of the TIME-condition has been met.
                    break;
                }
            }
        }
        if (!array_size) // if we are given : -"value" : []
        {
            TRACE_S("Time activate :- [00:00],  [field_hr_mm: %s]", field_hr_mm);
            if (EZPI_STRNCMP_IF_EQUAL(field_hr_mm, "00:00", sizeof(field_hr_mm), 6)) // 24-hr format
            {
                ret |= (1 << 0); // TIME-condition "00:00" has been met.
            }
        }
    }
    return ret;
}
uint8_t ISDATE_weekdays_check(e_isdate_modes_t *mode_type, struct tm *info, l_fields_v2_t *curr_field)
{
    uint8_t ret = 0;
    cJSON *cj_weekdays_arr = curr_field->field_value.u_value.cj_value;
    if (cj_weekdays_arr && (cJSON_Array == cj_weekdays_arr->type))
    {
        ret |= (1 << 5);
        // Only for comparisions dont change 'info->tm_wday' -> 'sun:0,mon:1, ... , sat:6' to 'sun:7,mon:1, ... ,sat:6'
        int field_weekdays = (0 == (info->tm_wday)) ? 7 : (info->tm_wday); // sunday => 0+1 ... saturday => 6+1

        int array_size = cJSON_GetArraySize(cj_weekdays_arr);
        for (int i = 0; i < array_size; i++)
        {
            cJSON *array_item = cJSON_GetArrayItem(cj_weekdays_arr, i);
            if (array_item && cJSON_IsNumber(array_item))
            {
                // TRACE_S("Weekdays activate_[%d]: %d, [field_weekdays: %d]", i, (int)(array_item->valuedouble), field_weekdays);
                if ((int)(array_item->valuedouble) == field_weekdays)
                {
                    ret |= (1 << 1); // One of the WEEKDAYS-condition has been met.
                    break;
                }
            }
        }
    }
    return ret;
}
uint8_t ISDATE_mdays_check(e_isdate_modes_t *mode_type, struct tm *info, l_fields_v2_t *curr_field)
{
    uint8_t ret = 0;
    cJSON *cj_days_arr = curr_field->field_value.u_value.cj_value;
    if (cj_days_arr && (cJSON_Array == cj_days_arr->type))
    {
        ret |= (1 << 6);
        int array_size = cJSON_GetArraySize(cj_days_arr);
        int field_days = info->tm_mday; // 1-31

        for (int i = 0; i < array_size; i++)
        {
            cJSON *array_item = cJSON_GetArrayItem(cj_days_arr, i);
            if (array_item && cJSON_IsNumber(array_item))
            {
                TRACE_S("Days activate_%d: %d, [field_days: %d]", i, (int)(array_item->valuedouble), field_days);
                if ((int)(array_item->valuedouble) == field_days)
                {
                    ret |= (1 << 2); // One of the DAYS-condition has been met.
                    break;
                }
            }
        }
    }
    return ret;
}

uint8_t ISDATE_year_weeks_check(e_isdate_modes_t *mode_type, struct tm *info, l_fields_v2_t *curr_field)
{
    uint8_t ret = 0;
    cJSON *cj_weeks_arr = curr_field->field_value.u_value.cj_value;
    if (cj_weeks_arr && (cJSON_Array == cj_weeks_arr->type))
    {
        ret |= (1 << 7);
        int array_size = cJSON_GetArraySize(cj_weeks_arr);
        for (int i = 0; i < array_size; i++)
        {
            // extract ;- [1,4,5,23,6,9,...,-1]
            cJSON *array_item = cJSON_GetArrayItem(cj_weeks_arr, i);
            if (array_item && cJSON_IsNumber(array_item))
            {
                if (-1 == (int)(array_item->valuedouble)) // for case :- '-1'
                {
                    if (0 != (ret = __isdate_check_endweek_conditon(*mode_type, info))) // ret = (1<<3), if last-week confirmed
                    {
                        TRACE_I("Weeks_condition : '-1' has been satisfied ; ret = %#x", ret);
                        break;
                    }
                }
                else // for case :- 'n'
                {
                    char field_weeks[10] = {0}; // week_value extracted from ESP32.
                    char week_val[10] = {0};    // week_value given to us from cloud.

                    // reducing array values by -1, for easier comparison ::==>  [1_54]--->[0_53]      or      [1_6]-->[0_5]
                    snprintf(week_val, 10, "%d", (int)(array_item->valuedouble - 1));
                    week_val[9] = '\0';

                    if (ISDATE_YEAR_WEEKS_MODE == *mode_type)
                    {
                        strftime(field_weeks, 10, "%W", info); // [First day => Monday] ; Week number within (00-53)
                        field_weeks[9] = '\0';
                    }
                    else if (ISDATE_WEEKS_MODE == *mode_type)
                    {
                        int tmp_week = __isdate_find_nth_week_curr_month(info); // return the current week-number with 'monday' as first day
                        snprintf(field_weeks, 10, "%d", tmp_week);
                        field_weeks[9] = '\0';
                    }

                    if (EZPI_STRNCMP_IF_EQUAL(week_val, field_weeks, sizeof(week_val), sizeof(field_weeks))) // comparsion in string formats only
                    {
                        ret |= (1 << 3); // One of the TIME-condition has been met.
                        TRACE_I("Weeks_condition '%sth week' [reqd : %s]  has been satisfied ; ret = (1<<3)", field_weeks, week_val);
                        break;
                    }
                }
            }
        }
    }
    return ret;
}
int ISDATE_check_flag_result(e_isdate_modes_t mode_type, uint8_t flag_check)
{
    int ret = 0;
    const uint8_t TIME_FLAG = (1 << 0);
    const uint8_t WEEKDAYS_FLAG = (1 << 1);
    const uint8_t DAYS_FLAG = (1 << 2);
    const uint8_t WEEKS_FLAG = (1 << 3);
    const uint8_t MASK_FOR_TIME_ARG = (1 << 4);
    const uint8_t MASK_FOR_WEEKDAYS_ARG = (1 << 5);
    const uint8_t MASK_FOR_DAYS_ARG = (1 << 6);
    const uint8_t MASK_FOR_WEEKS_ARG = (1 << 7);
    switch (mode_type)
    {
    case ISDATE_DAILY_MODE:
    {
        if (((flag_check & MASK_FOR_TIME_ARG) && (flag_check & TIME_FLAG)))
        {
            // TRACE_W("here! daily-time");
            ret = 1;
        }
        break;
    }
    case ISDATE_WEEKLY_MODE:
    {
        if ((((flag_check & MASK_FOR_TIME_ARG) && (flag_check & TIME_FLAG)) &&
             ((flag_check & MASK_FOR_WEEKDAYS_ARG) && (flag_check & WEEKDAYS_FLAG))))
        {
            // TRACE_W("here! week_days and time");
            ret = 1;
        }
        break;
    }
    case ISDATE_MONTHLY_MODE:
    {
        if ((((flag_check & MASK_FOR_TIME_ARG) && (flag_check & TIME_FLAG)) &&
             ((flag_check & MASK_FOR_DAYS_ARG) && (flag_check & DAYS_FLAG))))
        {
            // TRACE_W("here! month_days and time");
            ret = 1;
        }
        break;
    }
    case ISDATE_WEEKS_MODE:
    case ISDATE_YEAR_WEEKS_MODE:
    {
        if (((flag_check & MASK_FOR_TIME_ARG) && (flag_check & TIME_FLAG)) &&
            ((flag_check & MASK_FOR_WEEKS_ARG) && (flag_check & WEEKS_FLAG)))
        {
            // TRACE_W("here! week and time");
            ret = 1;
        }
        break;
    }
    default:
    {
        break;
    }
    }
    return ret;
}

uint8_t ISONCE_tm_check(l_fields_v2_t *curr_field, struct tm *info)
{
    uint8_t flag_check = 0;
    if ((EZLOPI_VALUE_TYPE_24_HOURS_TIME == curr_field->value_type) && (NULL != curr_field->field_value.u_value.value_string))
    {
        char field_hr_mm[10] = {0};
        strftime(field_hr_mm, 10, "%H:%M", info);
        field_hr_mm[9] = '\0';

        if (EZPI_STRNCMP_IF_EQUAL(curr_field->field_value.u_value.value_string, field_hr_mm, strlen(curr_field->field_value.u_value.value_string) + 1, sizeof(field_hr_mm)))
        {
            flag_check |= (1 << 0); // One of the TIME-condition has been met.
        }
    }
    return flag_check;
}
uint8_t ISONCE_day_check(l_fields_v2_t *curr_field, struct tm *info)
{
    uint8_t flag_check = 0;
    if (EZLOPI_VALUE_TYPE_INT == curr_field->value_type)
    {
        if ((int)(curr_field->field_value.u_value.value_double) == info->tm_mday)
        {
            flag_check |= (1 << 1);
        }
    }
    return flag_check;
}
uint8_t ISONCE_month_check(l_fields_v2_t *curr_field, struct tm *info)
{
    uint8_t flag_check = 0;
    if (EZLOPI_VALUE_TYPE_INT == curr_field->value_type)
    {
        if ((int)(curr_field->field_value.u_value.value_double) == (info->tm_mon + 1))
        {
            flag_check |= (1 << 2);
        }
    }
    return flag_check;
}
uint8_t ISONCE_year_check(l_fields_v2_t *curr_field, struct tm *info)
{
    uint8_t flag_check = 0;
    if (EZLOPI_VALUE_TYPE_INT == curr_field->value_type)
    {
        if ((int)(curr_field->field_value.u_value.value_double) == (info->tm_year + 1900))
        {
            flag_check |= (1 << 3);
        }
    }
    return flag_check;
}
int ISONCE_check_flag_result(l_scenes_list_v2_t *scene_node, uint8_t flag_check)
{
    int ret = 0;
    const uint8_t TIME_FLAG = (1 << 0);
    const uint8_t DAY_FLAG = (1 << 1);
    const uint8_t MONTH_FLAG = (1 << 2);
    const uint8_t YEAR_FLAG = (1 << 3);

    if ((flag_check & TIME_FLAG) && (flag_check & DAY_FLAG) && (flag_check & MONTH_FLAG) && (flag_check & YEAR_FLAG))
    {
        // TRACE_W("here! once and time");
        // now to disable the scene and also store in ezlopi_nvs
        scene_node->enabled = false;
        EZPI_core_scenes_enable_disable_scene_by_id_v2(scene_node->_id, false);
        ret = 1;
    }
    return ret;
}

uint8_t ISSUNSTATE_get_suntime(l_scenes_list_v2_t *scene_node, l_fields_v2_t *curr_field, struct tm *info, uint8_t curr_sunstate_mode)
{
    uint8_t flag_check = 0;
    /*Extract today's suntime via API call*/
    if ((EZLOPI_VALUE_TYPE_STRING == curr_field->value_type) && (NULL != curr_field->field_value.u_value.value_string))
    {
        flag_check |= (1 << 4); // indicates 'MASK_TIME_FLAG'

        // 1. check for valid data within 'user_arg'

        if (NULL == (scene_node->when_block->fields->user_arg))
        {
            s_sunstate_data_t *data = (s_sunstate_data_t *)ezlopi_malloc(__FUNCTION__, sizeof(s_sunstate_data_t));
            if (data)
            {
                memset(data, 0, sizeof(s_sunstate_data_t));
                scene_node->when_block->fields->user_arg = (void *)data;
                scene_node->when_block->fields->user_arg_type = FIELD_USER_ARG_HEAP;
            }
            else
            {
                TRACE_E("failed to create user_data...");
                return 0;
            }
        }
        // 2. Recalculate suntime for new-day or null
        s_sunstate_data_t *user_data = (s_sunstate_data_t *)(scene_node->when_block->fields->user_arg);
        if (user_data && (info->tm_mday != user_data->curr_tm_day))
        {
            /* Before the trace, not after: the trace below ran once a second
            ** for as long as a lookup kept failing, which is the same flood in
            ** the log that the TLS attempts were on the wire. */
            const int now_sod = (info->tm_hour * 3600) + (info->tm_min * 60) + info->tm_sec;
            if (false == EZPI_core_suntime_should_attempt(&(user_data->backoff), info->tm_mday, now_sod))
            {
                /* Silent. The attempt that started the backoff said what was
                ** wrong and when the next one would be; repeating it every
                ** second would bury the rest of the log, which is where
                ** whatever actually broke is going to be found. */
                return 0;
            }

            TRACE_S("curr_day = [%d] ; [%dth]", info->tm_mday, user_data->curr_tm_day);
            char *tmp_str = curr_field->field_value.u_value.value_string;
            size_t tmp_str_len = strlen(tmp_str);
            user_data->sunstate_mode = curr_sunstate_mode;            // this sets target sunstate for curr meshbot
            __issunsate_update_sunstate_tm(info->tm_mday, user_data); // assign 'curr_day' & 'suntime' only
            user_data->tmoffs_type = (EZPI_STRNCMP_IF_EQUAL(tmp_str, ezlopi_intime_str, tmp_str_len, 7))   ? ISSUNSTATE_INTIME_MODE
                                     : (EZPI_STRNCMP_IF_EQUAL(tmp_str, ezlopi_before_str, tmp_str_len, 7)) ? ISSUNSTATE_BEFORE_MODE
                                     : (EZPI_STRNCMP_IF_EQUAL(tmp_str, ezlopi_after_str, tmp_str_len, 6))  ? ISSUNSTATE_AFTER_MODE
                                                                                                           : ISSUNSTATE_UNDEFINED;
            // 3. check if, curr_tm_day has been updated successfully
            if ((0 == user_data->curr_tm_day) ||
                (0 == user_data->sunstate_mode) ||
                (ISSUNSTATE_UNDEFINED == user_data->tmoffs_type)) // if the curr_day or sunstate or offset is not set properly;
            {
                EZPI_core_suntime_note_failure(&(user_data->backoff), info->tm_mday, now_sod);

                /* The block is kept now, and that is the whole point: a
                 * deleted block takes the backoff state with it, so the next
                 * evaluation allocates a fresh zeroed one and retries from
                 * nothing. Deleting it is what made this unthrottleable.
                 *
                 * Keeping it is behaviour-identical because of what the readers
                 * test. Every one of them -- ISSUNSTATE_get_offs_tmval,
                 * ISSUNSTATE_eval_range and ISSUNSTATE_check_flag_result --
                 * gates on `0 != user_data->sunstate_mode`, so clearing that
                 * one field makes them all skip this scene exactly as a NULL
                 * user_arg did. It is safe to clear because it is not
                 * configuration: line ~536 above re-reads it from
                 * curr_sunstate_mode on every re-entry.
                 *
                 * moment_valid goes with it. No reader can reach it while
                 * sunstate_mode is 0, so this is not load-bearing -- but
                 * leaving a `true` next to a defined_moment computed from a
                 * suntime that has since been discarded is an invariant waiting
                 * to be broken by the next person to add a reader.
                 *
                 * (Before either of those, this used to free the block and
                 * leave the pointer in place, so the next evaluation
                 * dereferenced freed memory and scene teardown freed it a
                 * second time.)
                 *
                 * ONE CASE CHANGES MORE THAN THE RATE, AND IT IS THE WORST ONE.
                 * Of the three conditions above, two leave curr_tm_day at 0 and
                 * so stay on the ladder. The third -- tmoffs_type UNDEFINED,
                 * meaning the offset field holds something that is not
                 * "intime", "before" or "after" -- is reached with curr_tm_day
                 * already set to today, because the suntime was fetched before
                 * the offset string was looked at. The test above it is then
                 * false until midnight, so this scene goes quiet for the day
                 * rather than retrying at all.
                 *
                 * That is correct and it is the point. Retrying cannot fix a
                 * malformed offset field, and the old code retried it hardest:
                 * it deleted the block, the next evaluation allocated a fresh
                 * one with curr_tm_day back at 0, and the hub opened a TLS
                 * connection to api.sunrisesunset.io every second, indefinitely,
                 * because of a typo in a scene field. Fixing the field requires
                 * a scene edit, and EZPI_core_scenes_delete_when_blocks tears
                 * the field list down -- user_arg with it -- so the correction
                 * is picked up on the next evaluation and not at midnight. */
                user_data->sunstate_mode = 0;
                user_data->moment_valid = false;

                TRACE_E("isSunState lookup failed; next attempt in %d s (failure %d)",
                        EZPI_core_suntime_backoff_delay(user_data->backoff.backoff_step),
                        user_data->backoff.backoff_step);
                return 0;
            }
            EZPI_core_suntime_note_success(&(user_data->backoff));
            TRACE_W("update_day = [%d][%dth] , offset[%d] : intime=0,before=1,after=2,undefined=3 , SunState[%d] : sunrise=1,sunset=2", info->tm_mday, user_data->curr_tm_day, user_data->tmoffs_type, user_data->sunstate_mode);
        }
    }
    return flag_check;
}
uint8_t ISSUNSTATE_get_offs_tmval(l_scenes_list_v2_t *scene_node, l_fields_v2_t *curr_field, struct tm *info, uint8_t curr_sunstate_mode)
{
    uint8_t flag_check = 0;
    if (EZLOPI_VALUE_TYPE_HMS_INTERVAL == curr_field->value_type)
    {
        s_sunstate_data_t *user_data = (s_sunstate_data_t *)(scene_node->when_block->fields->user_arg);
        if ((user_data) && (0 != user_data->sunstate_mode))
        {
            /* Once per day, and once per failure -- not once per evaluation
            ** and not once per lifetime. See the s_sunstate_data_t comment. */
            if (user_data->moment_tm_day != user_data->curr_tm_day)
            {
                /* e_type, not value_type. value_type is the label the
                ** controller attached to the field; e_type is what
                ** EZPI_scenes_populate_fields_get_value actually put in the
                ** union, and it chooses by the JSON type it was handed. A
                ** `hms_interval` field whose value arrives as an object or an
                ** array is stored as `u_value.cj_value`, and reading
                ** `u_value.value_string` out of the same union then hands a
                ** cJSON node's address to snprintf("%s") -- the old
                ** `NULL != value_string` test passes for any of them, since
                ** every member of that union is nonzero when it is set. */
                const char *tm_offs_val = (VALUE_TYPE_STRING == curr_field->field_value.e_type)
                                              ? curr_field->field_value.u_value.value_string
                                              : NULL;

                user_data->moment_tm_day = user_data->curr_tm_day;
                user_data->moment_valid = false;

                if ((VALUE_TYPE_STRING != curr_field->field_value.e_type) &&
                    (ISSUNSTATE_INTIME_MODE != user_data->tmoffs_type))
                {
                    TRACE_E("isSunState offset field holds a %d, not a string; the offset cannot be read",
                            (int)curr_field->field_value.e_type);
                }

                user_data->moment_valid = __issunstate_add_offs(user_data->tmoffs_type,
                                                                &(user_data->choosen_suntime),
                                                                &(user_data->defined_moment),
                                                                tm_offs_val);

                TRACE_S("\r\nSunMode[%d]{sunrise=1,sunset=2,0=NULL},\r\nChoosen_suntime(hh:mm:ss = %d:%d:%d),\r\ndefined_moment(hh:mm:ss = %d:%d:%d),\r\nOffset(%s),\r\nvalid(%d)\r\n",
                        user_data->sunstate_mode,
                        user_data->choosen_suntime.tm_hour,
                        user_data->choosen_suntime.tm_min,
                        user_data->choosen_suntime.tm_sec,
                        user_data->defined_moment.tm_hour,
                        user_data->defined_moment.tm_min,
                        user_data->defined_moment.tm_sec,
                        (tm_offs_val) ? tm_offs_val : "none",
                        (int)user_data->moment_valid);
            }
        }
    }
    return flag_check;
}
uint8_t ISSUNSTATE_eval_weekdays(l_scenes_list_v2_t *scene_node, l_fields_v2_t *curr_field, struct tm *info, uint8_t curr_sunstate_mode)
{
    uint8_t flag_check = 0;
    if ((EZLOPI_VALUE_TYPE_INT_ARRAY == curr_field->value_type) && (curr_field))
    {
        flag_check |= (1 << 5);                                      // indicates : weekdays
        flag_check |= ISDATE_weekdays_check(NULL, info, curr_field); // checks condition : (1 << 1)
    }
    return flag_check;
}
uint8_t ISSUNSTATE_eval_days(l_scenes_list_v2_t *scene_node, l_fields_v2_t *curr_field, struct tm *info, uint8_t curr_sunstate_mode)
{
    uint8_t flag_check = 0;
    if ((EZLOPI_VALUE_TYPE_INT_ARRAY == curr_field->value_type) && (curr_field))
    {
        flag_check |= (1 << 6);                                   // indicates : month_days
        flag_check |= ISDATE_mdays_check(NULL, info, curr_field); // checks condition : (1 << 2)
    }
    return flag_check;
}
uint8_t ISSUNSTATE_eval_range(l_scenes_list_v2_t *scene_node, l_fields_v2_t *curr_field, struct tm *info, uint8_t curr_sunstate_mode)
{
    uint8_t flag_check = 0;
    /* `value_type` is the declared scene-schema type and says nothing about
    ** which union member is live; `field_value.e_type` is the discriminant and
    ** does. They are set from different places and nothing cross-checks them:
    ** EZPI_scenes_populate_assign_field takes value_type from the "type" string
    ** in the scene JSON, while EZPI_scenes_populate_fields_get_value sets
    ** e_type from the actual JSON type of "value". So a field declared
    ** {"type":"token","value":true} arrives here as TOKEN holding a bool.
    **
    ** A NULL test cannot catch that, which is why one was not enough. The
    ** union is zeroed at allocation and `value_bool = true` writes a single
    ** byte of it, so `value_string` reads back as 0x1 -- non-NULL -- and
    ** ISSUNSTATE_check_mdrn below calls strlen() on address 1. A number is the
    ** same shape: the low word of the double becomes the pointer.
    **
    ** Reading the discriminant is what the rest of the component already does.
    ** The serializer does it for this exact schema type: the TOKEN arm of
    ** __cjson_add_fields, ezlopi_core_scenes_cjson.c line 759, dispatches on
    ** e_type and warns rather than reading the union when it is neither a
    ** string nor a cJSON. The destructor does it too --
    ** EZPI_core_scenes_delete_field_value switches on e_type to decide whether
    ** the union holds something to free. A TOKEN holding a non-string is
    ** therefore a state two other readers already treat as legitimate; this
    ** one was the deviation.
    **
    ** No diagnostic is emitted on the mismatch arm, deliberately. This
    ** function runs about once a second, so a trace here would be the same
    ** log flood that the retry backoff above was written to stop. The sibling
    ** ISSUNSTATE_get_offs_tmval does trace its equivalent mismatch, and can,
    ** because its once-per-day `moment_tm_day != curr_tm_day` gate rate-limits
    ** it; there is no equivalent gate here. The cost is that a mistyped range
    ** field now makes the scene quietly not fire rather than crash, with
    ** nothing in the log -- recorded under Known gaps. */
    if ((EZLOPI_VALUE_TYPE_TOKEN == curr_field->value_type) &&
        (VALUE_TYPE_STRING == curr_field->field_value.e_type) &&
        (NULL != curr_field->field_value.u_value.value_string))
    {
        s_sunstate_data_t *user_data = (s_sunstate_data_t *)(scene_node->when_block->fields->user_arg);
        if ((user_data) && (0 != user_data->sunstate_mode))
        {
            // TRACE_S("checking midnight range offset");
            flag_check |= (1 << 7); // indicates : midnight-range
            /* Only a moment that was actually computed is worth comparing
            ** against. Without this the struct's own zeroes are a time --
            ** 00:00:00 -- so a scene whose offset was rejected, or whose
            ** suntime has not been read yet, matched at midnight. */
            if (user_data->moment_valid)
            {
                flag_check |= ISSUNSTATE_check_mdrn(user_data->sunstate_mode, curr_field->field_value.u_value.value_string, info, &(user_data->defined_moment));
            }
        }
    }
    return flag_check;
}
uint8_t ISSUNSTATE_check_mdrn(uint8_t sunstate_mode, const char *range_type, struct tm *info, struct tm *defined_moment)
{
    uint8_t ret = 0;
    if (sunstate_mode && range_type && info && defined_moment)
    {
        size_t len = (range_type) ? strlen(range_type) + 1 : 0;
        if (EZPI_STRNCMP_IF_EQUAL(range_type, ezlopi_at_str, len, 3)) // at desired moment
        {
            if ((info->tm_hour == defined_moment->tm_hour) &&
                (info->tm_min == defined_moment->tm_min) &&
                (info->tm_sec == defined_moment->tm_sec))
            {
                ret = (1 << 3);
            }
        }
        else if (EZPI_STRNCMP_IF_EQUAL(range_type, ezlopi_after_str, len, 6)) // all time after defined moment till midnight of current day
        {

            if ((info->tm_hour < 24) && (info->tm_hour > defined_moment->tm_hour))
            {
                ret = (1 << 3);
            }
            else if (((info->tm_hour < 24) && (info->tm_hour == defined_moment->tm_hour)) &&
                     ((info->tm_min < 60) && (info->tm_min >= defined_moment->tm_min)) &&
                     ((info->tm_sec < 60) && (info->tm_sec > defined_moment->tm_sec)))
            {
                ret = (1 << 3);
            }
        }
        else if (EZPI_STRNCMP_IF_EQUAL(range_type, ezlopi_before_str, len, 7)) // all time after midnight till defined moment;
        {
            if ((info->tm_hour >= 0) && (info->tm_hour < defined_moment->tm_hour))
            {
                ret = (1 << 3);
            }
            else if (((info->tm_hour >= 0) && (info->tm_hour == defined_moment->tm_hour)) &&
                     ((info->tm_min >= 0) && (info->tm_min <= defined_moment->tm_min)) &&
                     ((info->tm_sec >= 0) && (info->tm_sec <= defined_moment->tm_sec)))
            {
                ret = (1 << 3);
            }
        }
    }
    return ret;
}
int ISSUNSTATE_check_flag_result(l_scenes_list_v2_t *scene_node, struct tm *info, uint8_t flag_check)
{
    int ret = 0;
    /* An enum, not eight `const uint8_t` locals. A const-qualified object is
    ** not an integer constant expression in C, so the eight case labels below
    ** were not valid C: host GCC 13 rejects all eight with "case label does
    ** not reduce to an integer constant", and the only reason the firmware
    ** builds is that the xtensa GCC the IDF ships folds them as an extension.
    ** That is also the single thing that made this translation unit look
    ** un-hostable -- with the include set the sibling then-methods helper
    ** already uses, these eight errors were the *entire* cost, and no IDF
    ** header needed stubbing at all. The values are unchanged. */
    enum
    {
        TIME_FLAG = (1 << 0),
        WEEKDAYS_FLAG = (1 << 1),
        DAYS_FLAG = (1 << 2),
        MIDNIGHT_FLAG = (1 << 3),
        MASK_TIME_FLAG = (1 << 4),
        MASK_WEEKDAYS_FLAG = (1 << 5),
        MASK_DAYS_FLAG = (1 << 6),
        MASK_MIDNIGHT_FLAG = (1 << 7),
    };
    s_sunstate_data_t *user_data = (s_sunstate_data_t *)(scene_node->when_block->fields->user_arg);
    if (user_data)
    {
        /* `moment_valid` as well as `curr_tm_day`: the day says a suntime was
        ** read, which is not the same as the trigger having been worked out
        ** from it. The two come apart whenever the offset field is unreadable,
        ** and the all-zero defined_moment left behind in that case is
        ** midnight -- a time the clock reaches every night. */
        if ((0 != user_data->curr_tm_day) && (0 != user_data->sunstate_mode) && (user_data->moment_valid) && (flag_check & MASK_TIME_FLAG)) // defined_moment should have the current day
        {
            /* MASK_TIME_FLAG is consumed by the condition above and cleared
            ** here, because every case label below enumerates an exact value of
            ** `flag_check` and not one of the eight includes this bit.
            **
            ** It is always set. ISSUNSTATE_get_suntime sets it for the
            ** sunrise/sunset field, which is the field that makes a scene an
            ** isSunState scene in the first place, and the condition above
            ** refuses to go any further without it. So before this line
            ** `flag_check` always had bit 0x10 set and the eight labels are
            ** 0x88, 0x01, 0xee, 0x67, 0xcc, 0x45, 0xaa and 0x23 -- none of
            ** which can be reached. `switch (flag_check)` fell to `default`
            ** every time and this function returned 0 for every input it can
            ** be given: no isSunState condition could ever fire, with or
            ** without weekdays, month-days or a midnight range.
            **
            ** That is asserted from the arithmetic rather than from a trace on
            ** hardware, and it is checked both ways in tests/host/test_issun.c:
            ** exhaustively over all 512 (flag_check, moment-matches) pairs,
            ** against the real function, which is why this file is compiled
            ** there. Clearing the bit here leaves all eight labels meaning
            ** exactly what their trailing comments say they mean. */
            flag_check &= (uint8_t)~MASK_TIME_FLAG;

            // 1. Set 'time-flag', when defined moment is matched : [sunrise/sunset_moment + offsets (if any)]
            if ((info->tm_hour == user_data->defined_moment.tm_hour) &&
                (info->tm_min == user_data->defined_moment.tm_min) &&
                (info->tm_sec == user_data->defined_moment.tm_sec))
            {
                flag_check |= TIME_FLAG;
            }
            // 2. Identify :- Daily or Monthly+weeekdays case
            if (!(flag_check & MASK_DAYS_FLAG) && !(flag_check & MASK_WEEKDAYS_FLAG)) // without 'month_days' or 'week_days' condition
            {
                switch (flag_check)
                {
                case (MASK_MIDNIGHT_FLAG | MIDNIGHT_FLAG): // daily + midnight time?
                case (TIME_FLAG):                          // daily + exact time?
                {
                    ret = 1;
                    break;
                }
                default:
                {
                    ret = 0;
                    break;
                }
                }
            }
            else // (flag_check & MASK_DAYS_FLAG) || (flag_check & MASK_WEEKDAYS_FLAG) // month_days + week_days conditons
            {
                // Output filter for triggered in sunset/sunrise time
                switch (flag_check)
                {
                case ((MASK_DAYS_FLAG | DAYS_FLAG) | (MASK_WEEKDAYS_FLAG | WEEKDAYS_FLAG) | (MASK_MIDNIGHT_FLAG | MIDNIGHT_FLAG)): // mdays+weekday+midnight
                case ((MASK_DAYS_FLAG | DAYS_FLAG) | (MASK_WEEKDAYS_FLAG | WEEKDAYS_FLAG) | (TIME_FLAG)):                          // mdays+weekday+exact
                case ((MASK_DAYS_FLAG | DAYS_FLAG) | (MASK_MIDNIGHT_FLAG | MIDNIGHT_FLAG)):                                        // mdays+midnight
                case ((MASK_DAYS_FLAG | DAYS_FLAG) | (TIME_FLAG)):                                                                 // mdays +exact
                case ((MASK_WEEKDAYS_FLAG | WEEKDAYS_FLAG) | (MASK_MIDNIGHT_FLAG | MIDNIGHT_FLAG)):                                // weekday+midnight
                case ((MASK_WEEKDAYS_FLAG | WEEKDAYS_FLAG) | (TIME_FLAG)):                                                         // weekday+exact
                {
                    ret = 1;
                    break;
                }
                default:
                {
                    ret = 0; // don't activate if above conditions aren't satisfied
                    break;
                }
                }
            }
        }
        // TRACE_S("offset[%d](intime=0,before=1,after=2,undefined=3) , SunState[%d](sunrise=1,sunset=2) , FLAG_STATUS: %#x", (int)user_data->tmoffs_type, user_data->sunstate_mode, flag_check);
    }

    return ret;
}

void ISDATE_range_get_tm(l_fields_v2_t *curr_field, struct tm *tmp_tm)
{
    if ((EZLOPI_VALUE_TYPE_24_HOURS_TIME == curr_field->value_type) && (NULL != curr_field->field_value.u_value.value_string))
    {
        char time[10];
        snprintf(time, 10, "%s", curr_field->field_value.u_value.value_string);
        time[9] = '\0';
        char *ptr = NULL;
        if (0 != strlen(time))
        {
            tmp_tm->tm_hour = strtoul(time, &ptr, 10);
            tmp_tm->tm_min = strtoul(ptr + 1, NULL, 10);
        }
    }
}
void ISDATE_range_get_startday(l_fields_v2_t *curr_field, struct tm *tmp_tm)
{
    if (EZLOPI_VALUE_TYPE_INT == curr_field->value_type)
    {
        tmp_tm->tm_mday = (curr_field->field_value.u_value.value_double) ? (int)(curr_field->field_value.u_value.value_double) : 1;
    }
}
void ISDATE_range_get_endday(l_fields_v2_t *curr_field, struct tm *tmp_tm)
{
    if (EZLOPI_VALUE_TYPE_INT == curr_field->value_type)
    {
        tmp_tm->tm_mday = (curr_field->field_value.u_value.value_double) ? (int)(curr_field->field_value.u_value.value_double) : 31;
    }
}
void ISDATE_range_get_startmonth(l_fields_v2_t *curr_field, struct tm *tmp_tm)
{
    if (EZLOPI_VALUE_TYPE_INT == curr_field->value_type)
    {
        tmp_tm->tm_mon = (curr_field->field_value.u_value.value_double) ? (int)(curr_field->field_value.u_value.value_double) : 1;
    }
}
void ISDATE_range_get_endmonth(l_fields_v2_t *curr_field, struct tm *tmp_tm)
{
    if (EZLOPI_VALUE_TYPE_INT == curr_field->value_type)
    {
        tmp_tm->tm_mon = (curr_field->field_value.u_value.value_double) ? (int)(curr_field->field_value.u_value.value_double) : 12;
    }
}
void ISDATE_range_get_startyear(l_fields_v2_t *curr_field, struct tm *tmp_tm)
{
    if (EZLOPI_VALUE_TYPE_INT == curr_field->value_type)
    {
        tmp_tm->tm_year = (curr_field->field_value.u_value.value_double) ? (int)(curr_field->field_value.u_value.value_double) : 1;
    }
}
void ISDATE_range_get_endyear(l_fields_v2_t *curr_field, struct tm *tmp_tm)
{
    if (EZLOPI_VALUE_TYPE_INT == curr_field->value_type)
    {
        tmp_tm->tm_year = (curr_field->field_value.u_value.value_double) ? (int)(curr_field->field_value.u_value.value_double) : 2147483647;
    }
}

uint8_t ISDATE_range_check_tm(struct tm *start, struct tm *end, struct tm *info)
{
    uint8_t ret = 0;
    // first confirm if the time range has positive or negative difference (end - start)
    if ((end->tm_hour - start->tm_hour) > 0)
    {
        if ((info->tm_hour == start->tm_hour) && (info->tm_min >= start->tm_min))
        {
            ret |= (1 << 0);
            goto end;
        }
        else if ((info->tm_hour == end->tm_hour) && (info->tm_min <= end->tm_min))
        {
            ret |= (1 << 0);
            goto end;
        }
        else if ((info->tm_hour > start->tm_hour) && (info->tm_hour < end->tm_hour))
        {
            ret |= (1 << 0);
            goto end;
        }
    }
    else
    { // for inverted hours [ie. end(say): 2pm  / start(say): 5pm ; take 'mday' into consideration]
        if ((info->tm_mday >= start->tm_mday) && (info->tm_mday <= end->tm_mday))
        {
            if (((info->tm_hour >= start->tm_hour) && (info->tm_hour <= end->tm_hour)))
            {
                ret |= (1 << 0);
            }
        }
        else
        {
            TRACE_S("Invalid day-orders : start[%d] vs end[%d]", start->tm_mday, end->tm_mday);
        }
    }
end:
    return ret;
}
uint8_t ISDATE_range_check_day(struct tm *start, struct tm *end, struct tm *info)
{
    uint8_t ret = 0;
    if ((info->tm_mday >= start->tm_mday) && (info->tm_mday <= end->tm_mday))
    {
        ret |= (1 << 1);
    }
    else
    {
        TRACE_S("Invalid day-orders : start[%d] vs end[%d]", start->tm_mday, end->tm_mday);
    }
    return ret;
}
uint8_t ISDATE_range_check_month(struct tm *start, struct tm *end, struct tm *info)
{
    uint8_t ret = 0;
    if (((info->tm_mon + 1) >= start->tm_mon) && ((info->tm_mon + 1) <= end->tm_mon))
    {
        ret |= (1 << 2);
    }
    else
    {
        TRACE_S("Invalid month-orders : start[%d] vs end[%d]", start->tm_mon, end->tm_mon);
    }
    return ret;
}
uint8_t ISDATE_range_check_year(struct tm *start, struct tm *end, struct tm *info)
{
    uint8_t ret = 0;
    if (((info->tm_year + 1900) >= start->tm_year) && ((info->tm_year + 1900) <= end->tm_year))
    {
        ret |= (1 << 2);
    }
    else
    {
        TRACE_S("Invalid year-orders : start[%d] vs end[%d]", start->tm_year, end->tm_year);
    }
    return ret;
}
int ISDATE_range_check_flag_result(uint8_t flag_check)
{
    const uint8_t TIME_FLAG = (1 << 0);
    const uint8_t DAY_FLAG = (1 << 1);
    const uint8_t MONTH_FLAG = (1 << 2);
    const uint8_t YEAR_FLAG = (1 << 3);

    int ret = 0;
    if ((flag_check & TIME_FLAG) && (flag_check & DAY_FLAG) && (flag_check & MONTH_FLAG) && (flag_check & YEAR_FLAG))
    {
        ret = 1;
    }
    return ret;
}

int WHEN_function_for_opr(l_scenes_list_v2_t *scene_node, l_when_block_v2_t *when_block, cJSON *cj_func_opr)
{
    // TRACE_W("for_least");
    int ret = 0;
    if (scene_node && when_block && cj_func_opr)
    {
        cJSON *for_type = cJSON_GetObjectItem(__FUNCTION__, cj_func_opr, ezlopi_method_str);
        cJSON *for_interval = cJSON_GetObjectItem(__FUNCTION__, cj_func_opr, ezlopi_seconds_str);
        if (for_type && for_type->valuestring && for_interval)
        {
            /* first get the product of all children states*/
            bool transition_state = __and_when_block_condition(scene_node, when_block);

            /*now compare the intervals between each transtion result*/
            s_when_function_t *function_state_info = (s_when_function_t *)scene_node->when_block->fields->user_arg;

            if ((transition_state == 1) && (0 == function_state_info->current_state))
            {
                function_state_info->transtion_instant = (uint32_t)xTaskGetTickCount();
            }
            else if ((transition_state == 0) && (1 == function_state_info->current_state))
            {
                if (0 != function_state_info->transtion_instant)
                {
                    int threshold_time = for_interval->valuedouble;
                    if (EZPI_STRNCMP_IF_EQUAL(for_type->valuestring, ezlopi_less_str, for_type->str_value_len, 5))
                    {
                        if ((((uint32_t)xTaskGetTickCount() - function_state_info->transtion_instant) / 1000) < threshold_time)
                        {
                            // TRACE_S("activating-less");
                            ret = 1;
                        }
                    }
                    else if (EZPI_STRNCMP_IF_EQUAL(for_type->valuestring, ezlopi_least_str, for_type->str_value_len, 6))
                    {
                        if ((((uint32_t)xTaskGetTickCount() - function_state_info->transtion_instant) / 1000) >= threshold_time)
                        {
                            // TRACE_S("activating-atleast");
                            ret = 1;
                        }
                    }
                    // TRACE_I("end_Transtion_time[%d] , duration_time[%d] , threshold[%d]", function_state_info->transtion_instant / 1000, (uint32_t)xTaskGetTickCount() / 1000, threshold_time);
                }
            }
            function_state_info->current_state = transition_state;
        }
    }
    return ret;
}
int WHEN_function_for_repeat(l_scenes_list_v2_t *scene_node, l_when_block_v2_t *when_block, cJSON *cj_func_opr)
{
    int ret = 0;
    if (scene_node && when_block && cj_func_opr)
    {
        cJSON *for_times = cJSON_GetObjectItem(__FUNCTION__, cj_func_opr, ezlopi_times_str);
        cJSON *for_interval = cJSON_GetObjectItem(__FUNCTION__, cj_func_opr, ezlopi_seconds_str);
        if (for_times && for_interval)
        {
            /* first get the product of all children states*/
            s_when_function_t *function_state_info = (s_when_function_t *)scene_node->when_block->fields->user_arg;
            bool transition_state = __and_when_block_condition(scene_node, when_block);
            if ((transition_state) && (0 == function_state_info->current_state)) /*previous state?*/
            {
                if (0 == function_state_info->transtion_instant)
                {
                    /*store the first trigger time*/
                    // TRACE_W("Start 'repeat' activation Sequence ......!");
                    function_state_info->transtion_instant = (uint32_t)xTaskGetTickCount();
                    TRACE_W("first_trigger_time[%d] ", function_state_info->transtion_instant);
                }

                /*add the count*/
                function_state_info->transition_count++;
                // TRACE_W(" count[%d]", function_state_info->transition_count);

                /*compare with conditon*/
                int for_count = for_times->valuedouble; /*extract the type*/
                if (function_state_info->transition_count >= for_count)
                {
                    uint32_t dur = (((uint32_t)xTaskGetTickCount() - function_state_info->transtion_instant) / 1000);
                    int threshold_time = for_interval->valuedouble;
                    if (dur <= threshold_time) // from 'start' till 'now'
                    {
                        TRACE_W(" Successful activation sequence within [%dsec] ; threshold [%dsec]", dur, threshold_time);
                        ret = 1;
                    }
                    else
                    {
                        TRACE_E(" Time-Out !! consumed[%d (>%dsec)]", dur, threshold_time);
                    }
                    function_state_info->transtion_instant = 0;
                    function_state_info->transition_count = 0;
                }
            }
            function_state_info->current_state = transition_state;
        }
    }

    return ret;
}
int WHEN_function_for_follow(l_scenes_list_v2_t *scene_node, l_when_block_v2_t *when_block, cJSON *cj_func_opr)
{
    int ret = 0;
    if (scene_node && when_block && cj_func_opr)
    {
        cJSON *for_delay = cJSON_GetObjectItem(__FUNCTION__, cj_func_opr, "delayReset");
        if (for_delay)
        {
            s_when_function_t *function_state_info = (s_when_function_t *)scene_node->when_block->fields->user_arg;
            if ((0 == function_state_info->transtion_instant) && (0 == function_state_info->current_state))
            {
                /* first get the product of all children states*/
                bool transition_state = __and_when_block_condition(scene_node, when_block);
                if (transition_state) /*previous state?*/
                {
                    /* trigger phase ---> started */
                    function_state_info->current_state = transition_state;
                    function_state_info->transtion_instant = (uint32_t)xTaskGetTickCount();
                    // TRACE_W("start_follow_time");
                    ret = 1;
                }
            }
            else
            {
                int threshold_time = for_delay->valuedouble;
                if ((((uint32_t)xTaskGetTickCount() - function_state_info->transtion_instant) / 1000) <= threshold_time)
                {
                    ret = 1;
                    // TRACE_S(" Follow -> Active ");
                }
                else
                {
                    /* trigger phase ---> stop */
                    ret = 0;
                    TRACE_W("restart the trigger...");

                    /* now refreshing then block*/
                    if (false == __and_when_block_condition(scene_node, when_block))
                    {
                        function_state_info->transtion_instant = 0;
                        function_state_info->current_state = 0;
                    }
                }
            }
        }
    }

    return ret;
}
int WHEN_function_for_pulse(l_scenes_list_v2_t *scene_node, l_when_block_v2_t *when_block, cJSON *cj_func_opr)
{
    int ret = 0;
    if (scene_node && when_block && cj_func_opr)
    {
        cJSON *for_trueperiod = cJSON_GetObjectItem(__FUNCTION__, cj_func_opr, "truePeriod");
        cJSON *for_falseperiod = cJSON_GetObjectItem(__FUNCTION__, cj_func_opr, "falsePeriod");
        cJSON *for_times = cJSON_GetObjectItem(__FUNCTION__, cj_func_opr, ezlopi_times_str);
        if (for_trueperiod && for_times && for_falseperiod)
        {
            int true_time_dur = for_trueperiod->valuedouble;
            int false_time_dur = for_falseperiod->valuedouble;
            int seq_count = for_times->valuedouble;

            /*1. check for activation condition for 'pulse_flag' */
            s_when_function_t *function_state_info = (s_when_function_t *)scene_node->when_block->fields->user_arg;
            if ((0 == function_state_info->transition_count) &&
                (0 == function_state_info->current_state) &&
                (0 == function_state_info->transtion_instant) &&
                (false == function_state_info->activate_pulse_seq))
            {
                if (__and_when_block_condition(scene_node, when_block))
                {
                    TRACE_W("starting Pulse!");
                    function_state_info->activate_pulse_seq = true;
                    function_state_info->current_state = 1; // state you want to start with
                    function_state_info->transtion_instant = (uint32_t)xTaskGetTickCount();
                }
            }
            /*2. Activate pulse_sequence*/
            if (true == function_state_info->activate_pulse_seq)
            {
                if (seq_count == 0)
                {
                    function_state_info->transition_count = seq_count; /* If 'SEQ_COUNT' == 0 ; Then loop the sequence forever. */
                }
                else
                {
                    seq_count = (seq_count * 2) - 1; // 6 -> [0-5]
                }

                if (function_state_info->transition_count <= seq_count)
                {
                    uint32_t dur = (((uint32_t)xTaskGetTickCount() - function_state_info->transtion_instant) / 1000);
                    if (true == function_state_info->current_state)
                    {
                        if (dur <= true_time_dur)
                        {
                            TRACE_S("high_pulse , true[%dsec]", dur);
                        }
                        else
                        {
                            function_state_info->transtion_instant = (uint32_t)xTaskGetTickCount(); /*in sec*/
                            if (function_state_info->transition_count < seq_count)
                            {
                                function_state_info->transition_count++;
                            }
                            function_state_info->current_state = 0;
                        }
                    }
                    else if (false == function_state_info->current_state)
                    {
                        if (dur <= false_time_dur)
                        {
                            TRACE_S("low_pulse , false[%dsec]", dur);
                        }
                        else
                        {
                            function_state_info->transtion_instant = (uint32_t)xTaskGetTickCount(); /*in sec*/
                            if (function_state_info->transition_count < seq_count)
                            {
                                function_state_info->current_state = 1;
                                function_state_info->transition_count++;
                            }
                            else
                            {
                                function_state_info->current_state = 0;
                            }
                        }
                    }
                }
                else
                {
                    TRACE_E("Exceeded pulse count..............");
                    function_state_info->activate_pulse_seq = false;
                    function_state_info->current_state = 0;
                    function_state_info->transition_count = 0;
                }
            }
            else
            {
                /* trigger phase ---> stop */
                TRACE_W("restart the trigger....");

                /* now refreshing pulse*/
                if (false == __and_when_block_condition(scene_node, when_block))
                {
                    function_state_info->transtion_instant = 0;
                    function_state_info->transition_count = 0;
                }
                function_state_info->current_state = 0;
            }

            TRACE_W("trigger_state= %s , {seq_count: %d}.-----> return =>> [pulse_state = %s]",
                    (function_state_info->activate_pulse_seq) ? ezlopi_true_str : ezlopi_false_str,
                    function_state_info->transition_count,
                    (function_state_info->current_state) ? "high" : "low");
            ret = function_state_info->current_state;
        }
    }

    return ret;
}
int WHEN_function_for_latch(l_scenes_list_v2_t *scene_node, l_when_block_v2_t *when_block, cJSON *cj_func_opr)
{
    int ret = 0;
    if (scene_node && when_block && cj_func_opr)
    {
        cJSON *cj_for_enabled = cJSON_GetObjectItem(__FUNCTION__, cj_func_opr, ezlopi_enabled_str);
        if (cj_for_enabled)
        {
            s_when_function_t *function_state_info = (s_when_function_t *)scene_node->when_block->fields->user_arg;
            if ((0 == function_state_info->current_state) && (cJSON_True == cj_for_enabled->type) && (0 == function_state_info->transtion_instant)) /* if the trigger phase has not started */
            {
                /* first get the product of all children states*/
                if (__and_when_block_condition(scene_node, when_block)) /*if : previous state = 0*/
                {
                    function_state_info->current_state = 1;
                    function_state_info->transtion_instant = (uint32_t)xTaskGetTickCount();
                    TRACE_S("start_latched_time[%d]", (uint32_t)xTaskGetTickCount());
                }
            }
            else if (cJSON_False == cj_for_enabled->type)
            {
                function_state_info->current_state = 0;
                function_state_info->transtion_instant = 0;
                TRACE_S("reset_latched_time[%d]", (uint32_t)xTaskGetTickCount());
            }
            ret = function_state_info->current_state;
            TRACE_W("ret-> current_state = [%d] , active_instant[%d]", function_state_info->current_state, function_state_info->transtion_instant);
        }
    }

    return ret;
}

int IS_deviceitem_group_value_check(l_scenes_list_v2_t *scene_node, uint32_t device_group_id, uint32_t item_group_id)
{
    int ret = 0;
    if (scene_node && (0 < device_group_id) && (0 < item_group_id))
    {
        cJSON *cj_device_items_data = NULL;
        bool save_flag = false;
        if (NULL == scene_node->when_block->fields->user_arg)
        {
            /* This assigned to an inner declaration that shadowed the outer
             * one, so the object was leaked the moment the block ended and the
             * outer pointer stayed NULL -- which made every value comparison
             * below unreachable and the whole function a no-op. */
            cj_device_items_data = cJSON_CreateObject(__FUNCTION__);
            if (cj_device_items_data)
            {
                save_flag = true;
            }
        }
        else
        { // extract the previous cjson_data, if exists;
            save_flag = false;
            cj_device_items_data = (cJSON *)scene_node->when_block->fields->user_arg;
        }

        // 1.
        l_ezlopi_device_grp_t *curr_devgrp = EZPI_core_device_group_get_by_id(device_group_id);
        if (curr_devgrp)
        {
            cJSON *cj_get_devarr = NULL;
            cJSON_ArrayForEach(cj_get_devarr, curr_devgrp->devices)
            {
                if (false == cJSON_IsString(cj_get_devarr))
                {
                    continue;
                }

                uint32_t curr_devce_id = strtoul(cj_get_devarr->valuestring, NULL, 16);
                l_ezlopi_device_t *curr_device = EZPI_core_device_get_by_id(curr_devce_id); // immediately goto "102ec000" ...
                if (curr_device)
                {
                    l_ezlopi_item_t *curr_item_node = curr_device->items; // perform operation on items of above device --> "102ec000"
                    while (curr_item_node)
                    {
                        // 2.
                        l_ezlopi_item_grp_t *curr_item_grp = EZPI_core_item_group_get_by_id(item_group_id); // get  "ll_itemgrp_node"
                        if (curr_item_grp)
                        {
                            cJSON *cj_item_names = NULL;
                            cJSON_ArrayForEach(cj_item_names, curr_item_grp->item_names)
                            {
                                if (false == cJSON_IsString(cj_item_names))
                                {
                                    continue;
                                }

                                uint32_t req_item_id_from_itemgrp = strtoul(cj_item_names->valuestring, NULL, 16);
                                // if the item_ids match ; Then compare the "item_values" with that of the "scene's" requirement
                                if (req_item_id_from_itemgrp == curr_item_node->cloud_properties.item_id) // 202ec000 == 202ec000 , ...
                                {
                                    if (cj_device_items_data) // if destination arr
                                    {
                                        cJSON *cj_tmp_value = cJSON_CreateObject(__FUNCTION__);
                                        if (cj_tmp_value)
                                        {
                                            curr_item_node->func(EZLOPI_ACTION_GET_EZLOPI_VALUE, curr_item_node, (void *)cj_tmp_value, NULL);
                                            cJSON *cj_value_new = cJSON_GetObjectItem(__FUNCTION__, cj_tmp_value, ezlopi_value_str); // this 'cj_value_new' contains latest 'item_value'.
                                            if (cj_value_new)
                                            {
                                                // save or compare?
                                                if (true == save_flag) /*save*/
                                                {
                                                    // since 'save_flag' is true we will add the latest 'item_values' into user_arg.
                                                    switch (cj_value_new->type)
                                                    {
                                                    case cJSON_True:
                                                    {
                                                        cJSON_AddBoolToObject(__FUNCTION__, cj_device_items_data, cj_item_names->valuestring, true); // store as {"202ec000" : true , ...}
                                                        break;
                                                    }
                                                    case cJSON_False:
                                                    {
                                                        cJSON_AddBoolToObject(__FUNCTION__, cj_device_items_data, cj_item_names->valuestring, false); // store as {"202ec000" : false , ...}
                                                        break;
                                                    }
                                                    case cJSON_Number:
                                                    {
                                                        cJSON_AddNumberToObject(__FUNCTION__, cj_device_items_data, cj_item_names->valuestring, cj_value_new->valuedouble); // store as {"202ec000" : 20 , ...}
                                                        break;
                                                    }
                                                    case cJSON_String:
                                                    {
                                                        cJSON_AddStringToObject(__FUNCTION__, cj_device_items_data, cj_item_names->valuestring, cj_value_new->valuestring); // store as {"202ec000" : "text" , ...}
                                                        break;
                                                    }
                                                    case cJSON_Array:
                                                    case cJSON_Object:
                                                    {
                                                        cJSON_AddItemToObject(__FUNCTION__, cj_device_items_data, cj_item_names->valuestring, cJSON_Duplicate(__FUNCTION__, cj_value_new, true)); // store as {"202ec000" : [{...},{...} ] , ...}
                                                        break;
                                                    }
                                                    default:
                                                        TRACE_E("cjson Value type mis-matched!");
                                                        break;
                                                    }
                                                }
                                                else /*compare*/
                                                {
                                                    cJSON *cj_prev_item_val = cJSON_GetObjectItem(__FUNCTION__, cj_device_items_data, cj_item_names->valuestring); // grab the specific {"202ec000" :"..."}
                                                    if (cj_prev_item_val)
                                                    {
                                                        if (cj_prev_item_val->type == cj_value_new->type) // making sure both prev & new values, are of same type.
                                                        {
                                                            // since 'save_flag' is true we will add the latest 'item_values' into user_arg.
                                                            switch (cj_value_new->type)
                                                            {
                                                            case cJSON_True:
                                                            case cJSON_False:
                                                            {
                                                                ret = (cj_prev_item_val->type != cj_value_new->type) ? 1 : 0;
                                                                break;
                                                            }
                                                            case cJSON_Number:
                                                            {
                                                                ret = (cj_prev_item_val->valuedouble != cj_value_new->valuedouble) ? 1 : 0;
                                                                break;
                                                            }
                                                            case cJSON_String:
                                                            {
                                                                ret = EZPI_STRNCMP_IF_EQUAL(cj_prev_item_val->valuestring, cj_value_new->valuestring, cj_prev_item_val->str_value_len, cj_value_new->str_value_len);
                                                                break;
                                                            }
                                                            case cJSON_Array:
                                                            case cJSON_Object:
                                                            {
                                                                ret = cJSON_Compare(cj_prev_item_val, cj_value_new, false);
                                                                break;
                                                            }
                                                            default:
                                                                TRACE_E("cj_value_new --> type mis-matched!");
                                                                break;
                                                            }
                                                        }
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
                                                        else
                                                        {
                                                            TRACE_E("The old-value-type and new-value-type doesnot match");
                                                        }
#endif
                                                    }

#ifdef CONFIG_EZPI_UTIL_TRACE_EN
                                                    else
                                                    {
                                                        TRACE_E(" [%s] doesnot exist", cj_item_names->valuestring);
                                                    }
#endif
                                                }
                                            }
                                            cJSON_Delete(__FUNCTION__, cj_tmp_value);
                                        }
                                    }

                                    break;
                                }
                            }
                        }
                        curr_item_node = curr_item_node->next;
                    }
                }
            }
        }

        // store into user_arg 'save_flag == 1'
        if ((NULL == scene_node->when_block->fields->user_arg) && cj_device_items_data && (true == save_flag))
        {
#ifdef CONFIG_EZPI_UTIL_TRACE_EN
            CJSON_TRACE("first_device_item_data", cj_device_items_data);
#endif
            scene_node->when_block->fields->user_arg = (void *)cj_device_items_data;
            scene_node->when_block->fields->user_arg_type = FIELD_USER_ARG_CJSON;
            ret = 1;
        }
        else if (cj_device_items_data && (true == save_flag))
        {
            /* Built this pass but not adopted, so this function still owns it. */
            cJSON_Delete(__FUNCTION__, cj_device_items_data);
        }
    }
    return ret;
}
int IS_itemstate_changed(s_item_exp_data_t *new_extract_data, l_fields_v2_t *start_field, l_fields_v2_t *finish_field, l_scenes_list_v2_t *scene_node)
{
    int ret = 0;
    s_item_exp_data_t *prev_extract_data = (s_item_exp_data_t *)(scene_node->when_block->fields->user_arg);
    if (new_extract_data) // new vs old data
    {
        uint32_t flag = 0;
        // ---> New flag= (000) : ---> 1. compare 'new_extract_data' with 'start_field  / ANY'  ---> if true ; start_flag =1 ; ---> store data
        // --> 1.a . if start_field = Any ; start_flag =1 & return 'true' |  Else compare
        // -----> For flag= (001): ------> 2. compare 'new_extract_data' with 'finish_field / ANY'  -----> if true ; flag = (011) ; ----> store data ;  flag = (111) : ---->
        // ------> 2.a . if finish = Any ; start_flag =1 & return 'true' |  Else compare
        // --------> For flag = (111) : --------> 3. if(start_field & final_field == NULL or 'ANY') ? exit ; else, flag = (000).

        if (NULL == prev_extract_data)
        {
            flag |= __isitemState_vs_field_compare(new_extract_data, start_field, 0);
            // TRACE_D("First-time check.... flag=%08x", flag);
        }
        else
        {
            flag = prev_extract_data->status;
            // TRACE_I("# Before comparision : status_flag=%08x", flag);
            switch (flag) // this says what to do next
            {
            case 0: // no condition match   ---> check the start condition only.
            {
                // TRACE_D("checking start conditon");
                flag |= __isitemState_vs_field_compare(prev_extract_data, start_field, 0);
                break;
            }
            case (BIT0): // Only start condition activated ; ----> so check for finish condition
            {
                // TRACE_D("checking finish conditon");
                flag |= __isitemState_vs_field_compare(new_extract_data, finish_field, 1);

                if (flag == (BIT1 | BIT0))
                {
                    // activate BIT2 in status ; if both start and finish condition are satisfied.
                    flag |= BIT2;
                }
                break;
            }
            case (BIT1 | BIT0): // condition match but not activated [or reset] ---> need to active here
            {
                // TRACE_I("====>  pre-state : (011) --> activating");
                flag |= BIT2;
                break;
            }
            case (BIT2 | BIT1 | BIT0): // all condition matched and activated  ---> check if state have changed --> if changed , reset activation flag
            {
#warning "need to optimize these switch-case";
                // comparing 'start'
                if (NULL != start_field)
                {
                    if (!(BIT0 & __isitemState_vs_field_compare(prev_extract_data, start_field, 0))) // if BIT0 = 0;
                    {
                        flag &= (0 << 2);
                        // TRACE_I(" HERE :- Reseting the 'BIT2' ");
                    }
                    else
                    {
                        // TRACE_D("start or finish condition is still satisfied.... cannot reset 'BIT2'");
                    }
                }
                else // 'ANY'
                {
                    if (!____old_vs_new_extract_data(new_extract_data, prev_extract_data))
                    {
                        flag &= (0 << 2);
                        // TRACE_I(" HERE :- Reseting the 'BIT2' ");
                    }
                    else
                    {
                        // TRACE_D(" new-data == old-data");
                    }
                }

                // comparing 'finish'
                if (NULL != finish_field)
                {
                    if (!(BIT1 & __isitemState_vs_field_compare(new_extract_data, finish_field, 1))) // if BIT0 = 0;
                    {
                        flag &= (0 << 2);
                        // TRACE_I(" HERE :- Reseting the 'BIT2' ");
                    }
                    else
                    {
                        // TRACE_D("start or finish condition is still satisfied.... cannot reset 'BIT2'");
                    }
                }
                else // 'ANY'
                {
                    if (!____old_vs_new_extract_data(new_extract_data, prev_extract_data))
                    {
                        flag &= (0 << 2);
                        // TRACE_I(" HERE :- Reseting the 'BIT2' ");
                    }
                    else
                    {
                        // TRACE_D(" new-data == old-data");
                    }
                }
                break;
            }
            default: // all remaining conditon are invalid ; So 'Reset' all flags and start again in next iteration.
            {
                // TRACE_D("Reseting.... to start fresh");
                // flag = 0;
                break;
            }
            }
        }

        // assign latest flag to new-structure.
        new_extract_data->status = flag;

        // Store the 'new-extracted' data
        if (NULL == scene_node->when_block->fields->user_arg)
        {
            // TRACE_S("---> Creating new structure");
            s_item_exp_data_t *tmp_struct = ezlopi_malloc(__FUNCTION__, sizeof(s_item_exp_data_t));
            if (tmp_struct)
            {
                memset(tmp_struct, 0, sizeof(s_item_exp_data_t));
                __replace_old_with_new_data_val(new_extract_data, tmp_struct);
                scene_node->when_block->fields->user_arg = (void *)tmp_struct;
                /* ITEM_EXP rather than HEAP: when the remembered value is a
                 * string this struct owns a copy of it, so releasing the outer
                 * block alone would leak the copy. */
                scene_node->when_block->fields->user_arg_type = FIELD_USER_ARG_ITEM_EXP;
            }
            else
            {
                TRACE_E("Malloc failed!!");
            }
        }
        else
        {
            // TRACE_W("---> Replacing old extract data");
            __replace_old_with_new_data_val(new_extract_data, (scene_node->when_block->fields->user_arg));
        }

        // TRACE_W("#### Final..... flag result=%08x  ####", flag);
        if (flag & BIT2)
        {
            // Also return 1 for 'then-method'
            // TRACE_I("====> Return 1");
            ret = 1;
        }
    }
    else
    {
        TRACE_E("new_extract_data == NULL");
    }
    return ret;
}

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/
static int __compare_item_values(l_ezlopi_item_t *curr_item, l_fields_v2_t *value_field)
{
    int ret = 0;
    cJSON *cj_tmp_value = cJSON_CreateObject(__FUNCTION__);
    if (cj_tmp_value)
    {
        curr_item->func(EZLOPI_ACTION_GET_EZLOPI_VALUE, curr_item, (void *)cj_tmp_value, NULL);
        cJSON *cj_value = cJSON_GetObjectItem(__FUNCTION__, cj_tmp_value, ezlopi_value_str);
        if (cj_value)
        {
            switch (cj_value->type)
            {
            case cJSON_True:
            {
                if (true == value_field->field_value.u_value.value_bool)
                {
                    ret = 1;
                }
                break;
            }
            case cJSON_False:
            {
                if (false == value_field->field_value.u_value.value_bool)
                {
                    ret = 1;
                }
                break;
            }
            case cJSON_Number:
            {
                if (cj_value->valuedouble == value_field->field_value.u_value.value_double)
                {
                    ret = 1;
                }
                break;
            }
            case cJSON_String:
            {
                if (EZPI_STRNCMP_IF_EQUAL(cj_value->valuestring, value_field->field_value.u_value.value_string, cj_value->str_value_len, strlen(value_field->field_value.u_value.value_string) + 1))
                {
                    ret = 1;
                }
                break;
            }
            default:
            {
                TRACE_E("Value type mis-matched!");
            }
            }
        }

        cJSON_Delete(__FUNCTION__, cj_tmp_value);
    }

    return ret;
}
static uint8_t __isdate_check_endweek_conditon(e_isdate_modes_t mode_type, struct tm *info)
{
    uint8_t ret = 0;
    static uint8_t _last_day_of_curr_month = 0;
    static int _starting_date_of_last_week = -1;
    // filter out the first 22 days ; which do-not lie in last week of the month
    if (info->tm_mday > 22)
    {
        // 1. Find out the 'valid' starting date of last week ( return -1 , if invalid )
        if (-1 == _starting_date_of_last_week)
        {
            //-------------------------------------------------------------------
            // 1.1 find the nearest-prev sunday and assign it.
            // for this calculation dont change default 'info->tm_wday' -> '0-6'
            switch (info->tm_wday)
            {
            case 1: // monday
                _starting_date_of_last_week = info->tm_mday - 1;
                break;
            case 2: // tuesday
                _starting_date_of_last_week = info->tm_mday - 2;
                break;
            case 3: // wednasday
                _starting_date_of_last_week = info->tm_mday - 3;
                break;
            case 4: // thursday
                _starting_date_of_last_week = info->tm_mday - 4;
                break;
            case 5: // friday
                _starting_date_of_last_week = info->tm_mday - 5;
                break;
            case 6: // saturday
                _starting_date_of_last_week = info->tm_mday - 6;
                break;
            default:
                _starting_date_of_last_week = info->tm_mday;
                break;
            }
            TRACE_I("sunday :%dth_Day ", _starting_date_of_last_week);
            //-------------------------------------------------------------------
            // 1.2 validate the generated sunday number
            switch (info->tm_mon)
            {
            case 1: // FEB
            {
                int year = info->tm_year + 1900;
                if ((0 == (year % 4)) && ((0 == (year % 400)) || (0 != (year % 100))))
                {
                    _starting_date_of_last_week = ((29 - _starting_date_of_last_week) < 7) ? _starting_date_of_last_week : -1; // if this sunday's day_num lies within last 7-days of the month ; return this sunday's date
                    _last_day_of_curr_month = 29;
                }
                else // non-leap years
                {
                    _starting_date_of_last_week = ((28 - _starting_date_of_last_week) < 7) ? _starting_date_of_last_week : -1; // if this sunday's day_num lies within last 7-days of the month ; return this sunday's date
                    _last_day_of_curr_month = 28;
                }

                break;
            }
            case 3:  // APR
            case 5:  // JUN
            case 8:  // SEP
            case 10: // NOV
            {
                _starting_date_of_last_week = ((30 - _starting_date_of_last_week) < 7) ? _starting_date_of_last_week : -1;
                _last_day_of_curr_month = 30;
                break;
            }
            default: // JAN , MAR , MAY , JULY , AUG , OCT , DEC
            {
                _starting_date_of_last_week = ((31 - _starting_date_of_last_week) < 7) ? _starting_date_of_last_week : -1;
                _last_day_of_curr_month = 31;
                break;
            }
            }
        }

        // 2. Now to compare today's date and produce result ( if not -1 )
        if (-1 != _starting_date_of_last_week)
        {
            // TRACE_S(" Last week of [%d] starts from [%d] :", info->tm_mon, _starting_date_of_last_week);
            if (ISDATE_YEAR_WEEKS_MODE == mode_type) // last week of the year
            {
                if ((11 == info->tm_mon) && (info->tm_mday >= _starting_date_of_last_week))
                {
                    if (info->tm_mday >= _last_day_of_curr_month)
                    { // refresh the calculation flag '_starting_date_of_last_week'
                        _starting_date_of_last_week = -1;
                    }
                    ret = (1 << 3);
                }
            }
            else if (ISDATE_WEEKS_MODE == mode_type) // last week of the month
            {
                if (info->tm_mday >= _starting_date_of_last_week)
                {
                    if (info->tm_mday >= _last_day_of_curr_month)
                    { // refresh the calculation flag '_starting_date_of_last_week'
                        _starting_date_of_last_week = -1;
                    }
                    ret = (1 << 3);
                }
            }
        }
    }
    return ret;
}
static uint8_t __isdate_find_nth_week_curr_month(struct tm *info)
{
    // 2. find the fisrt day in this month
    uint8_t tmp_week_num = 1; // starts with 1 ; since are already in one of the week-count
    // for this calculation dont change default 'info->tm_wday' -> '0-6'
    int tmp_weekday_of_curr_month = (info->tm_wday); // 0-6 ; sun = 0
    for (uint8_t i = (info->tm_mday); i > 1; i--)    // total_days_in_curr_month - 1
    {
        if (1 == tmp_weekday_of_curr_month) // if 'monday' ; add count
        {
            tmp_week_num++;
        }
        tmp_weekday_of_curr_month--;
        if (0 > tmp_weekday_of_curr_month)
        {
            tmp_weekday_of_curr_month = 6; // sunday -> saturday
        }
    }
    // TRACE_S("First day in current month = %d", tmp_weekday_of_curr_month);
    // TRACE_S("[1-7] : %dth_Day  lies in week[%dth] of the current month", (info->tm_wday), tmp_week_num);
    return tmp_week_num;
}
/**
 * Fetch today's sunrise or sunset and store it in `user_data`.
 *
 * WHAT THIS USED TO DO
 *
 * It read the stored coordinates, traced them, and then ignored them: both
 * branches of the `if (lat_long_vals)` wrote the same hardcoded Kathmandu URL,
 * so the only effect of having coordinates was one extra trace line. It wrote
 * that URL into `.url`, which an uncredentialed GET never reads -- the request
 * line comes from `.target_page` -- so every request went out as "GET /". It
 * then discarded whatever came back without freeing it, assigned
 * 05:48:42 (or 17:48:42 for a sunset), and tested that literal for being
 * all-zero, which it cannot be, leaving the failure branch below it
 * unreachable. The result was an isSunState scene that fired at 05:48 or 17:48
 * local time anywhere on earth, and leaked a reply per day while doing it.
 *
 * WHAT IT DOES NOW
 *
 * The three parts that can be got wrong -- the query, the reply shape and the
 * clock arithmetic -- are in ezlopi-core-scenes/ezlopi_core_scenes_suntime.c
 * with 109 host-tested assertions behind them, and the framing of the reply is
 * removed by EZPI_core_http_body_in_place. What is left here is the part that
 * genuinely needs a radio, which is why it is still here and still untested.
 *
 * `curr_tm_day` is cleared first and set only once a time has actually been
 * parsed, so every way out of this function that does not produce one leaves
 * the caller's "no data for today" signal set. The caller -- ISSUNSTATE_get_suntime
 * -- re-enters here whenever `curr_tm_day` differs from today, so a failed
 * lookup is retried rather than remembered.
 *
 * How often it is retried is not decided here. The caller asks
 * EZPI_core_suntime_should_attempt first and records the outcome either way, so
 * a persistent failure walks a 60 s to 32 min ladder instead of being re-asked
 * on every one-second evaluation. This function is free to fail as often as it
 * needs to; what it must keep doing is clearing `curr_tm_day` on entry and
 * setting it only once a time has actually been parsed, because that is the
 * signal the caller reads to tell a failure from a success.
 */
static void __issunsate_update_sunstate_tm(int tm_mday, s_sunstate_data_t *user_data)
{
    if ((0 == tm_mday) || (NULL == user_data))
    {
        return;
    }

    user_data->curr_tm_day = 0;

    char *lat_long_vals = EZPI_core_nvs_read_latidtude_longitude();
    if (NULL == lat_long_vals)
    {
        TRACE_E("no coordinates stored; set them with hub.coordinates.set before using a sunrise/sunset scene");
        return;
    }

    /* 64 bytes against a longest possible output of 36 -- "json?lat=-90.000000&lng=-180.000000"
    ** and its terminator -- which is bounded by the %.6f in build_query. The
    ** truncation check in there is what makes that a bound and not a hope. */
    char tmp_target[64] = {0};
    const ezlopi_error_t query_err = EZPI_core_suntime_build_query(tmp_target, sizeof(tmp_target), lat_long_vals);

    ezlopi_free(__FUNCTION__, lat_long_vals);

    if (EZPI_SUCCESS != query_err)
    {
        /* build_query has already traced which of the three things was wrong:
        ** unparseable JSON, absent coordinates, or coordinates out of range.
        ** No default location is substituted -- sunrise at a place the user
        ** never chose is a plausible-looking wrong answer, and those are the
        ** hardest kind to notice. */
        return;
    }

    /* "Connection: close", which this used to send as "keep-alive". The read
    ** loop in __ezlopi_http_req_via_mbedTLS ends on a close-notify or a
    ** zero-length read, and no timeout is configured anywhere on that socket
    ** -- mbedtls_ssl_conf_read_timeout is never called in this tree -- so a
    ** server honouring keep-alive leaves mbedtls_ssl_read blocked on a
    ** connection it is deliberately holding open, with the scene-evaluation
    ** task inside it. That cost nothing while the reply was being thrown away.
    ** It is a hang as soon as anything waits for the body. */
    char tmp_headers[] = "Host: api.sunrisesunset.io\r\nAccept: */*\r\nConnection: close\r\n";
    char tmp_web_server[] = "api.sunrisesunset.io";

    s_ezlopi_core_http_mbedtls_t tmp_config = {
        .method = HTTP_METHOD_GET,
        /* target_page, not url: for a GET with no credentials
        ** __ezlopi_http_generate_request writes "GET /%s" from target_page and
        ** never looks at url. The path was the one thing the old code built
        ** and the one thing it did not send. */
        .target_page = tmp_target,
        .targe_page_maxlen = (uint16_t)sizeof(tmp_target),
        /* Not a url -- there is none -- but url_maxlen is also the term that
        ** sizes the request buffer in EZPI_core_http_mbedtls_req, and the path
        ** has to be accounted for there. */
        .url_maxlen = (uint16_t)sizeof(tmp_target),
        .web_port = 443,
        .header = tmp_headers,
        .header_maxlen = sizeof(tmp_headers),
        .web_server = tmp_web_server,
        .web_server_maxlen = sizeof(tmp_web_server),
        .response = NULL,
        .response_maxlen = 0,
    };

    EZPI_core_http_mbedtls_req(&tmp_config);

    if (NULL == tmp_config.response)
    {
        TRACE_E("no reply from %s for '%s'", tmp_web_server, tmp_target);
        return;
    }

    char *body = NULL;
    uint32_t body_len = 0;

    if (EZPI_SUCCESS == EZPI_core_http_body_in_place(tmp_config.response, &body, &body_len))
    {
        int hour = 0;
        int min = 0;
        int sec = 0;

        if (EZPI_SUCCESS == EZPI_core_suntime_from_reply(body, user_data->sunstate_mode, &hour, &min, &sec))
        {
            user_data->choosen_suntime.tm_hour = hour;
            user_data->choosen_suntime.tm_min = min;
            user_data->choosen_suntime.tm_sec = sec;

            /* Last, so that the day is recorded only for a time that was
            ** actually read. */
            user_data->curr_tm_day = tm_mday;

            TRACE_S("%s today is %02d:%02d:%02d",
                    (EZPI_SUNTIME_MODE_SUNSET == user_data->sunstate_mode) ? "sunset" : "sunrise",
                    hour, min, sec);
        }
    }

    /* The reply is allocated inside __ezlopi_http_req_via_mbedTLS and handed
    ** out through config->response; nothing in the HTTP component frees it, so
    ** it belongs to whoever called the request. Nothing freed it here, so each
    ** isSunState scene leaked one reply per day.
    **
    ** The leak also gets bigger with this change, for a reason worth naming:
    ** the old code sent "GET /", which api.sunrisesunset.io answers with a
    ** short error page. Now that the path is actually sent, the reply is a
    ** real one. So fixing the request without fixing the free would have made
    ** the leak worse than it was. */
    ezlopi_free(__FUNCTION__, tmp_config.response);

    if (0 == user_data->curr_tm_day)
    {
        TRACE_E("could not read today's %s time; this scene will not fire on it today",
                (EZPI_SUNTIME_MODE_SUNSET == user_data->sunstate_mode) ? "sunset" : "sunrise");
    }
}
static bool __issunstate_add_offs(e_issunstate_offset_t tmoffs_type, struct tm *choosen_suntime, struct tm *defined_moment, const char *tm_offs_val)
{
    if ((NULL == choosen_suntime) || (NULL == defined_moment))
    {
        return false;
    }

    int offs_sec = 0;

    /* INTIME wants the suntime itself, and the old switch did not look at the
    ** parsed offset in that arm either -- so an absent or unreadable offset is
    ** not a reason to refuse here. It is for the other two, where the offset
    ** is the whole instruction. */
    if (ISSUNSTATE_INTIME_MODE != tmoffs_type)
    {
        if (NULL == tm_offs_val)
        {
            TRACE_E("isSunState wants an offset for %s mode and the field has none",
                    (ISSUNSTATE_BEFORE_MODE == tmoffs_type) ? "before" : "after");
            return false;
        }

        if (EZPI_SUCCESS != EZPI_core_suntime_parse_offset(tm_offs_val, &offs_sec))
        {
            /* Refusing is the point. The shipped parser accepted anything and
            ** produced a number from it -- "99:99:99" became a trigger at
            ** 81:39:39, and "-1" an hour's shift the other way -- and because
            ** the moment is matched against the wall clock by equality, every
            ** one of those was a scene that sat in the list looking configured
            ** and could never fire. A trace and no trigger is the same outcome
            ** the user gets, with a reason attached. */
            TRACE_E("isSunState offset '%s' is not ss, mm:ss or h:mm:ss", tm_offs_val);
            return false;
        }
    }

    switch (tmoffs_type)
    {
    case ISSUNSTATE_INTIME_MODE:
    {
        offs_sec = 0;
        break;
    }
    case ISSUNSTATE_BEFORE_MODE:
    {
        offs_sec = -offs_sec;
        break;
    }
    case ISSUNSTATE_AFTER_MODE:
    {
        break;
    }
    default:
    {
        /* Not reachable from ISSUNSTATE_get_offs_tmval, which only runs once
        ** sunstate_mode is set, and ISSUNSTATE_get_suntime releases the whole
        ** user_arg rather than leaving an UNDEFINED type behind. Refused
        ** anyway, because the alternative is applying some other mode's
        ** meaning to the user's offset. */
        TRACE_E("isSunState offset type %d is not one of intime/before/after", (int)tmoffs_type);
        return false;
    }
    }

    int hour = 0;
    int min = 0;
    int sec = 0;

    if (EZPI_SUCCESS != EZPI_core_suntime_shift(choosen_suntime->tm_hour,
                                                choosen_suntime->tm_min,
                                                choosen_suntime->tm_sec,
                                                offs_sec, &hour, &min, &sec))
    {
        TRACE_E("isSunState suntime %d:%d:%d is not a time of day",
                choosen_suntime->tm_hour, choosen_suntime->tm_min, choosen_suntime->tm_sec);
        return false;
    }

    /* Written together and only now, so a refusal above leaves the previous
    ** moment untouched rather than half-replaced. */
    defined_moment->tm_hour = hour;
    defined_moment->tm_min = min;
    defined_moment->tm_sec = sec;

    return true;
}
static bool __and_when_block_condition(l_scenes_list_v2_t *scene_node, l_when_block_v2_t *when_block)
{
    bool transition_state = 1; // to make valid judgements
    l_when_block_v2_t *value_when_block = when_block->fields->field_value.u_value.when_block;
    while (value_when_block)
    {
        f_scene_method_v2_t scene_method = EZPI_core_scenes_get_method_v2(value_when_block->block_options.method.type);
        if (scene_method)
        {
            transition_state &= scene_method(scene_node, (void *)value_when_block);
            if (!transition_state)
            {
                break;
            }
        }
        value_when_block = value_when_block->next;
    }
    return transition_state;
}
static uint8_t __isitemState_vs_field_compare(s_item_exp_data_t *new_extract_data, l_fields_v2_t *tmp_field, uint8_t bit_mode_position)
{
    uint8_t flag = 0;
    if (tmp_field)
    {
        switch (tmp_field->value_type)
        {
        case EZLOPI_VALUE_TYPE_ITEM:
        {
            cJSON *cj_item_value = cJSON_CreateObject(__FUNCTION__);
            if (cj_item_value)
            {
                l_ezlopi_item_t *tmp_item = EZPI_core_device_get_item_by_id(tmp_field->field_value.u_value.value_double);
                tmp_item->func(EZLOPI_ACTION_GET_EZLOPI_VALUE, tmp_item, (void *)cj_item_value, NULL);
                cJSON *cj_val = cJSON_GetObjectItem(__FUNCTION__, cj_item_value, ezlopi_value_str);
                if (cj_val)
                {
                    switch (cj_val->type)
                    {
                    case cJSON_String:
                    {
                        if (cj_val->valuestring && new_extract_data->sample_data.u_value.value_string)
                        {
                            if (EZPI_STRNCMP_IF_EQUAL(cj_val->valuestring, new_extract_data->sample_data.u_value.value_string, cj_val->str_value_len, strlen(new_extract_data->sample_data.u_value.value_string)))
                            {
                                flag |= (1 << bit_mode_position);
                            }
                        }
                        break;
                    }
                    case cJSON_Number:
                    {
                        flag |= (cj_val->valuedouble == new_extract_data->sample_data.u_value.value_double) ? (1 << bit_mode_position) : 0;
                        break;
                    }
                    case cJSON_True: // bool_values can be converted to 1/0s
                    {
                        flag |= (new_extract_data->sample_data.u_value.value_bool) ? (1 << bit_mode_position) : 0;
                        break;
                    }
                    case cJSON_False: // bool_values can be converted to 1/0s
                    {
                        flag |= (!new_extract_data->sample_data.u_value.value_bool) ? (1 << bit_mode_position) : 0;
                        break;
                    }
                    default:
                        TRACE_E("Error!! (ITEM_value) can compare only :- string / bool / number ");
                        break;
                    }
                }

                cJSON_Delete(__FUNCTION__, cj_item_value);
            }
            break;
        }
        case EZLOPI_VALUE_TYPE_EXPRESSION:
        {
            s_ezlopi_expressions_t *tmp_expr = EZPI_scenes_expressions_get_node_by_name(tmp_field->field_value.u_value.value_string);
            if (tmp_expr)
            {
                switch (tmp_expr->exp_value.type)
                {
                case EXPRESSION_VALUE_TYPE_BOOL:
                {
                    flag |= (tmp_expr->exp_value.u_value.boolean_value == new_extract_data->sample_data.u_value.value_double) ? (1 << bit_mode_position) : 0;
                    break;
                }
                case EXPRESSION_VALUE_TYPE_NUMBER:
                {
                    flag |= (tmp_expr->exp_value.u_value.number_value == new_extract_data->sample_data.u_value.value_double) ? (1 << bit_mode_position) : 0;
                    break;
                }
                case EXPRESSION_VALUE_TYPE_STRING:
                {
                    if (tmp_expr->exp_value.u_value.str_value && new_extract_data->sample_data.u_value.value_string)
                    {
                        if (EZPI_STRNCMP_IF_EQUAL(tmp_expr->exp_value.u_value.str_value,
                                                  new_extract_data->sample_data.u_value.value_string,
                                                  strlen(tmp_expr->exp_value.u_value.str_value),
                                                  strlen(new_extract_data->sample_data.u_value.value_string)))
                        {
                            flag |= (1 << bit_mode_position);
                        }
                    }
                    break;
                }
                default:
                    TRACE_E("Error!! (Expression_value) can compare only :- string / bool / number ");
                    break;
                }
            }
            break;
        }
        case EZLOPI_VALUE_TYPE_INT:
        {
            flag |= (tmp_field->field_value.u_value.value_double == new_extract_data->sample_data.u_value.value_double) ? (1 << bit_mode_position) : 0;
            break;
        }
        case EZLOPI_VALUE_TYPE_BOOL:
        {
            flag |= (tmp_field->field_value.u_value.value_bool == new_extract_data->sample_data.u_value.value_bool) ? (1 << bit_mode_position) : 0;
            break;
        }
        case EZLOPI_VALUE_TYPE_STRING:
        {
            if (tmp_field->field_value.u_value.value_string && new_extract_data->sample_data.u_value.value_string)
            {
                if (EZPI_STRNCMP_IF_EQUAL(tmp_field->field_value.u_value.value_string,
                                          new_extract_data->sample_data.u_value.value_string,
                                          strlen(tmp_field->field_value.u_value.value_string),
                                          strlen(new_extract_data->sample_data.u_value.value_string)))
                {
                    flag |= (1 << bit_mode_position);
                }
            }
            break;
        }
        default:
            TRACE_E("Error!! Field-type  only support :- Item / Expression / string / bool / number ");
            break;
        }
    }
    else
    {
        flag |= (1 << bit_mode_position);
    }
    return flag;
}
static void __replace_old_with_new_data_val(s_item_exp_data_t *new_extract_data, s_item_exp_data_t *prev_extract_data)
{
    if (new_extract_data && prev_extract_data)
    {
        prev_extract_data->status = new_extract_data->status;
        prev_extract_data->value_type = new_extract_data->value_type;
        prev_extract_data->sample_data.e_type = new_extract_data->sample_data.e_type;

        switch (new_extract_data->sample_data.e_type)
        {
        case VALUE_TYPE_BOOL:
        {
            prev_extract_data->sample_data.u_value.value_bool = new_extract_data->sample_data.u_value.value_bool;
            break;
        }
        case VALUE_TYPE_NUMBER:
        {
            prev_extract_data->sample_data.u_value.value_double = new_extract_data->sample_data.u_value.value_double;
            break;
        }
        case VALUE_TYPE_STRING:
        {
            if (new_extract_data->sample_data.u_value.value_string)
            {
                uint32_t value_len = strlen(new_extract_data->sample_data.u_value.value_string) + 1;
                {
                    if (prev_extract_data->sample_data.u_value.value_string) // erasing prev-string
                    {
                        ezlopi_free(__FUNCTION__, prev_extract_data->sample_data.u_value.value_string);
                    }
                    prev_extract_data->sample_data.u_value.value_string = ezlopi_malloc(__FUNCTION__, value_len);
                    if (prev_extract_data->sample_data.u_value.value_string)
                    {
                        snprintf(prev_extract_data->sample_data.u_value.value_string, value_len, "%s", new_extract_data->sample_data.u_value.value_string);
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }
    else
    {
        TRACE_E("Invalid operation.... null structure");
    }
}

static int ____old_vs_new_extract_data(s_item_exp_data_t *new_extract_data, s_item_exp_data_t *prev_extract_data)
{
    int ret = 0;
    switch (new_extract_data->sample_data.e_type)
    {
    case VALUE_TYPE_BOOL:
    {
        if (prev_extract_data->sample_data.u_value.value_bool == new_extract_data->sample_data.u_value.value_bool)
        {
            ret = 1;
        }
        break;
    }
    case VALUE_TYPE_NUMBER:
    {
        if (prev_extract_data->sample_data.u_value.value_double == new_extract_data->sample_data.u_value.value_double)
        {
            ret = 1;
        }
        break;
    }
    case VALUE_TYPE_STRING:
    {
        if (new_extract_data->sample_data.u_value.value_string && prev_extract_data->sample_data.u_value.value_string)
        {
            if (EZPI_STRNCMP_IF_EQUAL(
                    new_extract_data->sample_data.u_value.value_string,
                    prev_extract_data->sample_data.u_value.value_string,
                    strlen(new_extract_data->sample_data.u_value.value_string),
                    strlen(prev_extract_data->sample_data.u_value.value_string)))
            {
                ret = 1;
            }
        }
        break;
    }
    default:
        break;
    }
    return ret; // if matched --> return 1
}

#endif // CONFIG_EZPI_SERV_ENABLE_MESHBOTS

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
