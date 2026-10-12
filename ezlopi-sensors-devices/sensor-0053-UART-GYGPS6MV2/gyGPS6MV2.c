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
 * @file    gyGPS6MV2.c
 * @brief   perform some function on gyGPS6MV2
 * @author
 * @version 0.1
 * @date    xx
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <string.h>
#include "ezlopi_util_trace.h"
#include "gyGPS6MV2.h"
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

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

//-------------------------------------------------------------------------
// GPGGA MESSAGE PARSING FUNCTION
//-------------------------------------------------------------------------
void GYGPS6MV2_parse_gpgga_mesg(GPS6MV2_t *sensor_0053_UART_gps6mv2_data)
{
    // TRACE_W("GPGGA_sentence => %s", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.GPGGA_sentence);
    uint8_t len = strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.GPGGA_sentence); // length => contents
    char *dummy_container = (char *)ezlopi_malloc(__FUNCTION__, (len + 1));
    if (NULL != dummy_container)
    {
        // duplicate the original message into dummy container ---> to perform parsing
        strncpy(dummy_container, (sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.GPGGA_sentence), len);
        uint8_t comma_count = 0; // or 'i'th word number
        uint8_t start_index = 0;
        uint8_t diff = 0;

        /* One pass over the copy, with comma_count naming the field the current
           character belongs to and start_index holding the position of the
           delimiter that opened it. Every field is filled by writing single
           bytes at an absolute index derived from diff = (i - start_index), so
           nothing in the fill path shortens a field -- which is why each field
           has to be cleared when it starts.

           That clear is the "diff == 0" branch in each case below. diff == 0
           holds exactly once per field, at the delimiter character itself,
           where the "diff > 0" fill guard already has nothing to write. So the
           field is cleared once and then filled, and an empty field still ends
           up cleared because nothing follows the delimiter.

           The branch used to test dummy_container[start_index + 1] == ','
           instead, and that was wrong twice over. It only fired when the
           incoming field was *empty*, so a non-empty value shorter than the
           previous sentence's left the previous value's tail in place: "12"
           satellites followed by "9" read back as "92", and since
           GPS_FIX = ((total_sat > 2) && (gps_quality != 0)) the inflated count
           made the device claim a valid fix on 1 or 2 satellites. And for a
           sentence ending in ',' or '*' the index start_index + 1 is len,
           which is the one byte of the (len + 1)-byte copy that the strncpy
           above never writes -- in bounds, so no sanitiser flags it, but
           uninitialised. Both are gone with the read.

           tests/host/test_gps6mv2.c covers this; it fails against the old
           form. */
        for (uint8_t i = 0; i < len; i++)
        {
            if (dummy_container[i] == '$')
            {
                comma_count = 0;
            }
            if (dummy_container[i] == ',')
            {
                comma_count++;
                start_index = i; // assigns the present starting index of comma
            }
            if (dummy_container[i] == '*')
            {
                comma_count = 15;
                start_index = i; // assigns the present starting index of comma
            }
            switch (comma_count)
            {
            case 0:
                break;
            case 1: // UTC time
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time));
                }
                else
                {
                    if ((diff = (i - start_index)) > 0)
                    {
                        if (diff < 3) // fixed for hour
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time.hour[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time.hour)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time.hour[diff - 1] = *(dummy_container + i);
                        }
                        else if (diff > 2 && diff < 5) // fixed for min
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time.min[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time.min)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time.min[diff - 3] = *(dummy_container + i);
                        }
                        else if (diff > 4 && diff < 10)
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time.sec[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time.sec)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.UTC_time.sec[diff - 5] = *(dummy_container + i);
                        }
                    }
                }
                break;
            }
            case 2: // Latitude
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Latitude, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Latitude));
                }
                else
                {
                    if ((diff = (i - start_index)) > 0)
                    {
                        if (diff < 3) // fixed for lat_degree
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Latitude.lat_degree[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Latitude.lat_degree)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Latitude.lat_degree[diff - 1] = *(dummy_container + i);
                        }
                        else if (diff > 2 && diff < 11)
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Latitude.lat_min[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Latitude.lat_min)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Latitude.lat_min[diff - 3] = *(dummy_container + i);
                        }
                    }
                }
                break;
            }
            case 3: // North/South indicator
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.N_S_indicator, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.N_S_indicator));
                }
                else
                {
                    if ((diff = (i - start_index)) == 1)
                    {
                        sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.N_S_indicator = *(dummy_container + i);
                    }
                }
                break;
            }
            case 4: // Longitude
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Longitude, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Longitude));
                }
                else
                {
                    if ((diff = (i - start_index)) > 0)
                    {
                        if (diff < 4) // fixed for long_degree
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Longitude.long_degree[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Longitude.long_degree)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Longitude.long_degree[diff - 1] = *(dummy_container + i);
                        }
                        else if (diff > 3 && diff < 12)
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Longitude.long_min[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Longitude.long_min)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Longitude.long_min[diff - 4] = *(dummy_container + i);
                        }
                    }
                }
                break;
            }
            case 5: // East/West indicator
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.E_W_indicator, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.E_W_indicator));
                }
                else
                {
                    if ((diff = (i - start_index)) == 1)
                    {
                        sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.E_W_indicator = *(dummy_container + i);
                    }
                }
                break;
            }
            case 6: // Positon_Fix_Indicator
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Positon_fix_quality, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Positon_fix_quality));
                }
                else
                {
                    if ((diff = (i - start_index)) == 1)
                    {
                        sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Positon_fix_quality = *(dummy_container + i);
                    }
                }
                break;
            }
            case 7: // Satellites_used
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Satellites_used, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Satellites_used));
                }
                else
                {
                    if ((diff = (i - start_index)) > 0)
                    {
                        if (diff < 3) // fixed for satellite array
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Satellites_used[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Satellites_used)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Satellites_used[diff - 1] = *(dummy_container + i);
                        }
                    }
                }
                break;
            }
            case 8: // HDOP
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.HDOP, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.HDOP));
                }
                else
                {
                    if ((diff = (i - start_index)) > 0)
                    {
                        if (diff < 5) // fixed for HDOP array
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.HDOP[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.HDOP)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.HDOP[diff - 1] = *(dummy_container + i);
                        }
                    }
                }
                break;
            }
            case 9: // Mean_sea_lvl
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Mean_sea_level, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Mean_sea_level));
                }
                else
                {
                    if ((diff = (i - start_index)) > 0)
                    {
                        if (diff < 7) // fixed for mean_sea_lvl array
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Mean_sea_level[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Mean_sea_level)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Mean_sea_level[diff - 1] = *(dummy_container + i);
                        }
                    }
                }
                break;
            }
            case 10: // MSL_Unit
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.MSL_Unit, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.MSL_Unit));
                }
                else
                {
                    if ((diff = (i - start_index)) == 1)
                    {
                        sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.MSL_Unit = *(dummy_container + i);
                    }
                }
                break;
            }
            case 11: // Geoid_Separation
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Geoid_Separation, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Geoid_Separation));
                }
                else
                {
                    if ((diff = (i - start_index)) > 0)
                    {
                        if (diff < 7) // fixed for Geoid_seperation array
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Geoid_Separation[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Geoid_Separation)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Geoid_Separation[diff - 1] = *(dummy_container + i);
                        }
                    }
                }
                break;
            }
            case 12: // Goid Seperation_Unit
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.GS_Unit, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.GS_Unit));
                }
                else
                {
                    if ((diff = (i - start_index)) == 1)
                    {
                        sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.GS_Unit = *(dummy_container + i);
                    }
                }
                break;
            }
            case 13: // age_from_last
            {        // TRACE_W("age_from_last:";
                break;
            }
            case 14: // station_ID
            {        // TRACE_W("station_ID:"");
                break;
            }
            case 15: // Checksum
            {
                if ((diff = (i - start_index)) == 0)
                { // new field: clear once (see the note above the loop)
                    memset(&sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Checksum, 0, sizeof(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Checksum));
                }
                else
                {
                    if ((diff = (i - start_index)) > 0)
                    {
                        if (diff < 3) // fixed for checksum array
                        {
                            // sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Checksum[strlen(sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Checksum)] = *(dummy_container + i);
                            sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Checksum[diff - 1] = *(dummy_container + i);
                        }
                    }
                }
                break;
            }
            }
        }
        ezlopi_free(__FUNCTION__, dummy_container);
    }

    //-------------------------------------------------------------------------
    // Statemnent that displays extracted GPGGA_data
    //-------------------------------------------------------------------------
    // TRACE_E("Latitude : %s deg, %s min ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Latitude.lat_degree,
    //         sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Latitude.lat_min);
    // TRACE_E("N_S_indicator : %c ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.N_S_indicator);
    // TRACE_E("Longitude : %s deg, %s min ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Longitude.long_degree,
    //         sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Longitude.long_min);
    // TRACE_E("E_W_indicator : %c ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.E_W_indicator);
    // TRACE_E("Positon_fix_quality : %c ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Positon_fix_quality);
    // TRACE_E("Satellites_used : %s ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Satellites_used);
    // TRACE_E("HDOP : %s ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.HDOP);
    // TRACE_E("Mean_sea_level : %s ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Mean_sea_level);
    // TRACE_E("MSL_Unit : %c ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.MSL_Unit);
    // TRACE_E("Geoid_Separation : %s ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Geoid_Separation);
    // TRACE_E("GS_Unit : %c ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.GS_Unit);
    // TRACE_E("age_from_last : %s ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.age_from_last);
    // TRACE_E("station_ID : %s ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.station_ID);
    // TRACE_E("Checksum : %s ", sensor_0053_UART_gps6mv2_data->GPGGA_data_structure.Checksum);
}

//-------------------------------------------------------------------------
// ___ MESSAGE PARSING FUNCTION
//-------------------------------------------------------------------------

/*******************************************************************************
 *                         Static Function Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
