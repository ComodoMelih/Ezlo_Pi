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
 * @file    ezlopi_util_localkey.c
 * @brief   Validation, comparison and generation of the local API key
 * @author
 * @version 1.0
 * @date    12th OCT 2025
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <string.h>

#include "ezlopi_util_localkey.h"

/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/**
 * @brief The generation alphabet: 32 symbols, so five bits of a random byte
 *        select one with no bias and no rejection loop.
 *
 * `l` and `o` are absent because of `1` and `0`; `0` and `1` are absent for the
 * same reason. Keys get read off a serial console and typed into a client, and
 * a character pair that two people transcribe differently costs more than the
 * 0.2 bits per character the omission saves.
 */
static const char localkey_alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";

/** @brief 32 symbols, five bits, one byte of entropy consumed per character. */
#define LOCALKEY_ALPHABET_SIZE 32u
#define LOCALKEY_ALPHABET_MASK (LOCALKEY_ALPHABET_SIZE - 1u)

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/

/**
 * @brief Compile-time check that the alphabet really is 32 symbols.
 *
 * If a character is ever added or removed, the five-bit mask above stops being
 * a bijection and generation starts favouring some symbols over others -- a
 * defect that produces perfectly plausible-looking keys and so would not be
 * noticed by reading the output. Spelled as a negative array size because this
 * component is built as C99, which has no _Static_assert.
 */
typedef char assert_localkey_alphabet_is_32[
    (sizeof(localkey_alphabet) == (LOCALKEY_ALPHABET_SIZE + 1u)) ? 1 : -1];

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/
bool EZPI_util_localkey_is_valid(const char *key)
{
    bool valid = false;

    if (NULL != key)
    {
        size_t len = 0;
        bool printable = true;

        while ((key[len] != '\0') && (len <= EZPI_LOCALKEY_MAX_LEN))
        {
            const unsigned char ch = (unsigned char)key[len];

            /* Printable ASCII excluding space. Checked directly rather than
            ** with isprint(), which accepts space, is locale-dependent, and
            ** has undefined behaviour for negative char values. */
            if ((ch < 0x21u) || (ch > 0x7Eu))
            {
                printable = false;
                break;
            }
            len++;
        }

        if (printable && (len >= EZPI_LOCALKEY_MIN_LEN) && (len <= EZPI_LOCALKEY_MAX_LEN))
        {
            valid = true;
        }
    }

    return valid;
}

bool EZPI_util_localkey_match(const char *stored, const char *supplied)
{
    bool matched = false;

    if ((NULL != stored) && (NULL != supplied))
    {
        const size_t stored_len = strlen(stored);

        /* A stored key too short to be real never authenticates. Blank flash
        ** reads back as an empty string here, and an empty stored key is the
        ** case the previous strncmp() accepted every token for. */
        if ((stored_len >= EZPI_LOCALKEY_MIN_LEN) && (stored_len == strlen(supplied)))
        {
            unsigned char diff = 0;

            /* Whole key every time: no early exit, so the loop does not report
            ** through its duration how many leading bytes were guessed right. */
            for (size_t i = 0; i < stored_len; i++)
            {
                diff |= (unsigned char)((unsigned char)stored[i] ^ (unsigned char)supplied[i]);
            }

            matched = (0 == diff);
        }
    }

    return matched;
}

size_t EZPI_util_localkey_entropy_needed(size_t key_len)
{
    return key_len;
}

bool EZPI_util_localkey_generate(const uint8_t *entropy, size_t entropy_len,
                                 size_t key_len, char *out, size_t out_size)
{
    bool done = false;

    if ((NULL != out) && (out_size > 0))
    {
        out[0] = '\0';
    }

    if ((NULL != entropy) && (NULL != out) &&
        (key_len >= EZPI_LOCALKEY_MIN_LEN) && (key_len <= EZPI_LOCALKEY_MAX_LEN) &&
        (out_size > key_len) &&
        (entropy_len >= EZPI_util_localkey_entropy_needed(key_len)))
    {
        for (size_t i = 0; i < key_len; i++)
        {
            out[i] = localkey_alphabet[entropy[i] & LOCALKEY_ALPHABET_MASK];
        }
        out[key_len] = '\0';
        done = true;
    }

    return done;
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
