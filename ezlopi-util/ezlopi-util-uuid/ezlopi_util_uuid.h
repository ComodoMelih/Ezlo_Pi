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
 * @file    main.c
 * @brief   perform some function on data
 * @author  
 * @version 0.1
 * @date    1st January 2024
 */

#ifndef __EZLOPI_UTIL_UUID_H__
#define __EZLOPI_UTIL_UUID_H__

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

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
/* `RAND_LENGTH 4` used to be defined here. It said how many bytes the
   generator took from each rand() call, and nothing takes bytes from rand()
   any more -- one esp_fill_random() fills the buffer. Removed rather than kept
   for compatibility: it was an implementation detail of a loop that is gone,
   and an unprefixed RAND_LENGTH in a public header is a name that collides. */

/* Bytes needed for the canonical string form: 32 hex digits + 4 hyphens + the
   terminator. ezlopi_util_uuid_unparse() writes out[36] unconditionally, so a
   caller passing char[36] is one byte short. That requirement was previously
   not stated anywhere. */
#define EZLOPI_UUID_STR_SIZE 37

    typedef unsigned char ezlopi_uuid_t[16];

#define uuid_generate(out) ezlopi_util_uuid_generate_random(out)

    /*******************************************************************************
     *                          Extern Data Declarations
     *******************************************************************************/

    /*******************************************************************************
     *                          Extern Function Prototypes
     *******************************************************************************/
    /**
     * @brief Function to generate random uuid
     *
     * @param out
     */
    void ezlopi_util_uuid_generate_random(ezlopi_uuid_t out);
    /**
     * @brief Writes uuid as the canonical 8-4-4-4-12 hyphenated form.
     *
     * @param uuid Target uuid
     * @param out Destination. MUST have room for at least
     *            EZLOPI_UUID_STR_SIZE (37) bytes: 32 hex digits, 4 hyphens
     *            and the terminator. This was previously undocumented and the
     *            function writes out[36] unconditionally, so a 36-byte buffer
     *            is one byte short.
     *
     * Emits lowercase hex.
     */
    void ezlopi_util_uuid_unparse(const ezlopi_uuid_t uuid, char *out);
    /**
     * @brief Function to copy uuid
     *
     * @param dst
     * @param src
     */
    void ezlopi_util_uuid_copy(ezlopi_uuid_t dst, const ezlopi_uuid_t src);
    /**
     * @brief Parses the canonical hyphenated form, or 32 bare hex digits.
     *
     * @param in NUL-terminated input. Hyphens may appear anywhere and are
     *           skipped; hex digits may be upper or lower case. May be NULL.
     * @param uuid Destination, always fully written.
     *
     * Stops at the terminator, so a short string is not read past its end.
     *
     * KNOWN LIMITATION: there is no way to report failure. Malformed, short
     * or NULL input yields the nil UUID (all zero), which a caller cannot
     * distinguish from successfully parsing the string
     * "00000000-0000-0000-0000-000000000000". The signature was left alone
     * rather than changed to return a bool, because nothing in this
     * repository calls this function and the change was not worth making in
     * the same commit that fixed the parsing itself. If you are about to rely
     * on this in a context where the input is untrusted, add the return value
     * first.
     */
    void ezlopi_util_uuid_parse(const char *in, ezlopi_uuid_t uuid);

#ifdef __cplusplus
}
#endif

#endif // __EZLOPI_UTIL_UUID_H__

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
