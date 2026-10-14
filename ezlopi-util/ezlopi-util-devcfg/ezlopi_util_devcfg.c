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
 * @file    ezlopi_util_devcfg.c
 * @brief   What to do when the stored device configuration will not parse
 * @author
 * @version 1.0
 * @date    13th OCT 2025
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include "ezlopi_util_devcfg.h"

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
e_ezpi_devcfg_action_t EZPI_util_devcfg_recovery(bool parse_ok,
                                                bool chipset_key_present,
                                                bool target_name_known)
{
    if (parse_ok)
    {
        return EZPI_DEVCFG_PROCEED;
    }

    /* The one repair that works, and the only case that may restart. Stamping
     * the key makes chipset_key_present true, so this branch cannot be reached
     * again on the following boot from the same stored configuration -- which
     * is what keeps the restart count at one rather than unbounded.
     *
     * target_name_known guards a build for a chip with no branch in the
     * stamping code: there would be no name to write, so the configuration
     * would go back to flash unchanged and the restart would repair nothing. */
    if (!chipset_key_present && target_name_known)
    {
        return EZPI_DEVCFG_STAMP_AND_REBOOT;
    }

    /* Everything else. The key is already there and is itself the problem, or
     * the chipset matched and a device entry failed. Both are untouched by
     * stamping, so restarting only costs the user every interface they might
     * have used to fix it. */
    return EZPI_DEVCFG_CONTINUE_BOOT;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
